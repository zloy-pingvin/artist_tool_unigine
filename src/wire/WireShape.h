#pragma once

#include "MeshBender.h"

#include <UnigineMathLib.h>
#include <UnigineMesh.h>

#include <QPair>
#include <QVector>

// The shape of a wire hanging between two points, and the geometry made along it.
namespace ArtistTool
{

// One wire from one attach point to another, hung under its own weight.
struct WireSpan
{
	Unigine::Math::Vec3 from;
	Unigine::Math::Vec3 to;
	// The hanging curve as a polyline from 'from' to 'to': its samples, the length along
	// it to every one of them and the way it goes there.
	QVector<Unigine::Math::Vec3> samples;
	QVector<double> distances;
	QVector<Unigine::Math::vec3> tangents;
	double length{0.0};
	// How far the wire hangs below the straight line between its ends, at the deepest
	// place, in units.
	double sag{0.0};
	// The turn of the shape about the wire at its start, in radians, and how fast it
	// turns along it, in radians per unit of length.
	double twist_start{0.0};
	double twist_rate{0.0};
	// A number from 0 to 1 the wire keeps whatever is changed around it: its vertex
	// colors tell it to the material (B), so that the wires of a line can differ.
	float random{0.0f};
	// A multiplier of the thickness of the look, this wire's own (set by the point of the
	// curve it goes from).
	float thickness_scale{1.0f};
	// Where the wire lies on the ground, as distances along it from - to: it does not sway
	// there, as at its ends.
	QVector<QPair<double, double>> rests;

	bool isEmpty() const { return samples.size() < 2; }
};

// Hangs a wire from 'from' to 'to' along a catenary - the curve a rope or a chain takes
// under its own weight - that goes 'sag' units below the straight line between them at
// the deepest place. It is cut into segments by the bend: the wire turns by at most 'max_angle' degrees from
// one segment to the next, so a wire pulled tight gets few of them and a hanging one gets
// more, the most at its bottom where it bends the most. At least 'min_segments' of them;
// none longer than 'max_length' units (0 - no limit).
void hangWire(const Unigine::Math::Vec3 &from, const Unigine::Math::Vec3 &to, double sag, double max_angle, int min_segments, double max_length, WireSpan &span);

// Measures the wire anew - the lengths along it and the way it goes - after its samples
// have been moved.
void measureWire(WireSpan &span);

// Cuts the segments of the wire that its twist turns by more than 'max_step' degrees into
// shorter ones, on the same straight lines: the shape turns smoothly about the wire instead
// of jumping from joint to joint. At most 'max_samples' samples.
void stepTwist(WireSpan &span, double max_step, int max_samples);

// The place on the wire at a distance along it and the way the shape is turned there:
// 'forward' along the wire, 'right' level across it, 'up' across it too; both of them
// turned about the wire by the twist.
BendFrame wireFrame(const WireSpan &span, double distance);

// What the wire is made of: the shape of its section.
enum WireShapeKind
{
	// Two strips crossed at a right angle along the wire, for a material with alpha.
	WIRE_SHAPE_CROSS = 0,
	// One strip along the wire: a flat band, a ribbon; turned by the twist.
	WIRE_SHAPE_RIBBON,
	// A round tube of real thickness.
	WIRE_SHAPE_TUBE,
	// A ready mesh of the project laid along the wire again and again, every copy bent
	// to follow it.
	WIRE_SHAPE_MESH,
};

// How the geometry of a wire of a strip shape or a tube is made.
struct WireLook
{
	WireShapeKind shape{WIRE_SHAPE_CROSS};
	// The width of a strip, the diameter of a tube, in units.
	float thickness{0.05f};
	// The sides of a tube.
	int sides{4};
	// The length of the wire one repeat of the texture takes, in units.
	float uv_length{1.0f};
	// Repeats of the texture across a strip, around a tube.
	float uv_width{1.0f};
	// How the texture is turned on the wire, in degrees, as on a road: 0 - the wire goes
	// along V, 90 - along U (a wire drawn in the texture from left to right).
	float uv_rotation{0.0f};
};

// Adds the geometry of a wire of a strip shape or a tube to the surface of the mesh, the
// positions relative to 'pivot'. Texture: across the strip (around the tube) U from 0 to
// uv_width, along the wire V, then both turned by uv_rotation. Vertex colors: see
// wireColor().
void addWireGeometry(const WireSpan &span, const WireLook &look, const Unigine::Math::Vec3 &pivot,
	const Unigine::MeshPtr &mesh, int surface);

// The vertex color of a wire at 't' along it (0 - its start, 1 - its end), for the vertex
// animation of the material: R and B - how far the wire may sway there (0 at the attach
// points and where it lies on the ground, 1 in the middle of a wire hanging free; a part
// hanging between two places on the ground sways as much less as it is shorter), G - the
// random number of the wire (to shift the timing of its animation), A - 1.
Unigine::Math::vec4 wireColor(const WireSpan &span, double t);
// Gives the vertices of the surface from 'first' on, a mesh laid along the wire, the
// colors of wireColor(); where a vertex is along the wire is told by the straight line
// between its ends. The positions are relative to 'pivot'.
void paintWireColors(const WireSpan &span, const Unigine::Math::Vec3 &pivot, const Unigine::MeshPtr &mesh,
	int surface, int first);
// Triangles that geometry has.
int wireTriangles(const WireSpan &span, const WireLook &look);

// The wire with 'factor' times fewer segments, for a LOD: every 'factor'-th joint of it
// is kept, its ends, and where it starts and stops lying on the ground.
WireSpan simplifyWire(const WireSpan &span, int factor);

} // namespace ArtistTool
