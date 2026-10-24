#pragma once

#include "CurveEditor.h"
#include "PlaceableAsset.h"
#include "ViewportPicking.h"

#include <UnigineCallback.h>
#include <UnigineMathLib.h>
#include <UnigineNode.h>
#include <UnigineVisualizer.h>

#include <QByteArray>
#include <QObject>
#include <QString>
#include <QVector>

#include <functional>

class QWidget;

namespace ArtistTool
{

// Direction of a local axis of an object. The values are the ids of the axis buttons.
enum AxisDirection
{
	AXIS_X_POSITIVE = 0,
	AXIS_X_NEGATIVE,
	AXIS_Y_POSITIVE,
	AXIS_Y_NEGATIVE,
	AXIS_Z_POSITIVE,
	AXIS_Z_NEGATIVE,
};

// How the objects stand along a path. Stored in the root node of the path.
struct PathSettings
{
	// Length of the path an object takes if it has no length of its own, in units.
	float step{1.0f};
	// Which axis of an object looks along the path and which one up.
	AxisDirection forward_axis{AXIS_Y_POSITIVE};
	AxisDirection up_axis{AXIS_Z_POSITIVE};
	// The last point is joined to the first one.
	bool closed{false};
	// Objects stay upright: they turn along the path but do not lean on slopes.
	bool yaw_only{true};
	// Objects are sheared along the slope instead of being tilted: the verticals stay
	// vertical and every object ends exactly where the next one starts (fences).
	bool skew{false};
	// Every object is dropped onto the surface under the path.
	bool snap_to_ground{false};
	// How far above that surface the objects are put, in units (negative - below it).
	// Straight up, not along the normal of the surface: the objects stay under the
	// curve and the sections of a fence stay joined on a slope.
	float ground_offset{0.0f};
	// What the objects and the points are put on; everything else is looked through
	// (a fence over rocks stands on the terrain under them).
	SurfaceFilter ground;
};

// A path found in the world.
struct PathInfo
{
	// ID of its root node.
	int id{0};
	QString name;
	int points{0};
	int objects{0};
};

// What an entry of the fill pattern is.
enum PathPatternRole
{
	// A part of what is repeated along the path.
	PATH_PATTERN_REPEAT = 0,
	// Placed once, first, at the start of the path (the post a fence starts with).
	PATH_PATTERN_START,
	// Placed once, after the last repeated object (the post that closes a fence): the
	// "tail" of the builds before.
	PATH_PATTERN_END,
};

// One entry of the fill pattern: an asset, the length of the path it takes and how
// many times in a row it is placed before the next entry.
struct PathPatternItem
{
	PlaceableAsset asset;
	float length{1.0f};
	int count{1};
	// There is at most one start entry, kept first in the pattern, and one end entry,
	// kept last; both are placed once.
	PathPatternRole role{PATH_PATTERN_REPEAT};
};

// Places objects along a curve drawn through points.
//
// The curve lives in the world as ordinary nodes, so it is saved with it and can be
// returned to later:
//   <root>
//     path      - its children are the points of the curve, in order
//     objects   - its children are placed along the curve, in order
// This is the structure of the old expressions/put_objects_along_path.usc setups,
// which are therefore picked up as they are. The settings are kept in the data of
// the root node, the "corner" flag of a point and the own length of an object - in
// the data of those nodes.
//
// The curve is smooth by default; a point marked as a corner makes a sharp bend.
// Objects are spaced by the straight distance between their start points, so rigid
// pieces of a given length (fence sections) join end to start.
class PathPlacer final : public QObject
{
public:
	enum MessageLevel
	{
		MSG_INFO = 0,
		MSG_WARNING,
		MSG_ERROR,
	};

	explicit PathPlacer(QObject *parent = nullptr);
	~PathPlacer() override;

	// The tool works (draws the curve, keeps the objects on it) only while active.
	void setActive(bool active);
	// Makes the path the selected node belongs to the current one. A selection
	// outside of any path leaves the current path as it is.
	void updateFromSelection();

	// All the paths of the world, sorted by name. The whole world is looked through,
	// so it is not for calling every frame.
	QVector<PathInfo> findPaths() const;
	// Makes the path with this root node the current one and selects the root in the
	// editor. False if there is no such path any more.
	bool selectPath(int root_id);
	// ID of the root node of the current path, 0 if there is none.
	int getPathId() const;

	bool hasPath() const;
	QString getPathName() const;
	int getNumPoints() const;
	int getNumObjects() const;
	// Objects at the end of the list the path is too short for.
	int getNumUnplaced() const { return unplaced_; }
	double getLength() const { return length_; }

	// A path made with the old script still has its WorldExpression node.
	bool hasOldScript() const;
	void removeOldScript();

	// The node new paths (and the copies of paths) are put into; none - the root of the
	// world. It can not be a part of a path.
	bool setParentNode(const Unigine::NodePtr &node);
	Unigine::NodePtr getParentNode() const;

