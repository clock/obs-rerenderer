#include "motion_blur_processor.hpp"

#include <cstring>
#include <cstdio>

// ---- helpers ---------------------------------------------------------------

static float sad_32x32(const uint8_t *a, const uint8_t *b, uint32_t linesize)
{
	// sum of absolute differences on R channel only (enough for dup detection)
	float acc = 0.0f;
	for (int y = 0; y < 32; ++y) {
		const uint8_t *ra = a + y * linesize;
		const uint8_t *rb = b + y * linesize;
		for (int x = 0; x < 32; ++x)
			acc += std::abs((int)ra[x * 4] - (int)rb[x * 4]);
	}
	return acc / (32.0f * 32.0f * 255.0f); // normalize 0..1
}

// ---- lifecycle -------------------------------------------------------------

motion_blur_processor::~motion_blur_processor()
{
	obs_enter_graphics();
	gs_effect_destroy(effect_);
	free_dedup_resources();
	obs_leave_graphics();
}

void motion_blur_processor::free_dedup_resources()
{
	gs_texrender_destroy(dedup_small_);
	dedup_small_ = nullptr;
	gs_stagesurface_destroy(dedup_stage_);
	dedup_stage_ = nullptr;
	gs_texrender_destroy(dedup_output_);
	dedup_output_ = nullptr;
	dedup_has_prev_ = false;
	output_valid_   = false;
}

void motion_blur_processor::load_effect()
{
	char *path = obs_module_file("effects/motion_blur.effect");
	char *err  = nullptr;
	effect_ = gs_effect_create_from_file(path, &err);
	bfree(path);
	if (!effect_) {
		blog(LOG_ERROR,
		     "[obs-rerenderer] motion_blur.effect failed: %s",
		     err ? err : "unknown");
		bfree(err);
		return;
	}
	bfree(err);

	char name[16];
	for (int i = 0; i < MAX_BLEND_FRAMES; ++i) {
		snprintf(name, sizeof(name), "tex_frame_%d", i);
		p_tex_[i] = gs_effect_get_param_by_name(effect_, name);

		snprintf(name, sizeof(name), "w%d", i);
		p_weight_[i] = gs_effect_get_param_by_name(effect_, name);
	}

	p_brightness_ = gs_effect_get_param_by_name(effect_, "brightness");
	p_saturation_ = gs_effect_get_param_by_name(effect_, "saturation");
	p_contrast_   = gs_effect_get_param_by_name(effect_, "contrast");
}

void motion_blur_processor::on_source_changed(const frame_processor_config &cfg)
{
	source_fps_ = cfg.fps;
	width_      = cfg.width;
	height_     = cfg.height;

	if (!effect_) {
		obs_enter_graphics();
		load_effect();
		obs_leave_graphics();
	}

	// (re)create dedup resources
	obs_enter_graphics();
	free_dedup_resources();
	dedup_small_  = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	dedup_stage_  = gs_stagesurface_create(32, 32, GS_RGBA);
	dedup_output_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	dedup_prev_.resize(32 * 32 * 4, 0);
	obs_leave_graphics();

	// apply any pending settings
	{
		std::lock_guard<std::mutex> lk(settings_mutex_);
		active_ = pending_;
		settings_dirty_ = false;
	}
	recompute_weights();
}

// ---- weights ---------------------------------------------------------------

void motion_blur_processor::recompute_weights()
{
	blend_count_ = compute_blend_count(source_fps_, active_.output_fps,
					   active_.blur_amount);

	frame_gap_ = (active_.output_fps > 0 && source_fps_ > 0.0)
			     ? source_fps_ / active_.output_fps
			     : 1.0;
	if (frame_gap_ < 1.0) frame_gap_ = 1.0;

	// trigger immediate render on next frame after settings change
	cadence_accum_ = frame_gap_;
	output_valid_  = false;

#ifdef _DEBUG
	printf("[motion_blur] recompute: src=%.4gfps out=%dfps blur=%.2f → blend_count=%d frame_gap=%.3f\n",
	       source_fps_, active_.output_fps, active_.blur_amount, blend_count_,
	       frame_gap_);
#endif

	weight_params p;
	p.curve          = active_.curve;
	p.count          = blend_count_;
	p.gaussian_stddev = active_.gaussian_stddev;
	p.gaussian_mean   = active_.gaussian_mean;
	p.gaussian_lo     = active_.gaussian_lo;
	p.gaussian_hi     = active_.gaussian_hi;

	auto w = compute_weights(p);

	memset(shader_weights_, 0, sizeof(shader_weights_));
	for (int i = 0; i < blend_count_ && i < MAX_BLEND_FRAMES; ++i)
		shader_weights_[i] = w[i];
}

