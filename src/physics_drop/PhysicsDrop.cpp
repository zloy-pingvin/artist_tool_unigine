#include "PhysicsDrop.h"
#include "Localization.h"
#include "PhysicsGlow.h"
#include "PhysicsVisibility.h"
#include "UiHelpers.h"

#include <editor/UnigineActions.h>
#include <editor/UnigineObjectMode.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>
#include <editor/UnigineUndo.h>

#include <UnigineEngine.h>
#include <UnigineGame.h>
#include <UnigineLog.h>
#include <UnigineMathLibConcave.h>
#include <UnigineMathLibConvex.h>
#include <UnigineMesh.h>
#include <UnigineNodes.h>
#include <UniginePlayers.h>
#include <UnigineWorld.h>

#include <QApplication>
#include <QEvent>
#include <QKeyEvent>
#include <QRandomGenerator>
#include <QStringList>
#include <QToolButton>
#include <QWidget>

#include <algorithm>
#include <cmath>

using namespace Unigine;
using namespace Unigine::Math;

namespace ArtistTool
{

namespace
{

// How close the hull of a mesh follows it, of the half of its diagonal. A hull with fewer
// points works faster.
const float HULL_ERROR = 0.01f;
// The same for the hull of a whole object: its points are those of a hull already.
const float OBJECT_HULL_ERROR = 0.001f;
// A hull thinner than that (its volume, in cubic units) is taken for a flat object.
const double MIN_HULL_VOLUME = 1e-7;
// An object thinner than that, in units, is flat: it falls as a thin box.
const float FLAT_SIZE = 0.0101f;
// A flat mesh has no hull: its own points are measured, as long as they are not too many.
const int MAX_FLAT_POINTS = 4096;
// What a matrix is rounded to before points are measured with it, of its scale.
const float MATRIX_ROUNDING = 1e-5f;

// The way things feel. Mass grows with the size, within limits: a pile is steady only while
// the heavy objects are not too much heavier than the light ones.
const float DENSITY = 200.0f;
const float MIN_MASS = 0.5f;
const float MAX_MASS = 50.0f;
const float RESTITUTION = 0.0f;
const float LINEAR_DAMPING = 0.05f;
// Round things stop rolling sooner, also on their rim, like a coin.
const float ANGULAR_DAMPING = 4.0f;
// Objects that start inside each other must not fly away.
const float MAX_LINEAR_SPEED = 30.0f;

// The physics for the time of the fall. By default the engine solves the contacts once per
// step, 60 steps a second: enough for a game, but objects lying on each other shiver and never
// settle. These values were chosen by measuring piles of cans and boxes without the editor: at
// 120 steps objects started inside each other stay stuck by up to 13 mm and take twice as long,
// with 8 iterations by up to 19 mm. A frame may not have time for all the steps: then the fall
// goes in slow motion, which changes nothing in the result.
const float PHYSICS_RATE = 240.0f;
const int SOLVER_ITERATIONS = 16;
const int FROZEN_FRAMES = 10;
// Slower than that an object that touches something stops being simulated.
const float FROZEN_LINEAR_VELOCITY = 0.03f;
const float FROZEN_ANGULAR_VELOCITY = 0.05f;
// How deep things may sink into each other, in units, and how fast what sinks deeper is pushed
// out. The engine lets them sink by a centimeter: fine in a game, but a tin can 8 cm wide then
// stays visibly in the floor. Much less than here and contacts come and go, which is a shiver.
const float PENETRATION_TOLERANCE = 0.002f;
const float PENETRATION_FACTOR = 0.1f;
// The shapes are that much bigger than the mesh: they rest that deep in what is under them, and
// the mesh rests on its surface.
const float SHAPE_SKIN = PENETRATION_TOLERANCE;
// Slower than that, in units per second, an object does not need continuous collision: in a step
// it moves less than the thinnest shape. On an object at rest it only makes contacts lose what
// the solver has remembered about them.
const float CONTINUOUS_SPEED = 1.0f;
// Nor does it while in a step it moves less than that part of its own thinnest size: it can not pass
// through anything unnoticed. The swept contacts are suspected of the throws take_back_jump() catches
// (contacts 24-143 units deep, only with something fast around), so they are kept to where they matter.
const float CONTINUOUS_OF_THINNEST = 0.5f;

// The fall is over when the center of every object has stayed within that distance, in units, for
// that long, in seconds of the fall. How it is turned does not count: what spins on the spot would
// do it for ever. Among hundreds of objects some always twitch by millimeters: with a stricter
// distance the fall never ends by itself, and waiting longer (1 or 2 s) was measured to take 2-5
// times as long and to leave as many unfinished objects. Objects are never stopped one by one: a
// stopped one has no gravity and hangs in the air when what is under it goes away, and it no longer
// pushes away what has got into it.
const float SETTLED_DISTANCE = 0.025f;
const double SETTLED_TIME = 0.5;

// An object this far under its start has nothing under it.
const double LOST_DEPTH = 500.0;
// But it does not take to fall all that way: it would take 20 seconds, and a fall is often over sooner,
// with the object left in the air under the floor. Whatever falls at least that fast, in units per
// second, is looked at once in that many frames: with nothing solid below, it is gone.
const float LOST_CHECK_SPEED = 3.0f;
const int LOST_CHECK_FRAMES = 16;
// In a heavy pile the engine now and then moves a resting object by units within one frame, up or down
// through the floor, with no speed to show for it. It is seen by the middle of the object going further
// in a frame than this much, in units, or this part of its size, plus this many times what its speed
// gives. Then it is put back. The cause is not known: on the bench it does not happen.
const float JUMP_LEAST = 0.2f;
const float JUMP_OF_SIZE = 0.5f;
const float JUMP_OF_SPEED = 2.0f;
// Real seconds before it is told that the physics does not go at all.
const double NO_PHYSICS_TIME = 3.0;
const double NOTIFY_PERIOD = 0.25;
// How far around the falling objects, in units, their surroundings are made solid.
const float AREA_MARGIN = 2.0f;
// Seconds of flight the area looks ahead.
const float AREA_AHEAD = 0.5f;
const int AREA_FRAMES = 5;
// The size of the pieces of the world that are looked through for what to make solid, in units:
// every piece once, as what falls gets near it. Without that a pile would ask the world about all
// that is around it several times a second, and a big world would feel it.
const float AREA_CELL = 8.0f;

// The dome: only what is inside it takes part in the physics. Its middle is that far in front of
// the camera, of its radius: most of it is where the camera looks, and the camera is inside.
const float DOME_AHEAD = 0.6f;
// The camera may look around that much, of the radius, before the dome goes after it, and then it
// goes until it is that close: it does not swing with every turn of the head.
const float DOME_SLACK = 0.25f;
const float DOME_CAUGHT_UP = 0.05f;
// What is left of the way goes down that many times per second.
const float DOME_FOLLOW_SPEED = 3.0f;
// An object stops that much farther out, of the radius, than where it starts: on the border of the
// dome it does not go on and off.
const float DOME_PARK_MARGIN = 0.1f;
// It shows as a wall that rises from the ground and melts away that high, of the radius.
const vec4 DOME_COLOR(0.35f, 0.75f, 1.0f, 0.35f);
const float DOME_FADE = 0.35f;
const int DOME_BANDS = 24;
// The ground is looked for under the middle of the dome and that far around it, of the radius, each
// time the dome has gone that far: the lowest place is taken, as a roof or a pile is not the ground.
const float DOME_GROUND_SPREAD = 0.5f;
const float DOME_GROUND_STEP = 0.05f;
const float DOME_GROUND_FOLLOW_SPEED = 3.0f;
// A body whose bounds hold the camera is stepped by the engine itself: see rebuild_stepped().
const float CAMERA_INSIDE_MARGIN = 0.1f;
const float CAMERA_INSIDE_TIME = 0.05f;

// An object held by the mouse: how fast it goes to the mouse for every unit it is away, and how
// fast it may go. Faster objects would plow through a pile.
const float HELD_GAIN = 15.0f;
const float HELD_MAX_SPEED = 8.0f;
const float HELD_ANGULAR_DAMPING = 15.0f;
// How fast the manipulator may turn an object, in radians a second.
const float GIZMO_MAX_TURN = 10.0f;
// Differences less than that in the matrix of a node are the rounding of the engine.
const double GIZMO_TOUCH = 1e-4;
// Frames to wait for the step of the manipulator after the button is let go.
const int GIZMO_UNDO_WAIT_FRAMES = 30;
// A new object appears only with this much room: of the sum of its size and the size of the other one.
// Measured pouring into a heap: pushed down instead, into what is there, 200 at a go got some flying at
// the limit of the speed, over and over, and one through the floor. Pushed up they did not, but they
// appeared over the portal. Waiting for room, with 400 at a go: none over 4 units a second, none lost.
const float SPAWN_APART = 0.75f;
// The size of the first one, not known yet.
const float SPAWN_FIRST_RADIUS = 0.1f;
// Deeper than that in something, of its size, an object does not stay where it appears: it goes up by
// steps of that much, of its size, no more than that many times.
const float SPAWN_DEEPEST = 0.2f;
const float SPAWN_RISE_STEP = 0.5f;
const int SPAWN_RISE_STEPS = 60;
// The zone that carries a pile: the part of its radius where it carries in full, and how fast the speed
// of what is in it gets to what is asked: what is left of the difference goes down that many times per
// second.
const float CARRY_CORE = 0.35f;
const float CARRY_GRIP_SPEED = 60.0f;
const float JITTER_SPEED = 0.5f;
const float BARELY_MOVED_DISTANCE = 0.005f;
const float BARELY_MOVED_COS = 0.9998f;
// Farther than anything in a world, for measuring a box around things.
const Scalar FAR_AWAY = Scalar(1e30);

Mat4 without_scale(const NodePtr &node)
{
	Mat4 transform(node->getWorldRotation());
	transform.setColumn3(3, node->getWorldPosition());
	return transform;
}

// Calls 'visit' for the node and all that is inside it, with the content of node references.
template<typename Visit>
void walk(const NodePtr &node, const Visit &visit)
{
	if (!node)
		return;
	visit(node);
	if (const NodeReferencePtr reference = checked_ptr_cast<NodeReference>(node))
		walk(reference->getReference(), visit);
	for (int i = 0; i < node->getNumChildren(); ++i)
		walk(node->getChild(i), visit);
}

bool has_geometry(const NodePtr &node)
{
	if (!node->isEnabled() || !node->isObject())
		return false;
	switch (node->getType())
	{
		case Node::OBJECT_MESH_STATIC:
		case Node::OBJECT_MESH_DYNAMIC:
		case Node::OBJECT_MESH_SKINNED:
		case Node::OBJECT_MESH_SKINNED_LEGACY:
			return true;
		default:
			return false;
	}
}

// The surfaces of an object that are what it looks like from close by: the far levels of
// detail are left out.
QVector<int> near_surfaces(const ObjectPtr &object)
{
	QVector<int> surfaces;
	QVector<int> unseen;
	for (int s = 0, num = object->getNumSurfaces(); s < num; ++s)
	{
		if (!object->isEnabled(s) || object->getMinVisibleDistance(s) > 0.0f)
			continue;
		// A surface only for shadows is not seen: it counts only if there is nothing else.
		(object->getViewportMask(s) != 0 ? surfaces : unseen).append(s);
	}
	return surfaces.isEmpty() ? unseen : surfaces;
}

// The faces of a hull as triangles, by the numbers of its points.
std::shared_ptr<const std::vector<int>> hull_triangles(const Convex &hull)
{
	auto triangles = std::make_shared<std::vector<int>>();
	for (int f = 0; f < hull.getNumFaces(); ++f)
	{
		for (int i = 2; i < hull.getNumFaceVertex(f); ++i)
		{
			triangles->push_back(hull.getFaceVertex(f, 0));
			triangles->push_back(hull.getFaceVertex(f, i - 1));
			triangles->push_back(hull.getFaceVertex(f, i));
		}
	}
	return triangles;
}

std::vector<vec3> hull_points(const Convex &hull)
{
	std::vector<vec3> points;
	points.reserve(hull.getNumVertex());
	for (int i = 0; i < hull.getNumVertex(); ++i)
		points.push_back(vec3(hull.getVertex(i)));
	return points;
}

bool looks_flat(const std::vector<vec3> &points)
{
	const vec3 size = fitPrimitive(points, PrimitiveFit::BOX).size;
	return min(size.x, min(size.y, size.z)) <= FLAT_SIZE;
}

// The convex hull of a set of points. False if they lie in one plane: they have none.
bool make_hull(const std::vector<vec3> &points, float error, Convex &hull)
{
	if (points.size() < 4 || looks_flat(points))
		return false;
	// With points very close to one plane the engine may fail to build it: a little rougher
	// it works. A real object must not be taken for a flat one because of that.
	for (const float tolerance : {error, error * 5.0f, error * 25.0f})
	{
		if (hull.create(points.data(), int(points.size()), tolerance) && hull.getVolume() > MIN_HULL_VOLUME)
			return true;
	}
	return false;
}

// The matrix of what the physics has already turned is a hair off. Then points of one plane are not
// quite in one plane, and on such points (the bottom of a cone) the engine gives up on a hull: 1000
// tries, tens of milliseconds and an error in the log, several times per object.
mat4 without_hair(const mat4 &transform)
{
	float size = 0.0f;
	for (int i = 0; i < 3; ++i)
		size = std::max(size, length(transform.getColumn3(i)));
	const float unit = size * MATRIX_ROUNDING;
	if (unit <= 0.0f)
		return transform;

	mat4 result = transform;
	for (int i = 0; i < 3; ++i)
	{
		const vec3 column = transform.getColumn3(i);
		result.setColumn3(i, vec3(std::round(column.x / unit) * unit, std::round(column.y / unit) * unit, std::round(column.z / unit) * unit));
	}
	return result;
}

// The shapes of a body do not follow the scale of its object: on a scaled one they would not
// fit it.
bool has_no_scale(const mat4 &transform)
{
	for (int i = 0; i < 3; ++i)
	{
		if (std::abs(length(transform.getColumn3(i)) - 1.0f) > 0.01f)
			return false;
	}
	return true;
}

void box_corners(const BoundBox &box, std::vector<vec3> &points)
{
	const vec3 low = box.minimum;
	const vec3 high = box.maximum;
	for (int i = 0; i < 8; ++i)
		points.push_back(vec3(i & 1 ? high.x : low.x, i & 2 ? high.y : low.y, i & 4 ? high.z : low.z));
}

QString shape_name(int kind, bool flat)
{
	if (flat)
		return uiText("Box (flat)", "Коробка (плоский)");
	switch (kind)
	{
		case PhysicsDrop::SHAPE_PIECES: return uiText("Pieces", "Куски");
		case PhysicsDrop::SHAPE_BOX: return uiText("Box", "Коробка");
		case PhysicsDrop::SHAPE_SPHERE: return uiText("Sphere", "Шар");
		case PhysicsDrop::SHAPE_CAPSULE: return uiText("Capsule", "Капсула");
		case PhysicsDrop::SHAPE_CYLINDER: return uiText("Cylinder", "Цилиндр");
		case PhysicsDrop::SHAPE_READY: return uiText("Its own shapes", "Свои шейпы");
		default: return uiText("Hull", "Оболочка");
	}
}

int shape_kind_of(PrimitiveFit::Kind kind)
{
	switch (kind)
	{
		case PrimitiveFit::SPHERE: return PhysicsDrop::SHAPE_SPHERE;
		case PrimitiveFit::CAPSULE: return PhysicsDrop::SHAPE_CAPSULE;
		case PrimitiveFit::CYLINDER: return PhysicsDrop::SHAPE_CYLINDER;
		default: return PhysicsDrop::SHAPE_BOX;
	}
}

// The kind of an object in the filter of what the objects fall on; 0 if it can not be solid
// (water and clouds have no collision surfaces).
int solid_type_of(const ObjectPtr &object)
{
	const int type = surfaceFilterType(object);
	return type & (SurfaceFilter::WATER | SurfaceFilter::CLOUDS) ? 0 : type;
}

// The engine collides with a mesh only while it is in memory, and loads it in the background
// when asked: what falls would go through a floor that is not there yet.
void load_mesh_now(const ObjectPtr &object)
{
	if (const ObjectMeshStaticPtr mesh = checked_ptr_cast<ObjectMeshStatic>(object))
		mesh->getMeshForceRAM();
	else if (const ObjectMeshClusterPtr cluster = checked_ptr_cast<ObjectMeshCluster>(object))
		cluster->getMeshForceRAM();
	else if (const ObjectMeshClutterPtr clutter = checked_ptr_cast<ObjectMeshClutter>(object))
		clutter->getMeshForceRAM();
}

// Within a few millimeters and a degree: the shapes are a little bigger than the mesh, so what
// lies in a pile always shifts that much when it gets a stand-in.
bool barely_moved(const Mat4 &from, const Mat4 &to)
{
	if (float(length(to.getColumn3(3) - from.getColumn3(3))) > BARELY_MOVED_DISTANCE)
		return false;
	for (int axis = 0; axis < 3; ++axis)
	{
		if (dot(normalize(vec3(to.getColumn3(axis))), normalize(vec3(from.getColumn3(axis)))) < BARELY_MOVED_COS)
			return false;
	}
	return true;
}

// How strongly the zone carries what is that far from its middle, of its radius: in full within the
// core, from there smoothly down to nothing at the edge.
float carry_strength(float distance)
{
	const float s = clamp((distance - CARRY_CORE) / (1.0f - CARRY_CORE), 0.0f, 1.0f);
	return 1.0f - s * s * (3.0f - 2.0f * s);
}

bool matrices_differ(const Mat4 &a, const Mat4 &b)
{
	for (int column = 0; column < 4; ++column)
	{
		const Vec3 difference = a.getColumn3(column) - b.getColumn3(column);
		if (std::abs(double(difference.x)) > GIZMO_TOUCH || std::abs(double(difference.y)) > GIZMO_TOUCH || std::abs(double(difference.z)) > GIZMO_TOUCH)
			return true;
	}
	return false;
}

// The scale of a matrix that does nothing else and does it evenly; 0 if it does more.
float even_scale_of(const mat4 &matrix)
{
	const float scale = length(matrix.getColumn3(0));
	if (scale <= 0.0f)
		return 0.0f;
	for (int column = 0; column < 4; ++column)
	{
		const vec3 expected(column == 0 ? scale : 0.0f, column == 1 ? scale : 0.0f, column == 2 ? scale : 0.0f);
		if (length(matrix.getColumn3(column) - expected) > scale * 1e-4f)
			return 0.0f;
	}
	return scale;
}

PrimitiveFit scaled_fit(const PrimitiveFit &fit, float scale)
{
	PrimitiveFit result = fit;
	result.transform.setColumn3(3, fit.transform.getColumn3(3) * scale);
	result.size = fit.size * scale;
	result.radius = fit.radius * scale;
	result.height = fit.height * scale;
	result.volume = fit.volume * double(scale) * double(scale) * double(scale);
	return result;
}

// The object can not turn, whatever hits it. The solver has to know that: with the angular scale
// alone it still counts on the turn when it works out what contacts do, and the turn is thrown away
// afterwards, so that friction and pushing out go wrong. Measured with 60 objects: they never stop,
// creeping apart at 0.12 units a second, and after 8 s the farthest is 2 units away; with no inertia
// they all are at rest within 4 s, the farthest 1 unit away. A body with an inertia of all zeros is
// what the engine takes for one that does not turn.
void forbid_turning(const BodyRigidPtr &body)
{
	const float mass = body->getMass();
	const vec3 center = body->getCenterOfMass();
	body->setShapeBased(false);
	body->setMass(mass);
	body->setCenterOfMass(center);
	mat3 no_inertia;
	no_inertia.setZero();
	body->setInertia(no_inertia);
	// For what sets its turn from outside, the shake for one.
	body->setAngularScale(vec3(0.0f, 0.0f, 0.0f));
}

// The buttons on the toolbar of the editor: logic, physics, sound.
QList<QToolButton *> find_play_buttons()
{
	for (QWidget *window : QApplication::topLevelWidgets())
	{
		for (QWidget *widget : window->findChildren<QWidget *>())
		{
			if (!QString::fromLatin1(widget->metaObject()->className()).endsWith(QString::fromUtf8("WorldActionToolWidget")))
				continue;
			const QList<QToolButton *> buttons = widget->findChildren<QToolButton *>();
			if (buttons.size() == 3)
				return buttons;
		}
	}
	return {};
}

// The main menu of the editor.
QMenuBar *find_main_menu()
{
	for (QWidget *window : QApplication::topLevelWidgets())
	{
		for (QMenuBar *menu : window->findChildren<QMenuBar *>())
		{
			if (QString::fromLatin1(menu->metaObject()->className()).endsWith(QString::fromUtf8("MainMenu")))
				return menu;
		}
	}
	return nullptr;
}

} // namespace

PhysicsDrop::PhysicsDrop(QObject *parent)
	: QObject(parent)
{
	connect(::UnigineEditor::Undo::instance(), &::UnigineEditor::Undo::activity, this, [this]() { gizmo_action_pushed_ = true; });
}

PhysicsDrop::~PhysicsDrop()
{
	cancelNow();
}

bool PhysicsDrop::canFall(const NodePtr &node)
{
	bool found = false;
	walk(node, [&found](const NodePtr &n) { found = found || has_geometry(n); });
	return found;
}

void PhysicsDrop::report(int level, const QString &text) const
{
	log_only(text);
	if (on_message)
		on_message(level, text);
}

void PhysicsDrop::log_only(const QString &text) const
{
	Log::message("Artist Tools, Physics Drop: %s\n", text.toUtf8().constData());
}

void PhysicsDrop::notify() const
{
	if (on_changed)
		on_changed();
}

bool PhysicsDrop::can_begin()
{
	if (running_ || !World::isLoaded())
		return false;
	if (Physics::isEnabled())
	{
		report(MSG_WARNING, uiText("The physics of the editor is on. Turn it off with its button on the toolbar first.",
			"Включена физика редактора. Сначала выключите её кнопкой на панели инструментов."));
		return false;
	}
	return true;
}

bool PhysicsDrop::beginSession(const Options &options)
{
	if (!can_begin())
		return false;

	options_ = options;
	session_ = true;
	falling_nodes_.clear();
	prepared_objects_.clear();
	surfaces_.clear();
	bodies_.clear();
	detached_bodies_.clear();
	items_.clear();
	item_of_node_.clear();
	gizmo_seen_.clear();
	mesh_shapes_.clear();
	told_shapes_.clear();

	begin_world();
	notify();
	return true;
}

bool PhysicsDrop::start(const Vector<NodePtr> &nodes, const Options &options)
{
	if (!can_begin())
		return false;

	options_ = options;
	session_ = false;

	// A node selected together with its parent falls with it.
	Vector<NodePtr> roots;
	for (const NodePtr &node : nodes)
	{
		if (!isNodeAlive(node) || isEditorInternal(node))
			continue;
		bool inside_other = false;
		for (const NodePtr &other : nodes)
			inside_other = inside_other || (other != node && isNodeAlive(other) && isInside(node, other));
		if (!inside_other)
			roots.append(node);
	}

	falling_nodes_.clear();
	prepared_objects_.clear();
	surfaces_.clear();
	bodies_.clear();
	detached_bodies_.clear();
	items_.clear();
	item_of_node_.clear();
	mesh_shapes_.clear();

	QVector<NodePtr> falling;
	int skipped = 0;
	for (const NodePtr &root : roots)
	{
		if (!canFall(root))
		{
			++skipped;
			continue;
		}
		falling.append(root);
		walk(root, [this](const NodePtr &n) { falling_nodes_.insert(n->getID()); });
	}
	if (falling.isEmpty())
	{
		report(MSG_WARNING, uiText("Nothing to drop: select objects with meshes.", "Нечего ронять: выделите объекты с мешами."));
		return false;
	}

	// All that is changed in the world is written down first: if something goes wrong below,
	// end() puts it back.
	begin_world();

	// The objects do not fall on themselves: they fall through their own surfaces, as the
	// stand-ins are not their part.
	for (const NodePtr &root : falling)
	{
		walk(root, [this](const NodePtr &n) {
			if (const ObjectPtr object = checked_ptr_cast<Object>(n))
				prepare_object(object);
		});
	}

	QHash<int, int> kinds;
	bool list_all = falling.size() <= 50;
	for (const NodePtr &root : falling)
	{
		Item item;
		ShapeResult result;
		if (!build_item(root, item, result))
		{
			++skipped;
			continue;
		}
		item.shape_kind = result.kind;
		register_item(item);
		++kinds[result.flat ? -1 : result.kind];
		if (list_all)
		{
			QString line = QString::fromUtf8("%1: %2").arg(item.name, shape_name(result.kind, result.flat));
			if (result.extra > 0.0)
				line += uiText(" (empty space %1%)", " (лишний объём %1%)").arg(int(result.extra * 100.0 + 0.5));
			report(MSG_INFO, line);
		}
	}

	if (items_.isEmpty())
	{
		end(END_RETURN, uiText("Could not make a shape for any of the objects.", "Не удалось построить форму ни для одного объекта."), MSG_WARNING);
		return false;
	}

	if (!list_all)
	{
		QStringList parts;
		for (auto it = kinds.constBegin(); it != kinds.constEnd(); ++it)
			parts.append(QString::fromUtf8("%1: %2").arg(shape_name(it.key() < 0 ? SHAPE_BOX : it.key(), it.key() < 0)).arg(it.value()));
		report(MSG_INFO, parts.join(QString::fromUtf8(", ")));
	}
	if (skipped > 0)
		report(MSG_WARNING, uiText("Skipped, nothing to fall in them: %1.", "Пропущено, в них нечему падать: %1.").arg(skipped));

	// What is out of the dome waits for it.
	int outside = 0;
	for (Item &item : items_)
	{
		if (is_in_dome(item))
			continue;
		item.waiting = true;
		set_parked(item, true);
		++outside;
	}
	if (outside == items_.size())
	{
		end(END_RETURN, uiText("The selected objects are out of the dome: fly closer to them or make the dome bigger.",
			"Выделенные объекты вне купола: подлетите к ним или увеличьте купол."), MSG_WARNING);
		return false;
	}
	if (outside > 0)
		report(MSG_WARNING, uiText("Out of the dome: %1. They wait until the dome gets to them.",
			"Вне купола: %1. Они ждут, пока купол до них дойдёт.").arg(outside));

	prepare_surroundings();
	rebuild_stepped();

	notify();
	return true;
}

void PhysicsDrop::begin_world()
{
	hold_other_bodies();

	convex_source_ = ObjectMeshDynamic::create();
	convex_source_->setName("artist_tool_physics_drop_source");
	convex_source_->setShowInEditorEnabled(false);
	convex_source_->setSaveToWorldEnabled(false);
	convex_source_->addSurface("source");
	// It is only read from: nothing falls on it.
	convex_source_->setEnabled(false);

	// Terrain is under everything, whatever the box around the objects is.
	Vector<NodePtr> terrains;
	World::getNodesByType(Node::OBJECT_LANDSCAPE_TERRAIN, terrains);
	for (const NodePtr &terrain : terrains)
		prepare_object(checked_ptr_cast<Object>(terrain));

	// The engine goes through all that can be collided with around the camera, before every step,
	// to find the bodies to step. With nothing around it finds none and only gets told which ones:
	// the size of the world costs nothing, wherever the camera is. Measured with 20000 objects that
	// can be collided with: 1.05 ms a step against 1.45 ms for the same pile of 300.
	saved_physics_distance_ = Physics::getDistance();
	Physics::setDistance(0.0f);
	stepped_nodes_.clear();
	stepped_dirty_ = true;
	num_parked_ = 0;
	jumps_taken_back_ = 0;
	last_followed_frame_ = Physics::getFrame();
	largest_new_radius_ = 0.0f;
	spawn_rise_ = 0.0f;
	prepared_cells_.clear();
	// Zero would mean that it never goes.
	saved_scale_ = Physics::getScale();
	running_scale_ = saved_scale_ > 0.0f ? saved_scale_ : 1.0f;
	Physics::setScale(running_scale_);
	saved_ifps_ = Physics::getIFps();
	saved_iterations_ = Physics::getNumIterations();
	saved_frozen_frames_ = Physics::getNumFrozenFrames();
	saved_penetration_tolerance_ = Physics::getPenetrationTolerance();
	saved_penetration_factor_ = Physics::getPenetrationFactor();
	Physics::setIFps(1.0f / PHYSICS_RATE);
	Physics::setNumIterations(SOLVER_ITERATIONS);
	Physics::setNumFrozenFrames(FROZEN_FRAMES);
	Physics::setPenetrationTolerance(PENETRATION_TOLERANCE);
	Physics::setPenetrationFactor(PENETRATION_FACTOR);
	// What the engine has now, not what was asked of it.
	log_only(uiText("Physics: %1 steps a second, %2 iterations, sinks up to %3 mm.",
		"Физика: %1 шагов в секунду, итераций %2, допуск проникновения %3 мм.")
		.arg(int(1.0f / Physics::getIFps() + 0.5f)).arg(Physics::getNumIterations())
		.arg(Physics::getPenetrationTolerance() * 1000.0f, 0, 'f', 1));

	saved_show_shapes_ = Physics::getShowShapes();
	saved_show_distance_ = Physics::getShowShapesDistance();
	apply_show_shapes(options_.show_shapes);
	apply_show_distance();
	requestVisualizer();
	update_dome(true);

	running_ = true;
	finish_requested_ = false;
	cancel_requested_ = false;
	elapsed_ = 0.0;
	last_notified_ = 0.0;
	moving_ = 0;
	alive_ = 0;
	held_ = -1;
	user_paused_ = false;
	idle_paused_ = false;
	last_activity_ = 0.0;
	frame_ = 0;
	physics_time_sum_ = 0.0;
	physics_time_peak_ = 0.0;
	physics_time_frames_ = 0;
	timer_.start();

	Engine::get()->getEventSyncBeginFramePhysics().connect(connections_, this, &PhysicsDrop::before_step);
	// The world is saved with the objects on their places, as they will be.
	Engine::get()->getEventEndWorldUpdate().connect(connections_, this, &PhysicsDrop::update);
	World::getEventPreWorldSave().connect(connections_, [this](const char *) {
		end(END_KEEP, uiText("The world is being saved: the objects stay where they are now.",
			"Мир сохраняется: объекты остаются там, где они сейчас."), MSG_WARNING);
		// The editor has already written the file of the physics settings of the world, if it
		// has one, with the values of the tool in it.
		const String settings = World::getPhysicsSettings();
		if (!settings.empty())
			Physics::saveSettings(settings.get(), 1);
	});
	World::getEventPreWorldClear().connect(connections_, [this](const char *) {
		end(END_WORLD_GONE, uiText("The world was closed.", "Мир закрыт."), MSG_WARNING);
	});
	qApp->installEventFilter(this);

	// The engine does not run the physics while the logic of the world is off.
	saved_game_enabled_ = Game::isEnabled();
	Game::setEnabled(true);
	first_physics_frame_ = Physics::getFrame();
	Physics::setEnabled(true);

	// Pressed now, the physics button would reload the world and lose what is not saved, and
	// the logic button would stop the physics.
	const QList<QToolButton *> play_buttons = find_play_buttons();
	if (play_buttons.size() == 3)
	{
		logic_button_ = play_buttons[0];
		physics_button_ = play_buttons[1];
		logic_button_->setEnabled(false);
		physics_button_->setEnabled(false);
	} else
		Log::warning("Artist Tools, Physics Drop: the play buttons of the editor are not found\n");

	// While the physics is on the editor does not ask to save anything when the world is closed
	// or another one is opened, and it saves its settings as they are for the time of the run.
	// So none of this is within reach until it is over.
	main_menu_ = find_main_menu();
	if (main_menu_)
		main_menu_->setEnabled(false);

	// A move of what falls would only leave a step in the history, with a place in the air. A session has
	// its own say about it, see PhysicsBrush::apply_gizmo().
	if (!session_)
	{
		manipulators_were_on_ = ::UnigineEditor::ObjectMode::isManipulatorsEnabled();
		::UnigineEditor::ObjectMode::setManipulatorsEnabled(false);
	}
}

void PhysicsDrop::take_over_node(const NodePtr &root)
{
	walk(root, [this](const NodePtr &n) {
		falling_nodes_.insert(n->getID());
		const ObjectPtr object = checked_ptr_cast<Object>(n);
		if (!object)
			return;
		// Its shapes move with it, and its stand-in would keep pushing itself away from them.
		if (const BodyPtr body = object->getBody())
		{
			remember_body(object);
			body->setEnabled(false);
		}
		// It may have been made solid as a part of the surroundings.
		prepared_objects_.remove(object->getID());
		prepare_object(object);
	});
}

bool PhysicsDrop::spawn(const PlaceableAsset &asset, const Vec3 &center, const quat &rotation, float scale, int stroke)
{
	if (!running_ || !session_)
		return false;
	const NodePtr node = createNode(asset);
	if (!node)
		return false;
	// Not a part of the world until the session is applied.
	node->setShowInEditorEnabledRecursive(false);
	node->setSaveToWorldEnabledRecursive(false);

	Mat4 transform = Mat4(rotation) * Mat4(Math::scale(vec3(scale)));
	transform.setColumn3(3, center);
	node->setWorldTransform(transform);

	take_over_node(node);

	Item item;
	ShapeResult result;
	if (!build_item(node, item, result, &center))
	{
		node.deleteLater();
		return false;
	}
	item.shape_kind = result.kind;
	item.is_new = true;
	item.stroke = stroke;
	item.place_since = elapsed_;
	largest_new_radius_ = std::max(largest_new_radius_, item.size_radius);
	rise_out_of_things(item);
	register_item(item);
	wake();

	if (!told_shapes_.contains(asset.name))
	{
		told_shapes_.insert(asset.name);
		report(MSG_INFO, QString::fromUtf8("%1: %2").arg(asset.name, shape_name(result.kind, result.flat)));
	}
	return true;
}

bool PhysicsDrop::hasRoom(const Vec3 &spot) const
{
	const float radius = largest_new_radius_ > 0.0f ? largest_new_radius_ : SPAWN_FIRST_RADIUS;
	for (const Item &item : items_)
	{
		if (!item.gone && float(length(spot - item.body->getWorldCenterOfMass())) < (radius + item.size_radius) * SPAWN_APART)
			return false;
	}
	return true;
}

void PhysicsDrop::rise_out_of_things(Item &item)
{
	const auto too_deep = [&item]() {
		const float limit = item.size_radius * SPAWN_DEEPEST;
		for (int i = 0; i < item.body->getNumShapes(); ++i)
		{
			Vector<ShapeContactPtr> contacts;
			item.body->getShape(i)->getCollision(contacts, 0.0f);
			for (const ShapeContactPtr &contact : contacts)
			{
				if (contact->getDepth() > limit)
					return true;
			}
		}
		return false;
	};

	const Scalar step = Scalar(item.size_radius * SPAWN_RISE_STEP);
	Mat4 place = item.stand_in->getWorldTransform();
	Scalar risen = Scalar(0);
	for (int i = 0; i < SPAWN_RISE_STEPS && too_deep(); ++i)
	{
		Vec3 position = place.getColumn3(3);
		position.z += step;
		place.setColumn3(3, position);
		item.stand_in->setWorldTransform(place);
		risen += step;
	}
	if (risen <= Scalar(0))
		return;

	item.node->setWorldTransform(place * item.relative);
	item.start_transform = item.node->getWorldTransform();
	item.place = item.body->getWorldCenterOfMass();
	spawn_rise_ = std::max(spawn_rise_, float(risen));
}

float PhysicsDrop::takeSpawnRise()
{
	const float rise = spawn_rise_;
	spawn_rise_ = 0.0f;
	return rise;
}

void PhysicsDrop::prepareArea(const Vec3 &center, float reach)
{
	if (!running_)
		return;
	const Scalar size = Scalar(reach);
	const Vec3 around(size, size, size);
	take_area(WorldBoundBox(center - around, center + around));
}

void PhysicsDrop::remove_item(Item &item)
{
	if (item.gone)
		return;
	const int index = int(&item - items_.data());
	if (held_ == index)
		release();
	item.gone = true;
	--alive_;
	stepped_dirty_ = true;
	item.stand_in.deleteLater();
	if (item.is_new)
		item.node.deleteLater();
}

int PhysicsDrop::register_item(const Item &item)
{
	const int index = items_.size();
	items_.append(item);
	items_[index].written = item.node->getWorldTransform();
	item_of_node_.insert(item.node->getID(), index);
	if (options_.lock_rotation)
		forbid_turning(item.body);
	++alive_;
	stepped_dirty_ = true;
	return index;
}

int PhysicsDrop::add_existing(const NodePtr &root)
{
	take_over_node(root);
	Item item;
	ShapeResult result;
	if (!build_item(root, item, result))
		return -1;
	item.shape_kind = result.kind;
	item.place_since = elapsed_;
	return register_item(item);
}

bool PhysicsDrop::hasNewObjects() const
{
	for (const Item &item : items_)
	{
		if (!item.gone && item.is_new)
			return true;
	}
	return false;
}

bool PhysicsDrop::removeLastStroke()
{
	int last = -1;
	for (const Item &item : items_)
	{
		if (!item.gone && item.is_new)
			last = std::max(last, item.stroke);
	}
	if (last < 0)
		return false;
	for (Item &item : items_)
	{
		if (!item.gone && item.is_new && item.stroke == last)
			remove_item(item);
	}
	wake();
	notify();
	return true;
}

int PhysicsDrop::eraseNew(const Vec3 &center, float radius)
{
	int erased = 0;
	for (Item &item : items_)
	{
		if (item.gone || !item.is_new)
			continue;
		if (float(length(item.body->getWorldCenterOfMass() - center)) <= radius)
		{
			remove_item(item);
			++erased;
		}
	}
	if (erased > 0)
	{
		// What lay on them falls.
		wake();
		for (Item &item : items_)
		{
			if (!item.gone && !item.parked)
				item.body->setFrozen(false);
		}
	}
	return erased;
}

void PhysicsDrop::clearNew()
{
	for (Item &item : items_)
	{
		if (!item.gone && item.is_new)
			remove_item(item);
	}
	wake();
	notify();
}

NodePtr PhysicsDrop::wholeNode(const NodePtr &node)
{
	NodePtr whole = node;
	for (NodePtr n = node; n;)
	{
		if (const NodePtr possessor = n->getPossessor())
		{
			whole = possessor;
			n = possessor;
		} else
			n = n->getParent();
	}
	return whole;
}

void PhysicsDrop::activateZone(const Vec3 &center, float radius, float max_size, const QVector<NodePtr> *only, const NodePtr &clicked)
{
	if (!running_ || !session_)
		return;

	const Scalar size = Scalar(radius);
	const Vec3 around(size, size, size);
	Vector<ObjectPtr> objects;
	World::getIntersection(WorldBoundBox(center - around, center + around), objects);

	// It can be out of the box if its middle is far from where it was clicked.
	if (clicked)
	{
		walk(clicked, [&objects](const NodePtr &part) {
			if (const ObjectPtr object = checked_ptr_cast<Object>(part))
				objects.append(object);
		});
	}

	bool added = false;
	QSet<int> seen;
	for (const ObjectPtr &object : objects)
	{
		if (!object || !has_geometry(object) || isEditorInternal(object))
			continue;
		const NodePtr root = wholeNode(object);
		if (!root || seen.contains(root->getID()))
			continue;
		seen.insert(root->getID());

		if (!root->isShowInEditorEnabled() || root->isImmovable())
			continue;
		// A node that has something falling inside it would carry it away from its stand-in.
		bool taken = false;
		walk(root, [this, &taken](const NodePtr &part) { taken = taken || falling_nodes_.contains(part->getID()); });
		if (taken)
			continue;
		if (only)
		{
			bool chosen = false;
			for (const NodePtr &node : *only)
				chosen = chosen || (node && !node.isDeleted() && isInside(root, node));
			if (!chosen)
				continue;
		}

		// The size of all that is inside it.
		Vec3 low(FAR_AWAY, FAR_AWAY, FAR_AWAY);
		Vec3 high(-FAR_AWAY, -FAR_AWAY, -FAR_AWAY);
		walk(root, [&low, &high](const NodePtr &part) {
			if (!has_geometry(part))
				return;
			const WorldBoundBox box = part->getWorldBoundBox();
			low = min(low, box.minimum);
			high = max(high, box.maximum);
		});
		if (float(length(high - low)) > max_size)
			continue;
		const Vec3 middle = (low + high) * Scalar(0.5);
		if (root != clicked && (float(length(middle - center)) > radius || !isInDome(middle, float(length(high - low)) * 0.5f)))
			continue;

		added = add_existing(root) >= 0 || added;
	}
	prepareArea(center, radius + AREA_MARGIN);
	if (added)
		wake();
}

int PhysicsDrop::findItem(const NodePtr &node) const
{
	if (!node)
		return -1;
	for (int i = 0; i < items_.size(); ++i)
	{
		if (!items_[i].gone && isInside(node, items_[i].node))
			return i;
	}
	return -1;
}

QVector<Vec3> PhysicsDrop::getGizmoTargets() const
{
	QVector<Vec3> targets;
	if (gizmo_holding_)
	{
		for (const Item &item : items_)
		{
			if (!item.gone && item.gizmo_held)
				targets.append(item.gizmo_target.getColumn3(3));
		}
	}
	return targets;
}

void PhysicsDrop::update_gizmo()
{
	if (!session_)
		return;

	QVector<NodePtr> selected;
	if (gizmo_enabled_)
	{
		if (const ::UnigineEditor::SelectorNodes *selector = ::UnigineEditor::Selection::getSelectorNodes())
		{
			const Vector<NodePtr> nodes = selector->getNodes();
			for (const NodePtr &node : nodes)
			{
				if (isNodeAlive(node) && !isEditorInternal(node))
					selected.append(node);
			}
		}
	}

	if (!(QApplication::mouseButtons() & Qt::LeftButton))
	{
		release_gizmo_items();
		if (gizmo_undo_open_ && (gizmo_action_pushed_ || ++gizmo_undo_wait_ > GIZMO_UNDO_WAIT_FRAMES))
			close_gizmo_undo();
		gizmo_seen_.clear();
		for (const NodePtr &node : selected)
			gizmo_seen_.insert(node->getID(), node->getWorldTransform());
		return;
	}

	const auto open_undo = [this]() {
		if (gizmo_undo_open_)
			return;
		::UnigineEditor::Undo::begin();
		gizmo_undo_open_ = true;
		gizmo_action_pushed_ = false;
		gizmo_undo_wait_ = 0;
	};

	for (const NodePtr &node : selected)
	{
		const auto known_item = item_of_node_.constFind(node->getID());
		int index = known_item != item_of_node_.constEnd() && !items_[known_item.value()].gone ? known_item.value() : -1;

		const Mat4 current = node->getWorldTransform();
		const Mat4 known = index >= 0 ? items_[index].written : gizmo_seen_.value(node->getID(), current);
		if (!matrices_differ(current, known))
			continue;

		if (index < 0)
		{
			// It is what the user has taken, so its size and flags do not matter here. It has to be
			// a thing of its own, though: what has a part of a moving object in it or is such a part
			// would pull that object away from its stand-in.
			bool can_take = canFall(node);
			walk(node, [this, &can_take](const NodePtr &part) { can_take = can_take && !falling_nodes_.contains(part->getID()); });
			for (const NodePtr &other : selected)
				can_take = can_take && (other == node || !isInside(node, other));
			if (can_take)
			{
				// From where it was, so that this is what it goes back to.
				node->setWorldTransform(known);
				index = add_existing(node);
			}
			if (index < 0)
			{
				// Its step of the history is taken away as well: Cancel could not put it back.
				open_undo();
				gizmo_refused_ = true;
				continue;
			}
		}

		open_undo();

		Item &item = items_[index];
		item.gizmo_target = current * inverse(item.relative);
		if (!item.gizmo_held)
		{
			item.gizmo_held = true;
			item.waiting = false;
			set_parked(item, false);
			item.body->setGravity(false);
			item.body->setAngularDamping(HELD_ANGULAR_DAMPING);
			gizmo_holding_ = true;
		}
	}
}

void PhysicsDrop::steer_gizmo_items()
{
	if (!gizmo_holding_ || user_paused_)
		return;

	for (Item &item : items_)
	{
		if (item.gone || !item.gizmo_held)
			continue;

		vec3 velocity = vec3(item.gizmo_target.getColumn3(3) - item.stand_in->getWorldPosition()) * HELD_GAIN;
		const float speed = length(velocity);
		if (speed > HELD_MAX_SPEED)
			velocity *= HELD_MAX_SPEED / speed;
		item.body->setFrozen(false);
		item.body->setLinearVelocity(velocity);

		if (options_.lock_rotation)
			continue;
		// What is hit does not turn it either: it keeps the way the manipulator holds it.
		quat turn = item.gizmo_target.getRotate() * inverse(item.stand_in->getWorldRotation());
		if (turn.w < 0.0f)
			turn = -turn;
		const vec3 axis(turn.x, turn.y, turn.z);
		const float sine = length(axis);
		vec3 angular(0.0f, 0.0f, 0.0f);
		if (sine > 1e-5f)
			angular = axis * (2.0f * std::atan2(sine, turn.w) / sine * HELD_GAIN);
		const float turn_speed = length(angular);
		if (turn_speed > GIZMO_MAX_TURN)
			angular *= GIZMO_MAX_TURN / turn_speed;
		item.body->setAngularVelocity(angular);
	}
	wake();
}

void PhysicsDrop::release_gizmo_items()
{
	if (!gizmo_holding_)
		return;
	gizmo_holding_ = false;
	for (Item &item : items_)
	{
		if (!item.gizmo_held)
			continue;
		item.gizmo_held = false;
		if (item.gone)
			continue;
		item.body->setGravity(true);
		item.body->setAngularDamping(ANGULAR_DAMPING);
		item.body->setFrozen(false);
		item.place_since = elapsed_;
	}
	wake();
}

void PhysicsDrop::close_gizmo_undo()
{
	gizmo_undo_open_ = false;
	// The objects get back where it took them from, for a moment: the stand-ins are in charge of them.
	::UnigineEditor::Undo::rollback();
	if (gizmo_refused_)
	{
		gizmo_refused_ = false;
		report(MSG_WARNING, uiText("What is selected can not take part in the physics: it went back. Move it after applying.",
			"Выделенное не может участвовать в физике и вернулось на место. Подвиньте его после применения."));
	}
}

void PhysicsDrop::grab(int index, const Vec3 &point)
{
	if (index < 0 || index >= items_.size() || items_[index].gone)
		return;
	release();
	held_ = index;
	Item &item = items_[index];
	set_parked(item, false);
	held_offset_ = vec3(point - item.body->getWorldCenterOfMass());
	held_target_ = point;
	held_damping_ = item.body->getAngularDamping();
	item.body->setGravity(false);
	item.body->setAngularDamping(HELD_ANGULAR_DAMPING);
	item.body->setFrozen(false);
	wake();
}

void PhysicsDrop::setGrabTarget(const Vec3 &target)
{
	held_target_ = target;
}

void PhysicsDrop::release()
{
	if (held_ < 0)
		return;
	Item &item = items_[held_];
	held_ = -1;
	if (item.gone)
		return;
	item.body->setGravity(true);
	item.body->setAngularDamping(held_damping_);
	item.body->setFrozen(false);
	wake();
}

void PhysicsDrop::update_hold()
{
	if (held_ < 0 || user_paused_)
		return;
	Item &item = items_[held_];
	if (item.gone)
	{
		held_ = -1;
		return;
	}
	vec3 velocity = vec3(held_target_ - item.body->getWorldCenterOfMass()) - held_offset_;
	velocity *= HELD_GAIN;
	const float speed = length(velocity);
	if (speed > HELD_MAX_SPEED)
		velocity *= HELD_MAX_SPEED / speed;
	item.body->setFrozen(false);
	item.body->setLinearVelocity(velocity);
	last_activity_ = elapsed_;
}

void PhysicsDrop::applyForce(const Vec3 &center, float radius, float strength, bool pull, float ifps)
{
	bool any = false;
	for (Item &item : items_)
	{
		if (item.gone || item.parked)
			continue;
		const vec3 away = vec3(item.body->getWorldCenterOfMass() - center);
		const float distance = length(away);
		if (distance > radius || distance < 1e-4f)
			continue;
		const float t = distance / radius;
		// A push lifts a little, so that it does not press things into each other.
		vec3 direction = away * (1.0f / distance);
		if (pull)
			direction = -direction;
		else
			direction.z = std::max(direction.z, 0.15f);
		vec3 velocity = item.body->getLinearVelocity() + direction * (strength * (1.0f - t * t) * ifps);
		const float speed = length(velocity);
		if (speed > HELD_MAX_SPEED)
			velocity *= HELD_MAX_SPEED / speed;
		item.body->setFrozen(false);
		item.body->setLinearVelocity(velocity);
		any = true;
	}
	if (any)
		wake();
}

void PhysicsDrop::carryZone(const Vec3 &center, float radius, const vec3 &velocity, float ifps)
{
	// How soft the zone is depends on the speed it asks for, not on how hard it asks: with the full
	// speed asked of all in the zone, however weakly at the edge, what lay near the edge still went 2.25
	// of 3 units: friction does not hold against a steady pull. Measured with a wide layer of objects,
	// how far they go from the middle of the way of the zone to its edge: 2.71 2.63 1.94 0.43 0.03. A
	// pile of 120 carried 3 units goes 2.58, 6 stay behind, it spreads from 0.7 to 1.1, none goes through
	// the floor. With a smaller core it is softer still, but a pile falls behind the zone and is lost.
	const float grip = 1.0f - std::exp(-ifps * CARRY_GRIP_SPEED);
	bool any = false;
	for (Item &item : items_)
	{
		if (item.gone || item.parked)
			continue;
		const float distance = float(length(item.body->getWorldCenterOfMass() - center));
		if (distance > radius)
			continue;
		const float strength = carry_strength(distance / radius);
		vec3 now = item.body->getLinearVelocity();
		now.x += (velocity.x * strength - now.x) * grip;
		now.y += (velocity.y * strength - now.y) * grip;
		item.body->setFrozen(false);
		item.body->setLinearVelocity(now);
		any = true;
	}
	if (any)
		wake();
}

void PhysicsDrop::stopZone(const Vec3 &center, float radius)
{
	for (Item &item : items_)
	{
		if (item.gone || item.parked)
			continue;
		const float distance = float(length(item.body->getWorldCenterOfMass() - center));
		if (distance > radius)
			continue;
		const float keep = 1.0f - carry_strength(distance / radius);
		vec3 now = item.body->getLinearVelocity();
		now.x *= keep;
		now.y *= keep;
		item.body->setFrozen(false);
		item.body->setLinearVelocity(now);
	}
	wake();
}

void PhysicsDrop::jitter()
{
	for (Item &item : items_)
	{
		if (item.gone || item.parked)
			continue;
		const auto random = []() { return float(QRandomGenerator::global()->bounded(2.0) - 1.0); };
		item.body->setFrozen(false);
		item.body->setLinearVelocity(item.body->getLinearVelocity() + vec3(random(), random(), std::abs(random())) * JITTER_SPEED);
		item.body->setAngularVelocity(item.body->getAngularVelocity() + vec3(random(), random(), random()) * JITTER_SPEED * 4.0f);
	}
	wake();
}

void PhysicsDrop::wake()
{
	last_activity_ = elapsed_;
	if (idle_paused_)
	{
		idle_paused_ = false;
		stepped_dirty_ = true;
		if (!user_paused_)
			Physics::setScale(running_scale_);
	}
}

void PhysicsDrop::setUserPaused(bool paused)
{
	if (!running_ || user_paused_ == paused)
		return;
	user_paused_ = paused;
	stepped_dirty_ = true;
	Physics::setScale(paused || idle_paused_ ? 0.0f : running_scale_);
	notify();
}

// When nothing moves and nothing is being done, the physics has nothing to do: it stands still
// until the next thing is done. Then what is left of the shiver of a pile stops too.
void PhysicsDrop::update_pause()
{
	if (idle_paused_ || user_paused_ || held_ >= 0 || gizmo_holding_)
		return;
	if (moving_ == 0 && elapsed_ - last_activity_ >= SETTLED_TIME)
	{
		idle_paused_ = true;
		Physics::setScale(0.0f);
	}
}

bool PhysicsDrop::isInDome(const Vec3 &point, float reach) const
{
	return float(length(point - dome_center_)) <= options_.dome_radius + reach;
}

bool PhysicsDrop::is_in_dome(const Item &item) const
{
	return isInDome(item.body->getWorldCenterOfMass(), item.size_radius);
}

void PhysicsDrop::setDomeRadius(float radius)
{
	options_.dome_radius = radius;
	if (running_)
	{
		apply_show_distance();
		measure_ground();
	}
}

void PhysicsDrop::apply_show_distance() const
{
	// The shapes of the world do not matter out of the dome, and they are a lot to draw.
	Physics::setShowShapesDistance(std::min(saved_show_distance_, options_.dome_radius * (1.0f + DOME_AHEAD)));
}

void PhysicsDrop::update_dome(bool jump)
{
	Vec3 camera;
	vec3 direction;
	if (!viewportCameraView(camera, direction))
		return;

	const float radius = options_.dome_radius;
	const Vec3 target = camera + Vec3(direction * (radius * DOME_AHEAD));
	if (jump)
	{
		dome_center_ = target;
		dome_moving_ = false;
		measure_ground();
		dome_ground_ = dome_ground_target_;
		return;
	}

	const float away = float(length(target - dome_center_));
	if (away > radius * DOME_SLACK)
		dome_moving_ = true;
	else if (away < radius * DOME_CAUGHT_UP)
		dome_moving_ = false;
	const float ifps = std::min(Engine::get()->getIFps(), 0.1f);
	if (dome_moving_)
		dome_center_ += (target - dome_center_) * Scalar(1.0f - std::exp(-ifps * DOME_FOLLOW_SPEED));

	if (float(length(dome_center_ - dome_measured_at_)) > radius * DOME_GROUND_STEP)
		measure_ground();
	dome_ground_ += (dome_ground_target_ - dome_ground_) * double(1.0f - std::exp(-ifps * DOME_GROUND_FOLLOW_SPEED));
}

void PhysicsDrop::measure_ground()
{
	dome_measured_at_ = dome_center_;

	const Scalar radius = Scalar(options_.dome_radius);
	const Scalar spread = radius * Scalar(DOME_GROUND_SPREAD);
	const Scalar zero = Scalar(0);
	const Vec3 up = Vec3(zero, zero, radius);
	const Vec3 places[] = {Vec3(zero, zero, zero), Vec3(spread, zero, zero), Vec3(-spread, zero, zero), Vec3(zero, spread, zero), Vec3(zero, -spread, zero)};

	SurfaceFilter filter = options_.filter;
	filter.look_through = true;
	bool found = false;
	double lowest = 0.0;
	for (const Vec3 &place : places)
	{
		const Vec3 under = dome_center_ + place;
		const SurfaceHit hit = raycastSurface(under + up, under - up, filter, Vector<NodePtr>());
		if (!hit.found)
			continue;
		lowest = found ? std::min(lowest, double(hit.point.z)) : double(hit.point.z);
		found = true;
	}
	// Nothing to stand on: it hangs in the air, and the wall starts from its very bottom.
	dome_ground_target_ = found ? lowest : double(dome_center_.z - radius);
}

void PhysicsDrop::draw_dome() const
{
	if (!options_.show_dome)
		return;
	const float radius = options_.dome_radius;
	drawGlowSphere(dome_center_, radius, dome_ground_, radius * DOME_FADE, DOME_COLOR, DOME_BANDS);
}

void PhysicsDrop::set_parked(Item &item, bool parked)
{
	if (item.parked == parked)
		return;
	item.parked = parked;
	// It has to be immovable, not just left out: measured, a body that is only not stepped comes
	// into the step of a body that touches it, with no contacts of its own, and goes through the
	// floor. As immovable it is a solid thing like the rest of the world, and costs nothing: 1000 of
	// them next to one falling object cost as much as that object alone.
	item.body->setImmovable(parked);
	stepped_dirty_ = true;
	if (parked)
		return;

	item.body->setFrozen(false);
	take_item_area(item);
	if (item.waiting)
	{
		item.waiting = false;
		item.place = item.body->getWorldCenterOfMass();
		item.place_since = elapsed_;
		wake();
	}
}

void PhysicsDrop::update_parking()
{
	const float margin = options_.dome_radius * DOME_PARK_MARGIN;
	for (int i = 0; i < items_.size(); ++i)
	{
		Item &item = items_[i];
		if (item.gone)
			continue;
		if (item.parked)
		{
			if (is_in_dome(item))
				set_parked(item, false);
			continue;
		}
		// What still moves goes on until it has settled, wherever the dome is: it does not stay
		// hanging in the air because the camera has turned away.
		if (i != held_ && !item.gizmo_held && elapsed_ - item.place_since >= SETTLED_TIME
			&& !isInDome(item.body->getWorldCenterOfMass(), item.size_radius + margin))
			set_parked(item, true);
	}
}

void PhysicsDrop::before_step()
{
	if (stepped_dirty_)
		rebuild_stepped();
	Physics::addUpdateNodes(stepped_nodes_);
}

void PhysicsDrop::rebuild_stepped()
{
	stepped_dirty_ = false;
	stepped_nodes_.clear();

	// The engine still asks the world about the very point of the camera, so a body whose bounds
	// hold the camera is found and stepped by it. Told about it as well, the engine would run its
	// contacts twice at the same time, on different threads. So that one is not told about: a body
	// the camera is only near to stands still for as long as the camera is there.
	const PlayerPtr player = Engine::get()->getMainPlayer();
	const Vec3 camera = player ? player->getWorldPosition() : Vec3_zero;
	const auto camera_inside = [&](const Item &item) {
		if (!player)
			return false;
		const Scalar margin = Scalar(CAMERA_INSIDE_MARGIN + length(item.body->getLinearVelocity()) * CAMERA_INSIDE_TIME);
		for (int i = 0; i < item.body->getNumShapes(); ++i)
		{
			const WorldBoundBox box = item.body->getShape(i)->getBoundBox();
			if (camera.x >= box.minimum.x - margin && camera.x <= box.maximum.x + margin
				&& camera.y >= box.minimum.y - margin && camera.y <= box.maximum.y + margin
				&& camera.z >= box.minimum.z - margin && camera.z <= box.maximum.z + margin)
				return true;
		}
		return false;
	};

	for (const Item &item : items_)
	{
		if (!item.gone && !item.parked && !camera_inside(item))
			stepped_nodes_.append(item.stand_in);
	}
}

std::shared_ptr<PhysicsDrop::MeshShape> PhysicsDrop::mesh_shape(const ObjectMeshStaticPtr &mesh)
{
	const QVector<int> surfaces = near_surfaces(mesh);

	QString key = QString::fromUtf8(mesh->getMeshPath());
	if (key.isEmpty())
		key = QString::fromUtf8("#%1").arg(mesh->getID());
	for (int surface : surfaces)
		key += QString::fromUtf8("/%1").arg(surface);

	std::shared_ptr<MeshShape> &shape = mesh_shapes_[key];
	if (shape)
		return shape;
	shape = std::make_shared<MeshShape>();

	const ConstMeshPtr data = mesh->getMeshForceRAM();
	if (!data)
		return shape;

	// As triangles, in the order of their indices: this is what splitting into parts takes.
	for (int surface : surfaces)
	{
		if (surface >= data->getNumSurfaces())
			continue;
		for (int i = 0, num = data->getNumCIndices(surface); i < num; ++i)
			shape->vertices.push_back(data->getVertex(data->getCIndex(i, surface), surface));
	}

	Convex hull;
	if (make_hull(shape->vertices, HULL_ERROR, hull))
	{
		shape->hull = hull_points(hull);
		shape->has_hull = true;
		shape->hull_volume = hull.getVolume();
		shape->hull_triangles = hull_triangles(hull);
	}
	else if (int(shape->vertices.size()) <= MAX_FLAT_POINTS)
		shape->hull = shape->vertices;
	else
		box_corners(mesh->getBoundBox(), shape->hull);
	return shape;
}

bool PhysicsDrop::build_item(const NodePtr &node, Item &item, ShapeResult &result, const Vec3 *center_at)
{
	item.node = node;
	item.name = QString::fromUtf8(node->getName());
	item.start_transform = node->getWorldTransform();

	Mat4 rigid = without_scale(node);
	const Mat4 inverse_rigid = inverse(rigid);
	item.relative = inverse_rigid * item.start_transform;

	// The points of everything inside the node, around the stand-in.
	std::vector<vec3> points;
	std::vector<std::vector<vec3>> pieces;
	// The shapes the object came with: Auto takes them if all its parts have them. The stand-in
	// gets copies; the body they are on stays where it is (a dummy body does not fall, and a
	// real one on the object would drop its scale and its Immovable flag).
	struct ReadyShape
	{
		ShapePtr shape;
		mat4 transform;
	};
	std::vector<ReadyShape> ready_shapes;
	bool all_ready = options_.shape == SHAPE_AUTO && options_.use_own_shapes;
	int num_parts = 0;
	std::shared_ptr<MeshShape> last_mesh;
	mat4 last_mesh_transform = mat4_identity;
	walk(node, [&](const NodePtr &part) {
		if (!has_geometry(part))
			return;
		++num_parts;
		last_mesh = nullptr;
		const ObjectPtr object = checked_ptr_cast<Object>(part);
		const mat4 to_rigid = mat4(inverse_rigid * part->getWorldTransform());
		const mat4 measured = without_hair(to_rigid);

		if (all_ready)
		{
			const size_t before = ready_shapes.size();
			const BodyPtr own_body = object->getBody();
			if (own_body && has_no_scale(to_rigid))
			{
				for (int i = 0; i < own_body->getNumShapes(); ++i)
				{
					const ShapePtr shape = own_body->getShape(i);
					if (shape && shape->isEnabledSelf())
						ready_shapes.push_back({shape, to_rigid * own_body->getShapeTransform(i)});
				}
			}
			all_ready = ready_shapes.size() > before;
		}

		if (const ObjectMeshStaticPtr mesh = checked_ptr_cast<ObjectMeshStatic>(part))
		{
			const std::shared_ptr<MeshShape> shape = mesh_shape(mesh);
			last_mesh = shape;
			last_mesh_transform = measured;
			for (const vec3 &point : shape->hull)
				points.push_back(measured * point);
			if (options_.shape == SHAPE_PIECES)
			{
				if (!shape->has_pieces)
				{
					shape->has_pieces = true;
					Concave concave;
					// What has no hull has no pieces either, and the engine only complains about it.
					if (shape->has_hull && concave.create(shape->vertices.data(), int(shape->vertices.size())))
					{
						for (int i = 0; i < concave.getNumConvexes(); ++i)
							shape->pieces.push_back(hull_points(*concave.getConvex(i)));
					}
				}
				for (const std::vector<vec3> &piece : shape->pieces)
				{
					pieces.emplace_back();
					for (const vec3 &point : piece)
						pieces.back().push_back(measured * point);
				}
			}
			return;
		}

		std::vector<vec3> corners;
		box_corners(object->getBoundBox(), corners);
		for (const vec3 &point : corners)
			points.push_back(measured * point);
	});
	if (points.empty())
		return false;

	for (const vec3 &point : points)
		item.radius = std::max(item.radius, length(point));

	// An object of one mesh has the hull of that mesh, measured once: every copy of it gets it at no
	// cost and with no way to fail. Only what has several parts needs a hull of its own.
	const bool one_mesh = num_parts == 1 && last_mesh && last_mesh->has_hull;
	Convex hull;
	bool has_hull = false;
	double hull_volume = 0.0;
	if (one_mesh)
	{
		const vec3 x = last_mesh_transform.getColumn3(0);
		const vec3 y = last_mesh_transform.getColumn3(1);
		const vec3 z = last_mesh_transform.getColumn3(2);
		hull_volume = last_mesh->hull_volume * std::abs(double(dot(cross(x, y), z)));
		has_hull = hull_volume > MIN_HULL_VOLUME && !looks_flat(points);
	} else
	{
		has_hull = make_hull(points, OBJECT_HULL_ERROR, hull);
		hull_volume = has_hull ? hull.getVolume() : 0.0;
	}
	const std::vector<vec3> outline = has_hull && !one_mesh ? hull_points(hull) : points;
	if (has_hull)
	{
		item.outline = outline;
		item.outline_triangles = one_mesh ? last_mesh->hull_triangles : hull_triangles(hull);
	}

	vec3 middle(0.0f, 0.0f, 0.0f);
	for (const vec3 &point : outline)
		middle += point;
	middle *= 1.0f / float(outline.size());
	for (const vec3 &point : outline)
		item.size_radius = std::max(item.size_radius, length(point - middle));

	vec3 lowest = outline.front();
	vec3 highest = outline.front();
	for (const vec3 &point : outline)
	{
		lowest = min(lowest, point);
		highest = max(highest, point);
	}
	item.thinnest = std::min(std::min(highest.x - lowest.x, highest.y - lowest.y), highest.z - lowest.z);

	if (center_at)
	{
		// The pivot may be far from the mesh: it is the middle of the object that goes there.
		const Vec3 spot = *center_at;
		const Vec3 turned_middle = rigid * Vec3(middle) - rigid.getColumn3(3);
		rigid.setColumn3(3, spot - turned_middle);
		node->setWorldTransform(rigid * item.relative);
		item.start_transform = node->getWorldTransform();
	}

	ObjectDummyPtr stand_in = ObjectDummy::create();
	stand_in->setName("artist_tool_physics_drop");
	stand_in->setShowInEditorEnabled(false);
	stand_in->setSaveToWorldEnabled(false);
	stand_in->setWorldTransform(rigid);

	BodyRigidPtr body = BodyRigid::create(stand_in);
	body->setFreezable(true);
	// Force fields and wind of the world do not touch it.
	body->setPhysicalMask(0);
	body->setMaxLinearVelocity(MAX_LINEAR_SPEED);
	body->setLinearDamping(LINEAR_DAMPING);
	body->setAngularDamping(ANGULAR_DAMPING);
	body->setFrozenLinearVelocity(FROZEN_LINEAR_VELOCITY);
	body->setFrozenAngularVelocity(FROZEN_ANGULAR_VELOCITY);

	item.stand_in = stand_in;
	item.body = body;
	item.place = body->getWorldCenterOfMass();

	const auto use_primitive = [&](PrimitiveFit::Kind kind) {
		const PrimitiveFit fit = fitPrimitive(outline, kind);
		add_primitive(body, fit);
		result.kind = shape_kind_of(kind);
		result.extra = has_hull ? extraVolume(fit, hull_volume) : 0.0;
	};

	if (all_ready)
	{
		for (const ReadyShape &ready : ready_shapes)
		{
			const ShapePtr shape = ready.shape->clone();
			if (!shape)
				continue;
			set_up_shape(shape, shape->getVolume());
			body->addShape(shape, ready.transform);
		}
		if (body->getNumShapes() > 0)
		{
			result.kind = SHAPE_READY;
			return true;
		}
	}

	// What lies in one plane has no hull and no volume to compare with: a thin box.
	if (!has_hull)
	{
		use_primitive(PrimitiveFit::BOX);
		result.flat = true;
		return true;
	}

	switch (options_.shape)
	{
		case SHAPE_BOX: use_primitive(PrimitiveFit::BOX); break;
		case SHAPE_SPHERE: use_primitive(PrimitiveFit::SPHERE); break;
		case SHAPE_CAPSULE: use_primitive(PrimitiveFit::CAPSULE); break;
		case SHAPE_CYLINDER: use_primitive(PrimitiveFit::CYLINDER); break;
		case SHAPE_HULL: add_convex(body, outline, result); break;
		case SHAPE_PIECES:
		{
			result.kind = SHAPE_PIECES;
			for (const std::vector<vec3> &piece : pieces)
				add_convex(body, piece, result);
			if (body->getNumShapes() == 0)
				add_convex(body, outline, result);
			break;
		}
		default:
		{
			PrimitiveFit fit;
			bool fits = false;
			const float even_scale = one_mesh ? even_scale_of(last_mesh_transform) : 0.0f;
			if (even_scale > 0.0f)
			{
				// Many copies of one mesh are poured at a go, and it takes nine fits of hundreds of points.
				if (!last_mesh->has_best_fit)
				{
					last_mesh->has_best_fit = true;
					last_mesh->best_fit_found = chooseBestPrimitive(last_mesh->hull, last_mesh->hull_volume, last_mesh->best_fit);
				}
				fits = last_mesh->best_fit_found;
				if (fits)
					fit = scaled_fit(last_mesh->best_fit, even_scale);
			} else
				fits = chooseBestPrimitive(outline, hull_volume, fit);
			if (fits)
			{
				add_primitive(body, fit);
				result.kind = shape_kind_of(fit.kind);
				result.extra = extraVolume(fit, hull_volume);
			} else
				add_convex(body, outline, result);
			break;
		}
	}

	// A hull the engine would not make: the box always works.
	if (body->getNumShapes() == 0)
		use_primitive(PrimitiveFit::BOX);
	return true;
}

void PhysicsDrop::add_convex(const BodyRigidPtr &body, const std::vector<vec3> &points, ShapeResult &result)
{
	if (points.size() < 4)
		return;

	vec3 center(0.0f, 0.0f, 0.0f);
	for (const vec3 &point : points)
		center += point;
	center *= 1.0f / float(points.size());

	convex_source_->clearVertex();
	convex_source_->allocateVertex(int(points.size()));
	for (const vec3 &point : points)
	{
		const vec3 out = point - center;
		const float distance = length(out);
		convex_source_->addVertex(distance > 0.0f ? point + out * (SHAPE_SKIN / distance) : point);
	}

	ShapeConvexPtr shape = ShapeConvex::create();
	if (!shape->setObject(convex_source_, 0, OBJECT_HULL_ERROR) && !shape->setObject(convex_source_, 0, OBJECT_HULL_ERROR * 25.0f))
		return;
	set_up_shape(shape, shape->getVolume());
	body->addShape(shape);
	if (result.kind != SHAPE_PIECES)
		result.kind = SHAPE_HULL;
}

void PhysicsDrop::add_primitive(const BodyRigidPtr &body, const PrimitiveFit &fit)
{
	ShapePtr shape;
	switch (fit.kind)
	{
		case PrimitiveFit::SPHERE: shape = ShapeSphere::create(fit.radius + SHAPE_SKIN); break;
		case PrimitiveFit::CAPSULE: shape = ShapeCapsule::create(fit.radius + SHAPE_SKIN, fit.height); break;
		case PrimitiveFit::CYLINDER: shape = ShapeCylinder::create(fit.radius + SHAPE_SKIN, fit.height + SHAPE_SKIN * 2.0f); break;
		default:
			shape = ShapeBox::create(fit.size + vec3(SHAPE_SKIN * 2.0f, SHAPE_SKIN * 2.0f, SHAPE_SKIN * 2.0f));
			break;
	}
	set_up_shape(shape, float(fit.volume));
	body->addShape(shape, fit.transform);
}

void PhysicsDrop::set_up_shape(const ShapePtr &shape, float volume) const
{
	// It falls on anything, whatever bits the masks of the surfaces have.
	shape->setCollisionMask(~0);
	shape->setExclusionMask(0);
	shape->setPhysicsIntersectionMask(0);
	shape->setContinuous(true);
	shape->setFriction(options_.friction);
	shape->setRestitution(RESTITUTION);
	shape->setMass(clamp(volume * DENSITY, MIN_MASS, MAX_MASS));
}

void PhysicsDrop::hold_other_bodies()
{
	// The physics is on for the whole world: all that has a body would start moving, and
	// nothing brings it back but a reload of the world. So rigid bodies stand still and are
	// still there to fall on; the others (cloth, ropes, ragdolls) are off for now. The ones
	// inside the falling objects are off too: their objects follow the stand-ins.
	Vector<NodePtr> nodes;
	World::getNodes(nodes, true, false);
	for (const NodePtr &node : nodes)
	{
		if (!node->isObject())
			continue;
		const ObjectPtr object = checked_ptr_cast<Object>(node);
		const BodyPtr body = object->getBody();
		const bool falling = falling_nodes_.contains(node->getID());
		// A dummy body does not move by itself: only its shapes matter, as something to fall
		// on. But the shapes of a falling object must be off, whatever its body is: they move
		// with it, and its stand-in would keep pushing itself away from them for ever.
		if (!body || (body->getType() == Body::BODY_DUMMY && !falling))
			continue;

		remember_body(object);
		if (body->getType() == Body::BODY_RIGID && !falling)
			body->setImmovable(true);
		else
			body->setEnabled(false);
	}
}

void PhysicsDrop::remember_body(const ObjectPtr &object)
{
	const BodyPtr body = object->getBody();
	BodyFlags flags;
	flags.object = object;
	flags.enabled = body->isEnabledSelf();
	flags.immovable = body->isImmovable();
	bodies_.append(flags);
}

void PhysicsDrop::remember_surface(const ObjectPtr &object, int surface)
{
	SurfaceFlags flags;
	flags.object = object;
	flags.surface = surface;
	if (surface < 0)
	{
		flags.collision = checked_ptr_cast<ObjectMeshClutter>(object)->getCollision();
	} else
	{
		flags.collision = object->getCollision(surface);
		flags.mask = object->getCollisionMask(surface);
	}
	surfaces_.append(flags);
}

void PhysicsDrop::prepare_object(const ObjectPtr &object)
{
	if (prepared_objects_.contains(object->getID()))
		return;
	prepared_objects_.insert(object->getID());

	if (isEditorInternal(object))
		return;

	const int type = solid_type_of(object);
	if (type == 0)
		return;

	const bool clutter = object->getType() == Node::OBJECT_MESH_CLUTTER;
	const int num = object->getNumSurfaces();
	const auto turn_off = [&]() {
		if (clutter && checked_ptr_cast<ObjectMeshClutter>(object)->getCollision())
		{
			remember_surface(object, -1);
			checked_ptr_cast<ObjectMeshClutter>(object)->setCollision(false);
		}
		for (int s = 0; s < num; ++s)
		{
			if (!object->getCollision(s))
				continue;
			remember_surface(object, s);
			object->setCollision(false, s);
		}
	};

	// What falls does not fall on itself, and what the filter leaves out is passed through.
	const SurfaceFilter &filter = options_.filter;
	const bool falling = falling_nodes_.contains(object->getID());
	const bool allowed = (type & filter.surface_types) != 0 && (!filter.only_immovable || object->isImmovable());
	if (falling || !allowed)
	{
		turn_off();
		// The bodies of what falls are off already.
		if (!falling && object->getBody() && object->getBody()->isEnabledSelf())
		{
			remember_body(object);
			object->getBody()->setEnabled(false);
		}
		return;
	}

	// An object with a body is solid only by the shapes of the body: the engine does not look
	// at its surfaces. Many imported props have a dummy body with no shapes at all: it comes
	// off for the time of the fall, so that they are solid where they are seen.
	if (const BodyPtr body = object->getBody())
	{
		if (body->getType() != Body::BODY_DUMMY || body->getNumShapes() > 0)
			return;
		DetachedBody detached;
		detached.object = object;
		detached.body = body;
		detached_bodies_.append(detached);
		object->setBody(BodyPtr(), false);
	}

	load_mesh_now(object);

	if (clutter && !checked_ptr_cast<ObjectMeshClutter>(object)->getCollision())
	{
		remember_surface(object, -1);
		checked_ptr_cast<ObjectMeshClutter>(object)->setCollision(true);
	}

	// Surfaces made for collision are what the author wanted to be solid: they are left as
	// they are. An object with none of them is solid where it is seen.
	bool has_collision = false;
	for (int s = 0; s < num; ++s)
	{
		if (object->isEnabled(s) && object->getCollision(s) && object->getCollisionMask(s) != 0)
			has_collision = true;
	}
	if (has_collision)
		return;

	for (int s : near_surfaces(object))
	{
		if (filter.only_intersection && (!object->getIntersection(s) || object->getIntersectionMask(s) == 0))
			continue;
		remember_surface(object, s);
		object->setCollision(true, s);
		if (object->getCollisionMask(s) == 0)
			object->setCollisionMask(1, s);
	}
}

void PhysicsDrop::take_area(const WorldBoundBox &area)
{
	const auto cell_of = [](Scalar value) { return int(std::floor(double(value) / double(AREA_CELL))); };
	const int first_x = cell_of(area.minimum.x);
	const int first_y = cell_of(area.minimum.y);
	const int first_z = cell_of(area.minimum.z);
	const int last_x = cell_of(area.maximum.x);
	const int last_y = cell_of(area.maximum.y);
	const int last_z = cell_of(area.maximum.z);

	const Scalar cell = Scalar(AREA_CELL);
	for (int x = first_x; x <= last_x; ++x)
	{
		for (int y = first_y; y <= last_y; ++y)
		{
			for (int z = first_z; z <= last_z; ++z)
			{
				const quint64 key = (quint64(quint32(x) & 0x1fffff) << 42) | (quint64(quint32(y) & 0x1fffff) << 21) | quint64(quint32(z) & 0x1fffff);
				if (prepared_cells_.contains(key))
					continue;
				prepared_cells_.insert(key);

				const Vec3 low(Scalar(x) * cell, Scalar(y) * cell, Scalar(z) * cell);
				Vector<ObjectPtr> objects;
				World::getIntersection(WorldBoundBox(low, low + Vec3(cell, cell, cell)), objects);
				for (const ObjectPtr &object : objects)
					prepare_object(object);
			}
		}
	}
}

void PhysicsDrop::take_item_area(const Item &item)
{
	const Scalar reach = Scalar(item.radius + AREA_MARGIN + length(item.body->getLinearVelocity()) * AREA_AHEAD);
	const Vec3 position = item.stand_in->getWorldPosition();
	const Vec3 around(reach, reach, reach);
	take_area(WorldBoundBox(position - around, position + around));
}

void PhysicsDrop::prepare_surroundings()
{
	take_moving_area(false);
}

void PhysicsDrop::take_moving_area(bool only_moving)
{
	for (const Item &item : items_)
	{
		if (!item.gone && !item.parked && (!only_moving || !item.body->isFrozen()))
			take_item_area(item);
	}
}

void PhysicsDrop::setShowShapes(bool show)
{
	if (running_)
		apply_show_shapes(show);
}

void PhysicsDrop::apply_show_shapes(bool show)
{
	if (show != visualizer_requested_)
	{
		visualizer_requested_ = show;
		if (show)
			requestVisualizer();
		else
			releaseVisualizer();
	}
	Physics::setShowShapes(show ? Physics::SHOW_TYPE_WIREFRAME : saved_show_shapes_);
}

void PhysicsDrop::finish()
{
	finish_requested_ = true;
}

void PhysicsDrop::cancel()
{
	cancel_requested_ = true;
}

void PhysicsDrop::cancelNow()
{
	end(END_RETURN, QString(), MSG_INFO);
}

bool PhysicsDrop::has_nothing_under(const Item &item) const
{
	const Vec3 center = item.body->getWorldCenterOfMass();
	const Scalar reach = Scalar(item.size_radius);
	const Vec3 low(center.x - reach, center.y - reach, center.z - Scalar(LOST_DEPTH));
	const Vec3 high(center.x + reach, center.y + reach, center.z);
	Vector<ObjectPtr> objects;
	World::getIntersection(WorldBoundBox(low, high), objects);

	const SurfaceFilter &filter = options_.filter;
	for (const ObjectPtr &object : objects)
	{
		if (!object || falling_nodes_.contains(object->getID()) || isEditorInternal(object))
			continue;
		const int type = solid_type_of(object);
		const bool allowed = (type & filter.surface_types) != 0 && (!filter.only_immovable || object->isImmovable());
		// As it is made solid: by the shapes of its body, see hold_other_bodies() and prepare_object(), by
		// the surfaces made for collision, or where it is seen.
		const BodyPtr body = object->getBody();
		if (body && body->isEnabled() && body->getNumShapes() > 0 && (type == 0 || allowed))
			return false;
		if (!allowed)
			continue;
		if (!filter.only_intersection)
			return false;
		for (int s = 0; s < object->getNumSurfaces(); ++s)
		{
			if ((object->getCollision(s) && object->getCollisionMask(s) != 0)
				|| (object->getIntersection(s) && object->getIntersectionMask(s) != 0))
				return false;
		}
	}

	// What is nothing but shapes has no surfaces for the world to know its bounds by. The stand-ins do not
	// take part in this: their masks are 0.
	return !Physics::getIntersection(center, Vec3(center.x, center.y, center.z - Scalar(LOST_DEPTH)), ~0, PhysicsIntersection::create());
}

void PhysicsDrop::lose_item(Item &item)
{
	const Vec3 position = item.stand_in->getWorldPosition();
	item.lost = true;

	// Something over it: it has gone through that. Nothing: it has gone past the edge of it.
	SurfaceFilter filter = options_.filter;
	filter.look_through = true;
	Vec3 above = position;
	above.z = std::max(Scalar(item.start_transform.getColumn3(3).z), Scalar(position.z + Scalar(1)));
	item.through_something = raycastSurface(above, position, filter, Vector<NodePtr>()).found;

	// What was in the world is out of sight from now on. It goes away when this is applied, as a part
	// of the step of the history, and is back if this is canceled.
	if (!item.is_new)
	{
		item.node->setWorldTransform(item.start_transform);
		item.was_enabled = item.node->isEnabledSelf();
		item.node->setEnabled(false);
	}
	remove_item(item);
}

void PhysicsDrop::take_back_jump(Item &item, float frame_time)
{
	const Vec3 center = item.body->getWorldCenterOfMass();
	if (!item.has_last_place)
	{
		item.has_last_place = true;
		item.last_center = center;
		item.last_stand_in = item.stand_in->getWorldTransform();
		return;
	}

	const float speed = length(item.body->getLinearVelocity());
	const float limit = std::max(JUMP_LEAST, item.size_radius * JUMP_OF_SIZE) + speed * frame_time * JUMP_OF_SPEED;
	const float moved = float(length(center - item.last_center));
	if (moved <= limit)
	{
		item.last_center = center;
		item.last_stand_in = item.stand_in->getWorldTransform();
		return;
	}

	++jumps_taken_back_;
	// It starts from rest: what has thrown it has nothing to go on with.
	item.stand_in->setWorldTransform(item.last_stand_in);
}

void PhysicsDrop::follow_stand_ins()
{
	const int physics_frame = Physics::getFrame();
	const float frame_time = float(std::max(physics_frame - last_followed_frame_, 1)) * Physics::getIFps();
	last_followed_frame_ = physics_frame;
	const float continuous_of_thinnest = CONTINUOUS_OF_THINNEST / Physics::getIFps();

	int moving = 0;
	int parked = 0;
	for (Item &item : items_)
	{
		if (item.gone)
			continue;
		if (!isNodeAlive(item.node))
		{
			item.is_new = false;
			remove_item(item);
			continue;
		}
		if (item.parked)
		{
			++parked;
			continue;
		}

		take_back_jump(item, frame_time);
		const Mat4 transform = item.stand_in->getWorldTransform();
		const int index = int(&item - items_.data());
		const bool to_look_at = (frame_ + index) % LOST_CHECK_FRAMES == 0 && index != held_ && !item.gizmo_held
			&& item.body->getLinearVelocity().z < -LOST_CHECK_SPEED;
		if (transform.getColumn3(3).z < item.start_transform.getColumn3(3).z - LOST_DEPTH || (to_look_at && has_nothing_under(item)))
		{
			lose_item(item);
			continue;
		}

		// A settled pile is thousands of objects that have not moved. What the manipulator has moved is
		// not what was written, so it gets its place back all the same.
		item.written = transform * item.relative;
		if (matrices_differ(item.written, item.node->getWorldTransform()))
			item.node->setWorldTransform(item.written);

		// Not the pivot: it may be far from the mesh, and it swings when the object only turns.
		const Vec3 center = item.body->getWorldCenterOfMass();
		if (float(length(center - item.place)) > SETTLED_DISTANCE)
		{
			item.place = center;
			item.place_since = elapsed_;
		}
		if (elapsed_ - item.place_since < SETTLED_TIME)
			++moving;

		const float speed = length(item.body->getLinearVelocity()) + length(item.body->getAngularVelocity()) * item.radius;
		const bool continuous = speed > std::max(CONTINUOUS_SPEED, item.thinnest * continuous_of_thinnest);
		if (continuous != item.continuous)
		{
			item.continuous = continuous;
			for (int i = 0; i < item.body->getNumShapes(); ++i)
				item.body->getShape(i)->setContinuous(continuous);
		}
	}
	moving_ = moving;
	num_parked_ = parked;
}

void PhysicsDrop::update()
{
	if (!running_)
		return;

	if (cancel_requested_)
	{
		end(END_RETURN, uiText("Canceled: the objects are back on their places.", "Отменено: объекты вернулись на свои места."), MSG_SUCCESS);
		return;
	}
	if (!Physics::isEnabled())
	{
		end(END_RETURN, uiText("The physics was turned off from outside: the objects are back on their places.",
			"Физику выключили снаружи: объекты вернулись на свои места."), MSG_WARNING);
		return;
	}

	// The engine does not run the physics without it; a hotkey of the editor may have turned it off.
	if (!Game::isEnabled())
		Game::setEnabled(true);

	// In seconds of the fall, not real ones: the steps may go slower than real time.
	elapsed_ =(Physics::getFrame() - first_physics_frame_) * double(Physics::getIFps());
	// Before the nodes get the places of the stand-ins: until then they have what it gave them.
	update_gizmo();
	follow_stand_ins();

	const double physics_time = Physics::getTotalTime();
	physics_time_sum_ += physics_time;
	physics_time_peak_ = std::max(physics_time_peak_, physics_time);
	++physics_time_frames_;

	if (finish_requested_)
	{
		end(END_KEEP, session_ ? uiText("Applied.", "Применено.") : uiText("Stopped: the objects stay where they are.", "Остановлено: объекты остаются там, где они сейчас."), MSG_SUCCESS);
		return;
	}
	update_hold();
	steer_gizmo_items();
	update_dome(false);
	draw_dome();
	const bool every_few_frames = ++frame_ % AREA_FRAMES == 0;
	if (every_few_frames)
		update_parking();
	// At rest nothing is stepped; whatever changes that makes it to be looked at again.
	if (!idle_paused_ && !user_paused_)
		rebuild_stepped();
	if (session_)
	{
		// It goes until it is applied.
		update_pause();
		if (every_few_frames)
			take_moving_area(true);
		if (double(timer_.elapsed()) / 1000.0 - last_notified_ >= NOTIFY_PERIOD)
		{
			last_notified_ = double(timer_.elapsed()) / 1000.0;
			notify();
		}
		return;
	}
	if (moving_ == 0)
	{
		end(END_KEEP, uiText("All the objects have settled.", "Все объекты улеглись."), MSG_SUCCESS);
		return;
	}
	if (elapsed_ > options_.max_time)
	{
		// Which ones, to see what does not settle.
		QStringList names;
		for (const Item &item : items_)
		{
			if (names.size() < 10 && !item.gone && elapsed_ - item.place_since < SETTLED_TIME)
				names.append(QString::fromUtf8("%1 (%2)").arg(item.name, shape_name(item.shape_kind, false)));
		}
		end(END_KEEP, uiText("Time is up, still moving: %1. They stay where they are: %2.",
			"Время вышло, ещё двигались: %1. Они остаются там, где сейчас: %2.")
			.arg(moving_).arg(names.join(QString::fromUtf8(", "))), MSG_WARNING);
		return;
	}
	if (timer_.elapsed() > NO_PHYSICS_TIME * 1000.0 && Physics::getFrame() == first_physics_frame_)
	{
		end(END_RETURN, uiText("The physics of the engine does not run. Nothing has moved.",
			"Физика движка не запускается. Ничего не сдвинулось."), MSG_ERROR);
		return;
	}

	// The surroundings of what falls become solid before it gets there.
	if (every_few_frames)
		take_moving_area(true);

	if (elapsed_ - last_notified_ >= NOTIFY_PERIOD)
	{
		last_notified_ = elapsed_;
		notify();
	}
}

void PhysicsDrop::restore_world()
{
	// From the last one back: what was written down twice gets what it had first.
	for (int i = surfaces_.size() - 1; i >= 0; --i)
	{
		const SurfaceFlags &flags = surfaces_[i];
		if (!flags.object || flags.object.isDeleted())
			continue;
		if (flags.surface < 0)
		{
			checked_ptr_cast<ObjectMeshClutter>(flags.object)->setCollision(flags.collision);
			continue;
		}
		flags.object->setCollision(flags.collision, flags.surface);
		flags.object->setCollisionMask(flags.mask, flags.surface);
	}
	surfaces_.clear();

	for (const DetachedBody &detached : detached_bodies_)
	{
		if (detached.object && !detached.object.isDeleted())
			detached.object->setBody(detached.body, false);
	}
	detached_bodies_.clear();

	// From the last one back: a body written down twice gets what it had first.
	for (int i = bodies_.size() - 1; i >= 0; --i)
	{
		const BodyFlags &flags = bodies_[i];
		if (!flags.object || flags.object.isDeleted())
			continue;
		const BodyPtr body = flags.object->getBody();
		if (!body)
			continue;
		body->setImmovable(flags.immovable);
		body->setEnabled(flags.enabled);
	}
	bodies_.clear();

	prepared_objects_.clear();
	prepared_cells_.clear();
	falling_nodes_.clear();
}

void PhysicsDrop::end(EndMode mode, const QString &summary, int level)
{
	if (!running_)
		return;
	running_ = false;

	if (gizmo_undo_open_)
		close_gizmo_undo();
	gizmo_holding_ = false;

	// No more of the physics from here on. This is called between frames of the engine,
	// or from update(): its thread is not working now.
	connections_.disconnectAll();
	qApp->removeEventFilter(this);
	if (held_ >= 0)
		release();
	Physics::setEnabled(false);
	Physics::setScale(saved_scale_);
	Game::setEnabled(saved_game_enabled_);
	if (physics_button_)
		physics_button_->setEnabled(true);
	physics_button_ = nullptr;
	if (logic_button_)
		logic_button_->setEnabled(true);
	logic_button_ = nullptr;
	if (main_menu_)
		main_menu_->setEnabled(true);
	main_menu_ = nullptr;
	if (!session_)
		::UnigineEditor::ObjectMode::setManipulatorsEnabled(manipulators_were_on_);

	apply_show_shapes(false);
	Physics::setShowShapesDistance(saved_show_distance_);
	releaseVisualizer();
	Physics::setDistance(saved_physics_distance_);
	stepped_nodes_.clear();
	const int physics_steps = Physics::getFrame() - first_physics_frame_;
	Physics::setIFps(saved_ifps_);
	Physics::setNumIterations(saved_iterations_);
	Physics::setNumFrozenFrames(saved_frozen_frames_);
	Physics::setPenetrationTolerance(saved_penetration_tolerance_);
	Physics::setPenetrationFactor(saved_penetration_factor_);
	if (jumps_taken_back_ > 0)
		log_only(uiText("Thrown by the physics and put back: %1.", "Выброшены физикой и возвращены: %1.").arg(jumps_taken_back_));

	// What is falling into nothing at this moment does not stay hanging under the floor.
	if (mode == END_KEEP)
	{
		for (Item &item : items_)
		{
			if (!item.gone && !item.parked && isNodeAlive(item.node) && item.body->getLinearVelocity().z < -LOST_CHECK_SPEED && has_nothing_under(item))
				lose_item(item);
		}
	}

	// Where the objects are to stay. For the step of the history they go back first: it
	// takes what they have now as what to return to.
	QVector<Mat4> results;
	results.reserve(items_.size());
	int kept = 0;
	int lost = 0;
	int lost_through = 0;
	Vector<NodePtr> lost_nodes;
	int waited = 0;
	for (Item &item : items_)
	{
		if (item.lost)
		{
			++lost;
			if (item.through_something)
				++lost_through;
			if (!item.is_new && mode != END_WORLD_GONE && isNodeAlive(item.node))
			{
				item.node->setEnabled(item.was_enabled);
				lost_nodes.append(item.node);
			}
		}
		if (!item.gone && item.waiting)
			++waited;
		if (item.gone || mode == END_WORLD_GONE)
		{
			results.append(Mat4_identity);
			continue;
		}
		results.append(item.stand_in->getWorldTransform() * item.relative);
		if (!item.is_new)
			item.node->setWorldTransform(item.start_transform);
		item.stand_in.deleteLater();
	}

	restore_world();

	if (convex_source_ && !convex_source_.isDeleted())
		convex_source_.deleteLater();
	convex_source_ = nullptr;
	mesh_shapes_.clear();

	if (mode == END_KEEP)
	{
		// What goes is taken out before anything is done with it.
		Vector<NodePtr> unseen_nodes;
		remove_unseen(results, unseen_nodes);

		::UnigineEditor::Undo::begin();
		for (int i = 0; i < items_.size(); ++i)
		{
			if (items_[i].gone)
				continue;
			++kept;
			// What the zone took in but what has not really moved stays as it was: it is not a
			// change, and it does not get a thousandth of a shift in the world file.
			if (!items_[i].is_new && !barely_moved(items_[i].start_transform, results[i]))
				::UnigineEditor::Undo::apply(new ::UnigineEditor::SetNodeTransformAction(items_[i].node, results[i]));
		}
		commit_new(results);
		if (lost_nodes.size() > 0)
			::UnigineEditor::Undo::apply(new ::UnigineEditor::RemoveNodesAction(lost_nodes));
		if (unseen_nodes.size() > 0)
			::UnigineEditor::Undo::apply(new ::UnigineEditor::RemoveNodesAction(unseen_nodes));
		::UnigineEditor::Undo::commit();
	} else if (mode == END_RETURN)
	{
		for (Item &item : items_)
		{
			if (!item.gone && item.is_new)
				item.node.deleteLater();
		}
	}

	if (!summary.isEmpty())
		report(level, summary);
	if (mode == END_KEEP && lost > 0)
	{
		QString text = uiText("Fell out of the world and removed: %1 (through a surface: %2, past an edge: %3).",
			"Провалились и удалены: %1 (сквозь поверхность: %2, мимо края: %3).").arg(lost).arg(lost_through).arg(lost - lost_through);
		if (lost_nodes.size() > 0)
			text += uiText(" Ctrl+Z brings back those that were in the world before.", " Те, что были в мире раньше, вернёт Ctrl+Z.");
		report(MSG_WARNING, text);
	}
	if (mode == END_KEEP && waited > 0)
		report(MSG_WARNING, uiText("The dome did not get to them, they stayed where they were: %1.",
			"Купол до них не дошёл, они остались на месте: %1.").arg(waited));
	if (mode == END_KEEP && physics_time_frames_ > 0)
	{
		const double real_time = double(timer_.elapsed()) / 1000.0;
		log_only(uiText("Objects: %1, fall: %2 s, real time: %6 s, %5 physics steps a second. Physics per frame: %3 ms on average, %4 ms at most.",
			"Объектов: %1, падение: %2 с, реальное время: %6 с, шагов физики в секунду: %5. Физика на кадр: в среднем %3 мс, максимум %4 мс.")
			.arg(kept).arg(elapsed_, 0, 'f', 1)
			.arg(physics_time_sum_ / physics_time_frames_ * 1000.0, 0, 'f', 1)
			.arg(physics_time_peak_ * 1000.0, 0, 'f', 1)
			.arg(int(physics_steps / std::max(real_time, 0.001) + 0.5))
			.arg(real_time, 0, 'f', 1));
	}

	items_.clear();
	item_of_node_.clear();
	alive_ = 0;
	session_ = false;
	notify();
}

void PhysicsDrop::remove_unseen(const QVector<Mat4> &results, Vector<NodePtr> &existing)
{
	if (!session_)
		return;
	// It is a lot of drawing: not for nothing.
	bool any_can_go = false;
	for (const Item &item : items_)
	{
		const float least_seen = item.is_new ? options_.least_seen : options_.least_seen_existing;
		any_can_go = any_can_go || (!item.gone && item.outline_triangles && least_seen > 0.0f);
	}
	if (!any_can_go)
		return;

	// All that takes part hides the others. The middle of them all is taken out: the rest is done in
	// floats.
	Vec3 middle(Scalar(0), Scalar(0), Scalar(0));
	int num_solids = 0;
	for (int i = 0; i < items_.size(); ++i)
	{
		if (items_[i].gone || !items_[i].outline_triangles)
			continue;
		middle += results[i].getColumn3(3);
		++num_solids;
	}
	if (num_solids == 0)
		return;
	middle *= Scalar(1.0f / float(num_solids));

	std::vector<VisibilitySolid> solids(size_t(items_.size()));
	for (int i = 0; i < items_.size(); ++i)
	{
		const Item &item = items_[i];
		if (item.gone || !item.outline_triangles)
			continue;
		Mat4 rigid = results[i] * inverse(item.relative);
		rigid.setColumn3(3, rigid.getColumn3(3) - middle);
		const mat4 around_middle(rigid);
		VisibilitySolid &solid = solids[i];
		solid.triangles = item.outline_triangles.get();
		solid.points.reserve(item.outline.size());
		for (const vec3 &point : item.outline)
			solid.points.push_back(around_middle * point);
	}

	const std::vector<float> shares = measureSeenShares(solids, VisibilitySettings());

	int removed = 0;
	int could_go = 0;
	for (int i = 0; i < items_.size(); ++i)
	{
		Item &item = items_[i];
		const float least_seen = item.is_new ? options_.least_seen : options_.least_seen_existing;
		if (item.gone || !item.outline_triangles || least_seen <= 0.0f)
			continue;
		++could_go;
		if (shares[i] > least_seen)
			continue;
		item.gone = true;
		++removed;
		if (item.is_new)
			item.node.deleteLater();
		else if (isNodeAlive(item.node))
			existing.append(item.node);
	}
	if (could_go > 0)
		report(MSG_SUCCESS, uiText("Hidden in the pile and removed: %1 of %2.", "Спрятанных в куче убрано: %1 из %2.").arg(removed).arg(could_go));
}

void PhysicsDrop::commit_new(const QVector<Mat4> &results)
{
	Vec3 low(FAR_AWAY, FAR_AWAY, FAR_AWAY);
	Vec3 high(-FAR_AWAY, -FAR_AWAY, -FAR_AWAY);
	bool any = false;
	for (int i = 0; i < items_.size(); ++i)
	{
		if (items_[i].gone || !items_[i].is_new)
			continue;
		any = true;
		const Vec3 position = results[i].getColumn3(3);
		low = min(low, position);
		high = max(high, position);
	}
	if (!any)
		return;

	NodeDummyPtr group = NodeDummy::create();
	group->setName("physics_pile");
	group->setWorldPosition((low + high) * Scalar(0.5));

	QVector<bool> baked(items_.size(), false);
	if (options_.as_clusters)
	{
		const int clusters = make_clusters(results, group, baked);
		if (clusters > 0)
			report(MSG_INFO, uiText("Baked into clusters: %1.", "Собрано в кластеры: %1.").arg(clusters));
	}
	for (int i = 0; i < items_.size(); ++i)
	{
		Item &item = items_[i];
		if (item.gone || !item.is_new)
			continue;
		if (baked[i])
		{
			item.node.deleteLater();
			continue;
		}
		item.node->setWorldTransform(results[i]);
		item.node->setWorldParent(group);
	}
	group->setShowInEditorEnabledRecursive(true);
	group->setSaveToWorldEnabledRecursive(true);
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(group));
}

