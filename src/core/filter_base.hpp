#pragma once

#include <obs-module.h>
#include <atomic>
#include <memory>
#include <vector>
#include "frame_buffer.hpp"
#include "frame_processor.hpp"

// obs vtable bridge: owns a frame_buffer and a frame_processor.
// concrete filters create one of these and wire up obs_source_info trampolines.
class filter_base {
public:
	filter_base(obs_source_t *source,
		    std::unique_ptr<frame_processor> proc);
	~filter_base();

	// call from obs video_render callback (render thread)
	void on_video_render(gs_effect_t *effect);

	// call from obs update callback (ui thread)
	void on_update(obs_data_t *settings);

	// populate props via the owned processor
	void get_properties_for(obs_properties_t *props);

private:
	obs_source_t                     *source_;
	std::unique_ptr<frame_buffer>     buffer_;
	std::unique_ptr<frame_processor>  processor_;
	std::atomic<bool>                 rebuild_requested_{true};
	uint32_t                          width_  = 0;
	uint32_t                          height_ = 0;
	std::vector<gs_texture_t *>       tex_list_;
	std::vector<uint64_t>             ts_list_;

	void apply_rebuild(uint32_t w, uint32_t h);
};
