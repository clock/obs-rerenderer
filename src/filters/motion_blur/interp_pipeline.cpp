#include "interp_pipeline.hpp"
#include "rife_session.hpp"

#include <obs-module.h>
#include <cstring>
#include <algorithm>
#include <cmath>

interp_pipeline::interp_pipeline()  = default;
interp_pipeline::~interp_pipeline() { stop(); }

void interp_pipeline::start(rife_session *sess, int src_w, int src_h,
			     int blend_count, int interp_factor,
			     const float *weights)
{
	stop();

	sess_          = sess;
	w_             = src_w;
	h_             = src_h;
	blend_count_   = blend_count;
	interp_factor_ = interp_factor;
	weights_.assign(weights, weights + blend_count);

	out_bgra_.resize((size_t)src_w * src_h * 4, 0);
	out_ready_ = false;

	stop_flag_.store(false);
	running_.store(true);
	worker_ = std::thread(&interp_pipeline::worker_loop, this);
}

void interp_pipeline::stop()
{
	if (!running_.load())
		return;

	stop_flag_.store(true);
	in_cv_.notify_all();
	if (worker_.joinable())
		worker_.join();
	running_.store(false);

	std::lock_guard<std::mutex> lk(in_mtx_);
	in_queue_.clear();
}

void interp_pipeline::push_frame(const uint8_t *data, uint32_t stride)
{
	capture cap;
	cap.rgb.resize((size_t)3 * h_ * w_);
	bgra_to_nchw(data, stride, w_, h_, cap.rgb.data());

	{
		std::lock_guard<std::mutex> lk(in_mtx_);
		if ((int)in_queue_.size() >= MAX_IN)
			in_queue_.pop_front(); // drop oldest
		in_queue_.push_back(std::move(cap));
	}
	in_cv_.notify_one();
}

bool interp_pipeline::get_output(uint8_t *bgra_out)
{
	std::lock_guard<std::mutex> lk(out_mtx_);
	if (!out_ready_)
		return false;
	std::memcpy(bgra_out, out_bgra_.data(), out_bgra_.size());
	return true;
}

// ---- worker ----------------------------------------------------------------

void interp_pipeline::worker_loop()
{
	// keep last two source frames to interpolate between
	std::vector<float> prev_rgb;
	bool               have_prev = false;

	// ring of virtual frames at (interp_factor * source_fps) rate
	// we accumulate blend_count_ consecutive virtual frames centred around
	// the output position and output one blended frame per source pair.
	//
	// virtual frames per source interval = interp_factor_
	// each source pair contributes interp_factor_ virtual frames
	// (t = 1/F, 2/F, ..., F/F where F = interp_factor_)
	//
	// we accumulate a sliding window of blend_count_ virtual frames
	// and emit one blended output per source pair.

	const int F = interp_factor_;
	const int B = blend_count_;

	// ring buffer of virtual frames (float32 NCHW in padded dims)
	// pad dims may not be known until first infer, so defer allocation
	int pw = 0, ph = 0;
	std::deque<std::vector<float>> vframes;

	while (!stop_flag_.load()) {
		capture cur;
		{
			std::unique_lock<std::mutex> lk(in_mtx_);
			in_cv_.wait(lk, [this] {
				return !in_queue_.empty() ||
				       stop_flag_.load();
			});
			if (stop_flag_.load())
				break;
			cur = std::move(in_queue_.front());
			in_queue_.pop_front();
		}

		if (!have_prev) {
			prev_rgb  = cur.rgb;
			have_prev = true;
			continue;
		}

		if (!sess_ || !sess_->loaded()) {
			prev_rgb = cur.rgb;
			continue;
		}

		// generate F virtual frames for the interval (prev, cur)
		// timesteps: i/F for i = 1..F  (i=F is the cur frame itself)
		if (pw == 0) {
			// first run: figure out padded dims
			int tmp_pw = ((w_ + 31) / 32) * 32;
			int tmp_ph = ((h_ + 31) / 32) * 32;
			std::vector<float> tmp(3 * tmp_ph * tmp_pw);
			sess_->infer(prev_rgb.data(), cur.rgb.data(),
				     w_, h_, 0.5f, tmp.data());
			pw = sess_->pad_w();
			ph = sess_->pad_h();
		}

		size_t padded_elems = (size_t)3 * ph * pw;

		for (int i = 1; i <= F; ++i) {
			float t = (float)i / F;
			std::vector<float> vf(padded_elems);

			if (i == F) {
				// last virtual frame = the cur source frame,
				// pad it directly
				vf.assign(padded_elems, 0.0f);
				for (int c = 0; c < 3; ++c) {
					for (int y = 0; y < h_; ++y) {
						const float *src =
							cur.rgb.data() +
							(size_t)c * h_ * w_ +
							(size_t)y * w_;
						float *dst = vf.data() +
							     (size_t)c * ph * pw +
							     (size_t)y * pw;
						std::memcpy(dst, src,
							    w_ * sizeof(float));
					}
				}
			} else {
				if (!sess_->infer(prev_rgb.data(), cur.rgb.data(),
						  w_, h_, t, vf.data())) {
					// infer failed — pad cur as fallback
					vf.assign(padded_elems, 0.0f);
				}
			}

			vframes.push_back(std::move(vf));
		}

		// when we have enough virtual frames, emit a blended output
		while ((int)vframes.size() >= B) {
			// take B frames centred — for a sliding window just
			// take the first B and slide by F each source pair
			std::vector<std::vector<float>> window;
			window.reserve(B);
			for (int i = 0; i < B; ++i)
				window.push_back(vframes[i]);

			// blend into bgra and publish
			{
				std::vector<uint8_t> blended((size_t)w_ * h_ * 4);
				blend_to_bgra(window, w_, h_, pw, ph,
					      blended.data());
				std::lock_guard<std::mutex> lk(out_mtx_);
				out_bgra_  = std::move(blended);
				out_ready_ = true;
			}

			// advance window by F (one source interval)
			for (int i = 0; i < F && !vframes.empty(); ++i)
				vframes.pop_front();
		}

		prev_rgb = cur.rgb;
	}
}

