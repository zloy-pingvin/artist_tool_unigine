#pragma once

#include "ViewportPicking.h"

#include <UnigineCallback.h>
#include <UnigineMathLib.h>
#include <UnigineObjects.h>
#include <UnigineVisualizer.h>

#include <QElapsedTimer>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QString>
#include <QVector>

#include <functional>

class QWheelEvent;
class QWidget;

namespace ArtistTool
{
class Loupe;
}

namespace ArtistTool
{

// Measures distances and angles between points clicked in a viewport, and stands
// figures of a person (1.80 m tall to start with) to judge the scale of things by.
//
// The measurements are temporary: they live only in the tool, nothing is added to the
// world and nothing goes to the undo history. They stay on the screen while the page
// of the tool is open, until they are cleared.
//
// A distance is measured between two points, an angle at the middle one of three, a
// figure stands on its one point.
// A measurement is done as soon as it has its points; to go on from where it ends,
// the next one is started on its last point (the points snap to each other).
class MeasureTool final : public QObject
{
public:
	enum Mode
	{
		MODE_DISTANCE = 0,
		MODE_ANGLE,
		// A figure of a person, to compare the size of things with.
		MODE_FIGURE,
	};

	// How tall the figure of a person is to start with, in units.
	static constexpr double FIGURE_HEIGHT = 1.8;

	// What a click puts the point on.
	enum Snap
	{
		SNAP_SURFACE = 0,
		SNAP_VERTEX,
	};

	// What the next point is held to, from the point before it: a world axis, or the
	// plane of the two other axes.
	enum Lock
	{
		LOCK_NONE = 0,
		LOCK_AXIS,
		LOCK_PLANE,
	};

	struct Measurement
	{
		Mode mode{MODE_DISTANCE};
		QVector<Unigine::Math::Vec3> points;
		bool finished{false};
		// How tall a figure is.
		double height{FIGURE_HEIGHT};
	};

	explicit MeasureTool(QObject *parent = nullptr);
	~MeasureTool() override;

	// The tool takes the clicks only while active.
	void setActive(bool active);
	// Whether the measurements stay on the screen while the tool is not active (another
	// tab of the plugin is open): they are shown, but can not be clicked or moved.
	void setShownInactive(bool shown);
	bool isShownInactive() const { return shown_inactive_; }

	// While on, a left click in a viewport puts a point of a measurement. While off, a
	// click on a measurement shows its numbers. Whether it is on or not, the points
	// of the measurements can be dragged.
	void setMeasuring(bool measuring);
	bool isMeasuring() const { return measuring_; }

	// A measurement being made goes on from its last point in the new mode (a figure
	// is not started from it: the measurement is dropped).
	void setMode(Mode mode);
	Mode getMode() const { return mode_; }
	// Ctrl pressed and let go over the viewport switches the snap.
	void setSnap(Snap snap);
	Snap getSnap() const { return snap_; }
	// Whether a click near a point of a measurement puts the new point right on it.
	// What the points are put on; the other kinds of objects are looked through (a point
	// clicked through dense grass goes onto the terrain under it).
	void setSurfaceFilter(const SurfaceFilter &filter);
	const SurfaceFilter &getSurfaceFilter() const { return filter_; }

	void setPointSnap(bool snap);
	bool isPointSnap() const { return point_snap_; }
	// Whether the magnifier is shown next to the mouse while points are put or
	// dragged.
	void setLoupe(bool loupe);
	bool isLoupe() const { return loupe_enabled_; }
	// How tall the next figure is. The mouse wheel changes it by 5 cm while figures are
	// put (over a figure - the height of that figure); the page puts it back to
	// FIGURE_HEIGHT whenever it is opened.
	void setFigureHeight(double height) { figure_height_ = height; notify_changed(); }
	double getFigureHeight() const { return figure_height_; }

	Lock getLock() const { return lock_; }
	// 0 - X, 1 - Y, 2 - Z: the axis of LOCK_AXIS, the one left out by LOCK_PLANE.
	int getLockAxis() const { return lock_axis_; }

	// The measurement the page shows: the one being made, with the point under the
	// mouse as its next point ('preview'); else the one last clicked or made. False
	// if there is none.
	bool getShown(Measurement &measurement, bool &preview) const;
	bool isMeasurementInProgress() const;
	int getNumMeasurements() const { return measurements_.size(); }

	// Removes the measurement the page shows.
	void deleteShown();
	// Takes the last point off the last measurement, which then goes on from the
	// point before; a measurement left without points is removed.
	void removeLastPoint();
	void clear();

	// How many points a measurement of the mode has.
	static int pointsNeeded(Mode mode);
	// The angle at 'corner' between the directions to 'a' and 'b', in degrees.
	static double angleAt(const Unigine::Math::Vec3 &a, const Unigine::Math::Vec3 &corner,
		const Unigine::Math::Vec3 &b);

	// Whether the window of the plugin is open: the measurements are not shown while
	// it is closed. Not set - always.
	std::function<bool()> is_window_shown;
	// Called when the measurements, the point under the mouse or the modes change.
	std::function<void()> on_changed;
	std::function<void(const QString &text)> on_message;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	// What the point under the mouse is on.
	enum SnapKind
	{
		SNAPPED_NONE = 0,
		SNAPPED_SURFACE,
		SNAPPED_VERTEX,
		// A point of a measurement.
		SNAPPED_POINT,
	};

	// Where the measurement point picked by the mouse is.
	struct PointRef
	{
		int measurement{-1};
		int index{-1};
		bool isValid() const { return measurement >= 0 && index >= 0; }
	};

