#pragma once

#include <vector>
#include <cstddef>

#define MAX_BLEND_FRAMES 16

enum class weight_curve : int {
	equal = 0,
	ascending,
	descending,
	pyramid,
	gaussian,
	gaussian_reverse,
	gaussian_sym,
	vegas,
};

struct weight_params {
	weight_curve curve          = weight_curve::equal;
	int          count          = 1;
	double       gaussian_stddev = 1.0;
	double       gaussian_mean   = 2.0;
	double       gaussian_lo     = 0.0;
	double       gaussian_hi     = 2.0;
};

// returns normalized weights of length p.count, all sum to 1.0
std::vector<float> compute_weights(const weight_params &p);

// number of frames to blend: floor(floor(source_fps/output_fps) * blur_amount), forced odd
// clamped to [1, MAX_BLEND_FRAMES]
int compute_blend_count(double source_fps, int output_fps, float blur_amount);
