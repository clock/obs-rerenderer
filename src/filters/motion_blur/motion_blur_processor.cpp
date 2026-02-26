#include "motion_blur_processor.hpp"

#include <windows.h>
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
	return acc / (32.0f * 32.0f * 255.0f);
}

static std::wstring utf8_to_wstring(const std::string &s)
{
	if (s.empty())
		return {};
	int n = MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, nullptr, 0);
	std::wstring w(n, 0);
	MultiByteToWideChar(CP_UTF8, 0, s.c_str(), -1, w.data(), n);
	return w;
}

// ---- lifecycle -------------------------------------------------------------

motion_blur_processor::~motion_blur_processor()
{
	// stop pipeline before entering graphics (thread join must not hold gs lock)
	if (interp_pipe_) {
		interp_pipe_->stop();
		interp_pipe_.reset();
	}

	obs_enter_graphics();
	gs_effect_destroy(effect_);
	free_dedup_resources();
	free_interp_gpu_resources();
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

void motion_blur_processor::free_interp_gpu_resources()
{
	// caller holds graphics context
	gs_stagesurface_destroy(interp_stages_[0]);
	gs_stagesurface_destroy(interp_stages_[1]);
	interp_stages_[0] = interp_stages_[1] = nullptr;
	interp_stage_ready_[0] = interp_stage_ready_[1] = false;
	interp_stage_idx_ = 0;
	gs_texture_destroy(interp_tex_);
	interp_tex_ = nullptr;
	interp_tex_valid_ = false;
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

	// stop pipeline before touching graphics (join must not hold gs lock)
	if (interp_pipe_) {
		interp_pipe_->stop();
		interp_pipe_.reset();
	}

	if (!effect_) {
		obs_enter_graphics();
		load_effect();
		obs_leave_graphics();
	}

	obs_enter_graphics();
	free_dedup_resources();
	dedup_small_  = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	dedup_stage_  = gs_stagesurface_create(32, 32, GS_RGBA);
	dedup_output_ = gs_texrender_create(GS_RGBA, GS_ZS_NONE);
	dedup_prev_.resize(32 * 32 * 4, 0);

	free_interp_gpu_resources();
	if (width_ > 0 && height_ > 0) {
		interp_stages_[0] = gs_stagesurface_create(
			width_, height_, GS_RGBA);
		interp_stages_[1] = gs_stagesurface_create(
			width_, height_, GS_RGBA);
		interp_tex_ = gs_texture_create(width_, height_, GS_RGBA,
						 1, nullptr, GS_DYNAMIC);
		interp_out_buf_.resize((size_t)width_ * height_ * 4);
	}
	obs_leave_graphics();

	{
		std::lock_guard<std::mutex> lk(settings_mutex_);
		active_         = pending_;
		settings_dirty_ = false;
	}
	recompute_weights();

	if (active_.interpolate && !active_.model_path.empty())
		restart_interp_pipeline();
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

	cadence_accum_ = frame_gap_;
	output_valid_  = false;

#ifdef _DEBUG
	printf("[motion_blur] recompute: src=%.4gfps out=%dfps blur=%.2f → blend_count=%d frame_gap=%.3f interp=%d x%d\n",
	       source_fps_, active_.output_fps, active_.blur_amount, blend_count_,
	       frame_gap_, (int)active_.interpolate, active_.interp_multiplier);
#endif

	weight_params p;
	p.curve           = active_.curve;
	p.count           = blend_count_;
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
	return MAX_BLEND_FRAMES;
}

// ---- interpolation pipeline ------------------------------------------------

void motion_blur_processor::restart_interp_pipeline()
{
	if (!active_.interpolate || active_.model_path.empty())
		return;
	if (width_ == 0 || height_ == 0)
		return;

	// reload model only if path changed
	if (active_.model_path != loaded_model_path_) {
		rife_sess_.unload();
		std::wstring wpath = utf8_to_wstring(active_.model_path);
		if (!rife_sess_.load(wpath.c_str(), true)) {
			blog(LOG_ERROR,
			     "[obs-rerenderer] failed to load RIFE model: %s",
			     active_.model_path.c_str());
			return;
		}
		loaded_model_path_ = active_.model_path;
	}

	weight_params p;
	p.curve           = active_.curve;
	p.count           = blend_count_;
	p.gaussian_stddev = active_.gaussian_stddev;
	p.gaussian_mean   = active_.gaussian_mean;
	p.gaussian_lo     = active_.gaussian_lo;
	p.gaussian_hi     = active_.gaussian_hi;
	auto w = compute_weights(p);

	interp_pipe_ = std::make_unique<interp_pipeline>();
	interp_pipe_->start(&rife_sess_, (int)width_, (int)height_,
			    blend_count_, active_.interp_multiplier,
			    w.data());

	interp_tex_valid_       = false;
	interp_stage_ready_[0]  = false;
	interp_stage_ready_[1]  = false;
	interp_stage_idx_       = 0;
}

// ---- duplicate detection ---------------------------------------------------

bool motion_blur_processor::is_duplicate(gs_texture_t *frame)
{
	if (!dedup_small_ || !dedup_stage_ || !frame)
		return false;

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

	gs_texture_t *last = textures[count - 1];
	int start = (int)count - blend_count_;
	if (start < 0) start = 0;
	for (int i = 0; i < MAX_BLEND_FRAMES; ++i) {
		int idx = start + i;
		gs_texture_t *t = (idx < (int)count) ? textures[idx] : last;
		if (p_tex_[i]) gs_effect_set_texture(p_tex_[i], t);
	}

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
			std::string old_model    = active_.model_path;
			int         old_mult     = active_.interp_multiplier;
			bool        old_interp   = active_.interpolate;

			active_         = pending_;
			settings_dirty_ = false;
			recompute_weights();

			bool interp_changed =
				active_.interpolate    != old_interp   ||
				active_.model_path     != old_model    ||
				active_.interp_multiplier != old_mult;

			if (interp_changed || (active_.interpolate && !interp_pipe_)) {
				if (interp_pipe_) {
					interp_pipe_->stop();
					interp_pipe_.reset();
				}
				if (active_.model_path != old_model)
					rife_sess_.unload();
				if (active_.interpolate)
					restart_interp_pipeline();
			} else if (!active_.interpolate && interp_pipe_) {
				interp_pipe_->stop();
				interp_pipe_.reset();
				interp_tex_valid_ = false;
			}
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

	// ---- interpolation path ------------------------------------------------
	if (active_.interpolate && interp_pipe_ && interp_pipe_->running()) {
		int write = interp_stage_idx_;
		int read  = 1 - write;

		// stage current frame for readback next iteration
		if (interp_stages_[write])
			gs_stage_texture(interp_stages_[write],
					 textures[count - 1]);
		interp_stage_ready_[write] = true;

		// map the previously staged frame and push to pipeline
		if (interp_stage_ready_[read] && interp_stages_[read]) {
			uint8_t *data   = nullptr;
			uint32_t stride = 0;
			if (gs_stagesurface_map(interp_stages_[read],
						&data, &stride)) {
				interp_pipe_->push_frame(data, stride);
				gs_stagesurface_unmap(interp_stages_[read]);
			}
		}

		interp_stage_idx_ = 1 - interp_stage_idx_;

		// pull latest blended frame from pipeline
		if (!interp_out_buf_.empty() &&
		    interp_pipe_->get_output(interp_out_buf_.data())) {
			if (interp_tex_)
				gs_texture_set_image(interp_tex_,
						     interp_out_buf_.data(),
						     width_ * 4, false);
			interp_tex_valid_ = true;
		}

		if (interp_tex_valid_ && interp_tex_) {
			gs_effect_t *copy =
				obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *img =
				gs_effect_get_param_by_name(copy, "image");
			gs_effect_set_texture(img, interp_tex_);
			while (gs_effect_loop(copy, "Draw"))
				gs_draw_sprite(interp_tex_, 0, width_, height_);
		} else {
			// pipeline warming up — show latest source frame
			gs_effect_t *pass =
				obs_get_base_effect(OBS_EFFECT_DEFAULT);
			gs_eparam_t *img =
				gs_effect_get_param_by_name(pass, "image");
			gs_effect_set_texture(img, textures[count - 1]);
			while (gs_effect_loop(pass, "Draw"))
				gs_draw_sprite(textures[count - 1], 0,
					       width_, height_);
		}
		return;
	}

	// ---- standard blend path -----------------------------------------------

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

		bool skip = active_.deduplicate &&
			    is_duplicate(textures[count - 1]);

		if (dedup_output_)
			gs_texrender_reset(dedup_output_);
		bool began = !skip && dedup_output_ &&
			     gs_texrender_begin(dedup_output_, width_, height_);
#ifdef _DEBUG
		printf("[motion_blur] new_blend: skip=%d began=%d\n",
		       (int)skip, (int)began);
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
		blit_cached_output();
	} else {
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
	s.enabled          = obs_data_get_bool(settings, "mb_enabled");
	s.blur_amount      = (float)obs_data_get_double(settings, "blur_amount");
	s.output_fps       = (int)obs_data_get_int(settings, "output_fps");
	s.curve            = (weight_curve)obs_data_get_int(settings, "weighting");
	s.gaussian_stddev  = (float)obs_data_get_double(settings, "gaussian_stddev");
	s.gaussian_mean    = (float)obs_data_get_double(settings, "gaussian_mean");
	s.gaussian_lo      = (float)obs_data_get_double(settings, "gaussian_lo");
	s.gaussian_hi      = (float)obs_data_get_double(settings, "gaussian_hi");
	s.deduplicate      = obs_data_get_bool(settings, "deduplicate");
	s.dedup_threshold  = (float)obs_data_get_double(settings, "dedup_threshold");
	s.brightness       = (float)obs_data_get_double(settings, "brightness");
	s.saturation       = (float)obs_data_get_double(settings, "saturation");
	s.contrast         = (float)obs_data_get_double(settings, "contrast");
	s.interpolate      = obs_data_get_bool(settings, "interpolate");
	s.interp_multiplier = (int)obs_data_get_int(settings, "interp_multiplier");
	const char *mp     = obs_data_get_string(settings, "model_path");
	s.model_path       = mp ? mp : "";

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

static bool on_interp_changed(obs_properties_t *props, obs_property_t *,
			      obs_data_t *settings)
{
	bool on = obs_data_get_bool(settings, "interpolate");
	obs_property_set_visible(obs_properties_get(props, "interp_multiplier"),
				 on);
	obs_property_set_visible(obs_properties_get(props, "model_path"), on);
	return true;
}

static bool on_blend_settings_changed(obs_properties_t *props,
				      obs_property_t *, obs_data_t *settings)
{
	struct obs_video_info ovi {};
	double src_fps = obs_get_video_info(&ovi)
				 ? (double)ovi.fps_num / (double)ovi.fps_den
				 : 60.0;

	int    out_fps  = (int)obs_data_get_int(settings, "output_fps");
	double blur_amt = obs_data_get_double(settings, "blur_amount");
	int    blend_n  = compute_blend_count(src_fps, out_fps,
					      (float)blur_amt);
	int    mult     = (int)obs_data_get_int(settings, "interp_multiplier");
	bool   interp   = obs_data_get_bool(settings, "interpolate");

	char buf[160];
	if (interp && mult > 1)
		snprintf(buf, sizeof(buf),
			 "blending %d frames  (%.4gfps source → %dfps, %dx interp → %.4gfps virtual)",
			 blend_n, src_fps, out_fps, mult, src_fps * mult);
	else
		snprintf(buf, sizeof(buf),
			 "blending %d frames  (source %.4gfps → output %dfps)",
			 blend_n, src_fps, out_fps);

	obs_property_t *info = obs_properties_get(props, "blend_info");
	obs_property_set_description(info, buf);
	return true;
}

void motion_blur_processor::get_properties(obs_properties_t *props)
{
	struct obs_video_info ovi {};
	double src_fps = obs_get_video_info(&ovi)
				 ? (double)ovi.fps_num / (double)ovi.fps_den
				 : source_fps_;
	int blend_n = compute_blend_count(src_fps, active_.output_fps,
					  active_.blur_amount);
	char info_buf[160];
	snprintf(info_buf, sizeof(info_buf),
		 "blending %d frames  (source %.4gfps → output %dfps)",
		 blend_n, src_fps, active_.output_fps);

	obs_properties_add_bool(props, "mb_enabled",
				obs_module_text("MotionBlur.Enabled"));

	obs_property_t *p;

	p = obs_properties_add_float_slider(props, "blur_amount",
					    obs_module_text("MotionBlur.BlurAmount"),
					    0.0, 1.0, 0.01);
	obs_property_set_modified_callback(p, on_blend_settings_changed);

	p = obs_properties_add_int(props, "output_fps",
				   obs_module_text("MotionBlur.OutputFPS"),
				   1, 300, 1);
	obs_property_set_modified_callback(p, on_blend_settings_changed);

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

	// interpolation
	p = obs_properties_add_bool(props, "interpolate",
				    obs_module_text("MotionBlur.Interpolate"));
	obs_property_set_modified_callback(p, on_interp_changed);

	p = obs_properties_add_int_slider(props, "interp_multiplier",
					  obs_module_text("MotionBlur.InterpMultiplier"),
					  2, 20, 1);
	obs_property_set_modified_callback(p, on_blend_settings_changed);
	obs_property_set_visible(p, active_.interpolate);

	p = obs_properties_add_path(props, "model_path",
				    obs_module_text("MotionBlur.ModelPath"),
				    OBS_PATH_FILE,
				    "ONNX model (*.onnx)", nullptr);
	obs_property_set_visible(p, active_.interpolate);
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
	obs_data_set_default_bool(settings, "interpolate", false);
	obs_data_set_default_int(settings, "interp_multiplier", 5);
	obs_data_set_default_string(settings, "model_path", "");
}