size_t motion_blur_processor::required_frame_count() const
{
	// always keep MAX so buffer doesn't shrink/flush when blend_count changes
	return MAX_BLEND_FRAMES;
}

// ---- duplicate detection ---------------------------------------------------

bool motion_blur_processor::is_duplicate(gs_texture_t *frame)
{
	if (!dedup_small_ || !dedup_stage_ || !frame)
		return false;

	// downscale to 32x32
	if (gs_texrender_begin(dedup_small_, 32, 32)) {
		struct vec4 black = {};
		gs_clear(GS_CLEAR_COLOR, &black, 1.0f, 0);
		gs_ortho(0.0f, (float)width_, 0.0f, (float)height_,
			 -100.0f, 100.0f);
		gs_effect_t *copy = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *img  = gs_effect_get_param_by_name(copy, "image");
		gs_effect_set_texture(img, frame);
		while (gs_effect_loop(copy, "Draw"))
			gs_draw_sprite(frame, 0, width_, height_);
		gs_texrender_end(dedup_small_);
	} else {
		return false;
	}

	// stage and readback
	gs_stage_texture(dedup_stage_,
			 gs_texrender_get_texture(dedup_small_));

	uint8_t *data   = nullptr;
	uint32_t stride = 0;
	if (!gs_stagesurface_map(dedup_stage_, &data, &stride))
		return false;

	bool dup = false;
	if (dedup_has_prev_) {
		float diff = sad_32x32(data, dedup_prev_.data(), stride);
		dup = (diff < active_.dedup_threshold);
	}

	// copy current into prev
	for (int y = 0; y < 32; ++y)
		memcpy(dedup_prev_.data() + y * 32 * 4, data + y * stride,
		       32 * 4);
	dedup_has_prev_ = true;

	gs_stagesurface_unmap(dedup_stage_);
	return dup;
}

// ---- rendering -------------------------------------------------------------

void motion_blur_processor::render_blend(gs_texture_t *const *textures,
					 size_t count)
{
	if (!effect_) return;

	// bind the most recent blend_count_ frames to slots 0..blend_count_-1,
	// fill remaining slots with the newest frame
	gs_texture_t *last = textures[count - 1];
	int start = (int)count - blend_count_;
	if (start < 0) start = 0;
	for (int i = 0; i < MAX_BLEND_FRAMES; ++i) {
		int idx = start + i;
		gs_texture_t *t = (idx < (int)count) ? textures[idx] : last;
		if (p_tex_[i]) gs_effect_set_texture(p_tex_[i], t);
	}

	// bind weights (unused are 0)
	for (int i = 0; i < MAX_BLEND_FRAMES; ++i) {
		if (p_weight_[i])
			gs_effect_set_float(p_weight_[i], shader_weights_[i]);
	}

	gs_effect_set_float(p_brightness_, active_.brightness);
	gs_effect_set_float(p_saturation_, active_.saturation);
	gs_effect_set_float(p_contrast_,   active_.contrast);

	while (gs_effect_loop(effect_, "Draw"))
		gs_draw_sprite(last, 0, width_, height_);
}

void motion_blur_processor::blit_cached_output()
{
	if (!dedup_output_) return;
	gs_texture_t *tex = gs_texrender_get_texture(dedup_output_);
	if (!tex) return;
	gs_effect_t *copy = obs_get_base_effect(OBS_EFFECT_DEFAULT);
	gs_eparam_t *img  = gs_effect_get_param_by_name(copy, "image");
	gs_effect_set_texture(img, tex);
	while (gs_effect_loop(copy, "Draw"))
		gs_draw_sprite(tex, 0, width_, height_);
}

