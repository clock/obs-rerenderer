#include "example_filter.hpp"
#include "example_processor.hpp"
#include "core/filter_base.hpp"
#include <obs-module.h>

static const char *get_name(void *)
{
	return obs_module_text("ExampleFilter");
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *base = new filter_base(source,
				     std::make_unique<example_processor>());
	base->on_update(settings);
	return base;
}

static void destroy(void *data)
{
	delete static_cast<filter_base *>(data);
}

static void video_render(void *data, gs_effect_t *effect)
{
	static_cast<filter_base *>(data)->on_video_render(effect);
}

static void update(void *data, obs_data_t *settings)
{
	static_cast<filter_base *>(data)->on_update(settings);
}

static obs_properties_t *get_properties(void *data)
{
	obs_properties_t *props = obs_properties_create();
	if (data)
		static_cast<filter_base *>(data)->get_properties_for(props);
	return props;
}

static void get_defaults(obs_data_t *settings)
{
	// delegate to processor — constructor is cheap, no gpu work
	example_processor{}.get_defaults(settings);
}

void register_example_filter()
{
	static obs_source_info info = {};
	info.id           = "obs-rerenderer-example";
	info.type         = OBS_SOURCE_TYPE_FILTER;
	info.output_flags = OBS_SOURCE_VIDEO;
	info.get_name     = get_name;
	info.create       = create;
	info.destroy      = destroy;
	info.video_render = video_render;
	info.update       = update;
	info.get_properties = get_properties;
	info.get_defaults   = get_defaults;
	obs_register_source(&info);
}
