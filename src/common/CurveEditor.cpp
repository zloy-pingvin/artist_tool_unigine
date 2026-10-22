#include "CurveEditor.h"
#include "Localization.h"
#include "UndoActions.h"

#include <editor/UnigineActions.h>
#include <editor/UnigineObjectMode.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>
#include <editor/UnigineUndo.h>

#include <UnigineNodes.h>
#include <UnigineVisualizer.h>
#include <UnigineWorld.h>

#include <QApplication>
#include <QByteArray>
#include <QCursor>
#include <QEvent>
#include <QMouseEvent>
#include <QSettings>
#include <QVariant>
#include <QWidget>

#include <cmath>

using namespace Unigine;
using ::UnigineEditor::Selection;
using ::UnigineEditor::SelectorNodes;

namespace ArtistTool
{

const char CURVE_PATH_NODE_NAME[] = "path";
const char CURVE_POINT_NODE_NAME[] = "path_point_%1";
const char CURVE_CORNER_DATA[] = "artist_tool_corner";
const char CURVE_OFF_GROUND_DATA[] = "artist_tool_off_ground";
const char CURVE_WIDTH_DATA[] = "artist_tool_width";

namespace
{

// A property of the application: set while a curve editor keeps the manipulators of
// the editor hidden. It outlives a reload of the plugin, so a new copy of the plugin
// can show the manipulators an old one has hidden.
QByteArray manipulators_property()
{
	return QByteArray("artist_tool_manipulators_hidden");
}

void hide_manipulators()
{
	if (!::UnigineEditor::ObjectMode::isManipulatorsEnabled())
		return;
	::UnigineEditor::ObjectMode::setManipulatorsEnabled(false);
	qApp->setProperty(manipulators_property().constData(), true);
}

void show_manipulators()
{
	if (!qApp->property(manipulators_property().constData()).toBool())
		return;
	qApp->setProperty(manipulators_property().constData(), QVariant());
	::UnigineEditor::ObjectMode::setManipulatorsEnabled(true);
}

// How long the button must be held before the picked point starts to follow the
// mouse. An ordinary click lasts 80-150 ms.
const int DRAG_DELAY_MS = 200;

// The points are drawn as spheres that nearly keep their size on the screen: they
// shrink as the camera comes closer and grow as it moves away, but grow a little
// slower than the distance does, so far points look somewhat smaller than near ones
// and a curve seen from afar is not hidden behind them. Up to POINT_FULL_SIZE_DISTANCE
// the radius is POINT_SCREEN_RADIUS of the height of the viewport; further away that
// part goes down as the distance in the power of POINT_DISTANCE_FALLOFF (half the
// size at 32 times the distance), but not below POINT_MIN_SCREEN_SCALE of it.
// A click counts as a click on a point a little outside of its sphere too.
const float POINT_SCREEN_RADIUS = 0.014f;
const float POINT_FULL_SIZE_DISTANCE = 5.0f;
const float POINT_DISTANCE_FALLOFF = 0.2f;
const float POINT_MIN_SCREEN_SCALE = 0.35f;

const float POINT_PICK_SCALE = 3.6f;
// A selected point carries the manipulator of the editor. Its zone is barely larger
// than its sphere, so the arrows of the manipulator stay with the editor.
const float POINT_SELECTED_PICK_SCALE = 1.3f;

Math::Vec3 bezier(const Math::Vec3 &p0, const Math::Vec3 &p1, const Math::Vec3 &p2, const Math::Vec3 &p3, double t)
{
	const Math::Scalar k0 = Math::Scalar(t);
	const Math::Scalar k1 = Math::Scalar(1.0 - t);
	return p0 * (k1 * k1 * k1) + p1 * (Math::Scalar(3.0) * k1 * k1 * k0)
		+ p2 * (Math::Scalar(3.0) * k0 * k0 * k1) + p3 * (k0 * k0 * k0);
}

} // namespace

NodePtr findChildNode(const NodePtr &node, const char *name)
{
	const int index = node->findChild(name);
	return index < 0 ? NodePtr() : node->getChild(index);
}

bool isPointFlag(const NodePtr &point, const char *data)
{
	const char *value = point->getData(data);
	return value && value[0] == '1';
}

bool isCornerPoint(const NodePtr &point)
{
	return isPointFlag(point, CURVE_CORNER_DATA);
}

Math::Vec3 unitOrZero(const Math::Vec3 &v)
{
	const double length = double(Math::length(v));
	return length > 1e-9 ? v * Math::Scalar(1.0 / length) : Math::Vec3(0.0, 0.0, 0.0);
}

float curvePointRadius(const Math::Vec3 &position)
{
	float scale = 1.0f;
	const float distance = viewportDistanceTo(position);
	if (distance > POINT_FULL_SIZE_DISTANCE)
		scale = std::pow(POINT_FULL_SIZE_DISTANCE / distance, POINT_DISTANCE_FALLOFF);
	if (scale < POINT_MIN_SCREEN_SCALE)
		scale = POINT_MIN_SCREEN_SCALE;

	return Math::clamp(viewportSizeAt(position, POINT_SCREEN_RADIUS * scale), 0.001f, 10000.0f);
}

double curvePointRoll(const NodePtr &point, const Math::Vec3 &direction)
{
	const Math::Vec3 forward = unitOrZero(Math::Vec3(direction.x, direction.y, 0.0));
	if (Math::length2(forward) == 0.0)
		return 0.0;
	const Math::Vec3 right(forward.y, -forward.x, 0.0);
	const Math::Vec3 up = unitOrZero(Math::Vec3(point->getWorldTransform().getColumn3(2)));
	return std::atan2(double(Math::dot(up, right)), double(up.z)) * Math::Consts::RAD2DEG_D;
}

Math::mat4 curvePointBasis(const Math::Vec3 &direction, double roll)
{
	Math::vec3 forward = Math::vec3(unitOrZero(Math::Vec3(direction.x, direction.y, 0.0)));
	if (Math::length2(forward) < 0.5f)
		forward = Math::vec3(0.0f, 1.0f, 0.0f);
	const Math::vec3 right(forward.y, -forward.x, 0.0f);
	Math::mat4 basis = Math::mat4_identity;
	basis.setColumn3(0, right);
	basis.setColumn3(1, forward);
	basis.setColumn3(2, Math::vec3(0.0f, 0.0f, 1.0f));
	return Math::rotate(forward, float(roll)) * basis;
}

////////////////////////////////////////////////////////////////////////////////
// The curve.
////////////////////////////////////////////////////////////////////////////////
namespace
{

// The spline through the points 'run' (indices into 'positions', in order; a closed
// run goes from the last one back to the first): the way it goes at every point, per
// unit of length. Its pieces between the points are cubic, joined with no break of
// the direction or of the bend; the ends of an open run are straight (no bend there).
// The equations are solved by iterations - every one depends on its neighbours more
// weakly than on itself, so they settle fast.
QVector<Math::Vec3> solve_spline(const QVector<Math::Vec3> &positions, const QVector<int> &run, bool closed)
{
	const int count = int(run.size());
	const int spans = closed ? count : count - 1;
	QVector<double> lengths(spans);
	for (int j = 0; j < spans; ++j)
	{
		lengths[j] = qMax(double(Math::length(positions[run[(j + 1) % count]] - positions[run[j]])), 1e-6);
	}

	// The chords to start from.
	QVector<Math::Vec3> d(count);
	for (int j = 0; j < count; ++j)
	{
		const int prev = closed ? (j + count - 1) % count : qMax(j - 1, 0);
		const int next = closed ? (j + 1) % count : qMin(j + 1, count - 1);
		d[j] = unitOrZero(positions[run[next]] - positions[run[prev]]);
	}
	if (count < 2)
		return d;

	for (int iteration = 0; iteration < 200; ++iteration)
	{
		double change = 0.0;
		for (int j = 0; j < count; ++j)
		{
			Math::Vec3 value;
			if (!closed && j == 0)
			{
				// 2 d0 + d1 = 3 (p1 - p0) / h0
				value = ((positions[run[1]] - positions[run[0]]) * Math::Scalar(3.0 / lengths[0]) - d[1]) * Math::Scalar(0.5);
			} else if (!closed && j == count - 1)
			{
				// d(n-1) + 2 dn = 3 (pn - p(n-1)) / h(n-1)
				const double h = lengths[count - 2];
				value = ((positions[run[j]] - positions[run[j - 1]]) * Math::Scalar(3.0 / h) - d[j - 1]) * Math::Scalar(0.5);
			} else
			{
				// h(j) d(j-1) + 2 (h(j-1) + h(j)) d(j) + h(j-1) d(j+1)
				//   = 3 (h(j) / h(j-1) (p(j) - p(j-1)) + h(j-1) / h(j) (p(j+1) - p(j)))
				const int prev = (j + count - 1) % count;
				const int next = (j + 1) % count;
				const double h_prev = lengths[(j + spans - 1) % spans];
				const double h_next = lengths[j % spans];
				const Math::Vec3 right = (positions[run[j]] - positions[run[prev]]) * Math::Scalar(3.0 * h_next / h_prev)
					+ (positions[run[next]] - positions[run[j]]) * Math::Scalar(3.0 * h_prev / h_next);
				value = (right - d[prev] * Math::Scalar(h_next) - d[next] * Math::Scalar(h_prev))
					* Math::Scalar(1.0 / (2.0 * (h_prev + h_next)));
			}
			change = qMax(change, double(Math::length(value - d[j])));
			d[j] = value;
		}
		if (change < 1e-9)
			break;
	}
	return d;
}

} // namespace

// Kept in the application, so that it is the same for all the tools and outlives a
// reload of the plugin; the Debug tab sets it from its remembered value.
void setOldCurve(bool old)
{
	qApp->setProperty("artist_tool_old_curve", old);
}

bool isOldCurve()
{
	return qApp->property("artist_tool_old_curve").toBool();
}

CurvePoints CurvePoints::collect(const NodePtr &path, bool closed)
{
	CurvePoints points;
	const int count = path ? path->getNumChildren() : 0;
	points.positions.resize(count);
	points.corners.resize(count);
	points.off_ground.resize(count);
	for (int i = 0; i < count; ++i)
	{
		const NodePtr point = path->getChild(i);
		points.positions[i] = point->getWorldPosition();
		points.corners[i] = isCornerPoint(point);
		points.off_ground[i] = isPointFlag(point, CURVE_OFF_GROUND_DATA);
	}
	points.closed = closed && count > 2;

	// The spline, piece by piece between the corners.
	points.incoming.fill(Math::Vec3(0.0, 0.0, 0.0), count);
	points.outgoing.fill(Math::Vec3(0.0, 0.0, 0.0), count);
	if (count >= 2 && isOldCurve())
	{
		// The old curve: halfway between the directions to the two neighbours, which
		// count the same however far they are; zero at a corner.
		for (int i = 0; i < count; ++i)
		{
			if (points.corners[i])
				continue;
			const int prev = points.closed ? (i + count - 1) % count : qMax(i - 1, 0);
			const int next = points.closed ? (i + 1) % count : qMin(i + 1, count - 1);
			const Math::Vec3 p = points.positions[i];
			const Math::Vec3 d = unitOrZero(unitOrZero(p - points.positions[prev]) + unitOrZero(points.positions[next] - p));
			points.incoming[i] = d;
			points.outgoing[i] = d;
		}
	} else if (count >= 2)
	{
		QVector<int> corners;
		for (int i = 0; i < count; ++i)
		{
			const bool end = !points.closed && (i == 0 || i == count - 1);
			if (points.corners[i] && !end)
				corners.append(i);
		}

		// The spline gives the way the curve goes at every point; the handles are a
		// third of the span whatever it says about their length. Its own lengths pull
		// the bend at a point between a short and a long span into a tight curl; these
		// spread it evenly, and the turns come out round.
		const auto apply = [&points](const QVector<int> &run, bool run_closed) {
			const QVector<Math::Vec3> d = solve_spline(points.positions, run, run_closed);
			for (int j = 0; j < run.size(); ++j)
			{
				const Math::Vec3 way = unitOrZero(d[j]);
				if (run_closed || j > 0)
					points.incoming[run[j]] = way;
				if (run_closed || j + 1 < run.size())
					points.outgoing[run[j]] = way;
			}
		};

		if (points.closed && corners.isEmpty())
		{
			// One closed spline through all the points.
			QVector<int> run;
			for (int i = 0; i < count; ++i)
				run.append(i);
			apply(run, true);
		} else
		{
			// From one corner (or end) to the next one; a closed curve goes on past its
			// last point to the first one.
			QVector<int> starts = points.closed ? corners : QVector<int>{0};
			if (!points.closed)
				starts += corners;
			for (int k = 0; k < starts.size(); ++k)
			{
				QVector<int> run;
				int i = starts[k];
				run.append(i);
				for (int step = 0; step < count; ++step)
				{
					i = (i + 1) % count;
					if (!points.closed && i == 0)
						break;
					run.append(i);
					if (points.corners[i] || (!points.closed && i == count - 1))
						break;
				}
				if (run.size() >= 2)
					apply(run, false);
			}
		}
	}

	points.rolls.resize(count);
	for (int i = 0; i < count; ++i)
		points.rolls[i] = curvePointRoll(path->getChild(i), points.direction(i));
	return points;
}

Math::Vec3 CurvePoints::direction(int i) const
{
	if (count() < 2)
		return Math::Vec3(0.0, 1.0, 0.0);

	// Halfway between the way the spline comes and the way it leaves (the same but at
	// a corner).
	Math::Vec3 d = unitOrZero(unitOrZero(incoming[i]) + unitOrZero(outgoing[i]));
	if (Math::length2(d) == 0.0)
	{
		const int prev = closed ? (i + count() - 1) % count() : qMax(i - 1, 0);
		const int next = closed ? (i + 1) % count() : qMin(i + 1, count() - 1);
		d = unitOrZero(positions[next] - positions[prev]);
	}
	return Math::length2(d) == 0.0 ? Math::Vec3(0.0, 1.0, 0.0) : d;
}

Math::Vec3 CurvePoints::spanPoint(int span, double t) const
{
	const int next = (span + 1) % count();
	const Math::Vec3 a = positions[span];
	const Math::Vec3 b = positions[next];

	// The piece of the spline as a Bezier: the handles go the way the spline goes at
	// the ends, a third of the length of the span (per unit of length).
	const Math::Scalar reach = Math::Scalar(double(Math::length(b - a)) / 3.0);
	return bezier(a, a + outgoing[span] * reach, b - incoming[next] * reach, b, t);
}

CurvePolyline buildPolyline(const CurvePoints &points, int subdivisions)
{
	CurvePolyline curve;
	const int spans = points.spans();
	if (spans == 0)
		return curve;

	curve.samples.reserve(spans * subdivisions + 1);
	curve.samples.append(points.positions[0]);
	for (int i = 0; i < spans; ++i)
	{
		for (int s = 1; s <= subdivisions; ++s)
		{
			const Math::Vec3 p = points.spanPoint(i, double(s) / subdivisions);
			curve.length += double(Math::length(p - curve.samples.last()));
			curve.samples.append(p);
		}
	}
	return curve;
}

////////////////////////////////////////////////////////////////////////////////
// Editing the points.
////////////////////////////////////////////////////////////////////////////////
CurveEditor::CurveEditor() = default;

CurveEditor::~CurveEditor()
{
	on_changed = nullptr;
	on_message = nullptr;
	reset();
	setAddingPoints(false);
}

void CurveEditor::recoverManipulators()
{
	// Builds made before this property existed hid the manipulators with no trace of
	// it, and a reload of the plugin left them hidden. Once per machine they are shown
	// again, whatever hid them.
	QSettings settings(QString::fromUtf8("zloy_pingvin"), QString::fromUtf8("artist_tool"));
	const QString recovered = QString::fromUtf8("curve_editor/manipulators_recovered");
	if (!settings.value(recovered, false).toBool())
	{
		settings.setValue(recovered, true);
		if (!::UnigineEditor::ObjectMode::isManipulatorsEnabled())
			::UnigineEditor::ObjectMode::setManipulatorsEnabled(true);
	}
	show_manipulators();
}

void CurveEditor::setAddingPoints(bool adding)
{
	adding_points_ = adding;
	update_manipulators();
}

void CurveEditor::update_manipulators() const
{
	if (adding_points_ || drag_point_)
		hide_manipulators();
	else
		show_manipulators();
}

void CurveEditor::setRoot(const NodePtr &root)
{
	if (root_ == root)
		return;
	reset();
	root_ = root;
}

NodePtr CurveEditor::getPathNode() const
{
	return root_ && !root_.isDeleted() ? findChildNode(root_, CURVE_PATH_NODE_NAME) : NodePtr();
}

CurvePoints CurveEditor::getPoints() const
{
	return CurvePoints::collect(getPathNode(), closed_);
}

void CurveEditor::reset()
{
	end_drag();
	pending_selection_ = NodePtr();
	click_taken_ = false;
	swallow_release_ = false;
}

void CurveEditor::update()
{
	update_drag();
}

Vector<NodePtr> CurveEditor::getSelectedPoints() const
{
	Vector<NodePtr> points;
	const NodePtr path = getPathNode();
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

CurvePointsState CurveEditor::getSelectedPointsState() const
{
	int smooth = 0;
	int corner = 0;
	for (const NodePtr &point : getSelectedPoints())
		++(isCornerPoint(point) ? corner : smooth);

	if (smooth && corner)
		return POINTS_MIXED;
	if (corner)
		return POINTS_CORNER;
	return smooth ? POINTS_SMOOTH : POINTS_NONE;
}

void CurveEditor::setSelectedPointsCorner(bool corner)
{
	setSelectedPointsFlag(CURVE_CORNER_DATA, corner);
}

void CurveEditor::countSelectedPointsFlag(const char *data, int &on, int &off) const
{
	on = 0;
	off = 0;
	for (const NodePtr &point : getSelectedPoints())
		++(isPointFlag(point, data) ? on : off);
}

void CurveEditor::setSelectedPointsFlag(const char *data, bool on)
{
	// One step of the undo history for all of them.
	SetNodeDataAction *action = new SetNodeDataAction;
	for (const NodePtr &point : getSelectedPoints())
		action->set(point, data, on ? "1" : "0");
	SetNodeDataAction::push(action, false);
	notify_changed();
}

bool CurveEditor::canSubdivide() const
{
	const NodePtr path = getPathNode();
	const Vector<NodePtr> selected = getSelectedPoints();
	if (!path || selected.size() < 2)
		return false;

	const int count = path->getNumChildren();
	const int spans = CurvePoints::collect(path, closed_).spans();
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

void CurveEditor::subdivide()
{
	const NodePtr path = getPathNode();
	if (!path)
		return;

	const CurvePoints points = CurvePoints::collect(path, closed_);
	const int count = points.count();
	const Vector<NodePtr> selected = getSelectedPoints();
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

	// The width of a point as a road reads it: none - 1.
	const auto width_of = [](const NodePtr &point, bool *set) {
		const char *data = point->getData(CURVE_WIDTH_DATA);
		bool ok = false;
		const double width = QByteArray(data ? data : "").toDouble(&ok);
		*set = *set || ok;
		return ok ? width : 1.0;
	};

	// From the end, so that the indices of the spans that are still to be split stay.
	// The new point is halfway between its neighbours in what they have too - the
	// width and the roll of a road; it is off the ground if one of them is, so that the
	// road keeps to the curve where it did.
	Vector<NodePtr> created;
	for (int k = int(spans.size()) - 1; k >= 0; --k)
	{
		const int span = spans[k];
		const int next = (span + 1) % count;
		const NodePtr a = path->getChild(span);
		const NodePtr b = path->getChild(next);
		NodeDummyPtr point = NodeDummy::create();
		point->setName(QString::fromUtf8(CURVE_POINT_NODE_NAME).arg(count + int(created.size())).toUtf8().constData());
		point->setWorldParent(path);
		path->setChildIndex(point, span + 1);
		const Math::Vec3 position = points.spanPoint(span, 0.5);
		const double roll = (points.rolls[span] + points.rolls[next]) * 0.5;
		if (std::abs(roll) > 1e-6)
		{
			const Math::Vec3 forward = points.spanPoint(span, 0.51) - points.spanPoint(span, 0.49);
			point->setWorldTransform(Math::translate(position) * Math::Mat4(curvePointBasis(forward, roll)));
		} else
			point->setWorldPosition(position);
		bool has_width = false;
		const double width = (width_of(a, &has_width) + width_of(b, &has_width)) * 0.5;
		if (has_width)
			point->setData(CURVE_WIDTH_DATA, QByteArray::number(width, 'g', 6).constData());
		if (points.off_ground[span] || points.off_ground[next])
			point->setData(CURVE_OFF_GROUND_DATA, "1");
		// What the point the span starts at sets for it, for both its halves.
		for (const QByteArray &name : span_data_)
		{
			if (const char *value = a->getData(name.constData()))
				point->setData(name.constData(), value);
		}
		point->setShowInEditorEnabledRecursive(true);
		point->setSaveToWorldEnabledRecursive(true);
		created.append(point);
	}
	// The new points keep their places between the old ones after Ctrl+Z and Ctrl+Y too.
	SetNodeOrderAction *order = new SetNodeOrderAction;
	for (const NodePtr &point : created)
		order->add(point);
	::UnigineEditor::Undo::begin();
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(created));
	::UnigineEditor::Undo::apply(order);
	::UnigineEditor::Undo::commit();

	report(MSG_INFO, QString());
	notify_changed();
}

// The point of the curve the world ray p0 -> p1 goes through, the closest one.
NodePtr CurveEditor::pick_point(const Math::Vec3 &p0, const Math::Vec3 &p1) const
{
	const NodePtr path = getPathNode();
	if (!path)
		return NodePtr();

	const Math::Vec3 direction = p1 - p0;
	const double ray_length = double(Math::length(direction));
	if (ray_length < 1e-9)
		return NodePtr();

	// The zone a click picks a point in is much larger than its sphere, so the zones of
	// points that are close on the screen overlap. The point the click is closest to,
	// in parts of its zone, wins: a click right on a point always takes that point.
	// While points are added the clicks are the tool's and the manipulator is not
	// used: every point keeps the large zone then.
	const bool manipulator = !adding_points_ && ::UnigineEditor::ObjectMode::isManipulatorsEnabled();
	const Vector<NodePtr> selected = manipulator ? getSelectedPoints() : Vector<NodePtr>();
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

		float pick_scale = POINT_PICK_SCALE;
		for (const NodePtr &selected_point : selected)
		{
			if (selected_point->getID() == point->getID())
				pick_scale = POINT_SELECTED_PICK_SCALE;
		}
		const double radius = double(curvePointRadius(position) * pick_scale);
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

// Adds a point at the end of the curve, whatever is selected: drawing only continues
// the curve. A point between two others is put by Subdivide.
NodePtr CurveEditor::add_point(const Math::Vec3 &position, bool corner)
{
	const NodePtr path = getPathNode();
	if (!path)
		return NodePtr();

	const int count = path->getNumChildren();
	NodeDummyPtr point = NodeDummy::create();
	point->setName(QString::fromUtf8(CURVE_POINT_NODE_NAME).arg(count).toUtf8().constData());
	point->setWorldParent(path);
	point->setWorldPosition(position);
	if (corner)
		point->setData(CURVE_CORNER_DATA, "1");
	point->setShowInEditorEnabledRecursive(true);
	point->setSaveToWorldEnabledRecursive(true);
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(point));
	return point;
}

void CurveEditor::select_point(const NodePtr &point, bool add_to_selection)
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

void CurveEditor::begin_drag(const NodePtr &point, bool is_new)
{
	drag_point_ = point;
	drag_start_transform_ = point->getWorldTransform();
	drag_is_new_ = is_new;
	drag_moving_ = false;
	drag_press_position_ = QCursor::pos();
	drag_timer_.start();

	// The manipulators of the editor sit right on the point; they must not grab it
	// while the tool is moving it.
	update_manipulators();
}

// Every frame while a point is held: it goes to the surface under the mouse.
void CurveEditor::update_drag()
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

	// The nodes of the setup itself are looked through. A point marked to stay off
	// the ground keeps its height and is not put onto the surfaces.
	if (!isPointFlag(drag_point_, CURVE_OFF_GROUND_DATA))
	{
		Vector<NodePtr> ignore;
		ignore.append(root_);
		if (look_through_)
			ignore.append(look_through_);
		const SurfaceHit hit = raycastSurface(p0, p1, filter_, ignore);
		if (hit.found)
		{
			drag_point_->setWorldPosition(hit.point);
			return;
		}
	}

	// No surface under the mouse (or the point stays off the ground): the point keeps
	// its height and goes along the horizontal plane it is in, to where the mouse ray
	// crosses that plane.
	const Math::Vec3 position = drag_point_->getWorldPosition();
	const Math::Vec3 direction = p1 - p0;
	if (std::abs(double(direction.z)) < 1e-9)
		return;
	const double along = double(position.z - p0.z) / double(direction.z);
	if (along > 0.0 && along <= 1.0)
		drag_point_->setWorldPosition(p0 + direction * Math::Scalar(along));
}

void CurveEditor::end_drag()
{
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
	update_manipulators();
}

// What a click taken by the tool ends with, when its button is released: the held
// point stays where it is and the picked point becomes selected.
void CurveEditor::finish_click()
{
	end_drag();

	if (pending_selection_ && !pending_selection_.isDeleted())
		select_point(pending_selection_, pending_selection_adds_);
	pending_selection_ = NodePtr();
}

bool CurveEditor::is_point_selected(const NodePtr &point) const
{
	for (const NodePtr &selected : getSelectedPoints())
	{
		if (selected->getID() == point->getID())
			return true;
	}
	return false;
}

bool CurveEditor::handleEvent(QObject *watched, QEvent *event)
{
	if (!getPathNode())
		return false;

	switch (event->type())
	{
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
			// Holding the button drags the point over the surfaces (over the empty
			// space - at its own height), selected or not.
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

			// Ctrl makes the new point a corner right away.
			Vector<NodePtr> ignore;
			ignore.append(root_);
			if (look_through_)
				ignore.append(look_through_);
			const SurfaceHit hit = raycastSurface(p0, p1, filter_, ignore);
			if (!hit.found)
				report(MSG_WARNING, uiText("Click on a surface to add a point there.", "Кликните по поверхности, чтобы добавить на неё точку."));
			else if (NodePtr point = add_point(hit.point, mouse->modifiers() & Qt::ControlModifier))
			{
				// The new point is not selected: it is the last one and is drawn green,
				// the point the next one continues from.
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

void CurveEditor::drawPoints() const
{
	const NodePtr path = getPathNode();
	if (!path)
		return;

	// Points: the selected ones are orange, the last one is green (a drawn point is
	// added after it), corners are white, smooth points blue.
	//
	// The editor gets the new selection only when the mouse button is released. Until
	// then the points are shown the way they are going to be: a point picked without
	// Ctrl or Shift replaces the selection, so the points selected before it are
	// already drawn as not selected while it is held and dragged.
	const bool has_pending = pending_selection_ && !pending_selection_.isDeleted();
	Vector<NodePtr> selected;
	if (!has_pending || pending_selection_adds_)
		selected = getSelectedPoints();
	for (int i = 0, num = path->getNumChildren(); i < num; ++i)
	{
		const NodePtr point = path->getChild(i);
		const Math::Vec3 position = point->getWorldPosition();

		Math::vec4 color(0.35f, 0.7f, 1.0f, 1.0f);
		if (isCornerPoint(point))
			color = Math::vec4(1.0f, 1.0f, 1.0f, 1.0f);
		if (i == num - 1)
			color = Math::vec4(0.3f, 0.9f, 0.3f, 1.0f);
		for (const NodePtr &selected_point : selected)
		{
			if (selected_point->getID() == point->getID())
				color = Math::vec4(1.0f, 0.55f, 0.1f, 1.0f);
		}
		// The point picked by the click that is still held is about to be selected.
		if (has_pending && pending_selection_->getID() == point->getID())
			color = Math::vec4(1.0f, 0.55f, 0.1f, 1.0f);

		Visualizer::renderSolidSphere(curvePointRadius(position), Math::translate(position), color, 0.0f, false);
	}
}

void CurveEditor::notify_changed() const
{
	if (on_changed)
		on_changed();
}

void CurveEditor::report(int level, const QString &text) const
{
	if (on_message)
		on_message(level, text);
}

} // namespace ArtistTool
