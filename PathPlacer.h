#pragma once

#include "PlaceableAsset.h"
#include "ViewportPicking.h"

#include <UnigineCallback.h>
#include <UnigineMathLib.h>
#include <UnigineNode.h>
#include <UnigineVisualizer.h>

#include <QByteArray>
#include <QElapsedTimer>
#include <QObject>
#include <QPoint>
#include <QPointF>
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

// One entry of the fill pattern: an asset, the length of the path it takes and how
// many times in a row it is placed before the next entry.
struct PathPatternItem
{
	PlaceableAsset asset;
	float length{1.0f};
	int count{1};
	// The tail entry is not a part of what is repeated along the path: it is placed
	// once, after the last repeated object (the post that closes a fence). There is at
	// most one tail entry, it is kept at the end of the pattern and its count is 1.
	bool tail{false};
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

	// What the points selected in the editor are.
	enum PointsState
	{
		POINTS_NONE = 0,
		POINTS_SMOOTH,
		POINTS_CORNER,
		POINTS_MIXED,
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

	void createPath();
	// While on, a left click on a surface in a viewport adds a point at the end of
	// the path, whatever is selected; with Ctrl it is a corner. A path made by createPath() that is left
	// without points when this is turned off is removed.
	// Whether it is on or not, a click on a point of the path selects it, and holding
	// the button drags it over the surfaces.
	void setAddingPoints(bool adding);
	bool isAddingPoints() const { return adding_points_; }

	const PathSettings &getSettings() const { return settings_; }
	void setSettings(const PathSettings &settings);

	// With live update on the objects follow every change of the path; with it off
	// they are put in place only by rearrange().
	void setLiveUpdate(bool live);
	bool isLiveUpdate() const { return live_update_; }
	void rearrange();

	PointsState getSelectedPointsState() const;
	void setSelectedPointsCorner(bool corner);
	// Puts a new point in the middle of every span whose both ends are selected.
	bool canSubdivide() const;
	void subdivide();

	const QVector<PathPatternItem> &getPattern() const { return pattern_; }
	// Adds the asset selected in the Asset Browser to the end of the pattern.
	// Adds all the assets selected in the Asset Browser to the end of the pattern.
	bool addPatternAssets();
	void removePatternItem(int index);
	// Moves an entry of the pattern up (offset -1) or down (+1) the list.
	void movePatternItem(int index, int offset);
	void setPatternLength(int index, float length);
	void setPatternCount(int index, int count);
	void setPatternTail(int index, bool tail);
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
	// The curve as a polyline.
	struct Curve
	{
		QVector<Unigine::Math::Vec3> samples;
		double length{0.0};
	};

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
	Unigine::Vector<Unigine::NodePtr> selected_points() const;

	void set_path(const Unigine::NodePtr &root);
	void load_settings();
	void save_settings() const;

	Curve build_curve() const;
	QVector<Slot> compute_slots(const Curve &curve, const QVector<float> &lengths, bool only_fitting) const;
	Unigine::Math::Mat4 slot_transform(const Slot &slot, float length, const Unigine::Math::vec3 &scale) const;
	Unigine::Math::Vec3 snap_to_ground(const Unigine::Math::Vec3 &point) const;
	float object_length(const Unigine::NodePtr &object) const;
	float measure_length(const PlaceableAsset &asset) const;
	// The entries of the pattern a fill places, in order: the repeated ones as many
	// times as fit the path, then the tail ones.
	// 'cut' tells that the repeated part was stopped by the limit of objects of one
	// fill before it got to the end of the path.
	QVector<int> fill_sequence(bool *cut = nullptr) const;

	void update();
	void draw(const Curve &curve) const;
	QByteArray snapshot() const;
	Unigine::NodePtr pick_point(const Unigine::Math::Vec3 &p0, const Unigine::Math::Vec3 &p1) const;
	Unigine::NodePtr add_point(const Unigine::Math::Vec3 &position, bool corner);
	void select_point(const Unigine::NodePtr &point, bool add_to_selection);
	void begin_drag(const Unigine::NodePtr &point, bool is_new);
	void update_drag();
	void end_drag();
	void finish_click();
	bool is_point_selected(const Unigine::NodePtr &point) const;

	void notify_changed() const;
	void report(int level, const QString &text) const;

	Unigine::NodePtr root_;
	PathSettings settings_;
	QVector<PathPatternItem> pattern_;

	bool active_{false};
	bool live_update_{true};
	bool adding_points_{false};
	// The current path has just been made by createPath(): if adding points to it ends
	// with no points, it is removed.
	bool discard_if_empty_{false};
	// Such a path after it has been removed, until it is gone from the world.
	Unigine::NodePtr removed_root_;
	int removed_root_frames_{0};
	// The press of the click in progress was taken by the tool; whether its release
	// is to be kept from the editor as well.
	bool click_taken_{false};
	bool swallow_release_{false};
	// The manipulators of the editor are switched off while a point is dragged, so
	// that they do not move it at the same time.
	bool manipulators_disabled_{false};

	// The point that follows the mouse while the button that picked it is held. It
	// starts to move only after the button has been held for a moment and the mouse
	// has left the place of the press, so that a mere click does not shift it.
	Unigine::NodePtr drag_point_;
	Unigine::Math::Mat4 drag_start_transform_;
	bool drag_is_new_{false};
	bool drag_moving_{false};
	QElapsedTimer drag_timer_;
	QPoint drag_press_position_;

	// The point to select when the button of the click that picked it is released.
	// The selection is not changed while the button is down: the editor could take
	// that for the start of a move of the newly selected node with its own tools.
	Unigine::NodePtr pending_selection_;
	bool pending_selection_adds_{false};

	// What the objects were last arranged for; they are rearranged when it changes.
	QByteArray arranged_state_;
	double length_{0.0};
	int unplaced_{0};

	Unigine::EventConnections connections_;
	Unigine::Visualizer::MODE saved_visualizer_mode_{Unigine::Visualizer::MODE_DISABLED};
};

} // namespace ArtistTool
