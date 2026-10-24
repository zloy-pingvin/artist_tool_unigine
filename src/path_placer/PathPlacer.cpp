#include "PathPlacer.h"
#include "UiHelpers.h"
#include "Localization.h"
#include "NodeData.h"
#include "UndoActions.h"

#include <editor/UnigineActions.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>
#include <editor/UnigineUndo.h>

#include <UnigineEngine.h>
#include <UnigineLog.h>
#include <UnigineNodes.h>
#include <UnigineRender.h>
#include <UnigineWorld.h>

#include <QApplication>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStringList>
#include <QKeyEvent>

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
const char OBJECTS_NODE_NAME[] = "objects";

// Names of the node data the tool keeps its state in.
const char SETTINGS_DATA[] = "artist_tool_path";
const char LENGTH_DATA[] = "artist_tool_length";
const char SCALE_DATA[] = "artist_tool_scale";
// "1" on an object made from the end ("tail") entry of the pattern: it stays the last
// one on the path whatever is added to "objects" after it. "1" in HEAD_DATA - from the
// start entry: it stays the first one.
const char TAIL_DATA[] = "artist_tool_tail";
const char HEAD_DATA[] = "artist_tool_head";
// The roles of the entries of the pattern in the settings.
const char *const ROLE_KEYS[3] = {"repeat", "start", "end"};

// Pieces every span between two points is cut into. The objects are placed on the
// resulting polyline.
const int CURVE_SUBDIVISIONS = 24;
const float MIN_OBJECT_LENGTH = 0.01f;
// How far ahead the path is looked at to turn an object that takes no length.
const double DIRECTION_PROBE = 0.05;
const int MAX_FILL_OBJECTS = 5000;
const int MAX_PATTERN_COUNT = 999;

// The entries of the pattern of one role as the row they are placed in: every entry
// is there as many times in a row as its count says.
QVector<int> pattern_sequence(const QVector<PathPatternItem> &pattern, PathPatternRole role)
{
	QVector<int> sequence;
	for (int i = 0; i < pattern.size(); ++i)
	{
		if (pattern[i].role != role)
			continue;
		for (int n = 0, count = qBound(1, pattern[i].count, MAX_PATTERN_COUNT); n < count; ++n)
			sequence.append(i);
	}
	return sequence;
}

// There is one start and one end at most (the first ones; the others are repeated),
// both placed once. The start goes to the top of the pattern, the end to the bottom,
// the order of the rest is kept.
void keep_roles_in_order(QVector<PathPatternItem> &pattern)
{
	bool has_role[3] = {false, false, false};
	for (PathPatternItem &item : pattern)
	{
		if (item.role == PATH_PATTERN_REPEAT)
			continue;
		if (has_role[item.role])
			item.role = PATH_PATTERN_REPEAT;
		else
			item.count = 1;
		has_role[item.role] = true;
	}
	std::stable_partition(pattern.begin(), pattern.end(), [](const PathPatternItem &item) { return item.role == PATH_PATTERN_START; });
	std::stable_partition(pattern.begin(), pattern.end(), [](const PathPatternItem &item) { return item.role != PATH_PATTERN_END; });
}

// How far above and below the path the ground is searched for.
const double SNAP_UP = 5.0;
const double SNAP_DOWN = 100.0;

