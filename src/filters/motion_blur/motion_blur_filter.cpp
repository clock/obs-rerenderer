#include "motion_blur_filter.hpp"
#include "motion_blur_processor.hpp"
#include "core/filter_base.hpp"
#include <obs-module.h>

#ifdef _DEBUG
#include <windows.h>
#include <cstdio>
#endif

static const char *get_name(void *)
{
	return obs_module_text("MotionBlur");
}

static void *create(obs_data_t *settings, obs_source_t *source)
{
	auto *base = new filter_base(
		source, std::make_unique<motion_blur_processor>());
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
	motion_blur_processor{}.get_defaults(settings);
}

void register_motion_blur_filter()
{
#ifdef _DEBUG
	AllocConsole();
	FILE *fp;
	freopen_s(&fp, "CONOUT$", "w", stdout);
	freopen_s(&fp, "CONOUT$", "w", stderr);
	printf("[motion_blur] debug console ready\n");
#endif

	static obs_source_info info = {};
	info.id             = "obs-rerenderer-motion-blur";
	info.type           = OBS_SOURCE_TYPE_FILTER;
	info.output_flags   = OBS_SOURCE_VIDEO;
	info.get_name       = get_name;
	info.create         = create;
	info.destroy        = destroy;
	info.video_render   = video_render;
	info.update         = update;
	info.get_properties = get_properties;
	info.get_defaults   = get_defaults;
	obs_register_source(&info);
}