int PhysicsDrop::make_clusters(const QVector<Mat4> &results, const NodePtr &group, QVector<bool> &baked)
{
	// A cluster for every mesh with its materials. An object goes there only if it has nothing
	// but static meshes: what else it has would be lost.
	struct Cluster
	{
		ObjectMeshStaticPtr source;
		Vector<Mat4> transforms;
		QVector<int> items;
	};
	QHash<QString, Cluster> clusters;
	for (int i = 0; i < items_.size(); ++i)
	{
		Item &item = items_[i];
		if (item.gone || !item.is_new)
			continue;

		item.node->setWorldTransform(results[i]);
		bool simple = true;
		QVector<ObjectMeshStaticPtr> parts;
		walk(item.node, [&simple, &parts](const NodePtr &part) {
			// What is off would be lost with the node, and in a cluster it would show up: the object stays
			// a node, with all it has.
			if (!part->isEnabled())
			{
				simple = false;
				return;
			}
			if (const ObjectMeshStaticPtr mesh = checked_ptr_cast<ObjectMeshStatic>(part))
				parts.append(mesh);
			else if (part->getType() != Node::NODE_DUMMY && part->getType() != Node::NODE_REFERENCE)
				simple = false;
		});
		if (!simple || parts.isEmpty())
			continue;

		for (const ObjectMeshStaticPtr &part : parts)
		{
			// All a cluster takes from a surface below: those that differ in it can not share one.
			QString key = QString::fromUtf8(part->getMeshPath());
			for (int s = 0; s < part->getNumSurfaces(); ++s)
			{
				key += QString::fromUtf8("/%1 %2 %3 %4 %5 %6 %7").arg(QString::fromUtf8(part->getMaterialGUID(s).makeString().get()))
					.arg(part->isEnabled(s) ? 1 : 0).arg(part->getViewportMask(s)).arg(part->getShadowMask(s))
					.arg(part->getCastShadow(s) ? 1 : 0).arg(part->getCastWorldShadow(s) ? 1 : 0).arg(part->getShadowMode(s));
				key += QString::fromUtf8(" %1 %2 %3 %4 %5 %6 %7 %8").arg(part->getIntersection(s) ? 1 : 0).arg(part->getIntersectionMask(s))
					.arg(part->getCollision(s) ? 1 : 0).arg(part->getCollisionMask(s))
					.arg(part->getMinVisibleDistance(s)).arg(part->getMaxVisibleDistance(s))
					.arg(part->getMinFadeDistance(s)).arg(part->getMaxFadeDistance(s));
			}
			Cluster &cluster = clusters[key];
			cluster.source = part;
			cluster.transforms.append(part->getWorldTransform());
			cluster.items.append(i);
		}
		baked[i] = true;
	}

	// All of them first: an object goes into none if one of its clusters can not be made, or it would
	// be there twice, as a part of a cluster and as itself.
	QHash<QString, ObjectMeshClusterPtr> made_clusters;
	for (auto it = clusters.begin(); it != clusters.end(); ++it)
	{
		ObjectMeshClusterPtr result = ObjectMeshCluster::create(it.value().source->getMeshPath());
		if (result)
		{
			made_clusters.insert(it.key(), result);
			continue;
		}
		for (int i : it.value().items)
			baked[i] = false;
	}

	int made = 0;
	for (auto it = clusters.begin(); it != clusters.end(); ++it)
	{
		const auto made_cluster = made_clusters.constFind(it.key());
		if (made_cluster == made_clusters.constEnd())
			continue;
		Cluster &cluster = it.value();
		ObjectMeshClusterPtr result = made_cluster.value();
		Vector<Mat4> transforms;
		for (int i = 0; i < cluster.items.size(); ++i)
		{
			if (baked[cluster.items[i]])
				transforms.append(cluster.transforms[i]);
		}
		if (transforms.empty())
		{
			result.deleteLater();
			continue;
		}
		result->setName(cluster.source->getName());
		result->setWorldTransform(group->getWorldTransform());
		const int surfaces = std::min(cluster.source->getNumSurfaces(), result->getNumSurfaces());
		for (int s = 0; s < surfaces; ++s)
		{
			result->setMaterial(cluster.source->getMaterial(s), s);
			result->setEnabled(cluster.source->isEnabled(s), s);
			result->setViewportMask(cluster.source->getViewportMask(s), s);
			result->setShadowMask(cluster.source->getShadowMask(s), s);
			result->setCastShadow(cluster.source->getCastShadow(s), s);
			result->setCastWorldShadow(cluster.source->getCastWorldShadow(s), s);
			result->setShadowMode(cluster.source->getShadowMode(s), s);
			result->setIntersection(cluster.source->getIntersection(s), s);
			result->setIntersectionMask(cluster.source->getIntersectionMask(s), s);
			result->setCollision(cluster.source->getCollision(s), s);
			result->setCollisionMask(cluster.source->getCollisionMask(s), s);
			result->setMinVisibleDistance(cluster.source->getMinVisibleDistance(s), s);
			result->setMaxVisibleDistance(cluster.source->getMaxVisibleDistance(s), s);
			result->setMinFadeDistance(cluster.source->getMinFadeDistance(s), s);
			result->setMaxFadeDistance(cluster.source->getMaxFadeDistance(s), s);
		}
		result->createMeshes(transforms);
		result->setWorldParent(group);
		++made;
	}
	return made;
}