// A path root is any node that has both a "path" and an "objects" child.
bool is_path_root(const NodePtr &node)
{
	return node->findChild(CURVE_PATH_NODE_NAME) >= 0 && node->findChild(OBJECTS_NODE_NAME) >= 0;
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

bool has_flag(const NodePtr &object, const char *name)
{
	const char *data = object->getData(name);
	return data && data[0] == '1';
}

// The objects in the order they stand along the path: the order of the "objects"
// node, but with the start objects before and the end ("tail") objects after all the
// rest. So a section added by hand - which goes to the end of the list, after the
// tail - still stands before the tail.
QVector<NodePtr> objects_in_path_order(const NodePtr &objects)
{
	QVector<NodePtr> heads;
	QVector<NodePtr> ordered;
	QVector<NodePtr> tails;
	for (int i = 0, num = objects->getNumChildren(); i < num; ++i)
	{
		const NodePtr object = objects->getChild(i);
		if (has_flag(object, HEAD_DATA))
			heads.append(object);
		else if (has_flag(object, TAIL_DATA))
			tails.append(object);
		else
			ordered.append(object);
	}
	return heads + ordered + tails;
}

int axis_index(AxisDirection axis)
{
	return int(axis) / 2;
}

float axis_sign(AxisDirection axis)
{
	return int(axis) % 2 ? -1.0f : 1.0f;
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

} // namespace

PathPlacer::PathPlacer(QObject *parent)
	: QObject(parent)
{
	editor_.on_changed = [this]() { notify_changed(); };
	editor_.on_message = [this](int level, const QString &text) { report(level, text); };
}

PathPlacer::~PathPlacer()
{
	on_changed = nullptr;
	on_message = nullptr;
	on_paths_changed = nullptr;
	shutdown();
	editor_.on_changed = nullptr;
	editor_.on_message = nullptr;
}

void PathPlacer::shutdown()
{
	// A new line left without points is kept in the world, not removed: removing it is
	// a step of the undo history, and the plugin resets the history right after it has
	// shut down - a step pushed while it shuts down crashes the editor.
	discard_if_empty_ = false;
	setActive(false);
}

void PathPlacer::setActive(bool active)
{
	if (active_ == active)
		return;

	active_ = active;
	if (active_)
	{
		CurveEditor::recoverManipulators();
		// The curve is drawn with the Visualizer, once per frame.
		requestVisualizer();
		Engine::get()->getEventEndWorldUpdate().connect(connections_, this, &PathPlacer::update);
		// Clicks on the points of the path are taken all the time the tool is active.
		qApp->installEventFilter(this);
		updateFromSelection();
	} else
	{
		setAddingPoints(false);
		editor_.reset();
		qApp->removeEventFilter(this);
		connections_.disconnectAll();
		releaseVisualizer();
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
	editor_.setRoot(root_);
	arranged_state_.clear();
	unplaced_ = 0;
	load_settings();
	editor_.setClosed(settings_.closed);
	// A path without settings yet (of the old script): they are written down now, not as a
	// step of the undo history - an undo of the first change goes back to them, not to
	// nothing (which would keep the change).
	if (saved_settings_.isEmpty())
		save_settings(false);

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
		info.points = findChildNode(node, CURVE_PATH_NODE_NAME)->getNumChildren();
		info.objects = findChildNode(node, OBJECTS_NODE_NAME)->getNumChildren();
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
	return isNodeAlive(root_) && path_node() && objects_node();
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
	return root_ && !root_.isDeleted() ? findChildNode(root_, CURVE_PATH_NODE_NAME) : NodePtr();
}

NodePtr PathPlacer::objects_node() const
{
	return root_ && !root_.isDeleted() ? findChildNode(root_, OBJECTS_NODE_NAME) : NodePtr();
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

bool PathPlacer::setParentNode(const NodePtr &node)
{
	if (node && find_path_root(node))
	{
		report(MSG_WARNING, uiText("A part of a path can not be the parent of paths: select another node.",
			"Часть кривой не может быть родителем кривых: выделите другую ноду."));
		return false;
	}
	parent_ = node;
	report(MSG_INFO, QString());
	notify_changed();
	return true;
}

NodePtr PathPlacer::getParentNode() const
{
	return isNodeAlive(parent_) ? parent_ : NodePtr();
}

void PathPlacer::forget_removed_path(const NodePtr &root)
{
	if (hasPath() && root_->getID() == root->getID())
	{
		root_ = NodePtr();
		editor_.setRoot(NodePtr());
		arranged_state_.clear();
		length_ = 0.0;
		unplaced_ = 0;
	}
	removed_root_ = root;
	removed_root_frames_ = 0;
	notify_changed();
}

bool PathPlacer::clonePath()
{
	if (!hasPath())
		return false;
	setAddingPoints(false);

	// The points, the settings and the pattern; the objects are the copy's own, made by
	// its Fill.
	const NodePtr source = root_;
	NodeDummyPtr root = NodeDummy::create();
	root->setName(source->getName());
	root->setWorldTransform(source->getWorldTransform());
	if (const char *data = source->getData(SETTINGS_DATA))
		root->setData(SETTINGS_DATA, data);
	NodeDummyPtr path = NodeDummy::create();
	path->setName(CURVE_PATH_NODE_NAME);
	path->setWorldParent(root);
	const NodePtr source_path = path_node();
	path->setWorldTransform(source_path->getWorldTransform());
	for (int i = 0, num = source_path->getNumChildren(); i < num; ++i)
	{
		NodePtr point = source_path->getChild(i)->clone();
		point->setWorldParent(path);
		point->setWorldTransform(source_path->getChild(i)->getWorldTransform());
	}
	NodeDummyPtr objects = NodeDummy::create();
	objects->setName(OBJECTS_NODE_NAME);
	objects->setWorldParent(root);
	// Into the parent of the new paths, or next to the path.
	if (const NodePtr parent = getParentNode() ? getParentNode() : source->getParent())
		root->setWorldParent(parent);
	root->setShowInEditorEnabledRecursive(true);
	root->setSaveToWorldEnabledRecursive(true);
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(root));

	selectPath(root->getID());
	if (on_paths_changed)
		on_paths_changed();
	report(MSG_INFO, uiText("The path is copied: its points, settings and pattern, no objects yet.",
		"Кривая скопирована: точки, настройки и паттерн, объектов пока нет."));
	return true;
}

bool PathPlacer::removePathCurve()
{
	if (!hasPath())
		return false;
	setAddingPoints(false);

	// The objects go to a node of the name of the path, where the path was; the path
	// itself with its curve is removed. One undo step.
	const NodePtr root = root_;
	const NodePtr holder = objects_node();
	Vector<NodePtr> objects;
	for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
		objects.append(holder->getChild(i));

	using namespace ::UnigineEditor;
	Undo::begin();
	if (!objects.empty())
	{
		NodeDummyPtr group = NodeDummy::create();
		group->setName(root->getName());
		if (const NodePtr parent = root->getParent())
			group->setWorldParent(parent);
		group->setWorldTransform(root->getWorldTransform());
		group->setShowInEditorEnabled(true);
		group->setSaveToWorldEnabled(true);
		Undo::apply(new CreateNodesAction(group));
		Undo::apply(new ReparentNodesAction(objects, group));
	}
	Undo::apply(new RemoveNodesAction(root));
	Undo::commit();

	forget_removed_path(root);
	report(MSG_INFO, objects.empty() ? uiText("The path is removed; it had no objects.", "Кривая удалена; объектов у неё не было.")
		: uiText("The curve is removed; the objects stay as ordinary nodes.", "Кривая удалена; объекты остались обычными нодами."));
	return true;
}

bool PathPlacer::removePath()
{
	if (!hasPath())
		return false;
	setAddingPoints(false);
	const NodePtr root = root_;
	::UnigineEditor::Undo::apply(new ::UnigineEditor::RemoveNodesAction(root));
	forget_removed_path(root);
	report(MSG_INFO, uiText("The path is removed with its objects.", "Кривая удалена вместе с объектами."));
	return true;
}

void PathPlacer::createPath()
{
	setAddingPoints(false);

	NodeDummyPtr root = NodeDummy::create();
	root->setName(ROOT_NODE_NAME);
	NodeDummyPtr path = NodeDummy::create();
	path->setName(CURVE_PATH_NODE_NAME);
	path->setWorldParent(root);
	NodeDummyPtr objects = NodeDummy::create();
	objects->setName(OBJECTS_NODE_NAME);
	objects->setWorldParent(root);
	if (const NodePtr parent = getParentNode())
		root->setWorldParent(parent);

	root->setShowInEditorEnabledRecursive(true);
	root->setSaveToWorldEnabledRecursive(true);
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(root));

	// The new path starts with the settings that are on the page and with an empty
	// pattern: the assets of the previous path are not its own.
	root_ = root;
	editor_.setRoot(root_);
	editor_.setClosed(settings_.closed);
	arranged_state_.clear();
	length_ = 0.0;
	unplaced_ = 0;
	pattern_.clear();
	// A part of the new path, not a step of its own.
	save_settings(false);
	arranged_state_ = snapshot();
	discard_if_empty_ = true;

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
	if (editor_.isAddingPoints() == adding)
		return;

	editor_.setAddingPoints(adding);

	// A path made by New Path is not kept if adding points to it is over before it
	// got a single one: there is no current path then.
	if (!adding && discard_if_empty_)
	{
		discard_if_empty_ = false;
		if (hasPath() && getNumPoints() == 0)
		{
			const NodePtr root = root_;
			root_ = NodePtr();
			editor_.setRoot(NodePtr());
			arranged_state_.clear();
			length_ = 0.0;
			unplaced_ = 0;
			::UnigineEditor::Undo::apply(new ::UnigineEditor::RemoveNodesAction(root));
			removed_root_ = root;
			removed_root_frames_ = 0;
			report(MSG_INFO, uiText("The new path got no points and was removed.",
				"У новой кривой не появилось ни одной точки - она удалена."));
		}
	}
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
	settings_.ground.look_through = true;
	editor_.setClosed(settings_.closed);
	editor_.setSurfaceFilter(settings_.ground);

	if (hasPath())
		save_settings();
	notify_changed();
}

void PathPlacer::setLiveUpdate(bool live)
{
	if (live_update_ == live)
		return;

	live_update_ = live;
	// Turning it on puts the objects back on the path at the next frame, as a step of
	// the undo history (they may have been set by hand meanwhile).
	if (live_update_)
	{
		arranged_state_.clear();
		rearrange_undoable_ = true;
	}
	notify_changed();
}

////////////////////////////////////////////////////////////////////////////////
// Settings, kept in the data of the root node as JSON.
////////////////////////////////////////////////////////////////////////////////
void PathPlacer::load_settings()
{
	const char *data = root_->getData(SETTINGS_DATA);
	saved_settings_ = QByteArray(data ? data : "");
	const QJsonObject json = QJsonDocument::fromJson(saved_settings_).object();
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
		// The assets are of the path: not those of the path picked before.
		pattern_.clear();
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
	settings_.ground.surface_types = json.value("ground_types").toInt(defaults.ground.surface_types);
	settings_.ground.only_immovable = json.value("ground_immovable").toBool(defaults.ground.only_immovable);
	settings_.ground.only_intersection = json.value("ground_intersection").toBool(defaults.ground.only_intersection);
	settings_.ground.look_through = true;
	editor_.setSurfaceFilter(settings_.ground);

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
		// The builds before 1.3 had only the tail: it is the end.
		item.role = entry.value("tail").toBool(false) ? PATH_PATTERN_END : PATH_PATTERN_REPEAT;
		const QString role = entry.value("role").toString();
		for (int r = 0; r < 3; ++r)
		{
			if (role == QString::fromUtf8(ROLE_KEYS[r]))
				item.role = PathPatternRole(r);
		}
		if (item.asset.isValid())
			pattern_.append(item);
	}
	keep_roles_in_order(pattern_);
}