// ---- main process ----------------------------------------------------------

void motion_blur_processor::process(gs_texture_t *const *textures,
				    const uint64_t * /*timestamps*/,
				    size_t count)
{
	if (!effect_ || count == 0) return;

	// consume pending settings
	{
		std::lock_guard<std::mutex> lk(settings_mutex_);
		if (settings_dirty_) {
			active_ = pending_;
			settings_dirty_ = false;
			recompute_weights();
		}
	}

	if (!active_.enabled) {
		gs_effect_t *pass = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *img  = gs_effect_get_param_by_name(pass, "image");
		gs_effect_set_texture(img, textures[count - 1]);
		while (gs_effect_loop(pass, "Draw"))
			gs_draw_sprite(textures[count - 1], 0, width_, height_);
		return;
	}

	// cadence: only render a new blend every frame_gap source frames,
	// hold the result for intermediate frames
	cadence_accum_ += 1.0;
	bool need_new_blend = (cadence_accum_ >= frame_gap_) || !output_valid_;

#ifdef _DEBUG
	static int s_frame = 0;
	++s_frame;
	if (s_frame % 60 == 1)
		printf("[motion_blur] frame=%d accum=%.3f gap=%.3f valid=%d need=%d count=%zu\n",
		       s_frame, cadence_accum_, frame_gap_, (int)output_valid_,
		       (int)need_new_blend, count);
#endif

	if (need_new_blend) {
		cadence_accum_ -= frame_gap_;
		if (cadence_accum_ < 0.0) cadence_accum_ = 0.0;

		// dedup: skip new render if this frame is a duplicate
		bool skip = active_.deduplicate &&
			    is_duplicate(textures[count - 1]);

		if (dedup_output_)
			gs_texrender_reset(dedup_output_);
		bool began = !skip && dedup_output_ &&
			     gs_texrender_begin(dedup_output_, width_, height_);
#ifdef _DEBUG
		printf("[motion_blur] new_blend: skip=%d dedup_output=%p began=%d\n",
		       (int)skip, (void *)dedup_output_, (int)began);
#endif
		if (began) {
			struct vec4 black = {};
			gs_clear(GS_CLEAR_COLOR, &black, 1.0f, 0);
			gs_ortho(0.0f, (float)width_, 0.0f, (float)height_,
				 -100.0f, 100.0f);
			render_blend(textures, count);
			gs_texrender_end(dedup_output_);
			output_valid_ = true;
		}
	}

	if (output_valid_) {
#ifdef _DEBUG
		gs_texture_t *dbg_tex = gs_texrender_get_texture(dedup_output_);
		if (!dbg_tex && s_frame % 60 == 1)
			printf("[motion_blur] blit: texture is NULL!\n");
#endif
		blit_cached_output();
	} else {
		// no cached output yet, show latest source frame
		gs_effect_t *pass = obs_get_base_effect(OBS_EFFECT_DEFAULT);
		gs_eparam_t *img  = gs_effect_get_param_by_name(pass, "image");
		gs_effect_set_texture(img, textures[count - 1]);
		while (gs_effect_loop(pass, "Draw"))
			gs_draw_sprite(textures[count - 1], 0, width_, height_);
	}
}

// ---- settings --------------------------------------------------------------

void motion_blur_processor::update(obs_data_t *settings)
{
	settings_t s;
	s.enabled     = obs_data_get_bool(settings, "mb_enabled");
	s.blur_amount = (float)obs_data_get_double(settings, "blur_amount");
	s.output_fps  = (int)obs_data_get_int(settings, "output_fps");
	s.curve       = (weight_curve)obs_data_get_int(settings, "weighting");
	s.gaussian_stddev = (float)obs_data_get_double(settings, "gaussian_stddev");
	s.gaussian_mean   = (float)obs_data_get_double(settings, "gaussian_mean");
	s.gaussian_lo     = (float)obs_data_get_double(settings, "gaussian_lo");
	s.gaussian_hi     = (float)obs_data_get_double(settings, "gaussian_hi");
	s.deduplicate     = obs_data_get_bool(settings, "deduplicate");
	s.dedup_threshold = (float)obs_data_get_double(settings, "dedup_threshold");
	s.brightness  = (float)obs_data_get_double(settings, "brightness");
	s.saturation  = (float)obs_data_get_double(settings, "saturation");
	s.contrast    = (float)obs_data_get_double(settings, "contrast");

	std::lock_guard<std::mutex> lk(settings_mutex_);
	pending_        = s;
	settings_dirty_ = true;
}

