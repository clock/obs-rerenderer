#include "frame_buffer.hpp"

frame_buffer::frame_buffer() = default;

frame_buffer::~frame_buffer()
{
	flush();
}

void frame_buffer::push_frame(obs_source_t *filter_ctx, uint32_t width,
			      uint32_t height, uint64_t ts)
{
	// evict oldest if at capacity
	while (frames_.size() >= capacity_) {
		gs_texrender_destroy(frames_.front().render);
		frames_.pop_front();
	}

	gs_texrender_t *render = gs_texrender_create(GS_RGBA, GS_ZS_NONE);

	if (gs_texrender_begin(render, width, height)) {
		struct vec4 clear_color;
		vec4_zero(&clear_color);
		gs_clear(GS_CLEAR_COLOR, &clear_color, 1.0f, 0);
		gs_ortho(0.0f, (float)width, 0.0f, (float)height, -100.0f,
			 100.0f);
		obs_source_video_render(obs_filter_get_target(filter_ctx));
		gs_texrender_end(render);
	}

	frames_.push_back({render, ts});
}

void frame_buffer::get_textures(std::vector<gs_texture_t *> &out) const
{
	out.clear();
	out.reserve(frames_.size());
	for (const auto &f : frames_)
		out.push_back(gs_texrender_get_texture(f.render));
}

void frame_buffer::get_timestamps(std::vector<uint64_t> &out) const
{
	out.clear();
	out.reserve(frames_.size());
	for (const auto &f : frames_)
		out.push_back(f.ts);
}

void frame_buffer::flush()
{
	for (auto &f : frames_)
		gs_texrender_destroy(f.render);
	frames_.clear();
}

void frame_buffer::set_capacity(size_t n)
{
	capacity_ = (n < 1) ? 1 : n;
	while (frames_.size() > capacity_) {
		gs_texrender_destroy(frames_.front().render);
		frames_.pop_front();
	}
}