void PathPlacer::save_settings(bool undoable)
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
	json.insert("ground_types", settings_.ground.surface_types);
	json.insert("ground_immovable", settings_.ground.only_immovable);
	json.insert("ground_intersection", settings_.ground.only_intersection);

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
		entry.insert("role", QString::fromUtf8(ROLE_KEYS[item.role]));
		// For the builds before 1.3: they know the end as the tail.
		entry.insert("tail", item.role == PATH_PATTERN_END);
		pattern.append(entry);
	}
	json.insert("pattern", pattern);

	// Which settings change: changes of the same ones made one right after another (a
	// value dragged in a field) are one step of the undo history, of other ones not.
	const QJsonObject previous = QJsonDocument::fromJson(saved_settings_).object();
	QStringList changed;
	for (auto it = json.constBegin(); it != json.constEnd(); ++it)
	{
		if (previous.value(it.key()) != it.value())
			changed.append(it.key());
	}
	changed.sort();

	saved_settings_ = QJsonDocument(json).toJson(QJsonDocument::Compact);
	if (undoable)
	{
		SetNodeDataAction *action = new SetNodeDataAction;
		action->setMergeKey(changed.join(QLatin1Char(',')));
		action->set(root_, SETTINGS_DATA, saved_settings_);
		SetNodeDataAction::push(action);
	} else
		root_->setData(SETTINGS_DATA, saved_settings_.constData());
}

