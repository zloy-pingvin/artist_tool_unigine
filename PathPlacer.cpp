#include "PathPlacer.h"
#include "Localization.h"

#include <editor/UnigineActions.h>
#include <editor/UnigineObjectMode.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>
#include <editor/UnigineUndo.h>

#include <UnigineEngine.h>
#include <UnigineLog.h>
#include <UnigineNodes.h>
#include <UnigineRender.h>
#include <UnigineWorld.h>

#include <QApplication>
#include <QCursor>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>

using namespace Unigine;
using ::UnigineEditor::Selection;
using ::UnigineEditor::SelectorNodes;

namespace ArtistTool
{

namespace
{

const char ROOT_NODE_NAME[] = "path_placer";
const char PATH_NODE_NAME[] = "path";
const char OBJECTS_NODE_NAME[] = "objects";
const char POINT_NODE_NAME[] = "path_point_%1";

// Names of the node data the tool keeps its state in.
const char SETTINGS_DATA[] = "artist_tool_path";
const char CORNER_DATA[] = "artist_tool_corner";
const char LENGTH_DATA[] = "artist_tool_length";
const char SCALE_DATA[] = "artist_tool_scale";
// "1" on an object made from a tail entry of the pattern: it stays the last one on
// the path whatever is added to "objects" after it.
const char TAIL_DATA[] = "artist_tool_tail";

// Pieces every span between two points is cut into. The objects are placed on the
// resulting polyline.
const int CURVE_SUBDIVISIONS = 24;
const float MIN_OBJECT_LENGTH = 0.01f;
// How far ahead the path is looked at to turn an object that takes no length.
const double DIRECTION_PROBE = 0.05;
const int MAX_FILL_OBJECTS = 5000;
const int MAX_PATTERN_COUNT = 999;

// The repeated part of the pattern (tail = false) or its tail (tail = true) as the row
// of entries it is placed in: every entry is there as many times in a row as its
// count says.
QVector<int> pattern_sequence(const QVector<PathPatternItem> &pattern, bool tail)
{
	QVector<int> sequence;
	for (int i = 0; i < pattern.size(); ++i)
	{
		if (pattern[i].tail != tail)
			continue;
		for (int n = 0, count = qBound(1, pattern[i].count, MAX_PATTERN_COUNT); n < count; ++n)
			sequence.append(i);
	}
	return sequence;
}

// There is one tail at most: the first tail entry stays the tail, placed once. It goes
// to the end of the pattern, the order of the rest is kept.
void keep_tails_last(QVector<PathPatternItem> &pattern)
{
	bool has_tail = false;
	for (PathPatternItem &item : pattern)
	{
		if (!item.tail)
			continue;
		if (has_tail)
			item.tail = false;
		else
			item.count = 1;
		has_tail = true;
	}
	std::stable_partition(pattern.begin(), pattern.end(), [](const PathPatternItem &item) { return !item.tail; });
}

// How far above and below the path the ground is searched for.
const double SNAP_UP = 5.0;
const double SNAP_DOWN = 100.0;

// How long the button must be held before the picked point starts to follow the
// mouse. An ordinary click lasts 80-150 ms.
const int DRAG_DELAY_MS = 200;

// The points are drawn as spheres that nearly keep their size on the screen: they
// shrink as the camera comes closer and grow as it moves away, but grow a little
// slower than the distance does, so far points look somewhat smaller than near ones
// and a path seen from afar is not hidden behind them. Up to POINT_FULL_SIZE_DISTANCE
// the radius is POINT_SCREEN_RADIUS of the height of the viewport; further away that
// part goes down as the distance in the power of POINT_DISTANCE_FALLOFF (half the
// size at 32 times the distance), but not below POINT_MIN_SCREEN_SCALE of it.
// A click counts as a click on a point a little outside of its sphere too.
const float POINT_SCREEN_RADIUS = 0.014f;
const float POINT_FULL_SIZE_DISTANCE = 5.0f;
const float POINT_DISTANCE_FALLOFF = 0.2f;
const float POINT_MIN_SCREEN_SCALE = 0.35f;

float point_radius(const Math::Vec3 &position)
{
	float scale = 1.0f;
	const float distance = viewportDistanceTo(position);
	if (distance > POINT_FULL_SIZE_DISTANCE)
		scale = std::pow(POINT_FULL_SIZE_DISTANCE / distance, POINT_DISTANCE_FALLOFF);
	if (scale < POINT_MIN_SCREEN_SCALE)
		scale = POINT_MIN_SCREEN_SCALE;

	return Math::clamp(viewportSizeAt(position, POINT_SCREEN_RADIUS * scale), 0.001f, 10000.0f);
}

const float POINT_PICK_SCALE = 3.6f;

NodePtr find_child(const NodePtr &node, const char *name)
{
	const int index = node->findChild(name);
	return index < 0 ? NodePtr() : node->getChild(index);
}

// A path root is any node that has both a "path" and an "objects" child.
bool is_path_root(const NodePtr &node)
{
	return node->findChild(PATH_NODE_NAME) >= 0 && node->findChild(OBJECTS_NODE_NAME) >= 0;
}

// The root of the path the node is a part of.
NodePtr find_path_root(const NodePtr &node)
{
	for (NodePtr n = node; n; n = n->getParent())
	{
		if (is_path_root(n))
			return n;
	}
	return NodePtr();
}

bool is_tail(const NodePtr &object)
{
	const char *data = object->getData(TAIL_DATA);
	return data && data[0] == '1';
}

// The objects in the order they stand along the path: the order of the "objects"
// node, but with the tail objects after all the rest. So a section added by hand -
// which goes to the end of the list, after the tail - still stands before the tail.
QVector<NodePtr> objects_in_path_order(const NodePtr &objects)
{
	QVector<NodePtr> ordered;
	QVector<NodePtr> tails;
	for (int i = 0, num = objects->getNumChildren(); i < num; ++i)
	{
		const NodePtr object = objects->getChild(i);
		(is_tail(object) ? tails : ordered).append(object);
	}
	ordered += tails;
	return ordered;
}

bool is_corner(const NodePtr &point)
{
	const char *data = point->getData(CORNER_DATA);
	return data && data[0] == '1';
}

int axis_index(AxisDirection axis)
{
	return int(axis) / 2;
}

float axis_sign(AxisDirection axis)
{
	return int(axis) % 2 ? -1.0f : 1.0f;
}

Math::Vec3 bezier(const Math::Vec3 &p0, const Math::Vec3 &p1, const Math::Vec3 &p2, const Math::Vec3 &p3, double t)
{
	const Math::Scalar k0 = Math::Scalar(t);
	const Math::Scalar k1 = Math::Scalar(1.0 - t);
	return p0 * (k1 * k1 * k1) + p1 * (Math::Scalar(3.0) * k1 * k1 * k0)
		+ p2 * (Math::Scalar(3.0) * k0 * k0 * k1) + p3 * (k0 * k0 * k0);
}

// A place on the polyline: between samples[index] and samples[index + 1].
struct Cursor
{
	int index{0};
	double t{0.0};
	Math::Vec3 position;
};

// Moves the cursor forward along the polyline to the first point that is 'distance'
// away from where it stands, in a straight line. False if the polyline ends sooner.
bool advance(const QVector<Math::Vec3> &samples, Cursor &cursor, double distance)
{
	const Math::Vec3 origin = cursor.position;
	for (int k = cursor.index; k + 1 < samples.size(); ++k)
	{
		const Math::Vec3 a = samples[k];
		const Math::Vec3 b = samples[k + 1];
		if (double(Math::length(b - origin)) < distance)
			continue;

		// The point of [a, b] on the sphere of that radius around the origin.
		const Math::Vec3 d = b - a;
		const Math::Vec3 m = a - origin;
		const double qa = double(Math::dot(d, d));
		const double qb = 2.0 * double(Math::dot(d, m));
		const double qc = double(Math::dot(m, m)) - distance * distance;
		const double discriminant = qb * qb - 4.0 * qa * qc;
		if (qa < 1e-12 || discriminant < 0.0)
			continue;

		const double t_min = k == cursor.index ? cursor.t : 0.0;
		double t = (-qb + std::sqrt(discriminant)) / (2.0 * qa);
		t = t < t_min ? t_min : (t > 1.0 ? 1.0 : t);

		cursor.index = k;
		cursor.t = t;
		cursor.position = a + d * Math::Scalar(t);
		return true;
	}
	return false;
}

// The points of a path, in order.
struct PathPoints
{
	QVector<Math::Vec3> positions;
	QVector<bool> corners;
	bool closed{false};

