#pragma once

#include <obs-module.h>
#include <deque>
#include <vector>
#include <cstdint>
#include <cstddef>

struct buffered_frame {
	gs_texrender_t *render;
	uint64_t        ts;
};

// gpu texture ring buffer — all gs_* calls must be on the render thread
class frame_buffer {
public:
	frame_buffer();
	~frame_buffer();

	// capture the upstream source (via filter_ctx) into a new slot
	void push_frame(obs_source_t *filter_ctx, uint32_t width,
			uint32_t height, uint64_t ts);

	// fill out with gs_texrender_get_texture() for each slot, oldest first
	void get_textures(std::vector<gs_texture_t *> &out) const;

	// fill out with capture timestamps for each slot, oldest first (nanoseconds)
	void get_timestamps(std::vector<uint64_t> &out) const;

	// destroy all stored textures
	void flush();

	// resize the ring; flushes if shrinking
	void set_capacity(size_t n);

	size_t capacity() const { return capacity_; }
	size_t size() const { return frames_.size(); }

private:
	std::deque<buffered_frame> frames_;
	size_t                     capacity_ = 1;
};