// ---- properties ------------------------------------------------------------

static bool on_weighting_changed(obs_properties_t *props, obs_property_t *,
				 obs_data_t *settings)
{
	weight_curve c = (weight_curve)obs_data_get_int(settings, "weighting");
	bool is_gauss  = (c == weight_curve::gaussian ||
			  c == weight_curve::gaussian_reverse ||
			  c == weight_curve::gaussian_sym);
	bool has_mean  = (c == weight_curve::gaussian ||
			  c == weight_curve::gaussian_reverse);

	obs_property_set_visible(obs_properties_get(props, "gaussian_stddev"),
				 is_gauss);
	obs_property_set_visible(obs_properties_get(props, "gaussian_mean"),
				 has_mean);
	obs_property_set_visible(obs_properties_get(props, "gaussian_lo"),
				 is_gauss);
	obs_property_set_visible(obs_properties_get(props, "gaussian_hi"),
				 is_gauss);
	return true;
}

static bool on_dedup_changed(obs_properties_t *props, obs_property_t *,
			     obs_data_t *settings)
{
	bool on = obs_data_get_bool(settings, "deduplicate");
	obs_property_set_visible(obs_properties_get(props, "dedup_threshold"),
				 on);
	return true;
}

static bool on_blend_settings_changed(obs_properties_t *props,
				      obs_property_t *, obs_data_t *settings)
{
	struct obs_video_info ovi {};
	double src_fps = obs_get_video_info(&ovi)
				 ? (double)ovi.fps_num / (double)ovi.fps_den
				 : 60.0;

	int    out_fps   = (int)obs_data_get_int(settings, "output_fps");
	double blur_amt  = obs_data_get_double(settings, "blur_amount");
	int    blend_n   = compute_blend_count(src_fps, out_fps,
					       (float)blur_amt);

	char buf[128];
	snprintf(buf, sizeof(buf),
		 "blending %d frames  (source %.4gfps → output %dfps)",
		 blend_n, src_fps, out_fps);

	obs_property_t *info = obs_properties_get(props, "blend_info");
	obs_property_set_description(info, buf);
	return true;
}

