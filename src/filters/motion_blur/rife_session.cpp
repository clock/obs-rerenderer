#include "rife_session.hpp"

#ifndef OBS_RERENDERER_HAS_ORT

// dummy definition so unique_ptr<rife_impl> destructor compiles
struct rife_impl {};

// stubs — build without ort.props
rife_session::rife_session()  = default;
rife_session::~rife_session() = default;
bool rife_session::load(const wchar_t *, bool) { return false; }
void rife_session::unload() {}
bool rife_session::loaded() const { return false; }
bool rife_session::infer(const float *, const float *, int, int, float, float *) { return false; }
int  rife_session::pad_w() const { return 0; }
int  rife_session::pad_h() const { return 0; }

#else // OBS_RERENDERER_HAS_ORT

#include <obs-module.h>
#include <onnxruntime_cxx_api.h>

#include <vector>
#include <string>
#include <cstring>
#include <algorithm>

static int align_up(int v, int a)
{
	return (v + a - 1) / a * a;
}

struct rife_impl {
	Ort::Env   env{ORT_LOGGING_LEVEL_WARNING, "rife"};
	std::unique_ptr<Ort::Session> session;

	std::vector<std::string>  in_names_s;
	std::vector<std::string>  out_names_s;
	std::vector<const char *> in_names;
	std::vector<const char *> out_names;

	int pad_w = 0;
	int pad_h = 0;
};

rife_session::rife_session() : d_(std::make_unique<rife_impl>()) {}
rife_session::~rife_session() = default;

bool rife_session::loaded() const { return d_->session != nullptr; }
int  rife_session::pad_w()   const { return d_->pad_w; }
int  rife_session::pad_h()   const { return d_->pad_h; }

bool rife_session::load(const wchar_t *model_path, bool prefer_cuda)
{
	unload();
	try {
		Ort::SessionOptions opts;
		opts.SetGraphOptimizationLevel(ORT_ENABLE_ALL);
		opts.SetIntraOpNumThreads(1);

		bool added = false;

		if (prefer_cuda && !added) {
			try {
				OrtCUDAProviderOptions cuda{};
				cuda.device_id = 0;
				opts.AppendExecutionProvider_CUDA(cuda);
				added = true;
				blog(LOG_INFO,
				     "[obs-rerenderer] rife: using CUDA");
			} catch (...) {}
		}

		if (!added) {
			try {
				const OrtApi &api = Ort::GetApi();
				OrtDmlApi const *dml = nullptr;
				api.GetExecutionProviderApi(
					"DML", ORT_API_VERSION,
					reinterpret_cast<const void **>(&dml));
				if (dml) {
					dml->SessionOptionsAppendExecutionProvider_DML(
						opts, 0);
					added = true;
					blog(LOG_INFO,
					     "[obs-rerenderer] rife: using DirectML");
				}
			} catch (...) {}
		}

		if (!added)
			blog(LOG_WARNING,
			     "[obs-rerenderer] rife: no GPU provider, using CPU");

		d_->session = std::make_unique<Ort::Session>(
			d_->env, model_path, opts);

		Ort::AllocatorWithDefaultOptions alloc;

		size_t n_in = d_->session->GetInputCount();
		d_->in_names_s.resize(n_in);
		d_->in_names.resize(n_in);
		for (size_t i = 0; i < n_in; ++i) {
			d_->in_names_s[i] =
				d_->session->GetInputNameAllocated(i, alloc).get();
			d_->in_names[i] = d_->in_names_s[i].c_str();
		}

		size_t n_out = d_->session->GetOutputCount();
		d_->out_names_s.resize(n_out);
		d_->out_names.resize(n_out);
		for (size_t i = 0; i < n_out; ++i) {
			d_->out_names_s[i] =
				d_->session->GetOutputNameAllocated(i, alloc).get();
			d_->out_names[i] = d_->out_names_s[i].c_str();
		}

		blog(LOG_INFO, "[obs-rerenderer] rife: model loaded (%zu in, %zu out)",
		     n_in, n_out);
		return true;
	} catch (const Ort::Exception &e) {
		blog(LOG_ERROR, "[obs-rerenderer] rife load: %s", e.what());
		return false;
	}
}

void rife_session::unload()
{
	d_->session.reset();
	d_->in_names_s.clear();
	d_->out_names_s.clear();
	d_->in_names.clear();
	d_->out_names.clear();
	d_->pad_w = d_->pad_h = 0;
}

bool rife_session::infer(const float *img0, const float *img1,
			 int src_w, int src_h, float timestep, float *out)
{
	if (!d_->session)
		return false;

	const int align = 32;
	int pw = align_up(src_w, align);
	int ph = align_up(src_h, align);
	d_->pad_w = pw;
	d_->pad_h = ph;

	size_t plane   = (size_t)ph * pw;
	size_t n_elems = 3 * plane;

	// pad inputs from [3, src_h, src_w] to [3, ph, pw] (zero right/bottom)
	std::vector<float> buf0(n_elems, 0.0f);
	std::vector<float> buf1(n_elems, 0.0f);
	for (int c = 0; c < 3; ++c) {
		for (int y = 0; y < src_h; ++y) {
			const float *s0 = img0 + (size_t)c * src_h * src_w + (size_t)y * src_w;
			const float *s1 = img1 + (size_t)c * src_h * src_w + (size_t)y * src_w;
			float *d0 = buf0.data() + (size_t)c * plane + (size_t)y * pw;
			float *d1 = buf1.data() + (size_t)c * plane + (size_t)y * pw;
			std::memcpy(d0, s0, src_w * sizeof(float));
			std::memcpy(d1, s1, src_w * sizeof(float));
		}
	}

	// timestep as spatial map [1, 1, ph, pw] — RIFE v4 expects this
	std::vector<float> ts_map(plane, timestep);

	try {
		Ort::MemoryInfo mem =
			Ort::MemoryInfo::CreateCpu(OrtArenaAllocator, OrtMemTypeDefault);

		int64_t img_dims[4]  = {1, 3, ph, pw};
		int64_t ts_dims[4]   = {1, 1, ph, pw};

		Ort::Value inputs[3] = {
			Ort::Value::CreateTensor<float>(mem, buf0.data(),
							n_elems, img_dims, 4),
			Ort::Value::CreateTensor<float>(mem, buf1.data(),
							n_elems, img_dims, 4),
			Ort::Value::CreateTensor<float>(mem, ts_map.data(),
							plane, ts_dims, 4),
		};

		auto outputs = d_->session->Run(
			Ort::RunOptions{nullptr},
			d_->in_names.data(), inputs, d_->in_names.size(),
			d_->out_names.data(), 1);

		const float *p = outputs[0].GetTensorData<float>();
		std::memcpy(out, p, n_elems * sizeof(float));
		return true;
	} catch (const Ort::Exception &e) {
		blog(LOG_ERROR, "[obs-rerenderer] rife infer: %s", e.what());
		return false;
	}
}

#endif // OBS_RERENDERER_HAS_ORT
