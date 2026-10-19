#include "MeasureTool.h"
#include "Localization.h"
#include "Loupe.h"
#include "VertexSnap.h"
#include "ViewportPicking.h"

#include <UnigineEngine.h>
#include <UnigineGui.h>
#include <UniginePlayers.h>
#include <UnigineWorld.h>

#include <QAbstractSpinBox>
#include <QApplication>
#include <QComboBox>
#include <QCursor>
#include <QKeyEvent>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>
#include <cstdint>

#ifdef _WIN32
// From user32 (windows.h is not included for one function: its macros clash with
// the names of the engine).
extern "C" __declspec(dllimport) short __stdcall GetAsyncKeyState(int key);
extern "C" __declspec(dllimport) int __stdcall SystemParametersInfoW(unsigned int action, unsigned int param,
	void *value, unsigned int update);
#endif

using namespace Unigine;

namespace ArtistTool
{

namespace
{

// How long the button must be held on a point before it starts to follow the mouse.
// An ordinary click lasts 80-150 ms.
const int DRAG_DELAY_MS = 200;

// The points are rings that keep nearly the same size on the screen (see
// point_radius() of the Path Placer). They are not filled, so that they do not hide
// what the mouse is aiming at.
const float POINT_SCREEN_RADIUS = 0.007f;
const float POINT_FULL_SIZE_DISTANCE = 5.0f;
const float POINT_DISTANCE_FALLOFF = 0.2f;
const float POINT_MIN_SCREEN_SCALE = 0.35f;
// A click counts as a click on a point a little outside of its ring too.
const float POINT_PICK_SCALE = 2.0f;
// How close to a line of a measurement a click selects it, in parts of the height of
// the viewport.
const float LINE_PICK_SCREEN = 0.01f;
// The radius of the arc that marks an angle, in parts of the height of the viewport,
// and at most this part of the shorter of its two sides.
const float ARC_SCREEN_RADIUS = 0.05f;
const float ARC_MAX_SIDE_PART = 0.4f;
const int ARC_SEGMENTS = 24;
// How far the guide of a lock goes each way, in heights of the viewport.
const float LOCK_GUIDE_SCREEN = 3.0f;
// How many times larger than the default text the numbers in the viewport are.
const float LABEL_SCALE = 2.0f;
// Two points closer than this are one point.
const double SAME_POINT = 1e-6;

const Math::vec4 DISTANCE_COLOR(1.0f, 0.82f, 0.2f, 1.0f);
const Math::vec4 ANGLE_COLOR(0.3f, 0.85f, 1.0f, 1.0f);
const Math::vec4 PREVIEW_COLOR(1.0f, 1.0f, 1.0f, 0.6f);
const Math::vec4 POINT_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
const Math::vec4 LAST_POINT_COLOR(0.3f, 0.9f, 0.3f, 1.0f);
const Math::vec4 SELECTED_COLOR(1.0f, 0.55f, 0.1f, 1.0f);
const Math::vec4 LABEL_COLOR(1.0f, 1.0f, 1.0f, 1.0f);
const Math::vec4 VERTEX_COLOR(0.27f, 0.98f, 0.65f, 1.0f);
const Math::vec4 VERTEX_WIREFRAME_COLOR(0.27f, 0.98f, 0.65f, 0.5f);
// The figure of a person: pale like the scale figure of SpeedTree, but solid, so that
// it is seen against light walls too; see-through while it only follows the mouse.
const Math::vec4 FIGURE_COLOR(0.58f, 0.65f, 0.77f, 1.0f);
const Math::vec4 FIGURE_PREVIEW_COLOR(0.58f, 0.65f, 0.77f, 0.7f);
const Math::vec4 FIGURE_SELECTED_COLOR(0.95f, 0.7f, 0.45f, 1.0f);
// How wide the figure is picked by a click, from its middle, in parts of its height.
const double FIGURE_PICK_HALF_WIDTH = 0.16;
// The height of a figure: the step of the mouse wheel and the limits, in units.
const double FIGURE_HEIGHT_STEP = 0.05;
const double FIGURE_MIN_HEIGHT = 0.3;
const double FIGURE_MAX_HEIGHT = 3.0;
// X - red, Y - green, Z - blue, the same as the axes in the editor.
const Math::vec4 AXIS_COLORS[3] = {
	Math::vec4(0.88f, 0.35f, 0.35f, 1.0f),
	Math::vec4(0.42f, 0.75f, 0.42f, 1.0f),
	Math::vec4(0.35f, 0.55f, 0.88f, 1.0f),
};

float point_radius(const Math::Vec3 &position)
{
	float scale = 1.0f;
	const float distance = viewportDistanceTo(position);
	if (distance > POINT_FULL_SIZE_DISTANCE)
		scale = std::pow(POINT_FULL_SIZE_DISTANCE / distance, POINT_DISTANCE_FALLOFF);
	if (scale < POINT_MIN_SCREEN_SCALE)
		scale = POINT_MIN_SCREEN_SCALE;
	return Math::clamp(viewportSizeAt(position, POINT_SCREEN_RADIUS * scale), 0.0005f, 10000.0f);
}

// The outline of a person seen from the front, traced from a picture of a standing
// man: x is to the side, y up, in parts of the height (the feet at 0, the top of the
// head at 1).
const float FIGURE_OUTLINE[][2] = {
	{-0.0138f, 1.0000f}, {0.0201f, 0.9979f}, {0.0350f, 0.9894f}, {0.0434f, 0.9746f},
	{0.0434f, 0.9364f}, {0.0498f, 0.9322f}, {0.0519f, 0.9237f}, {0.0477f, 0.9089f},
	{0.0413f, 0.9025f}, {0.0371f, 0.9025f}, {0.0307f, 0.8898f}, {0.0307f, 0.8538f},
	{0.0731f, 0.8305f}, {0.0858f, 0.8284f}, {0.1155f, 0.8136f}, {0.1197f, 0.8051f},
	{0.1261f, 0.8008f}, {0.1345f, 0.7775f}, {0.1345f, 0.7182f}, {0.1388f, 0.7055f},
	{0.1409f, 0.6589f}, {0.1451f, 0.6525f}, {0.1536f, 0.6081f}, {0.1557f, 0.4492f},
	{0.1430f, 0.4237f}, {0.1345f, 0.4153f}, {0.1176f, 0.4110f}, {0.1176f, 0.4216f},
	{0.1324f, 0.4343f}, {0.1345f, 0.4407f}, {0.1345f, 0.4576f}, {0.1282f, 0.4619f},
	{0.1239f, 0.4576f}, {0.1197f, 0.4386f}, {0.1112f, 0.4386f}, {0.1155f, 0.4788f},
	{0.1218f, 0.4936f}, {0.1303f, 0.5021f}, {0.1303f, 0.5212f}, {0.1091f, 0.5869f},
	{0.0922f, 0.6886f}, {0.0879f, 0.6886f}, {0.0816f, 0.6568f}, {0.0816f, 0.5932f},
	{0.0985f, 0.5021f}, {0.1091f, 0.4131f}, {0.1070f, 0.3030f}, {0.1239f, 0.1716f},
	{0.1218f, 0.0636f}, {0.1388f, 0.0318f}, {0.1536f, 0.0191f}, {0.1515f, 0.0000f},
	{0.1049f, 0.0000f}, {0.0985f, 0.0042f}, {0.0964f, 0.0106f}, {0.0794f, 0.0127f},
	{0.0752f, 0.0191f}, {0.0773f, 0.0508f}, {0.0561f, 0.1398f}, {0.0413f, 0.2669f},
	{0.0286f, 0.3178f}, {0.0286f, 0.3284f}, {0.0244f, 0.3369f}, {0.0244f, 0.3475f},
	{0.0032f, 0.4258f}, {-0.0011f, 0.4258f}, {-0.0053f, 0.4153f}, {-0.0392f, 0.2691f},
	{-0.0519f, 0.1504f}, {-0.0604f, 0.1186f}, {-0.0646f, 0.0805f}, {-0.0731f, 0.0551f},
	{-0.0731f, 0.0169f}, {-0.0816f, 0.0106f}, {-0.0922f, 0.0106f}, {-0.0922f, 0.0064f},
	{-0.0985f, 0.0000f}, {-0.1472f, 0.0000f}, {-0.1515f, 0.0042f}, {-0.1515f, 0.0191f},
	{-0.1345f, 0.0339f}, {-0.1345f, 0.0381f}, {-0.1303f, 0.0403f}, {-0.1303f, 0.0445f},
	{-0.1176f, 0.0614f}, {-0.1197f, 0.1992f}, {-0.1133f, 0.2479f}, {-0.1070f, 0.2712f},
	{-0.1070f, 0.4174f}, {-0.0964f, 0.5169f}, {-0.0816f, 0.5932f}, {-0.0816f, 0.6483f},
	{-0.0879f, 0.6822f}, {-0.0900f, 0.6886f}, {-0.0943f, 0.6864f}, {-0.1091f, 0.5911f},
	{-0.1324f, 0.5191f}, {-0.1324f, 0.5021f}, {-0.1197f, 0.4852f}, {-0.1112f, 0.4386f},
	{-0.1197f, 0.4364f}, {-0.1261f, 0.4576f}, {-0.1345f, 0.4555f}, {-0.1345f, 0.4343f},
	{-0.1197f, 0.4195f}, {-0.1218f, 0.4089f}, {-0.1324f, 0.4110f}, {-0.1515f, 0.4301f},
	{-0.1557f, 0.4386f}, {-0.1557f, 0.5996f}, {-0.1536f, 0.6229f}, {-0.1494f, 0.6292f},
	{-0.1430f, 0.6568f}, {-0.1388f, 0.7203f}, {-0.1345f, 0.7309f}, {-0.1367f, 0.7754f},
	{-0.1282f, 0.7987f}, {-0.1155f, 0.8136f}, {-0.1006f, 0.8220f}, {-0.0879f, 0.8242f},
	{-0.0710f, 0.8347f}, {-0.0646f, 0.8347f}, {-0.0350f, 0.8517f}, {-0.0328f, 0.8856f},
	{-0.0371f, 0.8983f}, {-0.0498f, 0.9089f}, {-0.0540f, 0.9301f}, {-0.0456f, 0.9386f},
	{-0.0456f, 0.9767f}, {-0.0328f, 0.9915f},
};

float cross2(const Math::vec2 &a, const Math::vec2 &b, const Math::vec2 &c)
{
	return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
}

// The figure as triangles (every three points are one), made once from the outline by
// cutting off its corners one by one. The triangles do not overlap, so the see-through
// figure is evenly pale.
const QVector<Math::vec2> &figure_triangles()
{
	static const QVector<Math::vec2> triangles = []() {
		QVector<Math::vec2> outline;
		for (const float(&point)[2] : FIGURE_OUTLINE)
			outline.append(Math::vec2(point[0], point[1]));

		// Counterclockwise, so that a corner that sticks out turns to the left.
		float area = 0.0f;
		for (int i = 0; i < outline.size(); ++i)
		{
			const Math::vec2 &a = outline[i];
			const Math::vec2 &b = outline[(i + 1) % outline.size()];
			area += a.x * b.y - b.x * a.y;
		}
		if (area < 0.0f)
			std::reverse(outline.begin(), outline.end());

		QVector<Math::vec2> result;
		while (outline.size() > 3)
		{
			bool cut = false;
			for (int i = 0; i < outline.size() && !cut; ++i)
			{
				const int count = outline.size();
				const Math::vec2 &a = outline[(i + count - 1) % count];
				const Math::vec2 &b = outline[i];
				const Math::vec2 &c = outline[(i + 1) % count];
				if (cross2(a, b, c) <= 0.0f)
					continue;
				// No other corner of the outline may be inside the cut-off triangle.
				bool empty = true;
				for (int j = 0; j < count && empty; ++j)
				{
					if (j == i || j == (i + count - 1) % count || j == (i + 1) % count)
						continue;
					const Math::vec2 &p = outline[j];
					empty = !(cross2(a, b, p) >= 0.0f && cross2(b, c, p) >= 0.0f && cross2(c, a, p) >= 0.0f);
				}
				if (!empty)
					continue;
				result.append(a);
				result.append(b);
				result.append(c);
				outline.removeAt(i);
				cut = true;
			}
			if (!cut)
				break;
		}
		if (outline.size() == 3)
			result.append(outline);
		return result;
	}();
	return triangles;
}

bool same_point(const Math::Vec3 &a, const Math::Vec3 &b)
{
	return double(Math::length(a - b)) < SAME_POINT;
}

Math::Vec3 axis_vector(int axis)
{
	return Math::Vec3(Math::Scalar(axis == 0 ? 1.0 : 0.0), Math::Scalar(axis == 1 ? 1.0 : 0.0),
		Math::Scalar(axis == 2 ? 1.0 : 0.0));
}

QString format_length(double length)
{
	return uiText("%1 m", "%1 м").arg(length, 0, 'f', 3);
}

// Across the view and up: the plane a figure on 'base' stands in, turned to the camera
// but always upright.
void figure_axes(const Math::Vec3 &base, Math::Vec3 &side, Math::Vec3 &up)
{
	Math::vec3 to_camera(viewportCameraPosition() - base);
	to_camera.z = 0.0f;
	if (Math::length2(to_camera) < 1e-8f)
		to_camera = Math::vec3(0.0f, -1.0f, 0.0f);
	side = Math::Vec3(Math::normalize(Math::cross(Math::vec3(0.0f, 0.0f, 1.0f), to_camera)));
	up = Math::Vec3(0.0f, 0.0f, 1.0f);
}

QString format_angle(double degrees)
{
	return QString::fromUtf8("%1\xC2\xB0").arg(degrees, 0, 'f', 2);
}

// The mouse is over an editor viewport.
// A widget that takes typed text: a key typed while it has the keyboard is its own.
bool is_text_input(const QObject *object)
{
	if (!object)
		return false;
	if (qobject_cast<const QLineEdit *>(object) || qobject_cast<const QAbstractSpinBox *>(object)
		|| qobject_cast<const QTextEdit *>(object) || qobject_cast<const QPlainTextEdit *>(object))
		return true;
	const QComboBox *combo = qobject_cast<const QComboBox *>(object);
	return combo && combo->isEditable();
}

QWidget *viewport_under_mouse()
{
	return findViewportWidget(QApplication::widgetAt(QCursor::pos()));
}

// How fast the cursor goes while Shift is held.
const double SLOW_CURSOR = 1.0 / 3.0;

// Keys and mouse buttons asked for by their Windows codes.
const int KEY_SHIFT = 0x10;
const int KEY_RIGHT_BUTTON = 0x02;
const int KEY_MIDDLE_BUTTON = 0x04;

// Whether the key is held right now. On Windows the state of the key itself is asked:
// Qt knows it only from the messages of its own windows, and the viewport does not
// always get them.
bool key_held(int key)
{
#ifdef _WIN32
	return (GetAsyncKeyState(key) & 0x8000) != 0;
#else
	switch (key)
	{
		case KEY_SHIFT: return QApplication::queryKeyboardModifiers() & Qt::ShiftModifier;
		case KEY_RIGHT_BUTTON: return QApplication::mouseButtons() & Qt::RightButton;
		case KEY_MIDDLE_BUTTON: return QApplication::mouseButtons() & Qt::MiddleButton;
		default: return false;
	}
#endif
}

#ifdef _WIN32
const unsigned int SPI_GETMOUSESPEED_ACTION = 0x0070;
const unsigned int SPI_SETMOUSESPEED_ACTION = 0x0071;

// The pointer speed of Windows (1..20) closest to SLOW_CURSOR of 'speed'. The speeds
// are not even steps: 1..10 are 1/32, 1/16, 1/8, 2/8 ... 8/8 of the mouse move,
// 11..20 add 1/4 each.
int slow_mouse_speed(int speed)
{
	const auto factor = [](int s) {
		static const double low[10] = {1.0 / 32, 1.0 / 16, 1.0 / 8, 2.0 / 8, 3.0 / 8, 4.0 / 8, 5.0 / 8, 6.0 / 8, 7.0 / 8, 1.0};
		return s <= 10 ? low[qBound(1, s, 10) - 1] : 1.0 + 0.25 * (qBound(11, s, 20) - 10);
	};
	const double wanted = factor(speed) * SLOW_CURSOR;
	int best = 1;
	for (int s = 1; s <= 20; ++s)
	{
		if (std::abs(factor(s) - wanted) < std::abs(factor(best) - wanted))
			best = s;
	}
	return best;
}
#endif

// The axis key X, Y or Z: 0, 1, 2; -1 for another key. On Windows the key is also
// found by its place on the keyboard, so that it works with any keyboard layout.
int axis_key(const QKeyEvent *key)
{
	switch (key->key())
	{
		case Qt::Key_X: return 0;
		case Qt::Key_Y: return 1;
		case Qt::Key_Z: return 2;
		default: break;
	}
#ifdef _WIN32
	switch (key->nativeVirtualKey())
	{
		case 0x58: return 0;
		case 0x59: return 1;
		case 0x5A: return 2;
		default: break;
	}
#endif
	return -1;
}

} // namespace

MeasureTool::MeasureTool(QObject *parent)
	: QObject(parent)
{
}

MeasureTool::~MeasureTool()
{
	on_changed = nullptr;
	on_message = nullptr;
	is_window_shown = nullptr;
	setShownInactive(false);
	setActive(false);
	delete loupe_;
}

void MeasureTool::hide_loupe()
{
	if (loupe_ && loupe_->isVisible())
		loupe_->hide();
}

void MeasureTool::setActive(bool active)
{
	if (active_ == active)
		return;

	active_ = active;
	update_drawing();
	if (active_)
		qApp->installEventFilter(this);
	else
	{
		drag_ = PointRef();
		drag_moving_ = false;
		click_taken_ = false;
		swallow_release_ = false;
		hover_ = SnapPoint();
		lock_ = LOCK_NONE;
		hide_loupe();
		// The pointer of Windows gets its own speed back.
		update_slow_cursor();
		qApp->removeEventFilter(this);
	}
}

void MeasureTool::setShownInactive(bool shown)
{
	shown_inactive_ = shown;
	update_drawing();
}

void MeasureTool::update_drawing()
{
	const bool drawing = active_ || shown_inactive_;
	if (drawing == drawing_)
		return;
	drawing_ = drawing;
	if (drawing_)
	{
		// The measurements are drawn with the Visualizer, once per frame.
		saved_visualizer_mode_ = Visualizer::getMode();
		if (saved_visualizer_mode_ == Visualizer::MODE_DISABLED)
			Visualizer::setMode(Visualizer::MODE_ENABLED_DEPTH_TEST_ENABLED);
		Engine::get()->getEventEndWorldUpdate().connect(connections_, this, &MeasureTool::update);
	} else
	{
		connections_.disconnectAll();
		if (saved_visualizer_mode_ == Visualizer::MODE_DISABLED)
			Visualizer::setMode(Visualizer::MODE_DISABLED);
	}
}

void MeasureTool::setMeasuring(bool measuring)
{
	if (measuring_ == measuring)
		return;
	// A measurement left unfinished is dropped.
	if (!measuring)
		remove_measurement(current_index());
	measuring_ = measuring;
	lock_ = LOCK_NONE;
	report(QString());
	notify_changed();
}

void MeasureTool::setMode(Mode mode)
{
	if (mode_ == mode)
		return;
	// The measurement being made goes on from its last point in the other mode. A
	// figure is not started from it - it would stand there right away.
	const int current = current_index();
	if (current >= 0 && mode == MODE_FIGURE)
		remove_measurement(current);
	else if (current >= 0)
	{
		Measurement &measurement = measurements_[current];
		measurement.points = {measurement.points.last()};
		measurement.mode = mode;
	}
	mode_ = mode;
	lock_ = LOCK_NONE;
	notify_changed();
}

void MeasureTool::setSnap(Snap snap)
{
	snap_ = snap;
	notify_changed();
}

void MeasureTool::setSurfaceFilter(const SurfaceFilter &filter)
{
	filter_ = filter;
	filter_.look_through = true;
}

void MeasureTool::setPointSnap(bool snap)
{
	point_snap_ = snap;
	notify_changed();
}

void MeasureTool::setLoupe(bool loupe)
{
	loupe_enabled_ = loupe;
	if (!loupe_enabled_)
		hide_loupe();
	notify_changed();
}

void MeasureTool::set_lock(Lock lock, int axis)
{
	lock_ = lock;
	lock_axis_ = qBound(0, axis, 2);
	notify_changed();
}

int MeasureTool::pointsNeeded(Mode mode)
{
	switch (mode)
	{
		case MODE_ANGLE: return 3;
		case MODE_FIGURE: return 1;
		default: return 2;
	}
}

double MeasureTool::angleAt(const Math::Vec3 &a, const Math::Vec3 &corner, const Math::Vec3 &b)
{
	const Math::dvec3 u(a - corner);
	const Math::dvec3 v(b - corner);
	const double lengths = Math::length(u) * Math::length(v);
	if (lengths < 1e-12)
		return 0.0;
	const double cosine = Math::clamp(Math::dot(u, v) / lengths, -1.0, 1.0);
	return std::acos(cosine) * 180.0 / 3.14159265358979323846;
}

int MeasureTool::current_index() const
{
	if (measurements_.isEmpty() || measurements_.last().finished)
		return -1;
	return measurements_.size() - 1;
}

int MeasureTool::shown_index() const
{
	const int current = current_index();
	if (current >= 0)
		return current;
	if (selected_ >= 0 && selected_ < measurements_.size())
		return selected_;
	return measurements_.size() - 1;
}

bool MeasureTool::isMeasurementInProgress() const
{
	return current_index() >= 0;
}

bool MeasureTool::getShown(Measurement &measurement, bool &preview) const
{
	preview = false;
	const int index = shown_index();
	if (index < 0)
		return false;

	measurement = measurements_[index];
	if (index == current_index() && measuring_ && hover_.found && !drag_moving_
		&& (measurement.points.isEmpty() || !same_point(measurement.points.last(), hover_.point)))
	{
		measurement.points.append(hover_.point);
		preview = true;
	}
	return true;
}

void MeasureTool::remove_measurement(int index)
{
	if (index < 0 || index >= measurements_.size())
		return;

	measurements_.removeAt(index);
	if (selected_ == index)
		selected_ = -1;
	else if (selected_ > index)
		--selected_;
	drag_ = PointRef();
	drag_moving_ = false;
	notify_changed();
}

void MeasureTool::deleteShown()
{
	remove_measurement(shown_index());
	lock_ = LOCK_NONE;
	notify_changed();
}

void MeasureTool::removeLastPoint()
{
	if (measurements_.isEmpty())
		return;

	Measurement &measurement = measurements_.last();
	measurement.points.removeLast();
	// The measurement goes on from the point before: the next click puts the point
	// that has been taken off anew.
	measurement.finished = false;
	if (measurement.points.isEmpty())
		remove_measurement(measurements_.size() - 1);
	drag_ = PointRef();
	drag_moving_ = false;
	notify_changed();
}

void MeasureTool::clear()
{
	measurements_.clear();
	selected_ = -1;
	drag_ = PointRef();
	drag_moving_ = false;
	lock_ = LOCK_NONE;
	report(QString());
	notify_changed();
}

void MeasureTool::add_point(const Math::Vec3 &point)
{
	int current = current_index();
	if (current < 0)
	{
		Measurement measurement;
		measurement.mode = mode_;
		measurement.height = figure_height_;
		measurements_.append(measurement);
		current = measurements_.size() - 1;
	}

	Measurement &measurement = measurements_[current];
	// A second click on the last point adds nothing.
	if (!measurement.points.isEmpty() && same_point(measurement.points.last(), point))
		return;
	measurement.points.append(point);
	if (measurement.points.size() >= pointsNeeded(measurement.mode))
		measurement.finished = true;

	selected_ = current;
	// A lock holds one span: the next one starts free.
	lock_ = LOCK_NONE;
	report(QString());
	notify_changed();
}

MeasureTool::PointRef MeasureTool::pick_point(const Math::Vec3 &p0, const Math::Vec3 &p1, const PointRef &skip) const
{
	PointRef picked;
	const Math::Vec3 direction = p1 - p0;
	const double ray_length = double(Math::length(direction));
	if (ray_length < 1e-9)
		return picked;

	// The point the ray is closest to, in parts of its pick zone, wins.
	double picked_miss = 0.0;
	for (int m = 0; m < measurements_.size(); ++m)
	{
		const QVector<Math::Vec3> &points = measurements_[m].points;
		for (int i = 0; i < points.size(); ++i)
		{
			if (m == skip.measurement && i == skip.index)
				continue;

			const Math::Vec3 &position = points[i];
			const double along = double(Math::dot(position - p0, direction)) / ray_length;
			if (along < 0.0 || along > ray_length)
				continue;
			const Math::Vec3 closest = p0 + direction * Math::Scalar(along / ray_length);
			const double radius = double(point_radius(position) * POINT_PICK_SCALE);
			const double miss = double(Math::length(position - closest)) / radius;
			if (miss > 1.0)
				continue;
			if (!picked.isValid() || miss < picked_miss)
			{
				picked.measurement = m;
				picked.index = i;
				picked_miss = miss;
			}
		}
	}
	return picked;
}

int MeasureTool::pick_figure(const Math::Vec3 &p0, const Math::Vec3 &p1) const
{
	const double ray_length = double(Math::length(p1 - p0));
	if (ray_length < 1e-9)
		return -1;
	const Math::Vec3 ray = (p1 - p0) / Math::Scalar(ray_length);

	// The ray goes through the upright rectangle the figure stands in. The ones put
	// last are in front of the list.
	for (int m = measurements_.size() - 1; m >= 0; --m)
	{
		const Measurement &measurement = measurements_[m];
		if (measurement.mode != MODE_FIGURE || measurement.points.isEmpty())
			continue;
		const Math::Vec3 &base = measurement.points[0];
		Math::Vec3 side, up;
		figure_axes(base, side, up);
		const Math::Vec3 normal = Math::cross(side, up);
		const double facing = double(Math::dot(normal, ray));
		if (std::abs(facing) < 1e-9)
			continue;
		const double t = double(Math::dot(normal, base - p0)) / facing;
		if (t <= 0.0)
			continue;
		const Math::Vec3 hit = p0 + ray * Math::Scalar(t) - base;
		const double across = double(Math::dot(hit, side));
		const double height = double(Math::dot(hit, up));
		if (std::abs(across) <= FIGURE_PICK_HALF_WIDTH * measurement.height && height >= 0.0
			&& height <= measurement.height)
			return m;
	}
	return -1;
}

int MeasureTool::pick_measurement(const Math::Vec3 &p0, const Math::Vec3 &p1) const
{
	const PointRef point = pick_point(p0, p1, PointRef());
	if (point.isValid())
		return point.measurement;

	const double ray_length = double(Math::length(p1 - p0));
	if (ray_length < 1e-9)
		return -1;
	const Math::Vec3 ray = (p1 - p0) / Math::Scalar(ray_length);

	const int figure = pick_figure(p0, p1);
	if (figure >= 0)
		return figure;

	// The line the ray passes closest to, in parts of its pick zone.
	int picked = -1;
	double picked_miss = 0.0;
	for (int m = 0; m < measurements_.size(); ++m)
	{
		const QVector<Math::Vec3> &points = measurements_[m].points;
		for (int i = 0; i + 1 < points.size(); ++i)
		{
			// The closest places of the span q0 + s * u (s in 0..1) and the ray.
			const Math::Vec3 u = points[i + 1] - points[i];
			const Math::Vec3 w = points[i] - p0;
			const double a = double(Math::dot(u, u));
			const double b = double(Math::dot(u, ray));
			const double d = double(Math::dot(u, w));
			const double e = double(Math::dot(ray, w));
			const double denominator = a - b * b;
			double s = denominator > 1e-12 ? (b * e - d) / denominator : 0.0;
			s = Math::clamp(s, 0.0, 1.0);
			const double t = e + s * b;
			if (t < 0.0)
				continue;

			const Math::Vec3 on_span = points[i] + u * Math::Scalar(s);
			const Math::Vec3 on_ray = p0 + ray * Math::Scalar(t);
			const double miss = double(Math::length(on_span - on_ray)) / double(viewportSizeAt(on_span, LINE_PICK_SCREEN));
			if (miss > 1.0)
				continue;
			if (picked < 0 || miss < picked_miss)
			{
				picked = m;
				picked_miss = miss;
			}
		}
	}
	return picked;
}

bool MeasureTool::lock_anchor(Math::Vec3 &anchor) const
{
	if (drag_.isValid())
	{
		anchor = drag_start_;
		return true;
	}
	const int current = current_index();
	if (current < 0 || measurements_[current].points.isEmpty())
		return false;
	anchor = measurements_[current].points.last();
	return true;
}

bool MeasureTool::apply_lock(const Math::Vec3 &anchor, const SnapPoint &snap, const Math::Vec3 &p0,
	const Math::Vec3 &p1, Math::Vec3 &result) const
{
	const Math::Vec3 axis = axis_vector(lock_axis_);
	// A vertex or a point aimed at is brought to the axis or the plane as it is: the
	// span then measures how far it is along the axis (the height of a vertex with Z).
	const bool exact_target = snap.kind == SNAPPED_VERTEX || snap.kind == SNAPPED_POINT;
	const auto project = [&]() {
		const Math::Scalar along = Math::dot(snap.target - anchor, axis);
		result = lock_ == LOCK_AXIS ? anchor + axis * along : snap.target - axis * along;
	};
	if (exact_target)
	{
		project();
		return true;
	}

	// Otherwise the point is where the mouse ray meets the axis (comes closest to it)
	// or crosses the plane, whether there is a surface there or not.
	const double ray_length = double(Math::length(p1 - p0));
	if (ray_length > 1e-9)
	{
		const Math::Vec3 ray = (p1 - p0) / Math::Scalar(ray_length);
		if (lock_ == LOCK_AXIS)
		{
			const Math::Vec3 w = anchor - p0;
			const double b = double(Math::dot(axis, ray));
			const double d = double(Math::dot(axis, w));
			const double e = double(Math::dot(ray, w));
			const double denominator = 1.0 - b * b;
			if (denominator > 1e-6)
			{
				const double s = (b * e - d) / denominator;
				if (e + s * b > 0.0)
				{
					result = anchor + axis * Math::Scalar(s);
					return true;
				}
			}
		} else
		{
			const double facing = double(Math::dot(axis, ray));
			if (std::abs(facing) > 1e-6)
			{
				const double t = double(Math::dot(axis, anchor - p0)) / facing;
				if (t > 0.0 && t <= ray_length)
				{
					result = p0 + ray * Math::Scalar(t);
					return true;
				}
			}
		}
	}

	// The ray runs along the axis or the plane: the surface point is brought to it.
	if (snap.kind == SNAPPED_SURFACE)
	{
		project();
		return true;
	}
	return false;
}

MeasureTool::SnapPoint MeasureTool::place_at(QWidget *viewport, const QPointF &position, const PointRef &skip) const
{
	SnapPoint result;
	Math::Vec3 p0, p1;
	if (!screenRay(viewport, position, p0, p1))
		return result;

	// What the mouse aims at: a point of a measurement, a vertex, the surface.
	if (point_snap_)
	{
		const PointRef picked = pick_point(p0, p1, skip);
		if (picked.isValid())
		{
			result.kind = SNAPPED_POINT;
			result.target = measurements_[picked.measurement].points[picked.index];
		}
	}
	if (result.kind == SNAPPED_NONE && snap_ == SNAP_VERTEX)
	{
		const VertexHit vertex = findVertex(viewport, position);
		if (vertex.found)
		{
			result.kind = SNAPPED_VERTEX;
			result.target = vertex.point;
			result.object = vertex.object;
		}
	}
	if (result.kind == SNAPPED_NONE)
	{
		// No vertex near the mouse (or the surface snap): the point under the mouse.
		const SurfaceHit hit = raycastSurface(p0, p1, filter_, Vector<NodePtr>());
		if (hit.found)
		{
			result.kind = SNAPPED_SURFACE;
			result.target = hit.point;
		}
	}

	Math::Vec3 anchor;
	if (lock_ != LOCK_NONE && lock_anchor(anchor))
	{
		result.found = apply_lock(anchor, result, p0, p1, result.point);
		return result;
	}
	result.found = result.kind != SNAPPED_NONE;
	result.point = result.target;
	return result;
}

void MeasureTool::update()
{
	update_slow_cursor();
	if (!World::isLoaded())
	{
		hover_ = SnapPoint();
		hide_loupe();
		return;
	}

	// While another tab is open the measurements are only shown, as long as the
	// window of the plugin is.
	if (!active_)
	{
		if (!is_window_shown || is_window_shown())
			draw();
		return;
	}

	update_drag();
	update_hover();
	draw();
	update_loupe();

	// The page is told only when what it shows changes.
	if (shown_signature() != shown_signature_)
		notify_changed();
}

void MeasureTool::update_hover()
{
	// While a point is dragged, update_drag() tells where it goes.
	if (drag_moving_)
		return;
	hover_ = SnapPoint();

	QWidget *viewport = viewport_under_mouse();
	if (!viewport)
		return;
	const QPointF position = viewport->mapFromGlobal(QPointF(QCursor::pos()));

	if (measuring_)
	{
		hover_ = place_at(viewport, position, PointRef());
		return;
	}

	// While not measuring only the points that can be dragged are lit up.
	Math::Vec3 p0, p1;
	if (!screenRay(viewport, position, p0, p1))
		return;
	const PointRef picked = pick_point(p0, p1, PointRef());
	if (picked.isValid())
	{
		hover_.found = true;
		hover_.kind = SNAPPED_POINT;
		hover_.target = measurements_[picked.measurement].points[picked.index];
		hover_.point = hover_.target;
	}
}

// The magnifier shows where the point goes while points are put or dragged.
void MeasureTool::update_loupe()
{
	QWidget *viewport = viewport_under_mouse();
	// A figure is not aimed at a pixel: it has no magnifier.
	if (!loupe_enabled_ || !viewport || !hover_.found || !(measuring_ || drag_moving_) || mode_ == MODE_FIGURE)
	{
		hide_loupe();
		return;
	}

	// Where the point goes, on the screen. It is taken as an offset from the mouse in
	// the pixels of the picture the engine renders: the viewport widget is a few
	// pixels larger than that picture, and its own coordinates would miss the point.
	::UnigineEditor::ViewportWindowPtr window = matchViewport(viewport);
	PlayerPtr player = window ? window->getPlayer() : PlayerPtr();
	int x = 0;
	int y = 0;
	if (!player || !player->getScreenPosition(x, y, hover_.point, window->getWidth(), window->getHeight()))
	{
		hide_loupe();
		return;
	}
	const Math::ivec2 mouse = window->getMousePos();
	const QPoint center = QCursor::pos() + QPoint(x - mouse.x, y - mouse.y);

	Math::vec4 color = POINT_COLOR;
	if (hover_.kind == SNAPPED_VERTEX)
		color = VERTEX_COLOR;
	else if (hover_.kind == SNAPPED_POINT)
		color = SELECTED_COLOR;
	if (lock_ != LOCK_NONE)
		color = AXIS_COLORS[lock_axis_];

	if (!loupe_)
		loupe_ = new Loupe;
	loupe_->showAt(center, QColor::fromRgbF(color.x, color.y, color.z));
}

// Every frame while a point is held: it follows the mouse, snapped and locked the way
// the new points are.
void MeasureTool::update_drag()
{
	if (!drag_.isValid())
		return;
	if (drag_.measurement >= measurements_.size() || drag_.index >= measurements_[drag_.measurement].points.size())
	{
		drag_ = PointRef();
		drag_moving_ = false;
		return;
	}
	if (!(QApplication::mouseButtons() & Qt::LeftButton))
	{
		// The release did not get to the tool (the window lost the mouse): the click
		// is over, and it does not count as a click on the point.
		click_taken_ = false;
		swallow_release_ = false;
		drag_ = PointRef();
		drag_moving_ = false;
		lock_ = LOCK_NONE;
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

	const SnapPoint snap = place_at(viewport, viewport->mapFromGlobal(QPointF(mouse)), drag_);
	hover_ = snap;
	if (snap.found)
		measurements_[drag_.measurement].points[drag_.index] = snap.point;
}

void MeasureTool::update_slow_cursor()
{
	// Only while points are put or dragged, and not while the camera flies (Shift
	// speeds it up then).
	const bool slow = active_ && (measuring_ || drag_.isValid()) && key_held(KEY_SHIFT)
		&& !key_held(KEY_RIGHT_BUTTON) && !key_held(KEY_MIDDLE_BUTTON) && viewport_under_mouse();

#ifdef _WIN32
	// The viewport gets no mouse moves of its own to slow down, so the speed of the
	// pointer of Windows is lowered while Shift is held and put back when it is let
	// go. It is not saved to the settings of Windows.
	if (slow == slow_active_)
		return;
	if (slow)
	{
		int speed = 0;
		if (!SystemParametersInfoW(SPI_GETMOUSESPEED_ACTION, 0, &speed, 0) || speed < 1)
			return;
		saved_mouse_speed_ = speed;
		SystemParametersInfoW(SPI_SETMOUSESPEED_ACTION, 0, reinterpret_cast<void *>(intptr_t(slow_mouse_speed(speed))), 0);
	} else
		SystemParametersInfoW(SPI_SETMOUSESPEED_ACTION, 0, reinterpret_cast<void *>(intptr_t(saved_mouse_speed_)), 0);
	slow_active_ = slow;
#else
	// Elsewhere the cursor is pulled back every frame: it keeps SLOW_CURSOR of the way
	// it has gone.
	if (!slow)
	{
		slow_active_ = false;
		return;
	}
	const QPoint now = QCursor::pos();
	if (!slow_active_)
	{
		slow_active_ = true;
		slow_exact_ = QPointF(now);
		slow_set_ = now;
		return;
	}
	if (now == slow_set_)
		return;
	// The parts of a pixel are kept, so that slow moves are not lost.
	slow_exact_ += QPointF(now - slow_set_) * SLOW_CURSOR;
	slow_set_ = slow_exact_.toPoint();
	QCursor::setPos(slow_set_);
#endif
}

bool MeasureTool::wheel_figure_height(QObject *watched, const QWheelEvent *wheel)
{
	if (!active_ || mode_ != MODE_FIGURE || !findViewportWidget(watched) || !World::isLoaded())
		return false;

	// The figure under the mouse, or else the one the next click stands, while
	// figures are put. Elsewhere the wheel stays with the editor camera.
	QWidget *viewport = findViewportWidget(watched);
	Math::Vec3 p0, p1;
	const int figure = screenRay(viewport, viewport->mapFromGlobal(wheel->globalPosition()), p0, p1)
		? pick_figure(p0, p1) : -1;
	if (figure < 0 && !measuring_)
		return false;

	// A notch of the wheel is 120; a touchpad sends it in small parts.
	wheel_rest_ += wheel->angleDelta().y();
	const int steps = wheel_rest_ / 120;
	wheel_rest_ -= steps * 120;
	if (steps == 0)
		return true;

	double &height = figure >= 0 ? measurements_[figure].height : figure_height_;
	height = qBound(FIGURE_MIN_HEIGHT, std::round(height / FIGURE_HEIGHT_STEP + steps) * FIGURE_HEIGHT_STEP,
		FIGURE_MAX_HEIGHT);
	if (figure >= 0)
		selected_ = figure;
	notify_changed();
	return true;
}

void MeasureTool::finish_click()
{
	const PointRef clicked = drag_;
	const bool moved = drag_moving_;
	drag_ = PointRef();
	drag_moving_ = false;

	if (!clicked.isValid() || clicked.measurement >= measurements_.size())
		return;

	if (moved)
	{
		// The moved measurement is the one the page shows now.
		selected_ = clicked.measurement;
		lock_ = LOCK_NONE;
		notify_changed();
	} else if (measuring_)
	{
		// A click on a point that has not been dragged puts the next point where the
		// press aimed: on that point when the points snap to each other.
		if (press_place_.found)
			add_point(press_place_.point);
	} else
	{
		selected_ = clicked.measurement;
		notify_changed();
	}
}

bool MeasureTool::eventFilter(QObject *watched, QEvent *event)
{
	if (!active_)
		return false;

	switch (event->type())
	{
		case QEvent::Wheel:
			if (wheel_figure_height(watched, static_cast<QWheelEvent *>(event)))
				return true;
			break;

		case QEvent::KeyRelease:
		{
			// Alt pressed and let go by itself switches the ruler and the angle, Ctrl -
			// the snap. A press of another key or of the mouse in between (Alt + LMB
			// of the camera, Ctrl + Z) makes it no switch. The keys are let through to
			// the editor when pressed; the release is kept from it, so that Alt does
			// not open the main menu.
			const QKeyEvent *key = static_cast<QKeyEvent *>(event);
			if (key->isAutoRepeat() || tap_key_ == 0 || key->key() != tap_key_)
				break;
			const int tapped = tap_key_;
			tap_key_ = 0;
			if (!measuring_ || !viewport_under_mouse())
				break;
			if (tapped == Qt::Key_Alt)
				setMode(Mode((int(mode_) + 1) % 3));
			else
				setSnap(snap_ == SNAP_VERTEX ? SNAP_SURFACE : SNAP_VERTEX);
			return true;
		}

		case QEvent::ShortcutOverride:
		case QEvent::KeyPress:
		{
			const QKeyEvent *key = static_cast<QKeyEvent *>(event);
			const int code = key->key();
			if (event->type() == QEvent::KeyPress && !key->isAutoRepeat())
			{
				const bool tap = (code == Qt::Key_Alt || code == Qt::Key_Control) && measuring_
					&& viewport_under_mouse();
				tap_key_ = tap ? code : 0;
			}
			const int axis = axis_key(key);
			if (code != Qt::Key_Escape && code != Qt::Key_Backspace && axis < 0)
				break;
			// The keys are the tool's only in a viewport: in a text field of the editor
			// they are left alone, wherever the mouse is.
			if (is_text_input(watched) || is_text_input(QApplication::focusWidget()))
				break;
			if (!findViewportWidget(watched) && !viewport_under_mouse())
				break;

			Math::Vec3 anchor;
			bool handled = false;
			if (code == Qt::Key_Escape)
				handled = lock_ != LOCK_NONE || isMeasurementInProgress() || measuring_;
			else if (code == Qt::Key_Backspace)
				handled = !measurements_.isEmpty();
			else
				handled = !(key->modifiers() & (Qt::ControlModifier | Qt::AltModifier)) && lock_anchor(anchor);
			if (!handled)
				break;

			// The editor gets no shortcut for the key, the tool gets the key press.
			if (event->type() == QEvent::ShortcutOverride)
			{
				event->accept();
				return true;
			}
			if (key->isAutoRepeat())
				return true;

			if (code == Qt::Key_Escape)
			{
				// One step back at a time: the lock, the measurement being made, the
				// measuring itself.
				if (lock_ != LOCK_NONE)
					set_lock(LOCK_NONE, lock_axis_);
				else if (isMeasurementInProgress())
					remove_measurement(current_index());
				else
					setMeasuring(false);
			} else if (code == Qt::Key_Backspace)
				removeLastPoint();
			else
			{
				// X / Y / Z - along the axis; with Shift - in the plane without it. The
				// same key once more frees the point.
				const Lock lock = key->modifiers() & Qt::ShiftModifier ? LOCK_PLANE : LOCK_AXIS;
				if (lock_ == lock && lock_axis_ == axis)
					set_lock(LOCK_NONE, axis);
				else
					set_lock(lock, axis);
			}
			return true;
		}

		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonDblClick:
		{
			const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
			if (mouse->button() != Qt::LeftButton)
				break;

			click_taken_ = false;
			swallow_release_ = false;
			// Alt or Ctrl held with a click is not a tap.
			tap_key_ = 0;

			// Alt + LMB stays with the editor camera.
			if (mouse->modifiers() & Qt::AltModifier)
				break;

			QWidget *viewport = findViewportWidget(watched);
			if (!viewport || !World::isLoaded())
				break;
			const QPointF position = viewport->mapFromGlobal(mouse->globalPosition());

			// The second click of a fast pair would put a point where the first one
			// has just put it: it is taken and does nothing.
			if (event->type() == QEvent::MouseButtonDblClick && measuring_)
			{
				click_taken_ = true;
				swallow_release_ = true;
				return true;
			}

			Math::Vec3 p0, p1;
			if (!screenRay(viewport, position, p0, p1))
				break;

			press_place_ = measuring_ ? place_at(viewport, position, PointRef())
				: SnapPoint();

			// A point of a measurement: holding the button drags it, a click goes on
			// as a click there.
			const PointRef picked = pick_point(p0, p1, PointRef());
			if (picked.isValid())
			{
				drag_ = picked;
				drag_start_ = measurements_[picked.measurement].points[picked.index];
				drag_moving_ = false;
				drag_timer_.start();
				drag_press_position_ = QCursor::pos();
				click_taken_ = true;
				swallow_release_ = true;
				return true;
			}

			// While not measuring, a click on a measurement shows its numbers.
			if (!measuring_)
			{
				const int measurement = pick_measurement(p0, p1);
				if (measurement < 0)
					break;
				selected_ = measurement;
				notify_changed();
				click_taken_ = true;
				swallow_release_ = true;
				return true;
			}

			if (press_place_.found)
				add_point(press_place_.point);
			else
				report(uiText("Click on a surface to put a point there.", "Кликните по поверхности, чтобы поставить на неё точку."));
			click_taken_ = true;
			swallow_release_ = true;
			return true;
		}

		case QEvent::MouseButtonRelease:
			if (static_cast<QMouseEvent *>(event)->button() != Qt::LeftButton)
				break;
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
// Drawing.
////////////////////////////////////////////////////////////////////////////////
void MeasureTool::draw() const
{
	for (int i = 0; i < measurements_.size(); ++i)
		draw_measurement(i);

	const int current = current_index();
	Math::Vec3 anchor;
	if (lock_ != LOCK_NONE && lock_anchor(anchor))
		draw_lock(anchor);

	// The figure that is to stand where the mouse is.
	if (measuring_ && mode_ == MODE_FIGURE && hover_.found && !drag_moving_ && current < 0)
		draw_figure(hover_.point, figure_height_, FIGURE_PREVIEW_COLOR);

	// The span from the last point to where the next one goes, with what it would
	// measure.
	if (measuring_ && hover_.found && !drag_moving_ && current >= 0)
	{
		const Measurement &measurement = measurements_[current];
		const Math::Vec3 &last = measurement.points.last();
		if (!same_point(last, hover_.point))
		{
			Visualizer::renderLine3D(last, hover_.point, PREVIEW_COLOR, 0.0f, false);
			if (measurement.mode == MODE_DISTANCE)
				draw_label((last + hover_.point) * Math::Scalar(0.5), format_length(Math::length(hover_.point - last)), LABEL_COLOR);
			else if (measurement.points.size() >= 2)
				draw_angle(measurement.points[measurement.points.size() - 2], last, hover_.point, PREVIEW_COLOR);
		}
	}

	// What the mouse aims at, and where the point goes if a lock moves it elsewhere.
	if (hover_.kind == SNAPPED_NONE && !hover_.found)
		return;
	if (!measuring_ && hover_.kind != SNAPPED_POINT && !drag_moving_)
		return;

	if (hover_.kind != SNAPPED_NONE)
	{
		const float radius = point_radius(hover_.target);
		switch (hover_.kind)
		{
			case SNAPPED_VERTEX:
				if (hover_.object && !hover_.object.isDeleted() && hover_.object->getType() != Node::OBJECT_MESH_CLUSTER)
				{
					for (int s = 0, num = hover_.object->getNumSurfaces(); s < num; ++s)
						Visualizer::renderObjectSurface(hover_.object, s, VERTEX_WIREFRAME_COLOR);
				}
				draw_ring(hover_.target, radius * 1.6f, VERTEX_COLOR);
				draw_ring(hover_.target, radius * 1.3f, VERTEX_COLOR);
				break;
			case SNAPPED_POINT:
				draw_ring(hover_.target, radius * 1.8f, SELECTED_COLOR);
				draw_ring(hover_.target, radius * 1.5f, SELECTED_COLOR);
				break;
			default:
				draw_ring(hover_.target, radius * 1.3f, PREVIEW_COLOR);
				break;
		}
	}
	if (hover_.found && (hover_.kind == SNAPPED_NONE || !same_point(hover_.point, hover_.target)))
	{
		if (hover_.kind != SNAPPED_NONE)
			Visualizer::renderLine3D(hover_.target, hover_.point, PREVIEW_COLOR, 0.0f, false);
		const float radius = point_radius(hover_.point);
		draw_ring(hover_.point, radius * 1.3f, POINT_COLOR);
		draw_ring(hover_.point, radius, POINT_COLOR);
	}

	// A dot right where the point goes.
	if (hover_.found)
		Visualizer::renderSolidSphere(point_radius(hover_.point) * 0.15f, Math::translate(hover_.point), POINT_COLOR, 0.0f, false);
}

void MeasureTool::draw_measurement(int index) const
{
	const Measurement &measurement = measurements_[index];
	const QVector<Math::Vec3> &points = measurement.points;
	const Math::vec4 &color = measurement.mode == MODE_ANGLE ? ANGLE_COLOR : DISTANCE_COLOR;

	for (int i = 0; i + 1 < points.size(); ++i)
		Visualizer::renderLine3D(points[i], points[i + 1], color, 0.0f, false);

	if (measurement.mode == MODE_FIGURE)
	{
		if (!points.isEmpty())
		{
			const bool shown = index == shown_index();
			draw_figure(points[0], measurement.height, shown ? FIGURE_SELECTED_COLOR : FIGURE_COLOR);
		}
	} else if (measurement.mode == MODE_DISTANCE)
	{
		for (int i = 0; i + 1 < points.size(); ++i)
		{
			const double length = double(Math::length(points[i + 1] - points[i]));
			draw_label((points[i] + points[i + 1]) * Math::Scalar(0.5), format_length(length), LABEL_COLOR);
		}
	} else
	{
		for (int i = 1; i + 1 < points.size(); ++i)
			draw_angle(points[i - 1], points[i], points[i + 1], color);
	}

	// The measurement the page shows is orange, the point a measurement being made
	// goes on from is green.
	const bool in_progress = index == current_index();
	const bool shown = !in_progress && index == shown_index();
	for (int i = 0; i < points.size(); ++i)
	{
		Math::vec4 point_color = shown ? SELECTED_COLOR : POINT_COLOR;
		if (in_progress && i == points.size() - 1)
			point_color = LAST_POINT_COLOR;
		draw_ring(points[i], point_radius(points[i]), point_color);
	}
}

void MeasureTool::draw_figure(const Math::Vec3 &base, double height, const Math::vec4 &color) const
{
	Math::Vec3 side, up;
	figure_axes(base, side, up);
	const QVector<Math::vec2> &triangles = figure_triangles();
	const auto place = [&](const Math::vec2 &p) {
		return base + (side * Math::Scalar(p.x) + up * Math::Scalar(p.y)) * Math::Scalar(height);
	};
	// Things in front of the figure hide it, as they would hide a person.
	for (int i = 0; i + 2 < triangles.size(); i += 3)
		Visualizer::renderTriangle3D(place(triangles[i]), place(triangles[i + 1]), place(triangles[i + 2]), color, 0.0f, true);

	draw_label(base + up * Math::Scalar(height + 0.03), uiText("%1 m", "%1 м").arg(height, 0, 'f', 2), LABEL_COLOR);
}

// A circle turned to the camera.
void MeasureTool::draw_ring(const Math::Vec3 &position, float radius, const Math::vec4 &color) const
{
	Math::vec3 to_camera(viewportCameraPosition() - position);
	if (Math::length2(to_camera) < 1e-12f)
		to_camera = Math::vec3(0.0f, 0.0f, 1.0f);
	const Math::Mat4 transform = Math::translate(position) * Math::Mat4(basisFromUp(Math::normalize(to_camera)));
	Visualizer::renderCircle(radius, transform, color, 0.0f, false);
}

void MeasureTool::draw_label(const Math::Vec3 &position, const QString &text, const Math::vec4 &color) const
{
	// LABEL_SCALE times the text the Visualizer writes by default (the default size of
	// the engine GUI).
	const GuiPtr gui = Gui::getCurrent();
	const int font_size = gui ? qRound(float(gui->getDefaultSize()) * LABEL_SCALE) : -1;
	// Anchored at the middle of its bottom edge: the text stands above the point.
	Visualizer::renderMessage3D(position, Math::vec3(0.0f, 1.0f, 0.0f), text.toUtf8().constData(), color, 1, font_size);
}

// An arc between the two sides of the angle and its value next to it.
void MeasureTool::draw_angle(const Math::Vec3 &a, const Math::Vec3 &corner, const Math::Vec3 &b,
	const Math::vec4 &color) const
{
	const Math::vec3 to_a(a - corner);
	const Math::vec3 to_b(b - corner);
	const float length_a = Math::length(to_a);
	const float length_b = Math::length(to_b);
	if (length_a < 1e-6f || length_b < 1e-6f)
		return;

	const Math::vec3 u = to_a / length_a;
	const Math::vec3 v = to_b / length_b;
	const float angle = std::acos(Math::clamp(Math::dot(u, v), -1.0f, 1.0f));
	const float radius = Math::min(viewportSizeAt(corner, ARC_SCREEN_RADIUS), Math::min(length_a, length_b) * ARC_MAX_SIDE_PART);

	// The arc lies in the plane of the two sides: 'w' is the direction in it at a
	// right angle to the first side. Two sides on one line have no such plane.
	Math::vec3 w = v - u * Math::dot(u, v);
	const float w_length = Math::length(w);
	const bool flat = w_length < 1e-4f;
	if (!flat)
	{
		w /= w_length;
		Math::Vec3 previous = corner + Math::Vec3(u * radius);
		for (int i = 1; i <= ARC_SEGMENTS; ++i)
		{
			const float t = angle * float(i) / float(ARC_SEGMENTS);
			const Math::Vec3 next = corner + Math::Vec3((u * std::cos(t) + w * std::sin(t)) * radius);
			Visualizer::renderLine3D(previous, next, color, 0.0f, false);
			previous = next;
		}
	}

	// The value is on the bisector, a little outside of the arc.
	Math::vec3 bisector = u + v;
	if (Math::length(bisector) < 1e-4f)
		bisector = flat ? Math::vec3(0.0f, 0.0f, 1.0f) : w;
	bisector = Math::normalize(bisector);
	draw_label(corner + Math::Vec3(bisector * (radius * 1.3f)), format_angle(angleAt(a, corner, b)), LABEL_COLOR);
}

// The guide of the lock through the point it is measured from: the axis, or the two
// axes of the plane, in their colors.
void MeasureTool::draw_lock(const Math::Vec3 &anchor) const
{
	const Math::Scalar length = Math::Scalar(viewportSizeAt(anchor, LOCK_GUIDE_SCREEN));
	for (int axis = 0; axis < 3; ++axis)
	{
		const bool drawn = lock_ == LOCK_AXIS ? axis == lock_axis_ : axis != lock_axis_;
		if (!drawn)
			continue;
		const Math::Vec3 offset = axis_vector(axis) * length;
		Visualizer::renderLine3D(anchor - offset, anchor + offset, AXIS_COLORS[axis], 0.0f, false);
	}
}

QString MeasureTool::shown_signature() const
{
	Measurement shown;
	bool preview = false;
	QString signature = QString::fromUtf8("%1|%2|%3|%4|%5|%6").arg(measurements_.size()).arg(shown_index())
		.arg(int(lock_)).arg(lock_axis_).arg(int(snap_)).arg(figure_height_);
	if (getShown(shown, preview))
	{
		signature += QString::fromUtf8("|%1|%2|%3|%4").arg(int(shown.mode)).arg(shown.finished).arg(preview)
			.arg(shown.height);
		for (const Math::Vec3 &point : shown.points)
		{
			signature += QString::fromUtf8("|%1,%2,%3").arg(double(point.x), 0, 'f', 5)
				.arg(double(point.y), 0, 'f', 5).arg(double(point.z), 0, 'f', 5);
		}
	}
	return signature;
}

void MeasureTool::notify_changed() const
{
	shown_signature_ = shown_signature();
	if (on_changed)
		on_changed();
}

void MeasureTool::report(const QString &text) const
{
	if (on_message)
		on_message(text);
}

} // namespace ArtistTool
