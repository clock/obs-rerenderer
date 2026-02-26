#include "blur_weights.hpp"

#include <algorithm>
#include <cmath>
#include <numeric>

static std::vector<double> normalize(std::vector<double> w)
{
	double mn = *std::min_element(w.begin(), w.end());
	if (mn < 0.0) {
		double shift = -mn + 1.0;
		for (auto &v : w) v += shift;
	}
	double total = std::accumulate(w.begin(), w.end(), 0.0);
	for (auto &v : w) v /= total;
	return w;
}

static std::vector<double> scale_range(int n, double lo, double hi)
{
	std::vector<double> r(n);
	if (n <= 1) {
		std::fill(r.begin(), r.end(), lo);
	} else {
		double step = (hi - lo) / (n - 1);
		for (int i = 0; i < n; ++i)
			r[i] = lo + i * step;
	}
	return r;
}

static std::vector<double> weights_equal(int n)
{
	return std::vector<double>(n, 1.0 / n);
}

static std::vector<double> weights_ascending(int n)
{
	std::vector<double> w(n);
	for (int i = 0; i < n; ++i) w[i] = i + 1.0;
	return normalize(w);
}

static std::vector<double> weights_descending(int n)
{
	std::vector<double> w(n);
	for (int i = 0; i < n; ++i) w[i] = n - i;
	return normalize(w);
}

static std::vector<double> weights_pyramid(int n)
{
	double half = (n - 1) / 2.0;
	std::vector<double> w(n);
	for (int i = 0; i < n; ++i)
		w[i] = half - std::abs(i - half) + 1.0;
	return normalize(w);
}

static std::vector<double> weights_gaussian(int n, double mean, double stddev,
					    double lo, double hi)
{
	if (lo == hi) return weights_equal(n);
	auto xs = scale_range(n, lo, hi);
	double denom = 2.0 * stddev * stddev;
	std::vector<double> w(n);
	for (int i = 0; i < n; ++i)
		w[i] = std::exp(-std::pow(xs[i] - mean, 2.0) / denom);
	return normalize(w);
}

static std::vector<double> weights_vegas(int n)
{
	std::vector<double> w(n, 1.0);
	if (n % 2 == 0 && n > 2) {
		for (int i = 1; i < n - 1; ++i) w[i] = 2.0;
	}
	return normalize(w);
}

std::vector<float> compute_weights(const weight_params &p)
{
	int n = p.count;
	if (n < 1) n = 1;

	std::vector<double> raw;

	switch (p.curve) {
	case weight_curve::equal:
		raw = weights_equal(n);
		break;
	case weight_curve::ascending:
		raw = weights_ascending(n);
		break;
	case weight_curve::descending:
		raw = weights_descending(n);
		break;
	case weight_curve::pyramid:
		raw = weights_pyramid(n);
		break;
	case weight_curve::gaussian:
		raw = weights_gaussian(n, p.gaussian_mean, p.gaussian_stddev,
				       p.gaussian_lo, p.gaussian_hi);
		break;
	case weight_curve::gaussian_reverse: {
		raw = weights_gaussian(n, p.gaussian_mean, p.gaussian_stddev,
				       p.gaussian_lo, p.gaussian_hi);
		std::reverse(raw.begin(), raw.end());
		break;
	}
	case weight_curve::gaussian_sym: {
		double mabs = std::max(std::abs(p.gaussian_lo),
				       std::abs(p.gaussian_hi));
		raw = weights_gaussian(n, 0.0, p.gaussian_stddev, -mabs, mabs);
		break;
	}
	case weight_curve::vegas:
		raw = weights_vegas(n);
		break;
	}

	std::vector<float> out;
	out.reserve(raw.size());
	for (double v : raw) out.push_back((float)v);
	return out;
}

int compute_blend_count(double source_fps, int output_fps, float blur_amount)
{
	if (source_fps <= 0.0 || output_fps <= 0 || blur_amount <= 0.0f)
		return 1;

	int frame_gap = (int)(source_fps / output_fps);
	int count     = (int)(frame_gap * (double)blur_amount);

	if (count < 1) count = 1;
	if (count % 2 == 0) count += 1; // blend window must be odd (symmetric)

	if (count > MAX_BLEND_FRAMES) {
		count = MAX_BLEND_FRAMES;
		if (count % 2 == 0) count -= 1; // keep odd within cap
	}
	return count;
}
