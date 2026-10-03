#pragma once

#include <editor/UnigineViewportManager.h>

#include <UnigineMathLib.h>
#include <UnigineNode.h>
#include <UnigineVector.h>

#include <QPointF>

class QObject;
class QWidget;

// What the tools that work with the mouse in an editor viewport have in common:
// finding the viewport under the mouse and the surface point under it.
namespace ArtistTool
{

// Kinds of objects a tool may put things on (the set of the Cluster Paint filter).
struct SurfaceFilter
{
	enum Type
	{
		MESH_STATIC = 1 << 0,
		MESH_SKINNED = 1 << 1,
		MESH_DYNAMIC = 1 << 2,
		MESH_CLUSTER = 1 << 3,
		MESH_CLUTTER = 1 << 4,
		TERRAIN = 1 << 5,
		WATER = 1 << 6,
		CLOUDS = 1 << 7,
		ALL = (1 << 8) - 1,
	};

	int surface_types{ALL};
	bool only_immovable{false};
	// Only surfaces with the Intersection flag and a non-zero Intersection Mask.
	bool only_intersection{false};
};

// Point of the surface a ray has hit.
struct SurfaceHit
{
	bool found{false};
	// Whether the filter lets an object be placed there.
	bool allowed{false};
	double distance{0.0};
	Unigine::Math::Vec3 point;
	Unigine::Math::vec3 normal{0.0f, 0.0f, 1.0f};
};

// Editor viewports are widgets of the "ViewportWindowScene" class. Other render
// windows (asset previews etc.) are not taken for a viewport.
bool isViewportWidget(const QWidget *widget);
// The viewport widget the object is or is inside of; null if it is not in one.
QWidget *findViewportWidget(QObject *watched);
// The editor viewport shown in the widget.
::UnigineEditor::ViewportWindowPtr matchViewport(const QWidget *widget);
// World ray under a point of the viewport widget. False if there is no camera.
bool screenRay(const QWidget *viewport, const QPointF &position, Unigine::Math::Vec3 &p0, Unigine::Math::Vec3 &p1);

// The size, in world units, of something that takes the given fraction of the height
// of the editor viewport when it is at 'point'. Lets a helper keep the same size on
// the screen: it shrinks as the camera comes closer and grows as it moves away.
float viewportSizeAt(const Unigine::Math::Vec3 &point, float fraction);
// The distance from the camera of the editor viewport to 'point', in units.
float viewportDistanceTo(const Unigine::Math::Vec3 &point);

// Editor helpers (gizmos, asset previews) live under nodes named "UnigineEditor.*".
bool isEditorInternal(const Unigine::NodePtr &node);
// Whether the node is 'root' itself, one of its children or a part of the content of
// a node reference found among them.
bool isInside(const Unigine::NodePtr &node, const Unigine::NodePtr &root);

// The closest geometry along the world ray p0 -> p1 and whether an object may be
// placed on it. Everything visible is tested - what is filtered out blocks the ray
// rather than lets it through, so the hit is "not allowed" while the ray ends on
// such an object. The nodes of 'ignore', with everything inside them, are skipped.
SurfaceHit raycastSurface(const Unigine::Math::Vec3 &p0, const Unigine::Math::Vec3 &p1,
	const SurfaceFilter &filter, const Unigine::Vector<Unigine::NodePtr> &ignore);

// Rotation that turns the Z axis to 'up'; identity for a vertical 'up'.
Unigine::Math::mat4 basisFromUp(const Unigine::Math::vec3 &up);

} // namespace ArtistTool
