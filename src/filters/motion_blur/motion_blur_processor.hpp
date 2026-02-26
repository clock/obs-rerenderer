#pragma once

#include <obs-module.h>
#include <atomic>
#include <mutex>
#include <vector>
#include "core/frame_processor.hpp"
#include "blur_weights.hpp"

class motion_blur_processor : public frame_processor {
public:
	motion_blur_processor() = default;
	~motion_blur_processor() override;

	void   on_source_changed(const frame_processor_config &cfg) override;
	void   process(gs_texture_t *const *textures,
		       const uint64_t *timestamps, size_t count) override;
	void   update(obs_data_t *settings) override;
	void   get_properties(obs_properties_t *props) override;
	void   get_defaults(obs_data_t *settings) override;
	size_t required_frame_count() const override;

private:
	// settings written by ui thread, consumed on render thread
	struct settings_t {
		bool         enabled        = true;
		float        blur_amount    = 1.0f;
		int          output_fps     = 60;
		weight_curve curve          = weight_curve::equal;
		float        gaussian_stddev = 1.0f;
		float        gaussian_mean   = 2.0f;
		float        gaussian_lo     = 0.0f;
		float        gaussian_hi     = 2.0f;
		bool         deduplicate    = false;
		float        dedup_threshold = 0.001f;
		float        brightness     = 1.0f;
		float        saturation     = 1.0f;
		float        contrast       = 1.0f;
	};

	std::mutex   settings_mutex_;
	settings_t   pending_;
	settings_t   active_;
	bool         settings_dirty_ = false;

	// gpu resources (render thread only)
	gs_effect_t       *effect_  = nullptr;
	gs_eparam_t       *p_tex_[MAX_BLEND_FRAMES]     = {};
	gs_eparam_t       *p_weight_[MAX_BLEND_FRAMES]  = {};
	gs_eparam_t       *p_brightness_  = nullptr;
	gs_eparam_t       *p_saturation_  = nullptr;
	gs_eparam_t       *p_contrast_    = nullptr;

	// dedup resources
	gs_texrender_t    *dedup_small_   = nullptr; // 32x32 downscale
	gs_stagesurf_t *dedup_stage_   = nullptr;
	gs_texrender_t    *dedup_output_  = nullptr; // cached last rendered output
	std::vector<uint8_t> dedup_prev_;
	bool               dedup_has_prev_ = false;

	// render thread state
	double             source_fps_   = 60.0;
	uint32_t           width_        = 0;
	uint32_t           height_       = 0;
	float              shader_weights_[MAX_BLEND_FRAMES] = {};
	int                blend_count_  = 1;
	double             frame_gap_    = 1.0; // source frames per output frame
	double             cadence_accum_ = 0.0;
	bool               output_valid_ = false;

	void load_effect();
	void free_dedup_resources();
	void recompute_weights();
	bool is_duplicate(gs_texture_t *frame);
	void render_blend(gs_texture_t *const *textures, size_t count);
	void blit_cached_output();
};
