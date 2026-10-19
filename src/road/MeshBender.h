#pragma once

#include "PathPlacer.h"
#include "PlaceableAsset.h"

#include <UnigineGUID.h>
#include <UnigineMathLib.h>
#include <UnigineMesh.h>

#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

// Bending a ready mesh along a curve: a kerb, a fence, a guard rail, a pipe. The mesh
// is laid along the curve again and again, its copies bent to follow it.
namespace ArtistTool
{

// The mesh that is bent: all the objects of an asset, in the space of the asset,
// surface by surface with their materials, the distances they are seen at and how they
// cast shadows - the LODs stay LODs, a shadow LOD stays a shadow LOD. The surfaces that
// are neither drawn nor cast shadows (collision ones) are left out.
struct BendSource
{
	struct Surface
	{
		QString name;
		// The material the surface has in the asset; invalid - the base one.
		Unigine::UGUID material;
		QVector<Unigine::Math::vec3> positions;
		QVector<Unigine::Math::vec3> normals;
		QVector<Unigine::Math::vec2> uv0;
		QVector<Unigine::Math::vec2> uv1;
		QVector<Unigine::Math::vec4> colors;
		QVector<int> indices;
		// From what distance to what distance the surface is seen, and over what
		// distances it fades in and out: the LOD switching of the asset.
		float min_visible{-Unigine::Math::Consts::INF};
		float max_visible{Unigine::Math::Consts::INF};
		float min_fade{0.0f};
		float max_fade{0.0f};
		// How the surface is drawn and casts shadows.
		int viewport_mask{~0};
		int shadow_mask{~0};
		bool cast_shadow{true};
		bool cast_world_shadow{true};
		// The LOD the name of the surface tells ("_lod1", "_lod_1", ...); -1 - none.
		int lod{-1};
		// The name ends with "shadow_lod" (and maybe a number): the mesh that casts the
		// shadow instead of the others.
		bool shadow_lod{false};
		// The object of the asset the surface comes from: the rules of the LODs and the
		// shadow LOD go object by object.
		int object{0};
	};
	QVector<Surface> surfaces;
	// What could not be taken as it is, for the user; empty if all went well.
	QString note;

	bool isEmpty() const { return surfaces.isEmpty(); }
	int numTriangles() const;

	// The meshes of the asset. False, with the reason in 'error', if it has none.
	static bool load(const PlaceableAsset &asset, BendSource &source, QString &error);
};

// How the asset lies along the curve: which of its axes goes along it, which one up,
// and its scale along its own X, Y and Z.
struct BendAxes
{
	AxisDirection forward{AXIS_Y_POSITIVE};
	AxisDirection up{AXIS_Z_POSITIVE};
	Unigine::Math::vec3 scale{1.0f, 1.0f, 1.0f};
};

// The extent of the scaled source along the curve, to its right and up.
struct BendBounds
{
	float along_min{0.0f};
	float along_max{0.0f};
	float right_min{0.0f};
	float right_max{0.0f};
	float up_min{0.0f};
	float up_max{0.0f};

	float length() const { return along_max - along_min; }
};
BendBounds bendBounds(const BendSource &source, const BendAxes &axes);

// The place on the curve and the way the mesh is turned there: 'forward' the way the
// curve goes, 'right' and 'up' across it. 'width' scales the mesh to the right.
struct BendFrame
{
	Unigine::Math::Vec3 position;
	Unigine::Math::vec3 forward{0.0f, 1.0f, 0.0f};
	Unigine::Math::vec3 right{1.0f, 0.0f, 0.0f};
	Unigine::Math::vec3 up{0.0f, 0.0f, 1.0f};
	float width{1.0f};
	// How fast the width changes along the curve, per unit of length.
	float width_slope{0.0f};
};

// Adds one copy of the source, bent along the curve from the distance 'start' on, to
// the mesh: every surface of the source goes to the surface of the mesh named by
// 'names' (one per surface of the source), added when the mesh has none of that name
// yet - surfaces of one name are merged; the positions are relative to
// 'pivot'. The copy keeps its own (scaled) length along the curve times 'fit' - the
// little stretch that makes a whole number of copies fill the curve. 'frame' tells
// the frame at a distance.
void bendCopy(const BendSource &source, const BendAxes &axes, const BendBounds &bounds, const QStringList &names,
	double start, double fit, const std::function<BendFrame(double)> &frame, const Unigine::Math::Vec3 &pivot,
	const Unigine::MeshPtr &mesh);

// Gives a surface of the result what its surface of the source has: the material, the
// distances of the LOD and the shadows.
void applySurface(const Unigine::ObjectPtr &object, int surface, const BendSource::Surface &source);

} // namespace ArtistTool
