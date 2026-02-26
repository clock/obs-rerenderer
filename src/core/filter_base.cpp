#include "filter_base.hpp"
#include <media-io/video-io.h>

filter_base::filter_base(obs_source_t *source,
			 std::unique_ptr<frame_processor> proc)
	: source_(source),
	  buffer_(std::make_unique<frame_buffer>()),
	  processor_(std::move(proc))
{
}

filter_base::~filter_base()
{
	obs_enter_graphics();
	buffer_->flush();
	obs_leave_graphics();
}

void filter_base::apply_rebuild(uint32_t w, uint32_t h)
{
	if (w == 0 || h == 0)
		return;

	struct obs_video_info ovi {};
	double fps = obs_get_video_info(&ovi)
			     ? (double)ovi.fps_num / (double)ovi.fps_den
			     : 30.0;

	frame_processor_config cfg{w, h, fps};
	buffer_->set_capacity(processor_->required_frame_count());
	buffer_->flush();
	processor_->on_source_changed(cfg);

	width_  = w;
	height_ = h;
	rebuild_requested_.store(false, std::memory_order_relaxed);
}

void filter_base::on_video_render(gs_effect_t * /*effect*/)
{
	obs_source_t *target = obs_filter_get_target(source_);
	if (!target) {
		obs_source_skip_video_filter(source_);
		return;
	}

	uint32_t w = obs_source_get_width(target);
	uint32_t h = obs_source_get_height(target);

	if (w != width_ || h != height_)
		rebuild_requested_.store(true, std::memory_order_relaxed);

	if (rebuild_requested_.load(std::memory_order_relaxed))
		apply_rebuild(w, h);

	if (width_ == 0 || height_ == 0) {
		obs_source_skip_video_filter(source_);
		return;
	}

	buffer_->push_frame(source_, width_, height_,
			    obs_get_video_frame_time());
	buffer_->get_textures(tex_list_);
	buffer_->get_timestamps(ts_list_);

	if (tex_list_.empty()) {
		obs_source_skip_video_filter(source_);
		return;
	}

	processor_->process(tex_list_.data(), ts_list_.data(), tex_list_.size());
}

void filter_base::on_update(obs_data_t *settings)
{
	processor_->update(settings);
	rebuild_requested_.store(true, std::memory_order_relaxed);
}

void filter_base::get_properties_for(obs_properties_t *props)
{
	processor_->get_properties(props);
}
