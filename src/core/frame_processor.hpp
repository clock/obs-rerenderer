#pragma once

#include <obs-module.h>
#include <cstddef>
#include <cstdint>

// configuration snapshot passed to a processor when the source changes
struct frame_processor_config {
	uint32_t width;
	uint32_t height;
	double   fps;
};

// pure abstract interface — no knowledge of any concrete use case
//
// constructor contract: must be cheap (no gpu work). gpu resources belong in
// on_source_changed(). this lets the framework instantiate a temporary
// processor to query defaults before a filter instance exists.
//
// passthrough: to render the source unchanged from process(), call
//   obs_source_draw(textures[count - 1], 0, 0, width, height, false)
// no custom shader needed.
class frame_processor {
public:
	virtual ~frame_processor() = default;

	// called on render thread when source dimensions/fps change;
	// load or rebuild gpu resources here
	virtual void on_source_changed(const frame_processor_config &cfg) = 0;

	// called on render thread each frame.
	// textures and timestamps are parallel arrays of length count, oldest first.
	// timestamps[count - 1] is the current frame time in nanoseconds.
	// processor is responsible for drawing to the current obs render target.
	virtual void process(gs_texture_t *const *textures,
			     const uint64_t *timestamps, size_t count) = 0;

	// called on ui thread when settings change — do not touch gpu resources here
	virtual void update(obs_data_t *settings) = 0;

	// populate the properties panel
	virtual void get_properties(obs_properties_t *props) = 0;

	// set obs_data defaults; called on a temporary cheap instance (see ctor contract)
	virtual void get_defaults(obs_data_t *settings) = 0;

	// how many frames the ring buffer must hold for this processor
	virtual size_t required_frame_count() const = 0;
};
