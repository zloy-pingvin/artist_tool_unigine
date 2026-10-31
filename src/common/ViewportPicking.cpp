#include "ViewportPicking.h"

#include <UnigineObjects.h>
#include <UniginePlayers.h>
#include <UnigineRender.h>
#include <UnigineWorld.h>

#include <QCursor>
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
// The names the editor gives: the widget inside a viewport widget the picture is rendered
// to, and the viewport widget itself ("SceneViewport_<slot>", its viewport is "<slot + 1>").
const char SCENE_WIDGET_NAME[] = "ViewportWindowRenderWidget";
const char VIEWPORT_WIDGET_PREFIX[] = "SceneViewport_";
// How much smaller than the viewport widget that picture is, in pixels: it starts 2 from the
// left and at the very top, and ends 2 from the right and from the bottom.
const int SCENE_INSET_LEFT = 2;
const int SCENE_INSET_WIDTH = 4;
const int SCENE_INSET_HEIGHT = 2;
// How much better than the viewport under the mouse another one has to fit to be taken instead
// of it: the mouse may have moved a little between the questions.
const int MATCH_TOLERANCE = 2;
// How far from the camera a ray starts to be a ray from it.
const double RAY_FROM_CAMERA_DISTANCE = 1e-3;

int surface_type_of(const ObjectPtr &object)
{
	switch (object->getType())
	{
		case Node::OBJECT_MESH_STATIC:       return SurfaceFilter::MESH_STATIC;
		case Node::OBJECT_MESH_SKINNED:
		case Node::OBJECT_MESH_SKINNED_LEGACY: return SurfaceFilter::MESH_SKINNED;
		case Node::OBJECT_MESH_DYNAMIC:      return SurfaceFilter::MESH_DYNAMIC;
		case Node::OBJECT_MESH_CLUSTER:
		case Node::OBJECT_MESH_SPLINE_CLUSTER: return SurfaceFilter::MESH_CLUSTER;
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
	const Math::Vec3 &point, const Math::vec3 &normal, const ObjectPtr &object)
{
	const double distance = Math::length(point - p0);
	if (hit.found && distance >= hit.distance)
		return;

	hit.found = true;
	hit.distance = distance;
	hit.point = point;
	hit.normal = normal;
	hit.object = object;
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

namespace
{

const QWidget *scene_widget(const QWidget *viewport)
{
	return viewport->findChild<QWidget *>(QLatin1String(SCENE_WIDGET_NAME));
}

// Whether the viewport has the name the editor gives to the viewport of the widget.
bool is_named_after(const ViewportWindowPtr &viewport, const QWidget *widget)
{
	const QString name = widget->objectName();
	const QLatin1String prefix(VIEWPORT_WIDGET_PREFIX);
	if (!name.startsWith(prefix))
		return false;
	bool is_number = false;
	const int slot = name.mid(prefix.size()).toInt(&is_number);
	return is_number && QString::number(slot + 1) == QString::fromUtf8(viewport->getName());
}

} // namespace

// The API gives no direct link between a viewport widget and its viewport, but every
// viewport tells where the mouse is in its picture and how large that is: only the viewport
// of this widget says the same as the picture widget inside it. Should that widget not be
// found, the size is all that is left, taking in that the picture is a little smaller than
// the widget.
ViewportWindowPtr matchViewport(const QWidget *widget)
{
	Vector<ViewportWindowPtr> viewports;
	ViewportManager::getViewports(viewports);
	if (viewports.size() == 1)
		return viewports[0];

	const QWidget *scene = scene_widget(widget);
	const QPoint mouse = scene ? scene->mapFromGlobal(QCursor::pos()) : QPoint();
	const auto difference_of = [&](const ViewportWindowPtr &viewport) {
		if (!scene)
		{
			return std::abs(viewport->getWidth() + SCENE_INSET_WIDTH - widget->width())
				+ std::abs(viewport->getHeight() + SCENE_INSET_HEIGHT - widget->height());
		}
		const Math::ivec2 position = viewport->getMousePos();
		return std::abs(position.x - mouse.x()) + std::abs(position.y - mouse.y())
			+ std::abs(viewport->getWidth() - scene->width()) + std::abs(viewport->getHeight() - scene->height());
	};

	// The viewport under the mouse wins as long as it fits. Two viewports in the tabs of one
	// dock area stand at the same place, and the hidden one fits just as well: the name
	// tells them apart.
	ViewportWindowPtr best = ViewportManager::getLastHoveredViewportWindow();
	int best_difference = best ? difference_of(best) : 1 << 30;
	for (const ViewportWindowPtr &viewport : viewports)
	{
		if (!viewport)
			continue;
		const int difference = difference_of(viewport);
		if (difference < best_difference - MATCH_TOLERANCE
			|| (difference <= best_difference + MATCH_TOLERANCE && is_named_after(viewport, widget)))
		{
			best = viewport;
			best_difference = difference;
		}
	}
	return best;
}

QPointF viewportScenePosition(const QWidget *viewport, const QPointF &position)
{
	if (const QWidget *scene = scene_widget(viewport))
		return scene->mapFromGlobal(viewport->mapToGlobal(position));
	return position - QPointF(SCENE_INSET_LEFT, 0.0);
}

bool screenRay(const QWidget *viewport, const QPointF &position, Math::Vec3 &p0, Math::Vec3 &p1)
{
	ViewportWindowPtr viewport_window = matchViewport(viewport);
	PlayerPtr player = viewport_window ? viewport_window->getPlayer() : PlayerPtr();
	if (!player)
		return false;

	// In the pixels of the picture, as the engine renders it: the same place of the widget
	// would be up to a couple of pixels off near its edges.
	const QPointF scene_position = viewportScenePosition(viewport, position);
	player->getDirectionFromScreen(p0, p1, qRound(scene_position.x()), qRound(scene_position.y()),
		0, 0, qMax(1, viewport_window->getWidth()), qMax(1, viewport_window->getHeight()));
	return true;
}

namespace
{

// The viewport the mouse is (or was last) in.
ViewportWindowPtr current_viewport()
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
	return viewport;
}

PlayerPtr viewport_player()
{
	const ViewportWindowPtr viewport = current_viewport();
	return viewport ? viewport->getPlayer() : PlayerPtr();
}

void viewport_camera(Math::Vec3 &camera, float &fov)
{
	camera = Renderer::getCameraPosition();
	fov = 60.0f;
	if (PlayerPtr player = viewport_player())
	{
		camera = player->getWorldPosition();
		fov = player->getFov();
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

float viewportPixelSizeAt(const Math::Vec3 &point)
{
	const ViewportWindowPtr viewport = current_viewport();
	const int height = viewport ? viewport->getHeight() : 0;
	return viewportSizeAt(point, 1.0f / float(height > 0 ? height : 1080));
}

float viewportDistanceTo(const Math::Vec3 &point)
{
	Math::Vec3 camera;
	float fov = 60.0f;
	viewport_camera(camera, fov);
	return float(Math::length(point - camera));
}

Math::Vec3 viewportCameraPosition()
{
	Math::Vec3 camera;
	float fov = 60.0f;
	viewport_camera(camera, fov);
	return camera;
}

bool viewportCameraView(Math::Vec3 &position, Math::vec3 &direction)
{
	const PlayerPtr player = viewport_player();
	if (!player)
		return false;
	position = player->getWorldPosition();
	direction = player->getViewDirection();
	return true;
}

int surfaceFilterType(const ObjectPtr &object)
{
	return surface_type_of(object);
}

bool isSurfaceDrawn(const ObjectPtr &object, int surface, float distance, int viewport_mask)
{
	if (!object->isEnabled(surface) || (object->getViewportMask(surface) & viewport_mask) == 0)
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

float surfaceDrawDistance(const ObjectPtr &object, const Math::Vec3 &camera)
{
	// Up to the bound of the whole object, as the engine counts it for its surfaces, not up
	// to its pivot: that may be far from what the camera looks at. Inside the bound - none.
	const float distance = float(object->getSpatialBoundSphere().distanceValid(camera));
	return Math::max(distance, 0.0f) / Render::getDistanceScale();
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

bool isNodeAlive(const NodePtr &node)
{
	if (!node || node.isDeleted())
		return false;
	for (NodePtr n = node; n; n = n->getParent())
	{
		if (n->hasData("unigine_editor_undo_tag"))
			return false;
	}
	return true;
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

namespace
{

// The camera of a viewport the ray is aimed from, the way the engine makes a ray from the
// screen: from the camera itself, or in an orthographic view from a plane a unit in front of
// it, along its view. Null for any other ray, as one down to the ground.
PlayerPtr camera_of_ray(const Math::Vec3 &p0, const Math::Vec3 &p1)
{
	Vector<ViewportWindowPtr> viewports;
	ViewportManager::getViewports(viewports);
	for (const ViewportWindowPtr &viewport : viewports)
	{
		PlayerPtr player = viewport ? viewport->getPlayer() : PlayerPtr();
		if (!player)
			continue;
		const Math::Vec3 offset = p0 - player->getWorldPosition();
		if (player->getProjectionMode() != Camera::PROJECTION_MODE_ORTHOGRAPHIC)
		{
			if (double(Math::length(offset)) < RAY_FROM_CAMERA_DISTANCE)
				return player;
			continue;
		}
		const Math::vec3 view = player->getViewDirection();
		const Math::Scalar ahead = Math::dot(offset, Math::Vec3(view));
		if (std::abs(double(ahead) - 1.0) < RAY_FROM_CAMERA_DISTANCE
			&& Math::dot(Math::normalize(Math::vec3(p1 - p0)), view) > 0.9999f)
			return player;
	}
	return PlayerPtr();
}

// Whether the object tells by itself which of its surfaces are drawn: every instance by how
// far it is from the start of the ray.
bool checks_distance_per_instance(const ObjectPtr &object)
{
	switch (object->getType())
	{
		case Node::OBJECT_MESH_CLUSTER:
		case Node::OBJECT_MESH_CLUTTER:
		case Node::OBJECT_MESH_SPLINE_CLUSTER:
			return true;
		default:
			return false;
	}
}

} // namespace

SurfaceHit raycastSurface(const Math::Vec3 &p0, const Math::Vec3 &p1, const SurfaceFilter &filter,
	const Vector<NodePtr> &ignore)
{
	SurfaceHit hit;
	bool hit_allowed = false;

	Vector<ObjectPtr> objects;
	World::getIntersection(p0, p1, objects, false);

	// A ray from a camera is about what that camera shows: what it does not draw (a level of
	// detail for another distance, a mesh that only casts a shadow) is not there for it.
	const PlayerPtr camera = camera_of_ray(p0, p1);
	const int camera_mask = camera ? camera->getViewportMask() : 0;
	const Math::Vec3 camera_position = camera ? camera->getWorldPosition() : Math::Vec3(p0);

	// Every object along the ray, surface by surface - the terrains too (a Landscape
	// Terrain answers per surface by its height map). A query of the world would test the
	// same objects the same way again, only fewer of them (the Intersection flag).
	ObjectIntersectionNormalPtr intersection = ObjectIntersectionNormal::create();
	for (const ObjectPtr &object : objects)
	{
		if (!object || !object->isEnabled() || isEditorInternal(object))
			continue;
		if (is_ignored(object, ignore))
			continue;

		const bool object_allowed = is_object_allowed(object, filter);
		// What is looked through is not tested at all: in a dense scene (grass clusters,
		// forests) most of the time goes there.
		if (!object_allowed && filter.look_through)
			continue;

		// Object::getIntersection() works in the object's local space.
		const Math::Mat4 transform = object->getWorldTransform();
		const Math::Mat4 itransform = object->getIWorldTransform();
		Math::Vec3 local_p0;
		Math::mul(local_p0, itransform, p0);

		const bool per_instance = checks_distance_per_instance(object);
		const float camera_distance = camera && !per_instance ? surfaceDrawDistance(object, camera_position) : 0.0f;

		for (int surface = 0, num = object->getNumSurfaces(); surface < num; ++surface)
		{
			if (!object->isEnabled(surface))
				continue;
			if (camera && !(per_instance ? (object->getViewportMask(surface) & camera_mask) != 0
					: isSurfaceDrawn(object, surface, camera_distance, camera_mask)))
				continue;
			const bool allowed = object_allowed && (!filter.only_intersection
				|| (object->getIntersection(surface) && object->getIntersectionMask(surface) != 0));
			if (!allowed && filter.look_through)
				continue;
			// Only what is closer than the hit found so far: a shorter ray is tested
			// faster.
			Math::Vec3 local_p1;
			Math::mul(local_p1, itransform, hit.found ? hit.point : p1);
			if (!object->getIntersection(local_p0, local_p1, intersection, surface))
				continue;

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
			take_closer(hit, hit_allowed, allowed, p0, point, normal, object);
		}
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
