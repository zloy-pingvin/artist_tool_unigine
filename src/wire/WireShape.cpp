#include "WireShape.h"

#include <QtGlobal>

#include <algorithm>
#include <cmath>

using namespace Unigine;

namespace ArtistTool
{

namespace
{

// The most pieces one wire is cut into.
const int MAX_SEGMENTS = 512;
// cosh() and sinh() of more than this do not fit into a double.
const double MAX_HYPERBOLIC = 700.0;

double safe_cosh(double x)
{
	return std::cosh(qBound(-MAX_HYPERBOLIC, x, MAX_HYPERBOLIC));
}

double safe_sinh(double x)
{
	return std::sinh(qBound(-MAX_HYPERBOLIC, x, MAX_HYPERBOLIC));
}

// A catenary z(x) = a * (cosh((x - x0) / a) - cosh(x0 / a)) going through (0, 0) and
// (d, h): x0 is where its lowest point is, along the span.
struct Catenary
{
	double a{1.0};
	double x0{0.0};

	double z(double x) const { return a * (safe_cosh((x - x0) / a) - safe_cosh(x0 / a)); }
};

Catenary catenary_through(double d, double h, double a)
{
	Catenary c;
	c.a = a;
	const double s = 2.0 * a * safe_sinh(d / (2.0 * a));
	c.x0 = d * 0.5 - a * std::asinh(s > 1e-300 ? h / s : 0.0);
	return c;
}

// How far the catenary goes below the straight line from (0, 0) to (d, h): the most
// at the place where it runs parallel to that line.
double sag_of(const Catenary &c, double d, double h)
{
	const double x = qBound(0.0, c.x0 + c.a * std::asinh(h / d), d);
	return h * x / d - c.z(x);
}

// The catenary from (0, 0) to (d, h) that hangs 'sag' below the straight line. The
// lower 'a' is, the more it hangs: 'a' is found by halving the range it is in.
Catenary solve_catenary(double d, double h, double sag)
{
	double low = std::log(d * 1e-3);
	double high = std::log(d * 1e6);
	for (int i = 0; i < 100; ++i)
	{
		const double middle = (low + high) * 0.5;
		if (sag_of(catenary_through(d, h, std::exp(middle)), d, h) > sag)
			low = middle;
		else
			high = middle;
	}
	return catenary_through(d, h, std::exp((low + high) * 0.5));
}

Math::vec3 unit(const Math::vec3 &v, const Math::vec3 &fallback)
{
	const float length2 = Math::length2(v);
	return length2 > 1e-12f ? v * (1.0f / std::sqrt(length2)) : fallback;
}

} // namespace

void hangWire(const Math::Vec3 &from, const Math::Vec3 &to, double sag, double max_angle,
	int min_segments, double max_length, WireSpan &span)
{
	span.from = from;
	span.to = to;
	span.samples.clear();
	span.distances.clear();
	span.tangents.clear();
	span.rests.clear();
	span.length = 0.0;

	const Math::Vec3 chord = to - from;
	const double d = std::sqrt(double(chord.x) * double(chord.x) + double(chord.y) * double(chord.y));
	const double h = double(chord.z);
	const double chord_length = std::sqrt(d * d + h * h);
	if (chord_length < 1e-6)
		return;

	// A wire that hangs straight down, or is pulled tight, is a straight line.
	sag = d > 1e-4 ? qMax(sag, 0.0) : 0.0;
	span.sag = sag;
	const bool hangs = sag > 1e-6;
	const Catenary c = hangs ? solve_catenary(d, h, sag) : Catenary();

	// The sag is straight down; the wind is the business of the material.
	const Math::Vec3 level = d > 1e-4 ? Math::Vec3(chord.x / d, chord.y / d, 0.0) : Math::Vec3(0.0, 1.0, 0.0);
	const Math::Vec3 drop(0.0, 0.0, 1.0);

	// The place on the wire at 't' along the straight line between its ends (0 to 1).
	const auto position = [&](double t) {
		// How far below the straight line the wire is there (negative - below it).
		const double below = hangs ? c.z(t * d) - h * t : 0.0;
		return from + chord * Math::Scalar(t) + drop * Math::Scalar(below);
	};

	// The joints of the segments, by the bend. The slope of a catenary at u = (x - x0) / a
	// is sinh(u), so the wire leans there by atan(sinh(u)); the joints are put where that
	// angle has grown by the same step from one to the next - many at the bottom of the
	// wire, where it bends the most, few at its ends, where it is nearly straight.
	const int least = qBound(1, min_segments, MAX_SEGMENTS);
	QVector<double> joints;
	if (hangs)
	{
		const double angle_from = std::atan(safe_sinh(-c.x0 / c.a));
		const double angle_to = std::atan(safe_sinh((d - c.x0) / c.a));
		const double turn = angle_to - angle_from;
		const double step = qMax(max_angle, 0.01) * Math::Consts::DEG2RAD_D;
		const int segments = qBound(least, int(std::ceil(turn / step - 1e-9)), MAX_SEGMENTS);
		for (int i = 0; i <= segments; ++i)
		{
			const double angle = angle_from + turn * double(i) / double(segments);
			const double x = c.x0 + c.a * std::asinh(std::tan(angle));
			joints.append(qBound(0.0, x / d, 1.0));
		}
	} else
	{
		for (int i = 0; i <= least; ++i)
			joints.append(double(i) / double(least));
	}
	joints.first() = 0.0;
	joints.last() = 1.0;

	// A segment longer than max_length is cut into equal ones: a twist or the vertex
	// animation needs vertices on the straight parts too.
	if (max_length > 1e-6)
	{
		QVector<double> cut;
		cut.append(0.0);
		for (int i = 1; i < joints.size(); ++i)
		{
			const double length = double(Math::length(position(joints[i]) - position(joints[i - 1])));
			const int room = qMax(1, MAX_SEGMENTS - int(cut.size()) - (int(joints.size()) - 1 - i));
			const int pieces = qBound(1, int(std::ceil(length / max_length - 1e-9)), room);
			for (int k = 1; k <= pieces; ++k)
				cut.append(joints[i - 1] + (joints[i] - joints[i - 1]) * double(k) / double(pieces));
		}
		joints = cut;
	}

	span.samples.reserve(joints.size());
	for (double t : joints)
		span.samples.append(position(t));
	span.samples.first() = from;
	span.samples.last() = to;
	measureWire(span);
}

void stepTwist(WireSpan &span, double max_step, int max_samples)
{
	const double rate = std::abs(span.twist_rate) * Math::Consts::RAD2DEG_D;
	if (span.isEmpty() || rate < 1e-9 || max_step <= 0.0)
		return;
	QVector<Math::Vec3> samples;
	QVector<double> distances;
	QVector<Math::vec3> tangents;
	samples.append(span.samples.first());
	distances.append(span.distances.first());
	tangents.append(span.tangents.first());
	for (int i = 0; i + 1 < span.samples.size(); ++i)
	{
		const double length = span.distances[i + 1] - span.distances[i];
		const int parts = qBound(1, int(std::ceil(rate * length / max_step)), qMax(1, max_samples / int(span.samples.size())));
		const Math::vec3 way = unit(Math::vec3(span.samples[i + 1] - span.samples[i]), span.tangents[i]);
		for (int k = 1; k < parts; ++k)
		{
			// On the straight segment: the wire keeps its shape, its distances stay.
			const double f = double(k) / double(parts);
			samples.append(span.samples[i] + (span.samples[i + 1] - span.samples[i]) * Math::Scalar(f));
			distances.append(span.distances[i] + length * f);
			tangents.append(way);
		}
		samples.append(span.samples[i + 1]);
		distances.append(span.distances[i + 1]);
		tangents.append(span.tangents[i + 1]);
	}
	span.samples = samples;
	span.distances = distances;
	span.tangents = tangents;
}

void measureWire(WireSpan &span)
{
	span.distances.clear();
	span.tangents.clear();
	span.length = 0.0;
	const int count = int(span.samples.size());
	if (count < 2)
		return;
	span.distances.reserve(count);
	span.distances.append(0.0);
	for (int i = 1; i < count; ++i)
		span.distances.append(span.distances.last() + double(Math::length(span.samples[i] - span.samples[i - 1])));
	span.length = span.distances.last();

	const Math::Vec3 chord = span.to - span.from;
	const Math::vec3 level = unit(Math::vec3(float(chord.x), float(chord.y), 0.0f), Math::vec3(0.0f, 1.0f, 0.0f));
	span.tangents.resize(count);
	for (int i = 0; i < count; ++i)
	{
		const Math::Vec3 way = span.samples[qMin(i + 1, count - 1)] - span.samples[qMax(i - 1, 0)];
		span.tangents[i] = unit(Math::vec3(way), level);
	}
}

BendFrame wireFrame(const WireSpan &span, double distance)
{
	BendFrame frame;
	const int count = int(span.samples.size());
	if (count < 2)
		return frame;

	distance = Math::clamp(distance, 0.0, span.length);
	int k = int(std::upper_bound(span.distances.begin(), span.distances.end(), distance) - span.distances.begin()) - 1;
	k = qBound(0, k, count - 2);
	const double piece = span.distances[k + 1] - span.distances[k];
	const double t = piece > 1e-12 ? Math::clamp((distance - span.distances[k]) / piece, 0.0, 1.0) : 0.0;
	frame.position = span.samples[k] + (span.samples[k + 1] - span.samples[k]) * Math::Scalar(t);
	const Math::vec3 forward = unit(span.tangents[k] + (span.tangents[k + 1] - span.tangents[k]) * float(t),
		Math::vec3(0.0f, 1.0f, 0.0f));

	// Right is level; a wire that goes straight up or down takes X for it.
	const Math::vec3 right = unit(Math::vec3(forward.y, -forward.x, 0.0f), Math::vec3(1.0f, 0.0f, 0.0f));
	const Math::vec3 up = unit(Math::cross(right, forward), Math::vec3(0.0f, 0.0f, 1.0f));
	const double angle = span.twist_start + span.twist_rate * distance;
	const float c = float(std::cos(angle));
	const float s = float(std::sin(angle));
	frame.forward = forward;
	frame.right = right * c + up * s;
	frame.up = up * c - right * s;
	return frame;
}

void addWireGeometry(const WireSpan &span, const WireLook &look, const Math::Vec3 &pivot, const MeshPtr &mesh,
	int surface)
{
	if (span.isEmpty() || look.shape == WIRE_SHAPE_MESH)
		return;

	const float uv_length = qMax(look.uv_length, 0.001f);
	const float half = qMax(look.thickness * span.thickness_scale, 0.0001f) * 0.5f;
	const int rows = int(span.samples.size());
	// The texture coordinates of a point 'across' (0 to 1) the strip or around the tube at
	// the sample 'i', as on a road: across U, along V, then turned by uv_rotation. V goes
	// back along the wire - in the engine V runs down the image, and with V going forward
	// the texture would lie mirrored.
	const double uv_angle = double(look.uv_rotation) * Math::Consts::DEG2RAD_D;
	const double uv_cos = std::cos(uv_angle);
	const double uv_sin = std::sin(uv_angle);
	const auto uv_at = [&](int i, float across) {
		const double u = double(across) * double(look.uv_width);
		const double v = -span.distances[i] / double(uv_length);
		return Math::vec2(float(u * uv_cos - v * uv_sin), float(u * uv_sin + v * uv_cos));
	};
	const auto color_at = [&](int i) { return wireColor(span, span.length > 1e-9 ? span.distances[i] / span.length : 0.0); };

	if (look.shape == WIRE_SHAPE_TUBE)
	{
		// A ring of vertices at every sample; the seam has its vertices twice, so that
		// the texture goes around the tube from 0 to 1.
		const int sides = qBound(3, look.sides, 64);
		const int base = mesh->getNumVertex(surface);
		for (int i = 0; i < rows; ++i)
		{
			const BendFrame f = wireFrame(span, span.distances[i]);
			for (int j = 0; j <= sides; ++j)
			{
				const double angle = 2.0 * Math::Consts::PI_D * double(j) / double(sides);
				const Math::vec3 normal = f.right * float(std::cos(angle)) + f.up * float(std::sin(angle));
				mesh->addVertex(Math::vec3(f.position - pivot) + normal * half, surface);
				mesh->addNormal(normal, surface);
				// Around the tube the other way than the angle goes: else the texture is
				// mirrored on it.
				const Math::vec2 uv = uv_at(i, 1.0f - float(j) / float(sides));
				mesh->addTexCoord0(uv, surface);
				mesh->addTexCoord1(uv, surface);
				mesh->addColor(color_at(i), surface);
			}
		}
		const int ring = sides + 1;
		for (int i = 0; i + 1 < rows; ++i)
		{
			for (int j = 0; j < sides; ++j)
			{
				const int a = base + i * ring + j;
				const int b = a + 1;
				const int c = a + ring;
				const int d = c + 1;
				for (int index : {a, c, b, b, c, d})
					mesh->addIndex(index, surface);
			}
		}
		return;
	}

	// The strips: one for a ribbon, two at a right angle for a cross. Every strip goes
	// across the wire from one edge to the other; its normal is across the strip.
	const int strips = look.shape == WIRE_SHAPE_CROSS ? 2 : 1;
	for (int strip = 0; strip < strips; ++strip)
	{
		const int base = mesh->getNumVertex(surface);
		for (int i = 0; i < rows; ++i)
		{
			const BendFrame f = wireFrame(span, span.distances[i]);
			const Math::vec3 across = strip == 0 ? f.right : f.up;
			const Math::vec3 normal = strip == 0 ? f.up : -f.right;
			for (int side = 0; side < 2; ++side)
			{
				mesh->addVertex(Math::vec3(f.position - pivot) + across * (side == 0 ? -half : half), surface);
				mesh->addNormal(normal, surface);
				const Math::vec2 uv = uv_at(i, float(side));
				mesh->addTexCoord0(uv, surface);
				mesh->addTexCoord1(uv, surface);
				mesh->addColor(color_at(i), surface);
			}
		}
		for (int i = 0; i + 1 < rows; ++i)
		{
			const int a = base + i * 2;
			const int b = a + 1;
			const int c = a + 2;
			const int d = a + 3;
			for (int index : {a, b, c, c, b, d})
				mesh->addIndex(index, surface);
		}
	}
}

Math::vec4 wireColor(const WireSpan &span, double t)
{
	t = Math::clamp(t, 0.0, 1.0);
	// The part of the wire hanging free the place is in: from the end or the place on the
	// ground before it to the one after it. It sways as a wire of its own, as much less
	// as it is shorter: a wire with nothing on the ground sways 4t(1 - t).
	const double length = span.length;
	const double d = t * length;
	double a = 0.0;
	double b = length;
	for (const QPair<double, double> &rest : span.rests)
	{
		if (d >= rest.first && d <= rest.second)
			return Math::vec4(0.0f, span.random, 0.0f, 1.0f);
		if (rest.second < d)
			a = qMax(a, rest.second);
		if (rest.first > d)
			b = qMin(b, rest.first);
	}
	const float sway = length > 1e-9 && b - a > 1e-9 ? float(4.0 * (d - a) * (b - d) / ((b - a) * length)) : 0.0f;
	return Math::vec4(sway, span.random, sway, 1.0f);
}

void paintWireColors(const WireSpan &span, const Math::Vec3 &pivot, const MeshPtr &mesh, int surface, int first)
{
	// Where a vertex is along the wire: how far it is along the level line between the ends
	// (the wire goes along it steadily, whatever it sags or lies), turned into the length
	// along the wire by its samples. A wire going straight up - along the line itself.
	Math::Vec3 chord = span.to - span.from;
	bool level = true;
	if (double(chord.x) * double(chord.x) + double(chord.y) * double(chord.y) < 1e-8)
		level = false;
	else
		chord.z = 0;
	const double length2 = double(Math::length2(chord));
	const auto along = [&](const Math::Vec3 &p) {
		Math::Vec3 offset = p - span.from;
		if (level)
			offset.z = 0;
		return length2 > 1e-12 ? double(Math::dot(offset, chord)) / length2 : 0.0;
	};
	QVector<double> sample_along;
	for (const Math::Vec3 &sample : span.samples)
		sample_along.append(along(sample));
	const bool by_samples = level && span.samples.size() > 1 && span.length > 1e-9;

	for (int v = first, num = mesh->getNumVertex(surface); v < num; ++v)
	{
		const double a = along(pivot + Math::Vec3(mesh->getVertex(v, surface)));
		double t = a;
		if (by_samples)
		{
			const int k = qBound(1, int(std::upper_bound(sample_along.begin(), sample_along.end(), a) - sample_along.begin()), int(sample_along.size()) - 1);
			const double width = sample_along[k] - sample_along[k - 1];
			const double f = width > 1e-12 ? Math::clamp((a - sample_along[k - 1]) / width, 0.0, 1.0) : 0.0;
			t = (span.distances[k - 1] + (span.distances[k] - span.distances[k - 1]) * f) / span.length;
		}
		mesh->setColor(v, wireColor(span, t), surface);
	}
}

WireSpan simplifyWire(const WireSpan &span, int factor)
{
	WireSpan result = span;
	if (span.isEmpty() || factor <= 1)
		return result;
	result.samples.clear();
	result.distances.clear();
	result.tangents.clear();
	const int last = int(span.samples.size()) - 1;
	for (int i = 0; i <= last; ++i)
	{
		// The ends, every factor-th joint, and where the wire starts and stops lying on the
		// ground: it would go through the ground without them.
		bool keep = i == last || i % factor == 0;
		for (const QPair<double, double> &rest : span.rests)
			keep = keep || span.distances[i] == rest.first || span.distances[i] == rest.second;
		if (!keep)
			continue;
		// The distances along the wire stay those of the whole one: the texture and the
		// twist go on the same.
		result.samples.append(span.samples[i]);
		result.distances.append(span.distances[i]);
		result.tangents.append(span.tangents[i]);
	}
	return result;
}

int wireTriangles(const WireSpan &span, const WireLook &look)
{
	if (span.isEmpty())
		return 0;
	const int segments = int(span.samples.size()) - 1;
	switch (look.shape)
	{
	case WIRE_SHAPE_CROSS:
		return segments * 4;
	case WIRE_SHAPE_RIBBON:
		return segments * 2;
	case WIRE_SHAPE_TUBE:
		return segments * qBound(3, look.sides, 64) * 2;
	default:
		return 0;
	}
}

} // namespace ArtistTool