	int count() const { return int(positions.size()); }
	// Spans between neighbouring points: the last one of a closed path goes back to
	// the first point.
	int spans() const { return count() < 2 ? 0 : (closed ? count() : count() - 1); }
};

PathPoints collect_points(const NodePtr &path, bool closed)
{
	PathPoints points;
	const int count = path ? path->getNumChildren() : 0;
	points.positions.resize(count);
	points.corners.resize(count);
	for (int i = 0; i < count; ++i)
	{
		const NodePtr point = path->getChild(i);
		points.positions[i] = point->getWorldPosition();
		points.corners[i] = is_corner(point);
	}
	points.closed = closed && count > 2;
	return points;
}

Math::Vec3 unit_or_zero(const Math::Vec3 &v)
{
	const double length = double(Math::length(v));
	return length > 1e-9 ? v * Math::Scalar(1.0 / length) : Math::Vec3(0.0, 0.0, 0.0);
}

// The direction the curve passes a smooth point in, a unit vector: halfway between
// the direction from the previous point and the direction to the next one, so the
// curve goes through the point without a bend. Zero for a corner point: the curve
// arrives and leaves it straight, so there is a sharp bend.
Math::Vec3 point_tangent(const PathPoints &points, int i)
{
	if (points.corners[i])
		return Math::Vec3(0.0, 0.0, 0.0);

	const int count = points.count();
	int prev = i - 1;
	int next = i + 1;
	if (points.closed)
	{
		prev = (prev + count) % count;
		next = next % count;
	} else
	{
		prev = prev < 0 ? 0 : prev;
		next = next > count - 1 ? count - 1 : next;
	}

	// The two directions count the same however far the neighbours are, so a far
	// neighbour does not pull the curve at a near one its way.
	const Math::Vec3 p = points.positions[i];
	return unit_or_zero(unit_or_zero(p - points.positions[prev]) + unit_or_zero(points.positions[next] - p));
}

// The place 't' (0..1) of the span that starts at the point 'span'.
Math::Vec3 span_point(const PathPoints &points, int span, double t)
{
	const int next = (span + 1) % points.count();
	const Math::Vec3 a = points.positions[span];
	const Math::Vec3 b = points.positions[next];

	// The handles of a span are a third of its own length: a short span between close
	// points gets short handles and a long one long handles, whatever the length of
	// the spans next to it. Handles sized by the neighbours overshoot a short span
	// and make loops and kinks there.
	const Math::Scalar reach = Math::Scalar(double(Math::length(b - a)) / 3.0);
	return bezier(a, a + point_tangent(points, span) * reach, b - point_tangent(points, next) * reach, b, t);
}

void put(QByteArray &bytes, double value)
{
	bytes.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

void put(QByteArray &bytes, const Math::Vec3 &value)
{
	put(bytes, double(value.x));
	put(bytes, double(value.y));
	put(bytes, double(value.z));
}

void put(QByteArray &bytes, const Math::Mat4 &value)
{
	for (int column = 0; column < 4; ++column)
		put(bytes, Math::Vec3(value.getColumn3(column)));
}

} // namespace

PathPlacer::PathPlacer(QObject *parent)
	: QObject(parent)
{
}

PathPlacer::~PathPlacer()
{
	on_changed = nullptr;
	on_message = nullptr;
	setActive(false);
}

void PathPlacer::setActive(bool active)
{
	if (active_ == active)
		return;

	active_ = active;
	if (active_)
	{
		// The curve is drawn with the Visualizer, once per frame.
		saved_visualizer_mode_ = Visualizer::getMode();
		if (saved_visualizer_mode_ == Visualizer::MODE_DISABLED)
			Visualizer::setMode(Visualizer::MODE_ENABLED_DEPTH_TEST_ENABLED);
		Engine::get()->getEventEndWorldUpdate().connect(connections_, this, &PathPlacer::update);
		// Clicks on the points of the path are taken all the time the tool is active.
		qApp->installEventFilter(this);
		updateFromSelection();
	} else
	{
		setAddingPoints(false);
		end_drag();
		pending_selection_ = NodePtr();
		click_taken_ = false;
		swallow_release_ = false;
		qApp->removeEventFilter(this);
		connections_.disconnectAll();
		if (saved_visualizer_mode_ == Visualizer::MODE_DISABLED)
			Visualizer::setMode(Visualizer::MODE_DISABLED);
	}
}

void PathPlacer::updateFromSelection()
{
	if (const SelectorNodes *selector = Selection::getSelectorNodes())
	{
		for (const NodePtr &node : selector->getNodes())
		{
			if (NodePtr root = find_path_root(node))
			{
				set_path(root);
				break;
			}
		}
	}
	notify_changed();
}

void PathPlacer::set_path(const NodePtr &root)
{
	if (hasPath() && root_->getID() == root->getID())
		return;

	setAddingPoints(false);
	root_ = root;
	arranged_state_.clear();
	unplaced_ = 0;
	load_settings();

	// The objects are not touched when a path is merely picked up: they are
	// rearranged when something of the path changes.
	length_ = build_curve().length;
	arranged_state_ = snapshot();
}

QVector<PathInfo> PathPlacer::findPaths() const
{
	// What is inside node references and clutters is not looked into: a path there can
	// not be edited.
	Vector<NodePtr> nodes;
	World::getNodes(nodes, false, false);

	QVector<PathInfo> paths;
	for (const NodePtr &node : nodes)
	{
		if (!node || !is_path_root(node) || isEditorInternal(node))
			continue;

		PathInfo info;
		info.id = node->getID();
		info.name = QString::fromUtf8(node->getName());
		info.points = find_child(node, PATH_NODE_NAME)->getNumChildren();
		info.objects = find_child(node, OBJECTS_NODE_NAME)->getNumChildren();
		paths.append(info);
	}

	std::stable_sort(paths.begin(), paths.end(), [](const PathInfo &a, const PathInfo &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return paths;
}

bool PathPlacer::selectPath(int root_id)
{
	const NodePtr root = World::getNodeByID(root_id);
	if (!root || root.isDeleted() || !is_path_root(root))
		return false;

	set_path(root);

	// The root is selected in the editor too: it is seen in the World Hierarchy and
	// can be framed in the viewport.
	Vector<NodePtr> nodes;
	nodes.append(root);
	::UnigineEditor::SelectionAction::applySelection(new SelectorNodes(nodes));

	report(MSG_INFO, QString());
	notify_changed();
	return true;
}

int PathPlacer::getPathId() const
{
	return hasPath() ? root_->getID() : 0;
}

bool PathPlacer::hasPath() const
{
	return root_ && !root_.isDeleted() && path_node() && objects_node();
}

QString PathPlacer::getPathName() const
{
	return hasPath() ? QString::fromUtf8(root_->getName()) : QString();
}

int PathPlacer::getNumPoints() const
{
	const NodePtr path = path_node();
	return path ? path->getNumChildren() : 0;
}

int PathPlacer::getNumObjects() const
{
	const NodePtr objects = objects_node();
	return objects ? objects->getNumChildren() : 0;
}

NodePtr PathPlacer::path_node() const
{
	return root_ && !root_.isDeleted() ? find_child(root_, PATH_NODE_NAME) : NodePtr();
}

NodePtr PathPlacer::objects_node() const
{
	return root_ && !root_.isDeleted() ? find_child(root_, OBJECTS_NODE_NAME) : NodePtr();
}

NodePtr PathPlacer::old_script_node() const
{
	if (!root_ || root_.isDeleted())
		return NodePtr();

	for (int i = 0, num = root_->getNumChildren(); i < num; ++i)
	{
		NodePtr child = root_->getChild(i);
		if (child->getType() == Node::WORLD_EXPRESSION)
			return child;
	}
	return NodePtr();
}

bool PathPlacer::hasOldScript() const
{
	return bool(old_script_node());
}

void PathPlacer::removeOldScript()
{
	if (NodePtr script = old_script_node())
	{
		::UnigineEditor::Undo::apply(new ::UnigineEditor::RemoveNodesAction(script));
		notify_changed();
	}
}

void PathPlacer::createPath()
{
	setAddingPoints(false);

	NodeDummyPtr root = NodeDummy::create();
	root->setName(ROOT_NODE_NAME);
	NodeDummyPtr path = NodeDummy::create();
	path->setName(PATH_NODE_NAME);
	path->setWorldParent(root);
	NodeDummyPtr objects = NodeDummy::create();
	objects->setName(OBJECTS_NODE_NAME);
	objects->setWorldParent(root);

	root->setShowInEditorEnabledRecursive(true);
	root->setSaveToWorldEnabledRecursive(true);
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(root));

	// The new path starts with the settings and the pattern that are on the page.
	root_ = root;
	arranged_state_.clear();
	length_ = 0.0;
	unplaced_ = 0;
	save_settings();
	arranged_state_ = snapshot();

	Vector<NodePtr> selection;
	selection.append(root);
	::UnigineEditor::SelectionAction::applySelection(new SelectorNodes(selection));

	notify_changed();
}

void PathPlacer::setAddingPoints(bool adding)
{
	if (adding && !hasPath())
	{
		report(MSG_WARNING, uiText("Create a path or select an existing one first.", "Сначала создайте кривую или выделите существующую."));
		adding = false;
	}
	if (adding_points_ == adding)
		return;

	adding_points_ = adding;
	notify_changed();
}

void PathPlacer::setSettings(const PathSettings &settings)
{
	settings_ = settings;

	// The forward and the up axis of an object can not be the same one.
	if (axis_index(settings_.forward_axis) == axis_index(settings_.up_axis))
	{
		settings_.up_axis = axis_index(settings_.forward_axis) == axis_index(AXIS_Z_POSITIVE)
			? AXIS_Y_POSITIVE : AXIS_Z_POSITIVE;
	}
	if (settings_.step < MIN_OBJECT_LENGTH)
		settings_.step = MIN_OBJECT_LENGTH;

	if (hasPath())
		save_settings();
	notify_changed();
}

void PathPlacer::setLiveUpdate(bool live)
{
	if (live_update_ == live)
		return;

	live_update_ = live;
	// Turning it on puts the objects back on the path at the next frame.
	if (live_update_)
		arranged_state_.clear();
	notify_changed();
}

////////////////////////////////////////////////////////////////////////////////
// Settings, kept in the data of the root node as JSON.
////////////////////////////////////////////////////////////////////////////////
void PathPlacer::load_settings()
{
	const char *data = root_->getData(SETTINGS_DATA);
	const QJsonObject json = QJsonDocument::fromJson(QByteArray(data ? data : "")).object();
	if (json.isEmpty())
	{
		// A path that the tool sees for the first time keeps what is on the page. For
		// a setup of the old script its step is taken: the X scale of the script node.
		if (NodePtr script = old_script_node())
		{
			const float step = script->getScale().x;
			if (step >= MIN_OBJECT_LENGTH)
				settings_.step = step;
		}
		return;
	}

	const PathSettings defaults;
	settings_.step = float(json.value("step").toDouble(defaults.step));
	// The axes are kept in range and apart: a damaged record must not send the
	// transform code out of its arrays.
	settings_.forward_axis = AxisDirection(qBound(0, json.value("forward_axis").toInt(defaults.forward_axis), 5));
	settings_.up_axis = AxisDirection(qBound(0, json.value("up_axis").toInt(defaults.up_axis), 5));
	if (axis_index(settings_.forward_axis) == axis_index(settings_.up_axis))
	{
		settings_.up_axis = axis_index(settings_.forward_axis) == axis_index(AXIS_Z_POSITIVE)
			? AXIS_Y_POSITIVE : AXIS_Z_POSITIVE;
	}
	settings_.closed = json.value("closed").toBool(defaults.closed);
	settings_.yaw_only = json.value("yaw_only").toBool(defaults.yaw_only);
	settings_.skew = json.value("skew").toBool(defaults.skew);
	settings_.snap_to_ground = json.value("snap_to_ground").toBool(defaults.snap_to_ground);
	settings_.ground_offset = float(json.value("ground_offset").toDouble(defaults.ground_offset));

	pattern_.clear();
	const QJsonArray pattern = json.value("pattern").toArray();
	for (const QJsonValue &value : pattern)
	{
		const QJsonObject entry = value.toObject();
		PathPatternItem item;
		item.asset.kind = PlaceableAsset::Kind(entry.value("kind").toInt());
		item.asset.path = String(entry.value("path").toString().toUtf8().constData());
		item.asset.asset_path = String(entry.value("asset_path").toString().toUtf8().constData());
		item.asset.name = entry.value("name").toString();
		item.length = float(entry.value("length").toDouble(1.0));
		item.count = qBound(1, entry.value("count").toInt(1), MAX_PATTERN_COUNT);
		item.tail = entry.value("tail").toBool(false);
		if (item.asset.isValid())
			pattern_.append(item);
	}
	keep_tails_last(pattern_);
}

void PathPlacer::save_settings() const
{
	if (!root_ || root_.isDeleted())
		return;

	QJsonObject json;
	json.insert("step", double(settings_.step));
	json.insert("forward_axis", int(settings_.forward_axis));
	json.insert("up_axis", int(settings_.up_axis));
	json.insert("closed", settings_.closed);
	json.insert("yaw_only", settings_.yaw_only);
	json.insert("skew", settings_.skew);
	json.insert("snap_to_ground", settings_.snap_to_ground);
	json.insert("ground_offset", double(settings_.ground_offset));

	QJsonArray pattern;
	for (const PathPatternItem &item : pattern_)
	{
		QJsonObject entry;
		entry.insert("kind", int(item.asset.kind));
		entry.insert("path", QString::fromUtf8(item.asset.path.get()));
		entry.insert("asset_path", QString::fromUtf8(item.asset.asset_path.get()));
		entry.insert("name", item.asset.name);
		entry.insert("length", double(item.length));
		entry.insert("count", item.count);
		entry.insert("tail", item.tail);
		pattern.append(entry);
	}
	json.insert("pattern", pattern);

	root_->setData(SETTINGS_DATA, QJsonDocument(json).toJson(QJsonDocument::Compact).constData());
}

////////////////////////////////////////////////////////////////////////////////
// Points.
////////////////////////////////////////////////////////////////////////////////
Vector<NodePtr> PathPlacer::selected_points() const
{
	Vector<NodePtr> points;
	const NodePtr path = path_node();
	if (!path)
		return points;

	if (const SelectorNodes *selector = Selection::getSelectorNodes())
	{
		for (const NodePtr &node : selector->getNodes())
		{
			const NodePtr parent = node->getParent();
			if (parent && parent->getID() == path->getID())
				points.append(node);
		}
	}
	return points;
}

PathPlacer::PointsState PathPlacer::getSelectedPointsState() const
{
	int smooth = 0;
	int corner = 0;
	for (const NodePtr &point : selected_points())
		++(is_corner(point) ? corner : smooth);

	if (smooth && corner)
		return POINTS_MIXED;
	if (corner)
		return POINTS_CORNER;
	return smooth ? POINTS_SMOOTH : POINTS_NONE;
}

void PathPlacer::setSelectedPointsCorner(bool corner)
{
	for (const NodePtr &point : selected_points())
		point->setData(CORNER_DATA, corner ? "1" : "0");
	notify_changed();
}

bool PathPlacer::canSubdivide() const
{
	const NodePtr path = path_node();
	const Vector<NodePtr> selected = selected_points();
	if (!path || selected.size() < 2)
		return false;

	const int count = path->getNumChildren();
	const int spans = collect_points(path, settings_.closed).spans();
	for (int i = 0; i < spans; ++i)
	{
		const int a = path->getChild(i)->getID();
		const int b = path->getChild((i + 1) % count)->getID();
		bool a_selected = false;
		bool b_selected = false;
		for (const NodePtr &point : selected)
		{
			a_selected = a_selected || point->getID() == a;
			b_selected = b_selected || point->getID() == b;
		}
		if (a_selected && b_selected)
			return true;
	}
	return false;
}

void PathPlacer::subdivide()
{
	const NodePtr path = path_node();
	if (!path)
		return;

	const PathPoints points = collect_points(path, settings_.closed);
	const int count = points.count();
	const Vector<NodePtr> selected = selected_points();
	const auto is_selected = [&](int index) {
		const int id = path->getChild(index)->getID();
		for (const NodePtr &point : selected)
		{
			if (point->getID() == id)
				return true;
		}
		return false;
	};

	// The spans to split, found before any point is added.
	QVector<int> spans;
	for (int i = 0; i < points.spans(); ++i)
	{
		if (is_selected(i) && is_selected((i + 1) % count))
			spans.append(i);
	}
	if (spans.isEmpty())
	{
		report(MSG_WARNING, uiText("Select two neighbouring points of the path to put a point between them.", "Выделите две соседние точки кривой, чтобы поставить точку между ними."));
		return;
	}

	// From the end, so that the indices of the spans that are still to be split stay.
	Vector<NodePtr> created;
	for (int k = int(spans.size()) - 1; k >= 0; --k)
	{
		const int span = spans[k];
		NodeDummyPtr point = NodeDummy::create();
		point->setName(QString::fromUtf8(POINT_NODE_NAME).arg(count + int(created.size())).toUtf8().constData());
		point->setWorldParent(path);
		path->setChildIndex(point, span + 1);
		point->setWorldPosition(span_point(points, span, 0.5));
		point->setShowInEditorEnabledRecursive(true);
		point->setSaveToWorldEnabledRecursive(true);
		created.append(point);
	}
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(created));

	report(MSG_INFO, QString());
	notify_changed();
}

// The point of the path the world ray p0 -> p1 goes through, the closest one.
NodePtr PathPlacer::pick_point(const Math::Vec3 &p0, const Math::Vec3 &p1) const
{
	const NodePtr path = path_node();
	if (!path)
		return NodePtr();

	const Math::Vec3 direction = p1 - p0;
	const double ray_length = double(Math::length(direction));
	if (ray_length < 1e-9)
		return NodePtr();

	// The zone a click picks a point in is much larger than its sphere, so the zones of
	// points that are close on the screen overlap. The point the click is closest to,
	// in parts of its zone, wins: a click right on a point always takes that point.
	NodePtr picked;
	double picked_miss = 0.0;
	for (int i = 0, num = path->getNumChildren(); i < num; ++i)
	{
		const NodePtr point = path->getChild(i);
		const Math::Vec3 position = point->getWorldPosition();

		// The place of the ray closest to the point.
		const double along = double(Math::dot(position - p0, direction)) / ray_length;
		if (along < 0.0 || along > ray_length)
			continue;
		const Math::Vec3 closest = p0 + direction * Math::Scalar(along / ray_length);

		const double radius = double(point_radius(position) * POINT_PICK_SCALE);
		const double miss = double(Math::length(position - closest)) / radius;
		if (miss > 1.0)
			continue;
		if (!picked || miss < picked_miss)
		{
			picked = point;
			picked_miss = miss;
		}
	}
	return picked;
}

// Adds a point: after the selected one, before it if it is the first point of the
// path (the path then grows from its start), at the end if none is selected.
NodePtr PathPlacer::add_point(const Math::Vec3 &position)
{
	const NodePtr path = path_node();
	if (!path)
		return NodePtr();

	const int count = path->getNumChildren();
	int index = count;
	const Vector<NodePtr> selected = selected_points();
	if (selected.size() == 1 && count > 1)
	{
		const int selected_index = path->getChildIndex(selected[0]);
		index = selected_index == 0 ? 0 : selected_index + 1;
	}

	NodeDummyPtr point = NodeDummy::create();
	point->setName(QString::fromUtf8(POINT_NODE_NAME).arg(count).toUtf8().constData());
	point->setWorldParent(path);
	path->setChildIndex(point, index);
	point->setWorldPosition(position);
	point->setShowInEditorEnabledRecursive(true);
	point->setSaveToWorldEnabledRecursive(true);
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(point));
	return point;
}

void PathPlacer::select_point(const NodePtr &point, bool add_to_selection)
{
	Vector<NodePtr> nodes;
	if (add_to_selection)
	{
		if (const SelectorNodes *selector = Selection::getSelectorNodes())
		{
			for (const NodePtr &node : selector->getNodes())
			{
				if (node->getID() != point->getID())
					nodes.append(node);
			}
		}
	}
	nodes.append(point);
	::UnigineEditor::SelectionAction::applySelection(new SelectorNodes(nodes));
}

void PathPlacer::begin_drag(const NodePtr &point, bool is_new)
{
	drag_point_ = point;
	drag_start_transform_ = point->getWorldTransform();
	drag_is_new_ = is_new;
	drag_moving_ = false;
	drag_press_position_ = QCursor::pos();
	drag_timer_.start();

	// The manipulators of the editor sit right on the point; they must not grab it
	// while the tool is moving it.
	if (::UnigineEditor::ObjectMode::isManipulatorsEnabled())
	{
		::UnigineEditor::ObjectMode::setManipulatorsEnabled(false);
		manipulators_disabled_ = true;
	}
}

// Every frame while a point is held: it goes to the surface under the mouse.
void PathPlacer::update_drag()
{
	if (!drag_point_)
		return;
	if (drag_point_.isDeleted() || !(QApplication::mouseButtons() & Qt::LeftButton))
	{
		// The release of the button did not get to the tool (the window lost the mouse):
		// the click is over, and no later release is to be taken for its own.
		click_taken_ = false;
		swallow_release_ = false;
		finish_click();
		return;
	}

	const QPoint mouse = QCursor::pos();
	if (!drag_moving_)
	{
		drag_moving_ = drag_timer_.elapsed() >= DRAG_DELAY_MS
			&& (mouse - drag_press_position_).manhattanLength() >= QApplication::startDragDistance();
		if (!drag_moving_)
			return;
	}

	QWidget *viewport = findViewportWidget(QApplication::widgetAt(mouse));
	if (!viewport)
		return;

	Math::Vec3 p0, p1;
	if (!screenRay(viewport, viewport->mapFromGlobal(QPointF(mouse)), p0, p1))
		return;

	// The objects of the path itself are looked through.
	Vector<NodePtr> ignore;
	ignore.append(root_);
	const SurfaceHit hit = raycastSurface(p0, p1, SurfaceFilter(), ignore);
	if (hit.found)
		drag_point_->setWorldPosition(hit.point);
}

void PathPlacer::end_drag()
{
	if (manipulators_disabled_)
	{
		::UnigineEditor::ObjectMode::setManipulatorsEnabled(true);
		manipulators_disabled_ = false;
	}

	// The move of an existing point is one undo step. A point that has just been
	// added needs none: undoing its creation removes it wherever it is.
	if (drag_point_ && !drag_point_.isDeleted() && !drag_is_new_)
	{
		const Math::Mat4 moved = drag_point_->getWorldTransform();
		if (moved != drag_start_transform_)
		{
			drag_point_->setWorldTransform(drag_start_transform_);
			::UnigineEditor::Undo::apply(new ::UnigineEditor::SetNodeTransformAction(drag_point_, moved));
		}
	}

	drag_point_ = NodePtr();
	drag_moving_ = false;
}

// What a click taken by the tool ends with, when its button is released: the held
// point stays where it is and the picked point becomes selected.
void PathPlacer::finish_click()
{
	end_drag();

	if (pending_selection_ && !pending_selection_.isDeleted())
		select_point(pending_selection_, pending_selection_adds_);
	pending_selection_ = NodePtr();
}

bool PathPlacer::is_point_selected(const NodePtr &point) const
{
	for (const NodePtr &selected : selected_points())
	{
		if (selected->getID() == point->getID())
			return true;
	}
	return false;
}

bool PathPlacer::eventFilter(QObject *watched, QEvent *event)
{
	if (!active_ || !hasPath())
		return false;

	switch (event->type())
	{
		case QEvent::KeyPress:
			if (adding_points_ && static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape)
			{
				setAddingPoints(false);
				return true;
			}
			break;

		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonDblClick:	// the second click of a fast pair
		{
			const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
			if (mouse->button() != Qt::LeftButton)
				break;

			// A press that is not taken below goes to the editor with its release -
			// whatever the reason it is not taken for.
			click_taken_ = false;
			swallow_release_ = false;

			// Alt + LMB stays with the editor camera.
			if (mouse->modifiers() & Qt::AltModifier)
				break;

			QWidget *viewport = findViewportWidget(watched);
			if (!viewport || !World::isLoaded())
				break;

			Math::Vec3 p0, p1;
			if (!screenRay(viewport, viewport->mapFromGlobal(mouse->globalPosition()), p0, p1))
				break;

			// While Snap to Surface of the editor is on, moving nodes over the surfaces
			// is the editor's job and the tool keeps out of it: taking the clicks on a
			// point away from the editor then breaks its snap (it does not let go of
			// the node). The tool drags points itself only while that snap is off.
			const bool editor_snap = ::UnigineEditor::ObjectMode::isSnapToSurfaceEnabled();

			// A click on a point selects it; Ctrl or Shift add it to the selection.
			// Holding the button drags the point over the surfaces, selected or not.
			if (NodePtr point = pick_point(p0, p1))
			{
				const bool add_to_selection = mouse->modifiers() & (Qt::ControlModifier | Qt::ShiftModifier);
				const bool already_selected = !add_to_selection && is_point_selected(point);

				// With the editor's snap on, a selected point is left to the editor
				// entirely: the click goes to its tools as if the tool were not there.
				if (editor_snap && already_selected)
					break;

				if (!already_selected)
				{
					pending_selection_ = point;
					pending_selection_adds_ = add_to_selection;
				}
				if (!add_to_selection && !editor_snap)
					begin_drag(point, false);

				click_taken_ = true;
				// The editor has its own tools on a selected point (the manipulators,
				// Snap to Surface) and may have started one of them on this press in
				// spite of the tool taking it. Its release is therefore let through:
				// the editor must be able to end what it started.
				swallow_release_ = !already_selected;
				return true;
			}

			// Anything else is the editor's own click, unless points are being added.
			if (!adding_points_)
				break;

			Vector<NodePtr> ignore;
			ignore.append(root_);
			const SurfaceHit hit = raycastSurface(p0, p1, SurfaceFilter(), ignore);
			if (!hit.found)
				report(MSG_WARNING, uiText("Click on a surface to add a point there.", "Кликните по поверхности, чтобы добавить на неё точку."));
			else if (NodePtr point = add_point(hit.point))
			{
				// The new point gets selected, so the next one continues from it.
				pending_selection_ = point;
				pending_selection_adds_ = false;
				if (!editor_snap)
					begin_drag(point, true);
				report(MSG_INFO, QString());
				notify_changed();
			}
			click_taken_ = true;
			swallow_release_ = true;
			return true;
		}

		case QEvent::MouseButtonRelease:
			if (static_cast<QMouseEvent *>(event)->button() != Qt::LeftButton)
				break;
			// The release of a press the tool has taken ends its click. It is kept
			// from the editor only if the press was, and never otherwise: the editor
			// must always see the end of a click it saw the start of.
			if (click_taken_ && watched->isWidgetType())
			{
				const bool swallow = swallow_release_ && findViewportWidget(watched) != nullptr;
				click_taken_ = false;
				swallow_release_ = false;
				finish_click();
				return swallow;
			}
			break;

		default:
			break;
	}

	return false;
}

////////////////////////////////////////////////////////////////////////////////
// Curve.
////////////////////////////////////////////////////////////////////////////////
PathPlacer::Curve PathPlacer::build_curve() const
{
	Curve curve;
	const PathPoints points = collect_points(path_node(), settings_.closed);
	const int spans = points.spans();
	if (spans == 0)
		return curve;

	curve.samples.reserve(spans * CURVE_SUBDIVISIONS + 1);
	curve.samples.append(points.positions[0]);
	for (int i = 0; i < spans; ++i)
	{
		for (int s = 1; s <= CURVE_SUBDIVISIONS; ++s)
		{
			const Math::Vec3 p = span_point(points, i, double(s) / CURVE_SUBDIVISIONS);
			curve.length += double(Math::length(p - curve.samples.last()));
			curve.samples.append(p);
		}
	}
	return curve;
}

// Lays objects of the given lengths one after another from the start of the curve.
// An object that the curve ends inside of still gets a place (it sticks out past the
// end), unless only the ones that fit entirely are asked for; the rest get none.
QVector<PathPlacer::Slot> PathPlacer::compute_slots(const Curve &curve, const QVector<float> &lengths,
	bool only_fitting) const
{
	QVector<Slot> result;
	const QVector<Math::Vec3> &samples = curve.samples;
	if (samples.size() < 2)
		return result;

	Cursor cursor;
	cursor.position = samples[0];
	for (float length : lengths)
	{
		Slot slot;
		slot.start = cursor.position;

		// An object of no length stands where the next one starts and looks the way
		// the path goes on from there (at the very end - the way the path ends). It
		// always fits and does not move the place of the next object.
		if (length < MIN_OBJECT_LENGTH)
		{
			Cursor ahead = cursor;
			Math::Vec3 direction = samples.last() - samples[samples.size() - 2];
			if (advance(samples, ahead, DIRECTION_PROBE))
				direction = ahead.position - cursor.position;

			slot.end = slot.start + unit_or_zero(direction);
			slot.no_length = true;
			result.append(slot);
			continue;
		}

		Cursor next = cursor;
		if (advance(samples, next, double(length)))
		{
			slot.end = next.position;
			cursor = next;
			result.append(slot);
			continue;
		}

		const Math::Vec3 end = samples.last();
		const Math::Vec3 direction = end - samples[samples.size() - 2];
		const double direction_length = double(Math::length(direction));
		// The first object that does not fit is still put where it starts, looking the
		// way the path ends: e.g. the tail post of a fence that takes the whole path.
		if (!only_fitting && direction_length > 1e-9)
		{
			slot.end = slot.start + direction * Math::Scalar(double(length) / direction_length);
			result.append(slot);
		}
		break;
	}
	return result;
}

Math::Vec3 PathPlacer::snap_to_ground(const Math::Vec3 &point) const
{
	Vector<NodePtr> ignore;
	ignore.append(root_);

	const Math::Vec3 above = point + Math::Vec3(0.0, 0.0, SNAP_UP);
	const Math::Vec3 below = point - Math::Vec3(0.0, 0.0, SNAP_DOWN);
	const SurfaceHit hit = raycastSurface(above, below, SurfaceFilter(), ignore);
	return hit.found ? Math::Vec3(point.x, point.y, hit.point.z + Math::Scalar(settings_.ground_offset)) : point;
}

// The transform of an object that starts at slot.start and looks at slot.end.
Math::Mat4 PathPlacer::slot_transform(const Slot &slot, float length, const Math::vec3 &scale) const
{
	const Math::vec3 world_up(0.0f, 0.0f, 1.0f);
	const Math::vec3 delta = Math::vec3(slot.end - slot.start);

	Math::vec3 forward = delta;
	Math::vec3 up = world_up;
	if (settings_.skew && !slot.no_length)
	{
		// Sheared: the object reaches exactly to the next one, its up stays vertical.
		forward = delta / (length > MIN_OBJECT_LENGTH ? length : MIN_OBJECT_LENGTH);
	} else
	{
		// An object of no length has nothing to reach to: with the skew on it just
		// stands upright.
		if (settings_.yaw_only || settings_.skew)
			forward.z = 0.0f;
		forward = Math::length2(forward) > 1e-10f ? Math::normalize(forward) : Math::vec3(0.0f, 1.0f, 0.0f);
	}

	Math::vec3 side = Math::cross(forward, world_up);
	side = Math::length2(side) > 1e-10f ? Math::normalize(side) : Math::vec3(1.0f, 0.0f, 0.0f);
	if (!settings_.skew && !settings_.yaw_only)
		up = Math::normalize(Math::cross(side, forward));	// leans with the slope

	// Columns of the matrix are where the local X, Y and Z of the object point to.
	const int forward_index = axis_index(settings_.forward_axis);
	const int up_index = axis_index(settings_.up_axis);
	const int third_index = 3 - forward_index - up_index;

	Math::vec3 columns[3];
	columns[forward_index] = forward * axis_sign(settings_.forward_axis);
	columns[up_index] = up * axis_sign(settings_.up_axis);
	// With the skew on, the forward of an object on a span that goes straight up is
	// along its up: the two give no third axis then, and the side of the path is taken.
	const Math::vec3 third = Math::cross(columns[(third_index + 1) % 3], columns[(third_index + 2) % 3]);
	columns[third_index] = Math::length2(third) > 1e-10f ? Math::normalize(third) : side;

	Math::mat4 basis = Math::mat4_identity;
	for (int i = 0; i < 3; ++i)
		basis.setColumn3(i, columns[i]);

	return Math::translate(slot.start) * Math::Mat4(basis * Math::scale(scale));
}

float PathPlacer::object_length(const NodePtr &object) const
{
	// An object with no length of its own (put into "objects" by hand) takes the step
	// of the path. A length of zero is a length too: the object takes no room.
	const char *data = object->getData(LENGTH_DATA);
	if (data && *data)
	{
		bool ok = false;
		const float length = QByteArray(data).toFloat(&ok);
		if (ok && length >= 0.0f)
			return length;
	}
	return settings_.step;
}

void PathPlacer::rearrange()
{
	const NodePtr objects = objects_node();
	if (!objects)
		return;

	const Curve curve = build_curve();
	length_ = curve.length;

	const QVector<NodePtr> ordered = objects_in_path_order(objects);
	const int count = int(ordered.size());
	QVector<float> lengths(count);
	for (int i = 0; i < count; ++i)
		lengths[i] = object_length(ordered[i]);

	const QVector<Slot> placed = compute_slots(curve, lengths, false);
	for (int i = 0; i < placed.size(); ++i)
	{
		const NodePtr object = ordered[i];

		Slot slot = placed[i];
		if (settings_.snap_to_ground)
		{
			slot.start = snap_to_ground(slot.start);
			slot.end = snap_to_ground(slot.end);
		}

		// A sheared object has no scale that can be read back from its transform, so
		// its own scale is remembered in the node while the skew is on.
		// The world scale, as the transform it goes into is a world one: with the local
		// scale an object of a path under a scaled node would change its size with
		// every rearrangement.
		Math::vec3 scale = object->getWorldScale();
		const char *saved_scale = object->getData(SCALE_DATA);
		if (saved_scale && *saved_scale)
		{
			const QList<QByteArray> parts = QByteArray(saved_scale).split(' ');
			if (parts.size() == 3)
				scale = Math::vec3(parts[0].toFloat(), parts[1].toFloat(), parts[2].toFloat());
		}
		if (settings_.skew)
		{
			if (!saved_scale || !*saved_scale)
			{
				object->setData(SCALE_DATA, QByteArray::number(double(scale.x)).append(' ')
					.append(QByteArray::number(double(scale.y))).append(' ')
					.append(QByteArray::number(double(scale.z))).constData());
			}
		} else if (saved_scale && *saved_scale)
			object->setData(SCALE_DATA, "");

		object->setWorldTransform(slot_transform(slot, lengths[i], scale));
	}

	unplaced_ = curve.samples.size() < 2 ? 0 : count - int(placed.size());
	arranged_state_ = snapshot();
}

// Everything the places of the objects depend on, and where the objects are: with
// live update on, an object moved off the path is put back at once, the way the old
// script kept them in place.
QByteArray PathPlacer::snapshot() const
{
	QByteArray state;
	const NodePtr path = path_node();
	const NodePtr objects = objects_node();
	if (!path || !objects)
		return state;

	put(state, double(settings_.step));
	put(state, double(settings_.forward_axis));
	put(state, double(settings_.up_axis));
	put(state, double(settings_.closed));
	put(state, double(settings_.yaw_only));
	put(state, double(settings_.skew));
	put(state, double(settings_.snap_to_ground));
	put(state, double(settings_.ground_offset));

	for (int i = 0, num = path->getNumChildren(); i < num; ++i)
	{
		const NodePtr point = path->getChild(i);
		put(state, point->getWorldPosition());
		put(state, double(is_corner(point)));
	}
	for (int i = 0, num = objects->getNumChildren(); i < num; ++i)
	{
		const NodePtr object = objects->getChild(i);
		put(state, double(object->getID()));
		put(state, double(object_length(object)));
		put(state, object->getWorldTransform());
	}
	return state;
}

////////////////////////////////////////////////////////////////////////////////
// Every frame while the tool is active.
////////////////////////////////////////////////////////////////////////////////
void PathPlacer::update()
{
	if (!active_)
		return;

	// The path was deleted or its world closed.
	if (root_ && !hasPath())
	{
		root_ = NodePtr();
		arranged_state_.clear();
		length_ = 0.0;
		unplaced_ = 0;
		// A point may have been held at that moment: the drag is over, and the
		// manipulators of the editor it had turned off come back.
		end_drag();
		pending_selection_ = NodePtr();
		click_taken_ = false;
		swallow_release_ = false;
		setAddingPoints(false);
		notify_changed();
	}
	if (!hasPath())
		return;

	update_drag();

	const Curve curve = build_curve();
	draw(curve);

	const QByteArray state = snapshot();
	if (state == arranged_state_)
		return;

	if (live_update_)
		rearrange();
	else
	{
		// Only the numbers shown on the page are refreshed.
		length_ = curve.length;
		arranged_state_ = state;
	}
	notify_changed();
}

void PathPlacer::draw(const Curve &curve) const
{
	const Math::vec4 line_color(1.0f, 1.0f, 1.0f, 1.0f);
	for (int i = 0; i + 1 < curve.samples.size(); ++i)
		Visualizer::renderLine3D(curve.samples[i], curve.samples[i + 1], line_color, 0.0f, false);

	const NodePtr path = path_node();
	if (!path)
		return;

	// Points: the selected ones are orange, the first one is green (the path starts
	// there), corners are white, smooth points blue.
	//
	// The editor gets the new selection only when the mouse button is released. Until
	// then the points are shown the way they are going to be: a point picked without
	// Ctrl or Shift replaces the selection, so the points selected before it are
	// already drawn as not selected while it is held and dragged.
	const bool has_pending = pending_selection_ && !pending_selection_.isDeleted();
	Vector<NodePtr> selected;
	if (!has_pending || pending_selection_adds_)
		selected = selected_points();
	for (int i = 0, num = path->getNumChildren(); i < num; ++i)
	{
		const NodePtr point = path->getChild(i);
		const Math::Vec3 position = point->getWorldPosition();

		Math::vec4 color(0.35f, 0.7f, 1.0f, 1.0f);
		if (is_corner(point))
			color = Math::vec4(1.0f, 1.0f, 1.0f, 1.0f);
		if (i == 0)
			color = Math::vec4(0.3f, 0.9f, 0.3f, 1.0f);
		for (const NodePtr &selected_point : selected)
		{
			if (selected_point->getID() == point->getID())
				color = Math::vec4(1.0f, 0.55f, 0.1f, 1.0f);
		}
		// The point picked by the click that is still held is about to be selected.
		if (has_pending && pending_selection_->getID() == point->getID())
			color = Math::vec4(1.0f, 0.55f, 0.1f, 1.0f);

		Visualizer::renderSolidSphere(point_radius(position), Math::translate(position), color, 0.0f, false);
	}
}

////////////////////////////////////////////////////////////////////////////////
// Fill pattern.
////////////////////////////////////////////////////////////////////////////////
bool PathPlacer::addPatternAssets()
{
	const QVector<PlaceableAsset> assets = resolveSelectedAssets();
	if (assets.isEmpty())
	{
		report(MSG_WARNING, uiText("Select meshes or nodes in the Asset Browser first.", "Сначала выберите меши или ноды в Asset Browser."));
		return false;
	}

	for (const PlaceableAsset &asset : assets)
	{
		PathPatternItem item;
		item.asset = asset;
		item.length = measure_length(asset);
		pattern_.append(item);
	}
	// New assets join the repeated part, above the tail.
	keep_tails_last(pattern_);
	save_settings();
	report(MSG_INFO, QString());
	notify_changed();
	return true;
}

void PathPlacer::removePatternItem(int index)
{
	if (index < 0 || index >= pattern_.size())
		return;
	pattern_.removeAt(index);
	save_settings();
	notify_changed();
}

void PathPlacer::movePatternItem(int index, int offset)
{
	const int target = index + offset;
	if (index < 0 || index >= pattern_.size() || target < 0 || target >= pattern_.size() || target == index)
		return;
	// The repeated entries and the tail ones do not mix.
	if (pattern_[index].tail != pattern_[target].tail)
		return;
	pattern_.move(index, target);
	save_settings();
	notify_changed();
}

void PathPlacer::setPatternLength(int index, float length)
{
	if (index < 0 || index >= pattern_.size())
		return;
	// Zero is allowed: such an asset takes no room, the next one starts where it stands.
	pattern_[index].length = length < 0.0f ? 0.0f : length;
	save_settings();
	notify_changed();
}

void PathPlacer::setPatternCount(int index, int count)
{
	if (index < 0 || index >= pattern_.size())
		return;
	// The tail is placed once.
	pattern_[index].count = pattern_[index].tail ? 1 : qBound(1, count, MAX_PATTERN_COUNT);
	save_settings();
	notify_changed();
}

void PathPlacer::setPatternTail(int index, bool tail)
{
	if (index < 0 || index >= pattern_.size())
		return;
	// The tail is not taken from another entry: it has to be turned off there first,
	// so that it does not change its place unnoticed.
	if (tail)
	{
		for (int i = 0; i < pattern_.size(); ++i)
		{
			if (i != index && pattern_[i].tail)
				return;
		}
	}
	pattern_[index].tail = tail;
	keep_tails_last(pattern_);
	save_settings();
	notify_changed();
}

void PathPlacer::clearPattern()
{
	pattern_.clear();
	save_settings();
	notify_changed();
}

// The size of the asset along the axis that looks along the path: the length of the
// path one such object takes. The step of the path if it can not be measured.
float PathPlacer::measure_length(const PlaceableAsset &asset) const
{
	NodePtr node = createNode(asset);
	if (!node)
		return settings_.step;

	node->setWorldTransform(Math::Mat4_identity);
	const Math::WorldBoundBox bounds = node->getHierarchyWorldBoundBox();
	const int axis = axis_index(settings_.forward_axis);
	const double size = double(bounds.maximum[axis]) - double(bounds.minimum[axis]);
	node.deleteLater();

	return std::isfinite(size) && size >= double(MIN_OBJECT_LENGTH) ? float(size) : settings_.step;
}

QVector<int> PathPlacer::fill_sequence(bool *cut) const
{
	QVector<int> result;
	if (cut)
		*cut = false;
	if (!hasPath() || pattern_.isEmpty())
		return result;

	// The repeated part: as many objects as fit the path.
	const QVector<int> repeated = pattern_sequence(pattern_, false);
	// An item shorter than the minimum takes no length, the same as in compute_slots():
	// such items do not add up to a length that could be repeated along the path.
	float repeated_length = 0.0f;
	for (int index : repeated)
	{
		if (pattern_[index].length >= MIN_OBJECT_LENGTH)
			repeated_length += pattern_[index].length;
	}

	if (repeated_length < MIN_OBJECT_LENGTH)
	{
		// Nothing of the repeated part takes any length: repeating it would never get
		// along the path, so it is placed once, at the start.
		result = repeated;
	} else
	{
		QVector<float> lengths(MAX_FILL_OBJECTS);
		for (int i = 0; i < MAX_FILL_OBJECTS; ++i)
			lengths[i] = pattern_[repeated[i % repeated.size()]].length;

		const int fitting = int(compute_slots(build_curve(), lengths, true).size());
		for (int i = 0; i < fitting; ++i)
			result.append(repeated[i % repeated.size()]);
		if (cut)
			*cut = fitting >= MAX_FILL_OBJECTS;
	}

	// The tail: once, right after the last repeated object.
	result += pattern_sequence(pattern_, true);
	return result;
}

int PathPlacer::countFill() const
{
	return int(fill_sequence().size());
}

bool PathPlacer::fill()
{
	const NodePtr objects = objects_node();
	if (!objects)
	{
		report(MSG_WARNING, uiText("Create a path or select an existing one first.", "Сначала создайте кривую или выделите существующую."));
		return false;
	}
	if (pattern_.isEmpty())
	{
		report(MSG_WARNING, uiText("Add at least one asset to the pattern.", "Добавьте в паттерн хотя бы один ассет."));
		return false;
	}
	if (getNumPoints() < 2)
	{
		report(MSG_WARNING, uiText("The path needs at least two points.", "Кривой нужно минимум две точки."));
		return false;
	}

	bool cut = false;
	const QVector<int> sequence = fill_sequence(&cut);
	const int count = int(sequence.size());
	if (count == 0)
	{
		report(MSG_WARNING, uiText("The path is shorter than the first asset of the pattern.", "Кривая короче первого ассета паттерна."));
		return false;
	}

	Vector<NodePtr> old_objects;
	for (int i = 0, num = objects->getNumChildren(); i < num; ++i)
		old_objects.append(objects->getChild(i));

	// The new objects are made before the old ones are taken away: if none can be
	// made (the assets of the pattern are gone from the project), the path keeps
	// what it has.
	Vector<NodePtr> created;
	for (int i = 0; i < count; ++i)
	{
		const PathPatternItem &item = pattern_[sequence[i]];
		NodePtr node = createNode(item.asset);
		if (!node)
			continue;

		node->setData(LENGTH_DATA, QByteArray::number(double(item.length)).constData());
		if (item.tail)
			node->setData(TAIL_DATA, "1");
		node->setWorldParent(objects);
		node->setShowInEditorEnabledRecursive(true);
		node->setSaveToWorldEnabledRecursive(true);
		created.append(node);
	}
	if (created.empty())
	{
		report(MSG_ERROR, uiText("Could not create objects from the assets of the pattern. The path is left as it was.",
			"Не удалось создать объекты из ассетов паттерна. Кривая оставлена как была."));
		return false;
	}

	// One undo step: the old objects out, the new ones in.
	using namespace ::UnigineEditor;
	Undo::begin();
	if (old_objects.size())
		Undo::apply(new RemoveNodesAction(old_objects));
	Undo::apply(new CreateNodesAction(created));
	Undo::commit();

	rearrange();
	if (cut)
	{
		report(MSG_WARNING, uiText(
			"Filled the path with %1 objects, but not to its end: one fill is limited to %2 objects.",
			"Кривая заполнена, объектов: %1, но не до конца: одно заполнение ограничено %2 объектами.")
			.arg(created.size()).arg(MAX_FILL_OBJECTS));
	} else
		report(MSG_INFO, uiText("Filled the path with %1 objects.", "Кривая заполнена, объектов: %1.").arg(created.size()));
	notify_changed();
	return true;
}

void PathPlacer::notify_changed() const
{
	if (on_changed)
		on_changed();
}

void PathPlacer::report(int level, const QString &text) const
{
	if (on_message)
		on_message(level, text);
}

} // namespace ArtistTool