// ---- helpers ---------------------------------------------------------------

void interp_pipeline::bgra_to_nchw(const uint8_t *bgra, uint32_t stride,
				    int w, int h, float *out)
{
	// out layout: [R plane, G plane, B plane], each plane h*w floats
	float *r_plane = out;
	float *g_plane = out + (size_t)h * w;
	float *b_plane = out + (size_t)2 * h * w;

	const float inv = 1.0f / 255.0f;
	for (int y = 0; y < h; ++y) {
		const uint8_t *row = bgra + (size_t)y * stride;
		float *r_row = r_plane + (size_t)y * w;
		float *g_row = g_plane + (size_t)y * w;
		float *b_row = b_plane + (size_t)y * w;
		for (int x = 0; x < w; ++x) {
			b_row[x] = row[x * 4 + 0] * inv;
			g_row[x] = row[x * 4 + 1] * inv;
			r_row[x] = row[x * 4 + 2] * inv;
		}
	}
}

void interp_pipeline::blend_to_bgra(
	const std::vector<std::vector<float>> &frames,
	int src_w, int src_h, int pad_w, int pad_h,
	uint8_t *bgra_out)
{
	const int n = (int)frames.size();
	size_t plane = (size_t)src_h * src_w;

	// accumulate into float buffer [3, src_h, src_w]
	std::vector<float> acc(3 * plane, 0.0f);

	for (int i = 0; i < n; ++i) {
		float w = (i < (int)weights_.size()) ? weights_[i]
						     : (1.0f / n);
		const float *f = frames[i].data();
		// frames are [3, pad_h, pad_w] — crop to src dimensions
		for (int c = 0; c < 3; ++c) {
			for (int y = 0; y < src_h; ++y) {
				const float *src_row =
					f + (size_t)c * pad_h * pad_w +
					(size_t)y * pad_w;
				float *acc_row =
					acc.data() + (size_t)c * plane +
					(size_t)y * src_w;
				for (int x = 0; x < src_w; ++x)
					acc_row[x] += src_row[x] * w;
			}
		}
	}

	// convert acc [R,G,B planes] → BGRA u8
	const float *r_plane = acc.data();
	const float *g_plane = acc.data() + plane;
	const float *b_plane = acc.data() + 2 * plane;

	for (int y = 0; y < src_h; ++y) {
		uint8_t *row = bgra_out + (size_t)y * src_w * 4;
		for (int x = 0; x < src_w; ++x) {
			auto clamp = [](float v) -> uint8_t {
				int i = (int)(v * 255.0f + 0.5f);
				if (i < 0)   i = 0;
				if (i > 255) i = 255;
				return (uint8_t)i;
			};
			row[x * 4 + 0] = clamp(b_plane[(size_t)y * src_w + x]);
			row[x * 4 + 1] = clamp(g_plane[(size_t)y * src_w + x]);
			row[x * 4 + 2] = clamp(r_plane[(size_t)y * src_w + x]);
			row[x * 4 + 3] = 255;
		}
	}
}
