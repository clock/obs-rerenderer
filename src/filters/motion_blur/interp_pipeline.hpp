#pragma once

#include <cstdint>
#include <vector>
#include <deque>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <memory>

class rife_session;

// async pipeline: receives BGRA source frames from the render thread,
// runs RIFE interpolation + temporal blend on a worker thread,
// and makes the blended BGRA output available back to the render thread.
class interp_pipeline {
public:
	interp_pipeline();
	~interp_pipeline();

	interp_pipeline(const interp_pipeline &) = delete;
	interp_pipeline &operator=(const interp_pipeline &) = delete;

	// start the worker thread.
	// blend_count: how many interpolated frames to blend per output frame.
	// interp_factor: how many virtual frames to generate per source pair
	//                (e.g. 5 for 5x interpolation: 4 intermediate + 1 endpoint).
	void start(rife_session *sess, int src_w, int src_h,
		   int blend_count, int interp_factor,
		   const float *weights); // weights[blend_count], normalised

	void stop();

	bool running() const { return running_.load(std::memory_order_relaxed); }

	// render thread: submit a new source BGRA frame.
	// data: BGRA pixels, stride = bytes per row.
	// drops oldest entry if the input queue is already full.
	void push_frame(const uint8_t *data, uint32_t stride);

	// render thread: get the latest blended output.
	// copies into caller-supplied bgra buffer (w*h*4 bytes).
	// returns false if no output is ready yet.
	bool get_output(uint8_t *bgra_out);

private:
	struct capture {
		std::vector<float> rgb; // float32 NCHW [3, h, w], values [0,1]
	};

	rife_session        *sess_         = nullptr;
	int                  w_            = 0;
	int                  h_            = 0;
	int                  blend_count_  = 1;
	int                  interp_factor_= 1;
	std::vector<float>   weights_;

	// input queue: render → worker
	static constexpr int MAX_IN = 6;
	std::deque<capture>      in_queue_;
	std::mutex               in_mtx_;
	std::condition_variable  in_cv_;

	// output: worker → render
	std::vector<uint8_t> out_bgra_;
	std::mutex           out_mtx_;
	bool                 out_ready_ = false;

	std::thread       worker_;
	std::atomic<bool> running_{false};
	std::atomic<bool> stop_flag_{false};

	void worker_loop();

	// convert BGRA row-pitched → float32 NCHW RGB
	static void bgra_to_nchw(const uint8_t *bgra, uint32_t stride,
				  int w, int h, float *out);

	// blend float32 NCHW frames (accumulate into accum, then crop & convert)
	void blend_to_bgra(const std::vector<std::vector<float>> &frames,
			   int src_w, int src_h, int pad_w, int pad_h,
			   uint8_t *bgra_out);
};