	void createPath();
	// A copy of the current path - its points, settings and pattern, no objects -
	// becomes the current one.
	bool clonePath();
	// Removes the curve of the current path and keeps its objects: they go on as
	// ordinary nodes, in a node of the name of the path.
	bool removePathCurve();
	// Removes the current path with its objects.
	bool removePath();
	// While on, a left click on a surface in a viewport adds a point at the end of
	// the path, whatever is selected; with Ctrl it is a corner. A path made by createPath() that is left
	// without points when this is turned off is removed.
	// Whether it is on or not, a click on a point of the path selects it, and holding
	// the button drags it over the surfaces.
	void setAddingPoints(bool adding);
	bool isAddingPoints() const { return editor_.isAddingPoints(); }

	const PathSettings &getSettings() const { return settings_; }
	void setSettings(const PathSettings &settings);

	// With live update on the objects follow every change of the path; with it off
	// they are put in place only by rearrange().
	void setLiveUpdate(bool live);
	bool isLiveUpdate() const { return live_update_; }
	// Puts the objects in place; 'undoable' - as one step of the undo history (Rearrange
	// Now), else as the live update does it, on top of the step that moved the path.
	void rearrange(bool undoable = false);

	CurvePointsState getSelectedPointsState() const;
	void setSelectedPointsCorner(bool corner);
	// Puts a new point in the middle of every span whose both ends are selected.
	bool canSubdivide() const;
	void subdivide();

	const QVector<PathPatternItem> &getPattern() const { return pattern_; }
	// Adds the asset selected in the Asset Browser to the end of the pattern.
	// Adds all the assets selected in the Asset Browser to the end of the pattern.
	bool addPatternAssets();
	// Puts another asset into the entry (picked in the asset dialog or dropped onto it); its
	// role and count stay, the length is measured again.
	bool replacePatternAsset(int index, const Unigine::Vector<Unigine::UGUID> &guids);
	void removePatternItem(int index);
	// Moves an entry of the pattern up (offset -1) or down (+1) the list.
	void movePatternItem(int index, int offset);
	void setPatternLength(int index, float length);
	void setPatternCount(int index, int count);
	// Makes the entry the start, the end or a repeated one. The entry that had that
	// role before becomes a repeated one.
	void setPatternRole(int index, PathPatternRole role);
	void clearPattern();
	// How many objects fill() would create.
	int countFill() const;
	// Fills the whole path with the pattern, repeated. One-off: the objects it
	// creates are ordinary children of "objects" and can be edited afterwards.
	// The objects that were there before are removed.
	bool fill();

	// Called when the path, the selection of points, the settings or the pattern
	// change.
	std::function<void()> on_changed;
	std::function<void(int level, const QString &text)> on_message;
	// Called when a path the tool has removed is gone from the world, so that a list
	// of the paths can be built anew.
	std::function<void()> on_paths_changed;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	// Where an object starts and where the next one does.
	struct Slot
	{
		Unigine::Math::Vec3 start;
		Unigine::Math::Vec3 end;
		// The object takes no length of the path (a post between two sections): 'end'
		// is then one unit from 'start' the way the path goes on, only to turn it.
		bool no_length{false};
	};

	Unigine::NodePtr path_node() const;
	Unigine::NodePtr objects_node() const;
	Unigine::NodePtr old_script_node() const;

	void set_path(const Unigine::NodePtr &root);
	// The current path is gone from the world by the tool: it is forgotten and the list
	// of the paths is told once it is gone.
	void forget_removed_path(const Unigine::NodePtr &root);
	void load_settings();
	// With 'undoable' - as a step of the undo history (see SetNodeDataAction).
	void save_settings(bool undoable = true);

	CurvePolyline build_curve() const;
	QVector<Slot> compute_slots(const CurvePolyline &curve, const QVector<float> &lengths, bool only_fitting) const;
	Unigine::Math::Mat4 slot_transform(const Slot &slot, float length, const Unigine::Math::vec3 &scale) const;
	Unigine::Math::Vec3 snap_to_ground(const Unigine::Math::Vec3 &point) const;
	float object_length(const Unigine::NodePtr &object) const;
	float measure_length(const PlaceableAsset &asset) const;
	// The entries of the pattern a fill places, in order: the start, the repeated ones
	// as many times as fit the path, then the end.
	// 'cut' tells that the repeated part was stopped by the limit of objects of one
	// fill before it got to the end of the path.
	QVector<int> fill_sequence(bool *cut = nullptr) const;

	void update();
	void draw(const CurvePolyline &curve) const;
	QByteArray snapshot() const;

	void notify_changed() const;
	void report(int level, const QString &text) const;

	Unigine::NodePtr root_;
	Unigine::NodePtr parent_;
	PathSettings settings_;
	QVector<PathPatternItem> pattern_;

	// Drawing and moving the points of the curve.
	CurveEditor editor_;

	bool active_{false};
	bool live_update_{true};
	// The next rearrange of Live Update is a step of the undo history (it was turned on).
	bool rearrange_undoable_{false};
	// The current path has just been made by createPath(): if adding points to it ends
	// with no points, it is removed.
	bool discard_if_empty_{false};
	// Such a path after it has been removed, until it is gone from the world.
	Unigine::NodePtr removed_root_;
	int removed_root_frames_{0};

	// What the objects were last arranged for; they are rearranged when it changes.
	QByteArray arranged_state_;
	// The settings as last written into the path or read from it: when the path has
	// others (an undo has put the old ones back), they are read again.
	QByteArray saved_settings_;
	double length_{0.0};
	int unplaced_{0};

	Unigine::EventConnections connections_;
};

} // namespace ArtistTool
