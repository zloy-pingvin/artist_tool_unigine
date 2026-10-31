#include "PhysicsVisibility.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <functional>
#include <limits>
#include <thread>

using namespace Unigine::Math;

namespace ArtistTool
{

namespace
{

// A picture of the pile no bigger than this, however small the objects are.
const int MAX_PICTURE_SIZE = 2048;

struct Picture
{
	int size{0};
	std::vector<float> depth;
	std::vector<int> numbers;
	std::vector<float> x;
	std::vector<float> y;
	std::vector<float> z;
	// How many pixels every solid covers when nothing is in its way.
	std::vector<int> covered;
};

// A solid as it is drawn: smaller around its own middle, and which way its triangles look.
struct PreparedSolid
{
	std::vector<vec3> points;
	std::vector<vec3> normals;
	const std::vector<int> *triangles{nullptr};
};

std::vector<PreparedSolid> prepare(const std::vector<VisibilitySolid> &solids, float shrink)
{
	std::vector<PreparedSolid> prepared(solids.size());
	for (size_t s = 0; s < solids.size(); ++s)
	{
		const VisibilitySolid &solid = solids[s];
		if (!solid.triangles || solid.points.empty())
			continue;
		PreparedSolid &result = prepared[s];
		result.triangles = solid.triangles;

		vec3 middle(0.0f, 0.0f, 0.0f);
		for (const vec3 &point : solid.points)
			middle += point;
		middle *= 1.0f / float(solid.points.size());

		result.points.reserve(solid.points.size());
		for (const vec3 &point : solid.points)
			result.points.push_back(middle + (point - middle) * shrink);

		const std::vector<int> &triangles = *solid.triangles;
		result.normals.reserve(triangles.size() / 3);
		for (size_t t = 0; t + 2 < triangles.size(); t += 3)
		{
			const vec3 &a = result.points[triangles[t]];
			const vec3 &b = result.points[triangles[t + 1]];
			const vec3 &c = result.points[triangles[t + 2]];
			vec3 normal = cross(b - a, c - a);
			// Away from the middle, whatever the order of the points.
			if (dot(normal, a - middle) < 0.0f)
				normal = -normal;
			result.normals.push_back(normal);
		}
	}
	return prepared;
}

void draw(const std::vector<PreparedSolid> &solids, const std::vector<char> *only, const vec3 &middle, float radius, float pixel,
	const vec3 &direction, Picture &picture)
{
	const int size = picture.size;
	std::fill(picture.depth.begin(), picture.depth.end(), -std::numeric_limits<float>::max());
	std::fill(picture.numbers.begin(), picture.numbers.end(), -1);

	const vec3 side = normalize(cross(direction, std::abs(direction.z) < 0.9f ? vec3(0.0f, 0.0f, 1.0f) : vec3(1.0f, 0.0f, 0.0f)));
	const vec3 up = cross(side, direction);
	const float scale = 1.0f / pixel;

	for (size_t s = 0; s < solids.size(); ++s)
	{
		const PreparedSolid &solid = solids[s];
		if (!solid.triangles || (only && !(*only)[s]))
			continue;

		const size_t num_points = solid.points.size();
		picture.x.resize(num_points);
		picture.y.resize(num_points);
		picture.z.resize(num_points);
		for (size_t p = 0; p < num_points; ++p)
		{
			const vec3 point = solid.points[p] - middle;
			picture.x[p] = (dot(point, side) + radius) * scale;
			picture.y[p] = (dot(point, up) + radius) * scale;
			picture.z[p] = dot(point, direction);
		}

		const std::vector<int> &triangles = *solid.triangles;
		for (size_t t = 0; t + 2 < triangles.size(); t += 3)
		{
			// What looks away is behind the rest of this very solid.
			if (dot(solid.normals[t / 3], direction) <= 0.0f)
				continue;
			const int a = triangles[t];
			const int b = triangles[t + 1];
			const int c = triangles[t + 2];
			const float ax = picture.x[a], ay = picture.y[a];
			const float bx = picture.x[b], by = picture.y[b];
			const float cx = picture.x[c], cy = picture.y[c];
			const float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
			if (std::abs(area) < 1e-6f)
				continue;
			const float to_share = 1.0f / area;

			const int first_x = std::max(int(std::floor(std::min({ax, bx, cx}))), 0);
			const int last_x = std::min(int(std::ceil(std::max({ax, bx, cx}))), size - 1);
			const int first_y = std::max(int(std::floor(std::min({ay, by, cy}))), 0);
			const int last_y = std::min(int(std::ceil(std::max({ay, by, cy}))), size - 1);
			for (int y = first_y; y <= last_y; ++y)
			{
				const float py = float(y) + 0.5f;
				for (int x = first_x; x <= last_x; ++x)
				{
					const float px = float(x) + 0.5f;
					const float share_a = ((bx - px) * (cy - py) - (by - py) * (cx - px)) * to_share;
					const float share_b = ((cx - px) * (ay - py) - (cy - py) * (ax - px)) * to_share;
					const float share_c = 1.0f - share_a - share_b;
					if (share_a < 0.0f || share_b < 0.0f || share_c < 0.0f)
						continue;
					if (!picture.covered.empty())
						++picture.covered[s];
					const float depth = share_a * picture.z[a] + share_b * picture.z[b] + share_c * picture.z[c];
					const int index = y * size + x;
					if (depth > picture.depth[index])
					{
						picture.depth[index] = depth;
						picture.numbers[index] = int(s);
					}
				}
			}
		}
	}
}

// Evenly over the upper half of a sphere, from the horizon to the top.
vec3 direction_of(int index, int count)
{
	const float golden_angle = 2.39996323f;
	const float height = (float(index) + 0.5f) / float(count);
	const float around = std::sqrt(std::max(1.0f - height * height, 0.0f));
	const float angle = golden_angle * float(index);
	return vec3(std::cos(angle) * around, std::sin(angle) * around, height);
}

} // namespace

void measureSolids(const std::vector<VisibilitySolid> &solids, vec3 &middle, float &radius, float &smallest)
{
	vec3 low(std::numeric_limits<float>::max(), std::numeric_limits<float>::max(), std::numeric_limits<float>::max());
	vec3 high = -low;
	smallest = std::numeric_limits<float>::max();
	for (const VisibilitySolid &solid : solids)
	{
		if (!solid.triangles || solid.points.empty())
			continue;
		vec3 solid_low = solid.points[0];
		vec3 solid_high = solid.points[0];
		for (const vec3 &point : solid.points)
		{
			solid_low = min(solid_low, point);
			solid_high = max(solid_high, point);
		}
		low = min(low, solid_low);
		high = max(high, solid_high);
		smallest = std::min(smallest, length(solid_high - solid_low));
	}
	middle = (low + high) * 0.5f;
	radius = length(high - low) * 0.5f;
}

void drawSolidNumbers(const std::vector<VisibilitySolid> &solids, const std::vector<char> *only, float shrink, const vec3 &direction,
	float pixel, std::vector<int> &numbers, int &size)
{
	vec3 middle;
	float radius = 0.0f;
	float smallest = 0.0f;
	measureSolids(solids, middle, radius, smallest);

	Picture picture;
	picture.size = int(radius * 2.0f / pixel) + 2;
	picture.depth.resize(size_t(picture.size) * size_t(picture.size));
	picture.numbers.resize(picture.depth.size());
	draw(prepare(solids, shrink), only, middle, radius, pixel, direction, picture);
	numbers.swap(picture.numbers);
	size = picture.size;
}

std::vector<float> measureSeenShares(const std::vector<VisibilitySolid> &solids, const VisibilitySettings &settings)
{
	std::vector<float> shares(solids.size(), 0.0f);

	vec3 middle;
	float radius = 0.0f;
	float smallest = 0.0f;
	measureSolids(solids, middle, radius, smallest);
	if (!(radius > 0.0f))
		return shares;

	const float pixel = std::max(smallest / settings.pixels_across, radius * 2.0f / float(MAX_PICTURE_SIZE - 2));
	const int size = int(radius * 2.0f / pixel) + 2;
	const std::vector<PreparedSolid> prepared = prepare(solids, settings.shrink);

	struct Counts
	{
		std::vector<long long> seen;
		std::vector<long long> covered;
	};
	std::atomic<int> next_direction{0};
	const auto work = [&](Counts &counts) {
		Picture picture;
		picture.size = size;
		picture.depth.resize(size_t(size) * size_t(size));
		picture.numbers.resize(picture.depth.size());
		picture.covered.assign(solids.size(), 0);
		counts.seen.assign(solids.size(), 0);
		for (int d = next_direction.fetch_add(1); d < settings.directions; d = next_direction.fetch_add(1))
		{
			draw(prepared, nullptr, middle, radius, pixel, direction_of(d, settings.directions), picture);
			for (const int number : picture.numbers)
			{
				if (number >= 0)
					++counts.seen[number];
			}
		}
		counts.covered.assign(picture.covered.begin(), picture.covered.end());
	};

	const int num_threads = std::clamp(int(std::thread::hardware_concurrency()), 1, 16);
	std::vector<Counts> counts;
	counts.resize(size_t(num_threads));
	std::vector<std::thread> threads;
	for (int t = 1; t < num_threads; ++t)
		threads.emplace_back(work, std::ref(counts[t]));
	work(counts[0]);
	for (std::thread &thread : threads)
		thread.join();

	for (size_t s = 0; s < solids.size(); ++s)
	{
		// What can not be drawn is never taken for hidden.
		if (!solids[s].triangles || solids[s].points.empty())
		{
			shares[s] = 1.0f;
			continue;
		}
		long long seen = 0;
		long long covered = 0;
		for (const Counts &part : counts)
		{
			seen += part.seen[s];
			covered += part.covered[s];
		}
		shares[s] = covered > 0 ? float(double(seen) / double(covered)) : 1.0f;
	}
	return shares;
}

} // namespace ArtistTool