bool PhysicsDrop::eventFilter(QObject *watched, QEvent *event)
{
	if (!running_)
		return QObject::eventFilter(watched, event);

	// The editor is being closed. With the physics off it asks to save what is not saved.
	if (event->type() == QEvent::Close && main_menu_ && watched == main_menu_->window())
	{
		cancelNow();
		return QObject::eventFilter(watched, event);
	}

	// Dialogs and menus have all of their keys.
	if ((event->type() != QEvent::KeyPress && event->type() != QEvent::ShortcutOverride)
		|| QApplication::activeModalWidget() || QApplication::activePopupWidget())
		return QObject::eventFilter(watched, event);

	const QKeyEvent *key = static_cast<const QKeyEvent *>(event);
	const int letter = latinKey(key);
	const bool control = (key->modifiers() & Qt::ControlModifier) != 0;
	// A field being typed into keeps what it edits with. The other shortcuts go on past it to the editor
	// with the cursor left in the field, and they must not, see begin_world().
	const bool editing_key = letter == Qt::Key_A || letter == Qt::Key_C || letter == Qt::Key_V || letter == Qt::Key_X
		|| letter == Qt::Key_Y || letter == Qt::Key_Z || letter == Qt::Key_Left || letter == Qt::Key_Right
		|| letter == Qt::Key_Home || letter == Qt::Key_End || letter == Qt::Key_Backspace || letter == Qt::Key_Delete
		|| letter == Qt::Key_Insert;
	if (isTypingInField(watched) && (!control || editing_key))
		return QObject::eventFilter(watched, event);

	const bool modifier_alone = key->key() == Qt::Key_Control || key->key() == Qt::Key_Shift
		|| key->key() == Qt::Key_Alt || key->key() == Qt::Key_Meta;
	// An undo in the middle of it would change what the fall has to put back: it stops
	// the fall instead. Esc does the same. In a session these are the keys of its tool.
	const bool history = control && (letter == Qt::Key_Z || letter == Qt::Key_Y);
	const bool escape = key->key() == Qt::Key_Escape && isEscapeFree(watched);
	const bool stops = !session_ && (history || escape);
	// The shortcuts of the editor are not within reach either, see begin_world().
	const bool shortcut = control && !modifier_alone;
	if (!stops && !shortcut)
		return QObject::eventFilter(watched, event);

	if (event->type() == QEvent::ShortcutOverride)
	{
		event->accept();
		return true;
	}
	if (stops)
		cancel();
	else if (letter == Qt::Key_S)
		report(MSG_WARNING, uiText("Nothing can be saved until this is over: apply it or cancel it first.",
			"Пока это не закончится, сохранить нельзя: сначала примените или отмените."));
	return true;
}

} // namespace ArtistTool
