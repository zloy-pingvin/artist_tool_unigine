#include "ViewportPicking.h"

#include <UnigineObjects.h>
#include <UniginePlayers.h>
#include <UnigineRender.h>
#include <UnigineWorld.h>

#include <QWidget>

#include <cmath>
#include <cstring>

using namespace Unigine;
using ::UnigineEditor::ViewportManager;
using ::UnigineEditor::ViewportWindowPtr;

namespace ArtistTool
{

namespace
{

const char VIEWPORT_CLASS_MARK[] = "ViewportWindow";
// How many ignored objects in a row a ray looks behind for a surface. A ray along a
// fence goes through many of its sections before it gets to the ground.
const int MAX_LOOK_BEHIND = 48;

int surface_type_of(const ObjectPtr &object)
{
	switch (object->getType())
	{
		case Node::OBJECT_MESH_STATIC:       return SurfaceFilter::MESH_STATIC;
		case Node::OBJECT_MESH_SKINNED:
		case Node::OBJECT_MESH_SKINNED_LEGACY: return SurfaceFilter::MESH_SKINNED;
		case Node::OBJECT_MESH_DYNAMIC:      return SurfaceFilter::MESH_DYNAMIC;
		case Node::OBJECT_MESH_CLUSTER:      return SurfaceFilter::MESH_CLUSTER;
		case Node::OBJECT_MESH_CLUTTER:      return SurfaceFilter::MESH_CLUTTER;
		case Node::OBJECT_LANDSCAPE_TERRAIN:
		case Node::OBJECT_TERRAIN_GLOBAL:    return SurfaceFilter::TERRAIN;
		case Node::OBJECT_WATER_GLOBAL:
		case Node::OBJECT_WATER_MESH:        return SurfaceFilter::WATER;
		case Node::OBJECT_CLOUD_LAYER:       return SurfaceFilter::CLOUDS;
		default:                             return 0;
	}
}

bool is_object_allowed(const ObjectPtr &object, const SurfaceFilter &filter)
{
	if ((surface_type_of(object) & filter.surface_types) == 0)
		return false;
	if (filter.only_immovable && !object->isImmovable())
		return false;
	return true;
}

bool is_ignored(const NodePtr &node, const Vector<NodePtr> &ignore)
{
	for (const NodePtr &root : ignore)
	{
		if (root && !root.isDeleted() && isInside(node, root))
			return true;
	}
	return false;
}

// Keeps the hit closest to p0. 'allowed' tells whether an object can be placed there.
void take_closer(SurfaceHit &hit, bool &hit_allowed, bool allowed, const Math::Vec3 &p0,
	const Math::Vec3 &point, const Math::vec3 &normal)
{
	const double distance = Math::length(point - p0);
	if (hit.found && distance >= hit.distance)
		return;

	hit.found = true;
	hit.distance = distance;
	hit.point = point;
	hit.normal = normal;
	hit_allowed = allowed;
}

} // namespace

bool isViewportWidget(const QWidget *widget)
{
	return QByteArray(widget->metaObject()->className()).contains(VIEWPORT_CLASS_MARK);
}

QWidget *findViewportWidget(QObject *watched)
{
	if (!watched || !watched->isWidgetType())
		return nullptr;

	for (QWidget *widget = static_cast<QWidget *>(watched); widget; widget = widget->parentWidget())
	{
		if (isViewportWidget(widget))
			return widget;
	}
	return nullptr;
}

// The API gives no direct link between a viewport widget and its viewport, so with
// several viewports the one closest in size is taken (the widget is a few pixels
// larger than what it renders, and its size is in logical pixels while the viewport
// one may be in physical ones).
ViewportWindowPtr matchViewport(const QWidget *widget)
{
	Vector<ViewportWindowPtr> viewports;
	ViewportManager::getViewports(viewports);
	if (viewports.size() == 1)
		return viewports[0];

	const double ratio = widget->devicePixelRatioF();
	const auto size_difference = [&](const ViewportWindowPtr &viewport) {
		const double width = viewport->getWidth();
		const double height = viewport->getHeight();
		const double logical = std::abs(width - widget->width()) + std::abs(height - widget->height());
		const double physical = std::abs(width - widget->width() * ratio)
			+ std::abs(height - widget->height() * ratio);
		return logical < physical ? logical : physical;
	};

	// The viewport under the mouse wins when several are the same size.
	ViewportWindowPtr best = ViewportManager::getLastHoveredViewportWindow();
	double best_difference = best ? size_difference(best) : 1e30;
	for (const ViewportWindowPtr &viewport : viewports)
	{
		if (!viewport)
			continue;
		const double difference = size_difference(viewport);
		if (difference < best_difference)
		{
			best = viewport;
			best_difference = difference;
		}
	}
	return best;
}

bool screenRay(const QWidget *viewport, const QPointF &position, Math::Vec3 &p0, Math::Vec3 &p1)
{
	ViewportWindowPtr viewport_window = matchViewport(viewport);
	PlayerPtr player = viewport_window ? viewport_window->getPlayer() : PlayerPtr();
	if (!player)
		return false;

	player->getDirectionFromScreen(p0, p1, qRound(position.x()), qRound(position.y()),
		0, 0, viewport->width(), viewport->height());
	return true;
}

namespace
{

// The camera of the viewport the mouse is (or was last) in.
void viewport_camera(Math::Vec3 &camera, float &fov)
{
	ViewportWindowPtr viewport = ViewportManager::getLastHoveredViewportWindow();
	if (!viewport)
		viewport = ViewportManager::getActiveViewportWindow();
	if (!viewport)
	{
		Vector<ViewportWindowPtr> viewports;
		ViewportManager::getViewports(viewports);
		if (viewports.size())
			viewport = viewports[0];
	}

	camera = Renderer::getCameraPosition();
	fov = 60.0f;
	if (viewport)
	{
		if (PlayerPtr player = viewport->getPlayer())
		{
			camera = player->getWorldPosition();
			fov = player->getFov();
		}
	}
	if (fov < 1.0f || fov > 179.0f)
		fov = 60.0f;
}

} // namespace

float viewportSizeAt(const Math::Vec3 &point, float fraction)
{
	Math::Vec3 camera;
	float fov = 60.0f;
	viewport_camera(camera, fov);

	// The height of what the camera sees at that distance.
	const float distance = float(Math::length(point - camera));
	const float visible_height = 2.0f * distance * std::tan(fov * 0.5f * 3.14159265f / 180.0f);
	return visible_height * fraction;
}

float viewportDistanceTo(const Math::Vec3 &point)
{
	Math::Vec3 camera;
	float fov = 60.0f;
	viewport_camera(camera, fov);
	return float(Math::length(point - camera));
}

bool isEditorInternal(const NodePtr &node)
{
	for (NodePtr n = node; n; n = n->getParent())
	{
		const char *name = n->getName();
		if (name && strncmp(name, "UnigineEditor.", 14) == 0)
			return true;
	}
	return false;
}

bool isInside(const NodePtr &node, const NodePtr &root)
{
	const int root_id = root->getID();
	for (NodePtr n = node; n;)
	{
		if (n->getID() == root_id)
			return true;

		NodePtr next = n->getParent();
		if (!next)
			next = n->getPossessor();
		n = next;
	}
	return false;
}

SurfaceHit raycastSurface(const Math::Vec3 &p0, const Math::Vec3 &p1, const SurfaceFilter &filter,
	const Vector<NodePtr> &ignore)
{
	SurfaceHit hit;
	bool hit_allowed = false;

	Vector<ObjectPtr> objects;
	World::getIntersection(p0, p1, objects, false);

	ObjectIntersectionNormalPtr intersection = ObjectIntersectionNormal::create();
	for (const ObjectPtr &object : objects)
	{
		if (!object || !object->isEnabled() || isEditorInternal(object))
			continue;
		if (is_ignored(object, ignore))
			continue;

		const bool object_allowed = is_object_allowed(object, filter);

		// Object::getIntersection() works in the object's local space.
		const Math::Mat4 transform = object->getWorldTransform();
		const Math::Mat4 itransform = object->getIWorldTransform();
		Math::Vec3 local_p0, local_p1;
		Math::mul(local_p0, itransform, p0);
		Math::mul(local_p1, itransform, p1);

		for (int surface = 0, num = object->getNumSurfaces(); surface < num; ++surface)
		{
			if (!object->isEnabled(surface))
				continue;
			if (!object->getIntersection(local_p0, local_p1, intersection, surface))
				continue;

			const bool allowed = object_allowed && (!filter.only_intersection
				|| (object->getIntersection(surface) && object->getIntersectionMask(surface) != 0));

			Math::Vec3 point;
			Math::mul(point, transform, intersection->getPoint());
			// A normal goes to world space by the inverse transpose of the transform:
			// with the transform itself it would lean off the surface of an object
			// scaled differently along its axes.
			Math::vec3 normal;
			Math::mul3(normal, Math::transpose(Math::mat4(itransform)), intersection->getNormal());
			const float normal_length2 = Math::length2(normal);
			normal = std::isfinite(normal_length2) && normal_length2 > 1e-20f
				? normal * (1.0f / std::sqrt(normal_length2)) : Math::vec3(0.0f, 0.0f, 1.0f);
			take_closer(hit, hit_allowed, allowed, p0, point, normal);
		}
	}

	// Objects that are not intersected surface by surface (e.g. terrains). This query
	// only sees surfaces with the Intersection flag.
	WorldIntersectionNormalPtr world_intersection = WorldIntersectionNormal::create();
	Vector<NodePtr> exclude;
	for (int attempt = 0; attempt < MAX_LOOK_BEHIND; ++attempt)
	{
		ObjectPtr object = World::getIntersection(p0, p1, ~0, exclude, world_intersection);
		if (!object)
			break;

		// Look behind the ignored objects and the editor helpers.
		if (isEditorInternal(object) || is_ignored(object, ignore))
		{
			exclude.append(object);
			continue;
		}

		take_closer(hit, hit_allowed, is_object_allowed(object, filter), p0,
			world_intersection->getPoint(), world_intersection->getNormal());
		break;
	}

	hit.allowed = hit.found && hit_allowed;

	// The normal always looks back along the ray, to the side the ray came from.
	if (hit.found && Math::dot(hit.normal, Math::vec3(p1 - p0)) > 0.0f)
		hit.normal = -hit.normal;

	return hit;
}

Math::mat4 basisFromUp(const Math::vec3 &up)
{
	Math::vec3 reference(0.0f, 1.0f, 0.0f);
	if (std::abs(Math::dot(reference, up)) > 0.99f)
		reference = Math::vec3(1.0f, 0.0f, 0.0f);

	const Math::vec3 x = Math::normalize(Math::cross(reference, up));
	const Math::vec3 y = Math::cross(up, x);

	Math::mat4 basis = Math::mat4_identity;
	basis.setColumn3(0, x);
	basis.setColumn3(1, y);
	basis.setColumn3(2, up);
	return basis;
}

} // namespace ArtistTool
