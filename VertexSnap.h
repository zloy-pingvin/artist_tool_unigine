#pragma once

#include <UnigineMathLib.h>
#include <UnigineObjects.h>

#include <QPointF>

class QWidget;

// Finding the vertex of a mesh under the mouse, the way the editor's own vertex snap
// (the V key of the move manipulator) does it.
namespace ArtistTool
{

struct VertexHit
{
	bool found{false};
	Unigine::Math::Vec3 point;
	// The object the vertex belongs to.
	Unigine::ObjectPtr object;
};

// The vertex closest to the mouse on the screen, within a small circle around it (a
// fortieth of the larger side of the viewport). Only the vertices the camera sees are
// taken: the ones on the side of a mesh turned away from it and the ones hidden
// behind other objects are not. Static, dynamic and cluster meshes are looked
// through; skinned meshes, terrains and clutters have no vertices to snap to.
// 'mouse' is in the coordinates of the viewport widget.
VertexHit findVertex(const QWidget *viewport, const QPointF &mouse);

} // namespace ArtistTool