	struct SnapPoint
	{
		bool found{false};
		// Where the point goes.
		Unigine::Math::Vec3 point;
		// What the mouse points at (a vertex, a point, the surface). With a lock it
		// is not where the point goes: the point is moved to the axis or the plane.
		SnapKind kind{SNAPPED_NONE};
		Unigine::Math::Vec3 target;
		// For a vertex - its object (drawn as a wireframe).
		Unigine::ObjectPtr object;
	};

	// Draws every frame while the tool is active or its measurements are shown anyway.
	void update_drawing();
	void update();
	void update_hover();
	void update_drag();
	void update_loupe();
	void hide_loupe();
	void draw() const;
	void draw_measurement(int index) const;
	void draw_ring(const Unigine::Math::Vec3 &position, float radius, const Unigine::Math::vec4 &color) const;
	void draw_label(const Unigine::Math::Vec3 &position, const QString &text, const Unigine::Math::vec4 &color) const;
	void draw_angle(const Unigine::Math::Vec3 &a, const Unigine::Math::Vec3 &corner, const Unigine::Math::Vec3 &b,
		const Unigine::Math::vec4 &color) const;
	void draw_lock(const Unigine::Math::Vec3 &anchor) const;
	// The figure of a person standing on 'base', turned to the camera.
	void draw_figure(const Unigine::Math::Vec3 &base, double height, const Unigine::Math::vec4 &color) const;
	// The mouse wheel in the Person mode: true if it has been taken.
	bool wheel_figure_height(QObject *watched, const QWheelEvent *wheel);

	// Where a point put by the mouse at 'position' of the viewport goes: the snap
	// ('skip' is a point not snapped to, the dragged one) and the lock from the anchor.
	SnapPoint place_at(QWidget *viewport, const QPointF &position, const PointRef &skip) const;
	// While Shift is held the mouse goes over the viewport three times slower, to aim
	// more exactly. Called every frame; when the tool is not active, it puts the
	// speed back.
	void update_slow_cursor();
	// The point the lock moves 'snap' to, for the mouse ray p0 -> p1.
	bool apply_lock(const Unigine::Math::Vec3 &anchor, const SnapPoint &snap, const Unigine::Math::Vec3 &p0,
		const Unigine::Math::Vec3 &p1, Unigine::Math::Vec3 &result) const;
	// What a lock is measured from: the last point of the measurement being made, or
	// where the dragged point was picked up. False if there is nothing.
	bool lock_anchor(Unigine::Math::Vec3 &anchor) const;
	void set_lock(Lock lock, int axis);

	PointRef pick_point(const Unigine::Math::Vec3 &p0, const Unigine::Math::Vec3 &p1, const PointRef &skip) const;
	// The figure the ray goes through, -1 if none.
	int pick_figure(const Unigine::Math::Vec3 &p0, const Unigine::Math::Vec3 &p1) const;
	// The measurement whose point or line the ray goes through, -1 if none.
	int pick_measurement(const Unigine::Math::Vec3 &p0, const Unigine::Math::Vec3 &p1) const;
	// The measurement being made, -1 if there is none.
	int current_index() const;
	// The measurement the page shows, -1 if there is none.
	int shown_index() const;
	void add_point(const Unigine::Math::Vec3 &point);
	void remove_measurement(int index);
	void finish_click();
	// What the page shows, as text: when it changes, the page is told.
	QString shown_signature() const;
	void notify_changed() const;
	void report(const QString &text) const;

	bool active_{false};
	bool shown_inactive_{false};
	bool drawing_{false};
	bool measuring_{false};
	Mode mode_{MODE_DISTANCE};
	Snap snap_{SNAP_SURFACE};
	SurfaceFilter filter_;
	bool point_snap_{true};
	bool loupe_enabled_{true};
	double figure_height_{FIGURE_HEIGHT};
	// The part of a notch of the mouse wheel not used yet.
	int wheel_rest_{0};
	// Alt or Ctrl has been pressed by itself over the viewport: its release switches
	// the mode or the snap. 0 if none.
	int tap_key_{0};
	// The slow cursor of Shift is on: the pointer speed of Windows it is to get back,
	// or (elsewhere) where the cursor was put last, exactly and on the screen.
	bool slow_active_{false};
	int saved_mouse_speed_{10};
	QPointF slow_exact_;
	QPoint slow_set_;
	// Made when it is first shown.
	Loupe *loupe_{nullptr};
	Lock lock_{LOCK_NONE};
	int lock_axis_{2};

	QVector<Measurement> measurements_;
	// The measurement last clicked or made, -1 if none.
	int selected_{-1};
	SnapPoint hover_;

	// The press of the click in progress was taken by the tool.
	bool click_taken_{false};
	bool swallow_release_{false};
	// Where a click that is still held would put the point, taken when it was
	// pressed: a click on a point that does not drag it puts the point there.
	SnapPoint press_place_;
	// A point held with the mouse. It starts to follow the mouse only after the button
	// has been held for a moment and the mouse has moved, so that a mere click does
	// not move it.
	PointRef drag_;
	Unigine::Math::Vec3 drag_start_;
	bool drag_moving_{false};
	QElapsedTimer drag_timer_;
	QPoint drag_press_position_;

	// What on_changed was last called for.
	mutable QString shown_signature_;

	Unigine::EventConnections connections_;
	Unigine::Visualizer::MODE saved_visualizer_mode_{Unigine::Visualizer::MODE_DISABLED};
};

} // namespace ArtistTool