////////////////////////////////////////////////////////////////////////////////
// Points.
////////////////////////////////////////////////////////////////////////////////
CurvePointsState PathPlacer::getSelectedPointsState() const
{
	return editor_.getSelectedPointsState();
}

void PathPlacer::setSelectedPointsCorner(bool corner)
{
	editor_.setSelectedPointsCorner(corner);
}

bool PathPlacer::canSubdivide() const
{
	return editor_.canSubdivide();
}

void PathPlacer::subdivide()
{
	editor_.subdivide();
}

bool PathPlacer::eventFilter(QObject *watched, QEvent *event)
{
	if (!active_ || !hasPath())
		return false;

	if (event->type() == QEvent::KeyPress && isAddingPoints()
		&& static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape && isEscapeFree(watched))
	{
		setAddingPoints(false);
		return true;
	}
	return editor_.handleEvent(watched, event);
}

////////////////////////////////////////////////////////////////////////////////
// Curve.
////////////////////////////////////////////////////////////////////////////////
CurvePolyline PathPlacer::build_curve() const
{
	return buildPolyline(CurvePoints::collect(path_node(), settings_.closed), CURVE_SUBDIVISIONS);
}

// Lays objects of the given lengths one after another from the start of the curve.
// An object that the curve ends inside of still gets a place (it sticks out past the
// end), unless only the ones that fit entirely are asked for; the rest get none.
QVector<PathPlacer::Slot> PathPlacer::compute_slots(const CurvePolyline &curve, const QVector<float> &lengths,
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

			slot.end = slot.start + unitOrZero(direction);
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
	const SurfaceHit hit = raycastSurface(above, below, settings_.ground, ignore);
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

void PathPlacer::rearrange(bool undoable)
{
	const NodePtr objects = objects_node();
	if (!objects)
		return;

	const CurvePolyline curve = build_curve();
	length_ = curve.length;

	const QVector<NodePtr> ordered = objects_in_path_order(objects);
	const int count = int(ordered.size());
	QVector<float> lengths(count);
	for (int i = 0; i < count; ++i)
		lengths[i] = object_length(ordered[i]);

	const QVector<Slot> placed = compute_slots(curve, lengths, false);
	// The objects moved, as one step the user can undo, with the scales they remember.
	Vector<NodePtr> moved;
	Vector<Math::Mat4> moved_to;
	SetNodeDataAction *scales = undoable ? new SetNodeDataAction : nullptr;
	const auto set_scale = [&scales](const NodePtr &object, const QByteArray &value) {
		if (scales)
			scales->set(object, SCALE_DATA, value);
		else
			object->setData(SCALE_DATA, value.constData());
	};
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
				set_scale(object, QByteArray::number(double(scale.x)).append(' ')
					.append(QByteArray::number(double(scale.y))).append(' ')
					.append(QByteArray::number(double(scale.z))));
			}
		} else if (saved_scale && *saved_scale)
			set_scale(object, QByteArray());

		const Math::Mat4 transform = slot_transform(slot, lengths[i], scale);
		if (!undoable)
			object->setWorldTransform(transform);
		else if (object->getWorldTransform() != transform)
		{
			moved.append(object);
			moved_to.append(transform);
		}
	}
	if (scales && (!moved.empty() || !scales->isEmpty()))
	{
		using namespace ::UnigineEditor;
		Undo::begin();
		SetNodeDataAction::push(scales, false);
		scales = nullptr;
		for (int i = 0; i < moved.size(); ++i)
			Undo::apply(new SetNodeTransformAction(moved[i], moved_to[i]));
		Undo::commit();
	}
	delete scales;

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

	putBytes(state, double(isOldCurve()));
	putBytes(state, double(settings_.step));
	putBytes(state, double(settings_.forward_axis));
	putBytes(state, double(settings_.up_axis));
	putBytes(state, double(settings_.closed));
	putBytes(state, double(settings_.yaw_only));
	putBytes(state, double(settings_.skew));
	putBytes(state, double(settings_.snap_to_ground));
	putBytes(state, double(settings_.ground_offset));
	putBytes(state, double(settings_.ground.surface_types));
	putBytes(state, double(settings_.ground.only_immovable));
	putBytes(state, double(settings_.ground.only_intersection));

	for (int i = 0, num = path->getNumChildren(); i < num; ++i)
	{
		const NodePtr point = path->getChild(i);
		putBytes(state, point->getWorldPosition());
		putBytes(state, double(isCornerPoint(point)));
	}
	for (int i = 0, num = objects->getNumChildren(); i < num; ++i)
	{
		const NodePtr object = objects->getChild(i);
		putBytes(state, double(object->getID()));
		putBytes(state, double(object_length(object)));
		putBytes(state, object->getWorldTransform());
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

	// A path the tool has removed is still in the world while it is being removed:
	// the list of paths is built anew in a later frame, when it is gone (or a few
	// frames later anyway, should the editor keep the node alive for its undo).
	if (removed_root_ && (removed_root_.isDeleted() || ++removed_root_frames_ >= 3))
	{
		removed_root_ = NodePtr();
		if (on_paths_changed)
			on_paths_changed();
	}

	// The path was deleted or its world closed.
	if (root_ && !hasPath())
	{
		root_ = NodePtr();
		// A point may have been held at that moment: the drag is over, and the
		// manipulators of the editor it had turned off come back.
		editor_.setRoot(NodePtr());
		arranged_state_.clear();
		length_ = 0.0;
		unplaced_ = 0;
		setAddingPoints(false);
		notify_changed();
	}
	if (!hasPath())
		return;

	// The settings in the path are not those last written or read: an undo or a redo
	// has put others there.
	{
		const char *data = root_->getData(SETTINGS_DATA);
		if (QByteArray(data ? data : "") != saved_settings_)
		{
			load_settings();
			editor_.setClosed(settings_.closed);
			notify_changed();
		}
	}

	editor_.update();

	const CurvePolyline curve = build_curve();
	draw(curve);

	const QByteArray state = snapshot();
	if (state == arranged_state_)
		return;

	if (live_update_)
	{
		rearrange(rearrange_undoable_);
		rearrange_undoable_ = false;
	} else
	{
		// Only the numbers shown on the page are refreshed.
		length_ = curve.length;
		arranged_state_ = state;
	}
	notify_changed();
}