void motion_blur_processor::get_properties(obs_properties_t *props)
{
	// compute initial blend info
	struct obs_video_info ovi {};
	double src_fps = obs_get_video_info(&ovi)
				 ? (double)ovi.fps_num / (double)ovi.fps_den
				 : source_fps_;
	int blend_n = compute_blend_count(src_fps, active_.output_fps,
					  active_.blur_amount);
	char info_buf[128];
	snprintf(info_buf, sizeof(info_buf),
		 "blending %d frames  (source %.4gfps → output %dfps)",
		 blend_n, src_fps, active_.output_fps);

	obs_properties_add_bool(props, "mb_enabled",
				obs_module_text("MotionBlur.Enabled"));

	obs_property_t *p;

	// blur amount
	p = obs_properties_add_float_slider(props, "blur_amount",
					    obs_module_text("MotionBlur.BlurAmount"),
					    0.0, 1.0, 0.01);
	obs_property_set_modified_callback(p, on_blend_settings_changed);

	// output fps
	p = obs_properties_add_int(props, "output_fps",
				   obs_module_text("MotionBlur.OutputFPS"),
				   1, 300, 1);
	obs_property_set_modified_callback(p, on_blend_settings_changed);

	// live blend info
	obs_properties_add_text(props, "blend_info", info_buf, OBS_TEXT_INFO);

	// weighting curve
	obs_property_t *wt = obs_properties_add_list(
		props, "weighting",
		obs_module_text("MotionBlur.Weighting"),
		OBS_COMBO_TYPE_LIST, OBS_COMBO_FORMAT_INT);
	obs_property_list_add_int(wt, obs_module_text("MotionBlur.Weighting.Equal"),
				  (int)weight_curve::equal);
	obs_property_list_add_int(wt, obs_module_text("MotionBlur.Weighting.Ascending"),
				  (int)weight_curve::ascending);
	obs_property_list_add_int(wt, obs_module_text("MotionBlur.Weighting.Descending"),
				  (int)weight_curve::descending);
	obs_property_list_add_int(wt, obs_module_text("MotionBlur.Weighting.Pyramid"),
				  (int)weight_curve::pyramid);
	obs_property_list_add_int(wt, obs_module_text("MotionBlur.Weighting.Gaussian"),
				  (int)weight_curve::gaussian);
	obs_property_list_add_int(wt, obs_module_text("MotionBlur.Weighting.GaussianReverse"),
				  (int)weight_curve::gaussian_reverse);
	obs_property_list_add_int(wt, obs_module_text("MotionBlur.Weighting.GaussianSym"),
				  (int)weight_curve::gaussian_sym);
	obs_property_list_add_int(wt, obs_module_text("MotionBlur.Weighting.Vegas"),
				  (int)weight_curve::vegas);
	obs_property_set_modified_callback(wt, on_weighting_changed);

	// gaussian params (hidden by default)
	p = obs_properties_add_float_slider(props, "gaussian_stddev",
					    obs_module_text("MotionBlur.GaussianStdDev"),
					    0.1, 4.0, 0.01);
	obs_property_set_visible(p, false);

	p = obs_properties_add_float_slider(props, "gaussian_mean",
					    obs_module_text("MotionBlur.GaussianMean"),
					    0.0, 4.0, 0.01);
	obs_property_set_visible(p, false);

	p = obs_properties_add_float_slider(props, "gaussian_lo",
					    obs_module_text("MotionBlur.GaussianBoundLo"),
					    -4.0, 4.0, 0.01);
	obs_property_set_visible(p, false);

	p = obs_properties_add_float_slider(props, "gaussian_hi",
					    obs_module_text("MotionBlur.GaussianBoundHi"),
					    -4.0, 4.0, 0.01);
	obs_property_set_visible(p, false);

	// color correction
	obs_properties_add_float_slider(props, "brightness",
					obs_module_text("MotionBlur.Brightness"),
					0.0, 2.0, 0.01);
	obs_properties_add_float_slider(props, "saturation",
					obs_module_text("MotionBlur.Saturation"),
					0.0, 2.0, 0.01);
	obs_properties_add_float_slider(props, "contrast",
					obs_module_text("MotionBlur.Contrast"),
					0.0, 2.0, 0.01);

	// dedup
	p = obs_properties_add_bool(props, "deduplicate",
				    obs_module_text("MotionBlur.Deduplicate"));
	obs_property_set_modified_callback(p, on_dedup_changed);

	obs_properties_add_float_slider(props, "dedup_threshold",
					obs_module_text("MotionBlur.DedupThreshold"),
					0.0001, 0.1, 0.0001);
}

void motion_blur_processor::get_defaults(obs_data_t *settings)
{
	obs_data_set_default_bool(settings, "mb_enabled", true);
	obs_data_set_default_double(settings, "blur_amount", 1.0);
	obs_data_set_default_int(settings, "output_fps", 60);
	obs_data_set_default_int(settings, "weighting",
				 (int)weight_curve::equal);
	obs_data_set_default_double(settings, "gaussian_stddev", 1.0);
	obs_data_set_default_double(settings, "gaussian_mean", 2.0);
	obs_data_set_default_double(settings, "gaussian_lo", 0.0);
	obs_data_set_default_double(settings, "gaussian_hi", 2.0);
	obs_data_set_default_bool(settings, "deduplicate", false);
	obs_data_set_default_double(settings, "dedup_threshold", 0.001);
	obs_data_set_default_double(settings, "brightness", 1.0);
	obs_data_set_default_double(settings, "saturation", 1.0);
	obs_data_set_default_double(settings, "contrast", 1.0);
}
