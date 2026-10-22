#pragma once

#include "ViewportPicking.h"

#include <UnigineMathLib.h>
#include <UnigineNode.h>
#include <UnigineVector.h>

#include <QElapsedTimer>
#include <QPoint>
#include <QString>
#include <QByteArray>
#include <QVector>

#include <functional>

class QEvent;
class QObject;

// The curve the tools draw through points, and the editing of its points in the
// viewport. The Path Placer and the Road Builder share it, so a curve is drawn and
// edited the same way in both.
//
// The points are ordinary nodes, the children of a node named "path" in the root of
// the tool's setup, in order. The "corner" flag of a point is kept in its data.
namespace ArtistTool
{

extern const char CURVE_PATH_NODE_NAME[];
extern const char CURVE_POINT_NODE_NAME[];
extern const char CURVE_CORNER_DATA[];
// "1" on a point the road keeps to the height of the curve at, not to the ground.
extern const char CURVE_OFF_GROUND_DATA[];
// The width of a point of a road, a multiplier of its Width. None - 1.
extern const char CURVE_WIDTH_DATA[];

// The child of 'node' with the name; null if there is none.
Unigine::NodePtr findChildNode(const Unigine::NodePtr &node, const char *name);
bool isCornerPoint(const Unigine::NodePtr &point);
// Whether the flag kept in the data of the point with that name is on ("1").
bool isPointFlag(const Unigine::NodePtr &point, const char *data);
// The vector made one unit long; zero for a vector of no length.
Unigine::Math::Vec3 unitOrZero(const Unigine::Math::Vec3 &v);
// Radius of the sphere a point of a curve is drawn with: it nearly keeps its size on
// the screen.
float curvePointRadius(const Unigine::Math::Vec3 &position);
// How much the point is turned about the curve, in degrees: the angle its up axis
// leans to the right of the way the curve goes ('direction'). A turn about the
// vertical does not count.
double curvePointRoll(const Unigine::NodePtr &point, const Unigine::Math::Vec3 &direction);
// The rotation of a point that looks along the curve: its Y axis the way the curve
// goes ('direction', level), X to the right, Z up, all turned about Y by 'roll'
// degrees. With the manipulator of the editor in its local basis, the Y ring of such
// a point turns the roll alone.
Unigine::Math::mat4 curvePointBasis(const Unigine::Math::Vec3 &direction, double roll);

// The curve of the builds before 1.3, for comparing: at every point the direction
// is halfway between those to its two neighbours, the handles a third of the span.
// It makes waves where a point is added between two others. A switch of the Debug
// tab, remembered on the machine.
void setOldCurve(bool old);
bool isOldCurve();

// The points of a curve, in order, and the curve through them: a cubic Bezier per
// span. The way it goes at every point is that of a smooth spline through all the
// points (a natural cubic spline over the lengths of the spans) - no waves at the
// points, so a point added on the curve hardly changes it; the handles are a third
// of the span, which keeps the turns round. A corner point cuts the curve into
// pieces that are each such a spline; it has a sharp bend between them.
struct CurvePoints
{
	QVector<Unigine::Math::Vec3> positions;
	QVector<bool> corners;
	// The way the curve comes to every point and leaves it, per unit of length (the
	// two differ at a corner only); found by collect().
	QVector<Unigine::Math::Vec3> incoming;
	QVector<Unigine::Math::Vec3> outgoing;
	QVector<bool> off_ground;
	// See curvePointRoll().
	QVector<double> rolls;
	bool closed{false};

	// The points of the "path" node; a closed curve needs at least three of them.
	static CurvePoints collect(const Unigine::NodePtr &path, bool closed);

	int count() const { return int(positions.size()); }
	// Spans between neighbouring points: the last one of a closed curve goes back to
	// the first point.
	int spans() const { return count() < 2 ? 0 : (closed ? count() : count() - 1); }
	// The way the curve goes through a point, a unit vector; for a corner too, where
	// it is halfway between the way it comes and the way it leaves.
	Unigine::Math::Vec3 direction(int point) const;
	// The place 't' (0..1) of the span that starts at the point 'span'.
	Unigine::Math::Vec3 spanPoint(int span, double t) const;
};

// The curve as a polyline: every span cut into 'subdivisions' pieces.
struct CurvePolyline
{
	QVector<Unigine::Math::Vec3> samples;
	double length{0.0};
};
CurvePolyline buildPolyline(const CurvePoints &points, int subdivisions);

// What the points selected in the editor are.
enum CurvePointsState
{
	POINTS_NONE = 0,
	POINTS_SMOOTH,
	POINTS_CORNER,
	POINTS_MIXED,
};

// Edits the points of one curve with the mouse in the viewports:
//  - a click on a point selects it, Ctrl or Shift add it to the selection;
//  - holding the button on a point drags it over the surfaces;
//  - while adding points is on, a click on a surface adds a point at the end of the
//    curve (with Ctrl - a corner one) and holding the button drags it.
// The owner gives it the events of the application (handleEvent) and calls update()
// every frame while its tool is active.
class CurveEditor
{
public:
	enum MessageLevel
	{
		MSG_INFO = 0,
		MSG_WARNING,
		MSG_ERROR,
	};

