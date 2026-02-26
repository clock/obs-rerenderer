#pragma once

#include <memory>

// thin wrapper around an ONNX Runtime RIFE inference session.
// all methods are called from the interp worker thread, not the render thread.
struct rife_impl;

class rife_session {
public:
	rife_session();
	~rife_session();

	rife_session(const rife_session &) = delete;
	rife_session &operator=(const rife_session &) = delete;

	// load model from .onnx file.
	// prefer_cuda: try CUDA EP first, fall back to DirectML, then CPU.
	bool load(const wchar_t *model_path, bool prefer_cuda = true);
	void unload();
	bool loaded() const;

	// interpolate between two frames at time t (0=img0, 1=img1).
	// img0/img1: float32 NCHW [3 * src_h * src_w], RGB, values in [0,1].
	// out: caller-allocated float32 NCHW [3 * pad_h() * pad_w()].
	//      crop back to src_h x src_w from the top-left corner.
	// returns false on failure.
	bool infer(const float *img0, const float *img1,
		   int src_w, int src_h, float timestep, float *out);

	// padded dimensions used by the model (>= src dimensions, multiple of 32)
	int pad_w() const;
	int pad_h() const;

private:
	std::unique_ptr<rife_impl> d_;
};