void PathPlacer::draw(const CurvePolyline &curve) const
{
	const Math::vec4 line_color(1.0f, 1.0f, 1.0f, 1.0f);
	for (int i = 0; i + 1 < curve.samples.size(); ++i)
		Visualizer::renderLine3D(curve.samples[i], curve.samples[i + 1], line_color, 0.0f, false);

	editor_.drawPoints();
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
	// New assets join the repeated part, above the end.
	keep_roles_in_order(pattern_);
	save_settings();
	report(MSG_INFO, QString());
	notify_changed();
	return true;
}

bool PathPlacer::replacePatternAsset(int index, const Vector<UGUID> &guids)
{
	if (index < 0 || index >= pattern_.size())
		return false;
	PlaceableAsset asset;
	if (!resolveSelection(guids, asset))
	{
		report(MSG_WARNING, uiText("That can not be placed: pick a .mesh, a .node or a model.",
			"Это нельзя расставить: выберите .mesh, .node или модель."));
		return false;
	}
	pattern_[index].asset = asset;
	pattern_[index].length = measure_length(asset);
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
	// The repeated entries, the start and the end do not mix.
	if (pattern_[index].role != pattern_[target].role)
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
	// The start and the end are placed once.
	pattern_[index].count = pattern_[index].role != PATH_PATTERN_REPEAT ? 1 : qBound(1, count, MAX_PATTERN_COUNT);
	save_settings();
	notify_changed();
}

