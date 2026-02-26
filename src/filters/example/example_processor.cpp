#include "example_processor.hpp"

example_processor::~example_processor()
{
	if (effect_) {
		obs_enter_graphics();
		gs_effect_destroy(effect_);
		obs_leave_graphics();
	}
}

void example_processor::on_source_changed(const frame_processor_config &cfg)
{
	width_  = cfg.width;
	height_ = cfg.height;

	// load effect once on first source attach
	if (effect_)
		return;

	obs_enter_graphics();
	char *path = obs_module_file("effects/example.effect");
	char *err  = nullptr;
	effect_ = gs_effect_create_from_file(path, &err);
	bfree(path);
	if (!effect_)
		blog(LOG_ERROR,
		     "[obs-rerenderer] example.effect compile failed: %s",
		     err ? err : "unknown");
	bfree(err);
	obs_leave_graphics();
}

void example_processor::process(gs_texture_t *const *textures,
				const uint64_t * /*timestamps*/, size_t count)
{
	if (!effect_ || count == 0 || !textures[count - 1])
		return;

	gs_texture_t *frame = textures[count - 1]; // most recent

	gs_eparam_t *p_image = gs_effect_get_param_by_name(effect_, "image");
	gs_eparam_t *p_tint  = gs_effect_get_param_by_name(effect_, "tint");

	gs_effect_set_texture(p_image, frame);
	gs_effect_set_vec4(p_tint, &tint_);

	while (gs_effect_loop(effect_, "Draw"))
		gs_draw_sprite(frame, 0, width_, height_);
}

void example_processor::update(obs_data_t *settings)
{
	// obs color picker stores 0xAARRGGBB; vec4_from_bgra handles the swap
	uint32_t c = (uint32_t)obs_data_get_int(settings, "tint_color");
	vec4_from_bgra(&tint_, c);
}

void example_processor::get_properties(obs_properties_t *props)
{
	obs_properties_add_color_alpha(props, "tint_color",
				       obs_module_text("ExampleFilter.TintColor"));
}

void example_processor::get_defaults(obs_data_t *settings)
{
	obs_data_set_default_int(settings, "tint_color", 0xFFFFFFFF);
}

size_t example_processor::required_frame_count() const
{
	return 1;
}
