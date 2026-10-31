#include "PhysicsShapeFit.h"

#include <algorithm>
#include <cfloat>
#include <cmath>

using namespace Unigine::Math;

namespace ArtistTool
{

namespace
{

const double PI_D = 3.14159265358979323846;

// A shape thinner than that, in units, does not lie on anything steadily.
const float MIN_SIZE = 0.01f;
// Steps of the search for the center of the smallest circle or sphere.
const int CENTER_STEPS = 256;

// How much empty space a shape may add to the hull to be used instead of it.
const double MAX_EXTRA[PrimitiveFit::NUM_KINDS] = {
	// Only what fits almost exactly: where a shape sticks out of the mesh, the object rests on
	// nothing that is seen. A milk carton in a box (12-23% more) stood on a corner of the box,
	// hanging in the air. A hull costs little: 20% more per step on a pile of 300.
	0.05, // box
	0.05, // sphere
	0.05, // capsule
	0.05, // cylinder
};

// Three axes the points are measured along: the columns of the rotation.
struct Frame
{
	vec3 axis[3];
};

mat4 frame_transform(const vec3 &x, const vec3 &y, const vec3 &z, const vec3 &position)
{
	mat4 transform = mat4_identity;
	transform.setColumn3(0, x);
	transform.setColumn3(1, y);
	transform.setColumn3(2, z);
	transform.setColumn3(3, position);
	return transform;
}

// The main directions of the points, the longest first (Jacobi's method on their
// covariance matrix).
Frame principal_frame(const std::vector<vec3> &points)
{
	dvec3 mean(0.0, 0.0, 0.0);
	for (const vec3 &point : points)
		mean += dvec3(point);
	mean = mean / double(points.size());

	double a[3][3] = {{0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}, {0.0, 0.0, 0.0}};
	for (const vec3 &point : points)
	{
		const dvec3 d = dvec3(point) - mean;
		const double v[3] = {d.x, d.y, d.z};
		for (int i = 0; i < 3; ++i)
		{
			for (int j = 0; j < 3; ++j)
				a[i][j] += v[i] * v[j];
		}
	}

	double e[3][3] = {{1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
	for (int sweep = 0; sweep < 32; ++sweep)
	{
		const double off = a[0][1] * a[0][1] + a[0][2] * a[0][2] + a[1][2] * a[1][2];
		if (off < 1e-24)
			break;
		for (int p = 0; p < 2; ++p)
		{
			for (int q = p + 1; q < 3; ++q)
			{
				if (std::abs(a[p][q]) < 1e-30)
					continue;
				const double theta = (a[q][q] - a[p][p]) / (2.0 * a[p][q]);
				const double t = (theta >= 0.0 ? 1.0 : -1.0) / (std::abs(theta) + std::sqrt(theta * theta + 1.0));
				const double c = 1.0 / std::sqrt(t * t + 1.0);
				const double s = t * c;
				for (int k = 0; k < 3; ++k)
				{
					const double akp = a[k][p];
					const double akq = a[k][q];
					a[k][p] = c * akp - s * akq;
					a[k][q] = s * akp + c * akq;
				}
				for (int k = 0; k < 3; ++k)
				{
					const double apk = a[p][k];
					const double aqk = a[q][k];
					a[p][k] = c * apk - s * aqk;
					a[q][k] = s * apk + c * aqk;
				}
				for (int k = 0; k < 3; ++k)
				{
					const double ekp = e[k][p];
					const double ekq = e[k][q];
					e[k][p] = c * ekp - s * ekq;
					e[k][q] = s * ekp + c * ekq;
				}
			}
		}
	}

	int order[3] = {0, 1, 2};
	std::sort(order, order + 3, [&a](int l, int r) { return a[l][l] > a[r][r]; });
	const auto column = [&e](int c) { return normalize(vec3(float(e[0][c]), float(e[1][c]), float(e[2][c]))); };

	Frame frame;
	frame.axis[0] = column(order[0]);
	frame.axis[1] = column(order[1]);
	// Right-handed, the way a rotation has to be.
	frame.axis[2] = normalize(cross(frame.axis[0], frame.axis[1]));
	frame.axis[1] = normalize(cross(frame.axis[2], frame.axis[0]));
	return frame;
}

// The smallest circle around the points of a plane, or the smallest sphere around points
// in space (Badoiu - Clarkson: every step goes a little towards the farthest point).
template<typename Point>
void smallest_ball(const std::vector<Point> &points, Point &center, float &radius)
{
	center = points.front();
	for (int step = 1; step <= CENTER_STEPS; ++step)
	{
		const Point *farthest = &points.front();
		float best = -1.0f;
		for (const Point &point : points)
		{
			const float d = length2(point - center);
			if (d > best)
			{
				best = d;
				farthest = &point;
			}
		}
		center += (*farthest - center) * (1.0f / float(step + 1));
	}
	float best = 0.0f;
	for (const Point &point : points)
		best = std::max(best, length2(point - center));
	radius = std::sqrt(best);
}

PrimitiveFit fit_box(const std::vector<vec3> &points, const Frame &frame)
{
	vec3 low(FLT_MAX, FLT_MAX, FLT_MAX);
	vec3 high(-FLT_MAX, -FLT_MAX, -FLT_MAX);
	for (const vec3 &point : points)
	{
		for (int i = 0; i < 3; ++i)
		{
			const float d = dot(point, frame.axis[i]);
			low[i] = std::min(low[i], d);
			high[i] = std::max(high[i], d);
		}
	}

	PrimitiveFit fit;
	fit.kind = PrimitiveFit::BOX;
	vec3 position(0.0f, 0.0f, 0.0f);
	for (int i = 0; i < 3; ++i)
	{
		fit.size[i] = std::max(high[i] - low[i], MIN_SIZE);
		position += frame.axis[i] * ((low[i] + high[i]) * 0.5f);
	}
	fit.transform = frame_transform(frame.axis[0], frame.axis[1], frame.axis[2], position);
	fit.volume = double(fit.size.x) * fit.size.y * fit.size.z;
	return fit;
}

PrimitiveFit fit_sphere(const std::vector<vec3> &points)
{
	vec3 center;
	float radius = 0.0f;
	smallest_ball(points, center, radius);

	PrimitiveFit fit;
	fit.kind = PrimitiveFit::SPHERE;
	fit.radius = std::max(radius, MIN_SIZE * 0.5f);
	fit.transform = frame_transform(vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, 0.0f, 1.0f), center);
	fit.volume = 4.0 / 3.0 * PI_D * double(fit.radius) * fit.radius * fit.radius;
	return fit;
}

// A capsule or a cylinder along one of the axes of the frame.
PrimitiveFit fit_round(const std::vector<vec3> &points, const Frame &frame, int axis, PrimitiveFit::Kind kind)
{
	const vec3 &z = frame.axis[axis];
	const vec3 &x = frame.axis[(axis + 1) % 3];
	const vec3 &y = frame.axis[(axis + 2) % 3];

	std::vector<vec2> flat;
	flat.reserve(points.size());
	float low = FLT_MAX;
	float high = -FLT_MAX;
	for (const vec3 &point : points)
	{
		flat.push_back(vec2(dot(point, x), dot(point, y)));
		const float d = dot(point, z);
		low = std::min(low, d);
		high = std::max(high, d);
	}

	vec2 center;
	float radius = 0.0f;
	smallest_ball(flat, center, radius);
	radius = std::max(radius, MIN_SIZE * 0.5f);

	PrimitiveFit fit;
	fit.kind = kind;
	fit.radius = radius;
	float middle = (low + high) * 0.5f;
	if (kind == PrimitiveFit::CYLINDER)
	{
		fit.height = std::max(high - low, MIN_SIZE);
		fit.volume = PI_D * double(radius) * radius * fit.height;
	} else
	{
		// The middle part of a capsule is as short as its round ends let it be: every
		// point has to be inside one end or the other.
		float top = -FLT_MAX;
		float bottom = FLT_MAX;
		for (size_t i = 0; i < points.size(); ++i)
		{
			const float d = dot(points[i], z);
			const float rest = std::sqrt(std::max(radius * radius - length2(flat[i] - center), 0.0f));
			top = std::max(top, d - rest);
			bottom = std::min(bottom, d + rest);
		}
		if (top < bottom)
			top = bottom = (top + bottom) * 0.5f;
		fit.height = top - bottom;
		middle = (top + bottom) * 0.5f;
		fit.volume = PI_D * double(radius) * radius * (fit.height + 4.0 / 3.0 * radius);
	}

	const vec3 position = x * center.x + y * center.y + z * middle;
	fit.transform = frame_transform(x, y, z, position);
	return fit;
}

} // namespace

PrimitiveFit fitPrimitive(const std::vector<vec3> &points, PrimitiveFit::Kind kind)
{
	if (kind == PrimitiveFit::SPHERE)
		return fit_sphere(points);

	// Along the axes of the object, and along the main directions of the points for what
	// is turned inside the mesh.
	Frame frames[2];
	frames[0].axis[0] = vec3(1.0f, 0.0f, 0.0f);
	frames[0].axis[1] = vec3(0.0f, 1.0f, 0.0f);
	frames[0].axis[2] = vec3(0.0f, 0.0f, 1.0f);
	frames[1] = principal_frame(points);

	PrimitiveFit best;
	best.volume = -1.0;
	const auto take = [&best](const PrimitiveFit &fit) {
		if (best.volume < 0.0 || fit.volume < best.volume)
			best = fit;
	};
	for (const Frame &frame : frames)
	{
		if (kind == PrimitiveFit::BOX)
		{
			take(fit_box(points, frame));
			continue;
		}
		for (int axis = 0; axis < 3; ++axis)
			take(fit_round(points, frame, axis, kind));
	}
	return best;
}

double extraVolume(const PrimitiveFit &fit, double hull_volume)
{
	return hull_volume > 0.0 ? fit.volume / hull_volume - 1.0 : 1e30;
}

bool chooseBestPrimitive(const std::vector<vec3> &points, double hull_volume, PrimitiveFit &fit)
{
	bool found = false;
	for (int kind = 0; kind < PrimitiveFit::NUM_KINDS; ++kind)
	{
		// Cylinders do not get to rest in a pile: on real tin cans they went on spinning
		// and shivering where hulls settled by themselves, for a tiny part of a frame more.
		// A cylinder can still be asked for by hand.
		if (kind == PrimitiveFit::CYLINDER)
			continue;
		const PrimitiveFit candidate = fitPrimitive(points, PrimitiveFit::Kind(kind));
		if (extraVolume(candidate, hull_volume) > MAX_EXTRA[kind])
			continue;
		// When two are almost the same the simpler one stays: a ball is not a capsule with
		// no middle part.
		if (!found || candidate.volume < fit.volume * 0.98)
			fit = candidate;
		found = true;
	}
	return found;
}

} // namespace ArtistTool