void PathPlacer::setPatternRole(int index, PathPatternRole role)
{
	if (index < 0 || index >= pattern_.size())
		return;
	// One start and one end: the entry that had the role is repeated now.
	if (role != PATH_PATTERN_REPEAT)
	{
		for (int i = 0; i < pattern_.size(); ++i)
		{
			if (i != index && pattern_[i].role == role)
				pattern_[i].role = PATH_PATTERN_REPEAT;
		}
	}
	pattern_[index].role = role;
	keep_roles_in_order(pattern_);
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

	// The start, then the repeated part: as many objects as fit the path after it.
	const QVector<int> start = pattern_sequence(pattern_, PATH_PATTERN_START);
	const QVector<int> repeated = pattern_sequence(pattern_, PATH_PATTERN_REPEAT);
	// An item shorter than the minimum takes no length, the same as in compute_slots():
	// such items do not add up to a length that could be repeated along the path.
	float repeated_length = 0.0f;
	for (int index : repeated)
	{
		if (pattern_[index].length >= MIN_OBJECT_LENGTH)
			repeated_length += pattern_[index].length;
	}

	result = start;
	if (repeated.isEmpty())
	{
		// Only the start and the end.
	} else if (repeated_length < MIN_OBJECT_LENGTH)
	{
		// Nothing of the repeated part takes any length: repeating it would never get
		// along the path, so it is placed once, after the start.
		result += repeated;
	} else
	{
		// The start takes its length of the path first.
		QVector<float> lengths;
		for (int index : start)
			lengths.append(pattern_[index].length);
		for (int i = 0; i < MAX_FILL_OBJECTS; ++i)
			lengths.append(pattern_[repeated[i % repeated.size()]].length);

		const int fitting = qMax(0, int(compute_slots(build_curve(), lengths, true).size()) - int(start.size()));
		for (int i = 0; i < fitting; ++i)
			result.append(repeated[i % repeated.size()]);
		if (cut)
			*cut = fitting >= MAX_FILL_OBJECTS;
	}

	// The end: once, right after the last repeated object. However the sequence was
	// made, it has no more than MAX_FILL_OBJECTS objects: the end is kept, what does
	// not fit before it is cut.
	const QVector<int> end = pattern_sequence(pattern_, PATH_PATTERN_END);
	const int before_end = qMax(0, MAX_FILL_OBJECTS - int(end.size()));
	if (result.size() > before_end)
	{
		result.resize(before_end);
		if (cut)
			*cut = true;
	}
	result += end;
	if (result.size() > MAX_FILL_OBJECTS)
		result.resize(MAX_FILL_OBJECTS);
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

	// The new objects are made before the old ones are taken away: if any of them can
	// not be made (an asset of the pattern is gone from the project), the path keeps
	// what it has, rather than get a part of the pattern only.
	Vector<NodePtr> created;
	for (int i = 0; i < count; ++i)
	{
		const PathPatternItem &item = pattern_[sequence[i]];
		NodePtr node = createNode(item.asset);
		if (!node)
		{
			for (const NodePtr &made : created)
				made.deleteLater();
			report(MSG_ERROR, uiText("Could not create an object from %1. The path is left as it was.",
				"Не удалось создать объект из %1. Кривая оставлена как была.").arg(item.asset.name));
			return false;
		}

		node->setData(LENGTH_DATA, QByteArray::number(double(item.length)).constData());
		if (item.role == PATH_PATTERN_END)
			node->setData(TAIL_DATA, "1");
		else if (item.role == PATH_PATTERN_START)
			node->setData(HEAD_DATA, "1");
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
