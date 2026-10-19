#include "VertexSnap.h"
#include "ViewportPicking.h"

#include <UnigineMesh.h>
#include <UniginePlayers.h>
#include <UnigineRender.h>
#include <UnigineWorld.h>

#include <QSet>
#include <QWidget>

#include <algorithm>
#include <cmath>

using namespace Unigine;

namespace ArtistTool
{

namespace
{

// The circle around the mouse a vertex is looked for in, in parts of the larger side
// of the viewport - the same as in the editor.
const float SNAP_RADIUS = 0.025f;
// How far from the camera the vertices are looked for, in units.
const float MAX_DISTANCE = 1000.0f;
// How many of the vertices closest to the mouse are checked for being hidden behind
// something before the search gives up.
const int MAX_OCCLUSION_TESTS = 32;

struct View
{
	PlayerPtr player;
	int width{1};
	int height{1};
	Math::ivec2 mouse;
	// The square of the radius of the circle, in pixels.
	int radius2{1};
	// The narrow frustum that goes from the camera through that circle.
	Math::mat4 frustum;
	Math::Vec3 camera;
};

struct Candidate
{
	Math::Vec3 point;
	Math::ivec2 screen;
	int distance2{0};
	ObjectPtr object;
};

// Scratch space reused between the meshes of one search.
struct Scratch
{
	Vector<int> surfaces;
	Vector<int> triangles;
	QSet<int> visited;
};

bool make_view(const QWidget *viewport, const QPointF &mouse, View &view)
{
	::UnigineEditor::ViewportWindowPtr window = matchViewport(viewport);
	view.player = window ? window->getPlayer() : PlayerPtr();
	if (!view.player)
		return false;

	view.width = qMax(1, viewport->width());
	view.height = qMax(1, viewport->height());
	view.mouse = Math::ivec2(qRound(mouse.x()), qRound(mouse.y()));
	view.camera = view.player->getWorldPosition();

	const int radius = qMax(1, int(float(qMax(view.width, view.height)) * SNAP_RADIUS));
	view.radius2 = radius * radius;

	// The frustum of the camera is cut down to the square around the circle: the
	// corners of the view volume are found in camera space, and the square is taken
	// on its near plane.
	Math::mat4 projection = view.player->getProjection();
	projection.m00 *= float(view.height) / float(view.width);
	Math::vec3 points[8] = {
		Math::vec3(-1.0f, -1.0f, -1.0f), Math::vec3(1.0f, -1.0f, -1.0f),
		Math::vec3(-1.0f, 1.0f, -1.0f), Math::vec3(1.0f, 1.0f, -1.0f),
		Math::vec3(-1.0f, -1.0f, 1.0f), Math::vec3(1.0f, -1.0f, 1.0f),
		Math::vec3(-1.0f, 1.0f, 1.0f), Math::vec3(1.0f, 1.0f, 1.0f),
	};
	const Math::mat4 iprojection = Math::inverse(projection);
	for (Math::vec3 &point : points)
	{
		const Math::vec4 p = iprojection * Math::vec4(point, 1.0f);
		point = Math::vec3(p.x, p.y, p.z) / p.w;
	}

	const float dx = points[1].x - points[0].x;
	const float dy = points[0].y - points[2].y;
	const float x0 = points[0].x + dx * float(view.mouse.x - radius) / float(view.width);
	const float x1 = points[0].x + dx * float(view.mouse.x + radius) / float(view.width);
	const float y0 = points[2].y + dy * float(view.mouse.y - radius) / float(view.height);
	const float y1 = points[2].y + dy * float(view.mouse.y + radius) / float(view.height);

	const bool ortho = std::abs(projection.m33 - 1.0f) < 1e-4f;
	view.frustum = ortho ? Math::ortho(x0, x1, y0, y1, -points[0].z, MAX_DISTANCE)
		: Math::frustum(x0, x1, y0, y1, -points[0].z, MAX_DISTANCE);
	return true;
}

// The frustum of the view in the space of a mesh placed with 'transform'.
Math::BoundFrustum local_frustum(const View &view, const Math::Mat4 &transform)
{
	const Math::mat4 camera_local(Math::inverse(transform) * view.player->getWorldTransform());
	return Math::BoundFrustum(view.frustum, Math::inverse4(camera_local));
}

// Whether the surface is drawn at this distance (its visibility and fade distances).
bool is_surface_visible(const ObjectPtr &object, int surface, float distance)
{
	if (!object->isEnabled(surface))
		return false;

	const float min_visible = object->getMinVisibleDistance(surface);
	const float max_visible = object->getMaxVisibleDistance(surface);
	const float min_fade = object->getMinFadeDistance(surface);
	const float max_fade = object->getMaxFadeDistance(surface);

	float fade = 1.0f;
	if (distance < min_visible + min_fade)
		fade = Math::saturate(1.0f - (min_visible + min_fade - distance) / Math::max(min_fade, Math::Consts::EPS));
	else if (distance > max_visible)
		fade = Math::saturate(1.0f - (distance - max_visible) / Math::max(max_fade, Math::Consts::EPS));
	return fade >= Math::Consts::EPS;
}

// The objects a world clutter makes are not snapped to, as in the editor.
bool is_in_clutter(const NodePtr &node)
{
	for (NodePtr n = node; n;)
	{
		if (NodePtr possessor = n->getPossessor())
		{
			if (possessor->getType() == Node::WORLD_CLUTTER)
				return true;
			n = possessor;
		} else
			n = n->getParent();
	}
	return false;
}

// Takes a vertex if it is turned to the camera and is inside the circle around the
// mouse. 'local' is in the space of the mesh, 'normal' in world space.
// False when the vertex faces away from the camera: the same place may be a vertex of
// another triangle too, with another normal (on a hard edge), that faces it.
bool consider(const View &view, const ObjectPtr &object, const Math::Mat4 &transform, const Math::vec3 &local,
	const Math::vec3 &normal, Vector<Candidate> &candidates)
{
	Math::Vec3 point;
	Math::mul(point, transform, Math::Vec3(local));

	const Math::vec3 to_camera(view.camera - point);
	if (Math::dot(normal, to_camera) < 0.0f)
		return false;

	Candidate candidate;
	if (!view.player->getScreenPosition(candidate.screen.x, candidate.screen.y, point, view.width, view.height))
		return true;
	const int x = candidate.screen.x - view.mouse.x;
	const int y = candidate.screen.y - view.mouse.y;
	candidate.distance2 = x * x + y * y;
	if (candidate.distance2 > view.radius2)
		return true;

	candidate.point = point;
	candidate.object = object;
	candidates.append(candidate);
	return true;
}

// The vertices of a mesh placed with 'transform': a static mesh, or one instance of a
// cluster.
void collect_mesh(const View &view, const ObjectPtr &object, const ConstMeshPtr &mesh,
	const Math::Mat4 &transform, Scratch &scratch, Vector<Candidate> &candidates)
{
	Math::BoundFrustum frustum = local_frustum(view, transform);
	if (!frustum.insideValidFast(mesh->getBoundBox()))
		return;
	scratch.surfaces.clear();
	if (!mesh->getSurfaceCollision(frustum, scratch.surfaces))
		return;

	const float distance = float(Math::length(transform.getTranslate() - view.camera)) / Render::getDistanceScale();
	// Normals go to world space by the inverse transpose, as in raycastSurface().
	const Math::mat3 normal_transform = Math::transpose(Math::mat3(Math::mat4(Math::inverse(transform))));

	for (int surface : scratch.surfaces)
	{
		if (surface >= object->getNumSurfaces() || !is_surface_visible(object, surface, distance))
			continue;
		scratch.triangles.clear();
		if (!mesh->getTriangleCollision(frustum, scratch.triangles, surface))
			continue;

		const Vector<int> &cindices = mesh->getCIndices(surface);
		const Vector<int> &tindices = mesh->getTIndices(surface);
		const Vector<Math::vec3> &vertices = mesh->getVertices(surface);
		const Vector<Math::quat> &tangents = mesh->getTangents(surface);
		if (cindices.size() % 3 != 0 || tindices.size() != cindices.size())
			continue;

		scratch.visited.clear();
		for (int triangle : scratch.triangles)
		{
			if (triangle < 0 || triangle + 2 >= cindices.size())
				continue;
			for (int i = 0; i < 3; ++i)
			{
				const int cindex = cindices[triangle + i];
				if (cindex < 0 || cindex >= vertices.size() || scratch.visited.contains(cindex))
					continue;

				const Math::vec3 &position = vertices[cindex];
				if (!frustum.insideFast(position))
				{
					scratch.visited.insert(cindex);
					continue;
				}

				// A vertex without a tangent is taken whichever way it is turned.
				const int tindex = tindices[triangle + i];
				if (tindex < 0 || tindex >= tangents.size())
				{
					Math::Vec3 point;
					Math::mul(point, transform, Math::Vec3(position));
					consider(view, object, transform, position, Math::vec3(view.camera - point), candidates);
					scratch.visited.insert(cindex);
					continue;
				}
				// The place is done with once a vertex of it faces the camera; one that
				// faces away leaves it to the other triangles it is a corner of.
				if (consider(view, object, transform, position, normal_transform * tangents[tindex].getNormal(), candidates))
					scratch.visited.insert(cindex);
			}
		}
	}
}

void collect_dynamic(const View &view, const ObjectMeshDynamicPtr &object, Vector<Candidate> &candidates,
	Scratch &scratch)
{
	if (object->getNumVertex() == 0)
		return;

	const Math::Mat4 transform = object->getWorldTransform();
	Math::BoundFrustum frustum = local_frustum(view, transform);
	const float distance = float(Math::length(transform.getTranslate() - view.camera)) / Render::getDistanceScale();
	const Math::mat3 normal_transform = Math::transpose(Math::mat3(Math::mat4(Math::inverse(transform))));

	const ObjectMeshDynamic::Vertex *vertices = object->getVertexArray();
	const int *indices = object->getIndicesArray();
	const int num_vertices = object->getNumVertex();
	if (!vertices || !indices)
		return;

	for (int surface = 0, num = object->getNumSurfaces(); surface < num; ++surface)
	{
		if (!frustum.insideValidFast(object->getBoundBox(surface)) || !is_surface_visible(object, surface, distance))
			continue;

		scratch.visited.clear();
		for (int i = object->getSurfaceBegin(surface), end = object->getSurfaceEnd(surface); i < end; ++i)
		{
			const int index = indices[i];
			if (index < 0 || index >= num_vertices || scratch.visited.contains(index))
				continue;
			scratch.visited.insert(index);

			const ObjectMeshDynamic::Vertex &vertex = vertices[index];
			if (!frustum.insideFast(vertex.xyz))
				continue;
			consider(view, object, transform, vertex.xyz, normal_transform * vertex.tangent.getNormal(), candidates);
		}
	}
}

// Whether something is between the camera and the vertex.
bool is_hidden(const View &view, const Candidate &candidate)
{
	Math::Vec3 p0, p1;
	view.player->getDirectionFromScreen(p0, p1, candidate.screen.x, candidate.screen.y, 0, 0, view.width, view.height);

	const double distance = double(Math::length(candidate.point - p0));
	const SurfaceHit hit = raycastSurface(p0, candidate.point, SurfaceFilter(), Vector<NodePtr>());
	if (!hit.found)
		return false;
	// The ray ends on the surfaces of the vertex itself; whatever it meets clearly
	// before that hides the vertex.
	const double tolerance = std::max(0.002, distance * 0.001);
	return hit.distance < distance - tolerance;
}

} // namespace

VertexHit findVertex(const QWidget *viewport, const QPointF &mouse)
{
	VertexHit result;
	if (!viewport || !World::isLoaded())
		return result;

	View view;
	if (!make_view(viewport, mouse, view))
		return result;

	const Math::WorldBoundFrustum world_frustum(view.frustum, view.player->getIWorldTransform());
	Vector<ObjectPtr> objects;
	if (!World::getIntersection(world_frustum, objects))
		return result;

	Vector<Candidate> candidates;
	Scratch scratch;
	for (const ObjectPtr &object : objects)
	{
		if (!object || !object->isEnabled() || isEditorInternal(object) || is_in_clutter(object))
			continue;

		switch (object->getType())
		{
			case Node::OBJECT_MESH_STATIC:
			{
				const ObjectMeshStaticPtr mesh_object = static_ptr_cast<ObjectMeshStatic>(object);
				// A mesh that is not in memory yet starts loading and is there a frame
				// or two later.
				if (const ConstMeshPtr mesh = mesh_object->getMeshAsyncRAM())
					collect_mesh(view, object, mesh, object->getWorldTransform(), scratch, candidates);
				break;
			}
			case Node::OBJECT_MESH_CLUSTER:
			{
				const ObjectMeshClusterPtr cluster = static_ptr_cast<ObjectMeshCluster>(object);
				const ConstMeshPtr mesh = cluster->getMeshAsyncRAM();
				if (!mesh)
					break;
				// A cluster may have thousands of instances: the ones whose bounding
				// sphere is out of the frustum are left out right away.
				const Math::BoundBox bounds = mesh->getBoundBox();
				const Math::vec3 center = (bounds.minimum + bounds.maximum) * 0.5f;
				const float radius = Math::length(bounds.maximum - bounds.minimum) * 0.5f;
				const Math::Mat4 world = object->getWorldTransform();
				for (int instance = 0, num = cluster->getNumMeshes(); instance < num; ++instance)
				{
					const Math::Mat4 transform = world * Math::Mat4(cluster->getMeshTransform(instance));
					Math::Vec3 world_center;
					Math::mul(world_center, transform, Math::Vec3(center));
					const Math::vec3 scale = Math::mat4(transform).getScale();
					const float world_radius = radius * Math::max(scale.x, Math::max(scale.y, scale.z));
					if (!world_frustum.inside(world_center, Math::Scalar(world_radius)))
						continue;
					collect_mesh(view, object, mesh, transform, scratch, candidates);
				}
				break;
			}
			case Node::OBJECT_MESH_DYNAMIC:
				collect_dynamic(view, static_ptr_cast<ObjectMeshDynamic>(object), candidates, scratch);
				break;
			default:
				break;
		}
	}

	// The closest to the mouse that is not hidden wins.
	std::sort(candidates.begin(), candidates.end(),
		[](const Candidate &a, const Candidate &b) { return a.distance2 < b.distance2; });
	for (int i = 0; i < candidates.size() && i < MAX_OCCLUSION_TESTS; ++i)
	{
		if (is_hidden(view, candidates[i]))
			continue;
		result.found = true;
		result.point = candidates[i].point;
		result.object = candidates[i].object;
		break;
	}
	return result;
}

} // namespace ArtistTool
