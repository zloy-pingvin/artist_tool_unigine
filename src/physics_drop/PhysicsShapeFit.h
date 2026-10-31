#pragma once

#include <UnigineMathLib.h>

#include <vector>

namespace ArtistTool
{

// A simple shape that wraps a set of points: all of them are inside it. It is measured
// by the vertices of a convex hull: what wraps them wraps the mesh too.
struct PrimitiveFit
{
	enum Kind
	{
		BOX = 0,
		SPHERE,
		CAPSULE,
		CYLINDER,
		NUM_KINDS,
	};

	Kind kind{BOX};
	// Where the shape stands among the points. The Z axis is the axis of a capsule and of
	// a cylinder, as the engine has it.
	Unigine::Math::mat4 transform{Unigine::Math::mat4_identity};
	// Of a box.
	Unigine::Math::vec3 size{0.0f, 0.0f, 0.0f};
	float radius{0.0f};
	// The whole height of a cylinder; of a capsule, without its round ends: the engine
	// counts them apart.
	float height{0.0f};
	double volume{0.0};
};

// The tightest shape of that kind around the points. There must be some points.
PrimitiveFit fitPrimitive(const std::vector<Unigine::Math::vec3> &points, PrimitiveFit::Kind kind);

// The simple shape to use instead of the convex hull of the points (of 'hull_volume'),
// the tightest of the kinds that add almost no empty space to it.
// False if none of them fits: the convex hull is to be used.
bool chooseBestPrimitive(const std::vector<Unigine::Math::vec3> &points, double hull_volume, PrimitiveFit &fit);

// How much empty space the shape adds to the hull: 0.25 - a quarter of the hull.
double extraVolume(const PrimitiveFit &fit, double hull_volume);

} // namespace ArtistTool
