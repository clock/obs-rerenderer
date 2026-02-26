#include "plugin_main.hpp"
#include "filters/example/example_filter.hpp"

OBS_DECLARE_MODULE()
OBS_MODULE_USE_DEFAULT_LOCALE("obs-rerenderer", "en-US")

bool obs_module_load(void)
{
	register_example_filter();
	return true;
}

void obs_module_unload(void) {}
