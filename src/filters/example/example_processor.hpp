#pragma once

#include <obs-module.h>
#include <graphics/vec4.h>
#include "core/frame_processor.hpp"

// minimal concrete processor: applies a configurable rgba tint to the latest frame.
// required_frame_count = 1; validates the full framework pipeline end-to-end.
class example_processor : public frame_processor {
public:
	// default constructor — no gpu work (see frame_processor constructor contract)
	example_processor() = default;
	~example_processor() override;

	void   on_source_changed(const frame_processor_config &cfg) override;
	void   process(gs_texture_t *const *textures, const uint64_t *timestamps,
		       size_t count) override;
	void   update(obs_data_t *settings) override;
	void   get_properties(obs_properties_t *props) override;
	void   get_defaults(obs_data_t *settings) override;
	size_t required_frame_count() const override;

private:
	gs_effect_t *effect_  = nullptr;
	struct vec4  tint_    = {1.0f, 1.0f, 1.0f, 1.0f};
	uint32_t     width_   = 0;
	uint32_t     height_  = 0;
};