	CurveEditor();
	~CurveEditor();

	// Shows the manipulators of the editor again if a curve editor has hidden them and
	// is not there to show them any more (the plugin was reloaded meanwhile). Called
	// when a tool with a curve is turned on.
	static void recoverManipulators();

	// The root of the setup: the raycasts look through it and everything in it; its
	// child "path" holds the points.
	void setRoot(const Unigine::NodePtr &root);
	Unigine::NodePtr getPathNode() const;
	void setClosed(bool closed) { closed_ = closed; }
	// What the points are put on when they are added or dragged.
	void setSurfaceFilter(const SurfaceFilter &filter) { filter_ = filter; }
	// A node outside of the root the points are not put on either: the preview of
	// what the tool is going to make, which moves with the points.
	void setLookThrough(const Unigine::NodePtr &node) { look_through_ = node; }
	CurvePoints getPoints() const;

	// While on, a left click on a surface adds a point at the end of the curve. The
	// manipulator of the editor is hidden meanwhile: a click on its arrows would add
	// a point instead of moving the selected one (the editor does not tell what is
	// under the mouse), so it is not shown at all; the points are dragged with the
	// mouse instead.
	void setAddingPoints(bool adding);
	bool isAddingPoints() const { return adding_points_; }

	// Ends a drag and forgets the click in progress: the tool is turned off or its
	// curve is gone.
	void reset();
	// Every frame: the point that is held follows the mouse.
	void update();
	// The clicks on the points of the curve, and on the surfaces while points are
	// added. True if the event is taken by the editor and must not go further.
	bool handleEvent(QObject *watched, QEvent *event);
	// The points as spheres: the selected ones orange, the last one green, corners
	// white, smooth points blue.
	void drawPoints() const;

	Unigine::Vector<Unigine::NodePtr> getSelectedPoints() const;
	CurvePointsState getSelectedPointsState() const;
	void setSelectedPointsCorner(bool corner);
	// The same for any flag kept in the data of the points: how many of the selected
	// points have it on and off, and turning it on or off for all of them.
	void countSelectedPointsFlag(const char *data, int &on, int &off) const;
	void setSelectedPointsFlag(const char *data, bool on);
	// The names of the data a point keeps for the span from it to the next point: a point
	// put into the span by subdivide() gets them from the point the span starts at.
	void setSpanData(const QVector<QByteArray> &names) { span_data_ = names; }
	// Puts a new point in the middle of every span whose both ends are selected.
	bool canSubdivide() const;
	void subdivide();

	// Called when the points or the selection of points change.
	std::function<void()> on_changed;
	std::function<void(int level, const QString &text)> on_message;

private:
	Unigine::NodePtr pick_point(const Unigine::Math::Vec3 &p0, const Unigine::Math::Vec3 &p1) const;
	Unigine::NodePtr add_point(const Unigine::Math::Vec3 &position, bool corner);
	void select_point(const Unigine::NodePtr &point, bool add_to_selection);
	void begin_drag(const Unigine::NodePtr &point, bool is_new);
	void update_drag();
	void end_drag();
	void finish_click();
	bool is_point_selected(const Unigine::NodePtr &point) const;

	// The manipulators of the editor are hidden while points are added and while a
	// point is dragged (they must not move it at the same time), and shown again
	// otherwise.
	void update_manipulators() const;
	void notify_changed() const;
	void report(int level, const QString &text) const;

	Unigine::NodePtr root_;
	bool closed_{false};
	SurfaceFilter filter_;
	Unigine::NodePtr look_through_;
	QVector<QByteArray> span_data_;
	bool adding_points_{false};
	// The press of the click in progress was taken by the tool; whether its release
	// is to be kept from the editor as well.
	bool click_taken_{false};
	bool swallow_release_{false};

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
};

} // namespace ArtistTool
