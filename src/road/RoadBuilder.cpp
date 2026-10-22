#include "RoadBuilder.h"
#include "UiHelpers.h"
#include "Localization.h"
#include "GeneratedFiles.h"
#include "NodeData.h"
#include "PlaceableAsset.h"
#include "UndoActions.h"

#include <editor/UnigineActions.h>
#include <editor/UnigineAssetManager.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>
#include <editor/UnigineUndo.h>

#include <UnigineDecals.h>
#include <UnigineEngine.h>
#include <UnigineFileSystem.h>
#include <UnigineMaterials.h>
#include <UnigineMesh.h>
#include <UnigineNodes.h>
#include <UnigineObjects.h>
#include <UnigineRender.h>
#include <UnigineWorld.h>

#include <QApplication>
#include <QCryptographicHash>
#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QRegularExpression>
#include <QSet>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Unigine;
using ::UnigineEditor::AssetManager;
using ::UnigineEditor::Selection;
using ::UnigineEditor::SelectorNodes;

namespace ArtistTool
{

namespace
{

// The height of the ground where there is none. Not a NaN: the fast floating point
// options of the build may take a NaN check for always false.
const double NO_GROUND = -1.0e300;
bool no_ground(double height)
{
	return height <= NO_GROUND * 0.5;
}

const char ROOT_NODE_NAME[] = "road";
const char MESH_NODE_NAME[] = "mesh";
const char DEFAULT_FOLDER_NAME[] = "roads";

// Names of the node data the tool keeps its state in: the settings, and what the last
// Generate did - the files it wrote, the name and the folder they got, and what the
// road was like then.
const char SETTINGS_DATA[] = "artist_tool_road";
const char FILES_DATA[] = "artist_tool_road_files";
// The GUIDs of those files, line by line: a file moved away and another one put at its
// path is not taken for one of them.
const char FILE_GUIDS_DATA[] = "artist_tool_road_file_guids";
const char BASE_NAME_DATA[] = "artist_tool_road_base";
const char FOLDER_DATA[] = "artist_tool_road_folder";
const char STATE_DATA[] = "artist_tool_road_state";
// What the pieces made by the last Generate are: "mesh", "decal" or "bent:" and the
// asset bent. Pieces of another kind are not kept by the next Generate.
const char KIND_DATA[] = "artist_tool_road_kind";
// The ID of the road whose files these are: a road copied by the editor (Duplicate,
// Copy / Paste) takes the data of the original, and must not write or remove the
// files of the original.
const char OWNER_DATA[] = "artist_tool_road_owner";

// The data of the last Generate, for the helpers of common/GeneratedFiles.
GeneratedData road_generated_data()
{
	return GeneratedData{FILES_DATA, FILE_GUIDS_DATA, BASE_NAME_DATA, FOLDER_DATA, OWNER_DATA};
}

// "1" on a piece hidden while it is out of date (see setHideOutdated()).
const char HIDDEN_DATA[] = "artist_tool_hidden";
// The width of a point of the road, a multiplier of Width. None - 1.
const char *const POINT_WIDTH_DATA = CURVE_WIDTH_DATA;
// The roles of the meshes of a bent road in the settings.
const char *const BEND_ROLE_KEYS[3] = {"pattern", "start", "end"};
// The most copies a bent road may have.
const int MAX_BEND_COPIES = 100000;

RoadBendPart bend_part_from_json(const QJsonObject &item)
{
	RoadBendPart part;
	part.source = item.value("source").toString();
	part.source_kind = item.value("kind").toInt();
	part.source_name = item.value("name").toString();
	const QString role = item.value("role").toString();
	for (int r = 0; r < 3; ++r)
	{
		if (role == QString::fromUtf8(BEND_ROLE_KEYS[r]))
			part.role = RoadBendRole(r);
	}
	part.scale = Math::vec3(float(item.value("scale_x").toDouble(1.0)), float(item.value("scale_y").toDouble(1.0)),
		float(item.value("scale_z").toDouble(1.0)));
	part.length = float(item.value("length").toDouble(1.0));
	part.auto_length = item.value("auto_length").toBool(true);
	part.count = item.value("count").toInt(1);
	return part;
}

// Every span of the curve is measured on a polyline of this many pieces.
const int DENSE_SUBDIVISIONS = 64;

// How far above and below the curve the ground is searched for.
const double SNAP_UP = 5.0;
const double SNAP_DOWN = 100.0;

const float MIN_WIDTH = 0.01f;
const int MAX_COLUMNS = 64;
const float MIN_STEP = 0.05f;
const float MIN_ANGLE = 0.5f;
// The most heights of the ground kept; then they are measured anew.
const int MAX_GROUND_CACHE = 2000000;
// Fetches from the Landscape Terrain are done in batches of this many.
const int FETCH_BATCH = 4096;
// A mesh decal of more triangles than this gets slow to draw; a decal road is cut
// into pieces of at most this many.
const int MAX_DECAL_TRIANGLES = 10000;
const double MIN_POINT_WIDTH = 0.01;
const double MAX_POINT_WIDTH = 100.0;
// The most triangles the copies of a bent mesh may have all together.
const int MAX_BEND_TRIANGLES = 1000000;

// A ray finds the ground from above the point down, so what it finds depends on the
// height of the point too (one level of a bridge or another); the Landscape Terrain
// has one height at a place whatever the height of the point is.
GroundKey ground_key(const Math::Vec3 &point, bool with_height)
{
	return qMakePair(qMakePair(qint64(std::llround(double(point.x) * 1000.0)), qint64(std::llround(double(point.y) * 1000.0))),
		with_height ? qint64(std::llround(double(point.z) * 1000.0)) : qint64(0));
}
// A grid with more rows than this is not built: it would be too heavy to draw and to
// put on the ground every time a point moves.
const int MAX_STATIONS = 20000;
// At a corner the edges of the road meet at an angle; the farthest the outer corner
// may stick out, in half widths of the road.
const double MITER_LIMIT = 3.0;

bool is_road_root(const NodePtr &node)
{
	// A line of wires drawn as a curve has a "path" and a "mesh" too; it has the data of
	// the Wires tool.
	return node->findChild(CURVE_PATH_NODE_NAME) >= 0 && node->findChild(MESH_NODE_NAME) >= 0
		&& !node->hasData("artist_tool_wire");
}

// The root of the road the node is a part of.
NodePtr find_road_root(const NodePtr &node)
{
	for (NodePtr n = node; n; n = n->getParent())
	{
		if (is_road_root(n))
			return n;
	}
	return NodePtr();
}

double point_width(const NodePtr &point)
{
	const char *data = point->getData(POINT_WIDTH_DATA);
	bool ok = false;
	const double width = QByteArray(data ? data : "").toDouble(&ok);
	return ok ? Math::clamp(width, MIN_POINT_WIDTH, MAX_POINT_WIDTH) : 1.0;
}

// The side of the road to the right of the direction it goes in, level.
Math::Vec3 right_of(const Math::Vec3 &direction)
{
	return unitOrZero(Math::Vec3(direction.y, -direction.x, 0.0));
}

Math::Vec3 level(const Math::Vec3 &v)
{
	return Math::Vec3(v.x, v.y, 0.0);
}

RoadSettings limited(RoadSettings settings)
{
	settings.width = qMax(settings.width, MIN_WIDTH);
	settings.columns = qBound(1, settings.columns, MAX_COLUMNS);
	settings.step = qMax(settings.step, MIN_STEP);
	settings.max_angle = qBound(MIN_ANGLE, settings.max_angle, 90.0f);
	settings.uv_length = qMax(settings.uv_length, 0.001f);
	settings.piece_length = qMax(settings.piece_length, 0.0f);
	settings.decal_depth = qMax(settings.decal_depth, 0.01f);
	settings.decal_lift = qMax(settings.decal_lift, 0.0f);
	settings.decal_visible_distance = qMax(settings.decal_visible_distance, 0.0f);
	settings.decal_fade_distance = qMax(settings.decal_fade_distance, 0.0f);
	settings.inner_round = qMax(settings.inner_round, 0.0f);
	settings.roll = qBound(-90.0f, settings.roll, 90.0f);
	settings.bend_mesh_angle = qBound(0.0f, settings.bend_mesh_angle, 90.0f);
	settings.bend_forward = AxisDirection(qBound(int(AXIS_X_POSITIVE), int(settings.bend_forward), int(AXIS_Z_NEGATIVE)));
	settings.bend_up = AxisDirection(qBound(int(AXIS_X_POSITIVE), int(settings.bend_up), int(AXIS_Z_NEGATIVE)));
	// One start and one end at most (the first ones), the start first in the list, the
	// end last, the pattern in its order between them.
	bool has_start = false;
	bool has_end = false;
	QVector<RoadBendPart> starts;
	QVector<RoadBendPart> patterns;
	QVector<RoadBendPart> ends;
	for (RoadBendPart part : settings.bend_parts)
	{
		for (int axis = 0; axis < 3; ++axis)
			part.scale[axis] = qMax(part.scale[axis], 0.001f);
		part.length = qMax(part.length, 0.0f);
		part.count = qBound(1, part.count, 999);
		if (part.role == ROAD_BEND_START && has_start)
			part.role = ROAD_BEND_PATTERN;
		if (part.role == ROAD_BEND_END && has_end)
			part.role = ROAD_BEND_PATTERN;
		if (part.role == ROAD_BEND_START)
		{
			has_start = true;
			starts.append(part);
		} else if (part.role == ROAD_BEND_END)
		{
			has_end = true;
			ends.append(part);
		} else
			patterns.append(part);
	}
	settings.bend_parts = starts + patterns + ends;
	settings.ground.look_through = true;
	return settings;
}

// Whether the material can be put on the pieces of the kind: a decal material on
// decals, a mesh one on meshes. The engine itself is asked, so a material made with
// the Material Graph is told right too.
bool material_suits(const MaterialPtr &material, Node::TYPE piece_type)
{
	return material && material->isNodeTypeSupported(piece_type);
}

// The pieces of a road: the mesh nodes and the decals in its "mesh" node.
bool is_piece(const NodePtr &node)
{
	return node->getType() == Node::OBJECT_MESH_STATIC || node->getType() == Node::DECAL_MESH;
}

MaterialPtr material_of(const NodePtr &piece)
{
	if (piece->getType() == Node::DECAL_MESH)
		return checked_ptr_cast<DecalMesh>(piece)->getMaterial();
	if (piece->getType() == Node::OBJECT_MESH_STATIC)
	{
		ObjectMeshStaticPtr object = checked_ptr_cast<ObjectMeshStatic>(piece);
		return object->getNumSurfaces() > 0 ? object->getMaterial(0) : MaterialPtr();
	}
	return MaterialPtr();
}

String mesh_path_of(const NodePtr &piece)
{
	if (piece->getType() == Node::DECAL_MESH)
		return String(checked_ptr_cast<DecalMesh>(piece)->getMeshPath());
	if (piece->getType() == Node::OBJECT_MESH_STATIC)
		return String(checked_ptr_cast<ObjectMeshStatic>(piece)->getMeshPath());
	return String();
}

void set_mesh_path(const NodePtr &piece, const String &path)
{
	if (mesh_path_of(piece) == path)
		return;
	if (piece->getType() == Node::DECAL_MESH)
		checked_ptr_cast<DecalMesh>(piece)->setMeshPath(path.get());
	else if (piece->getType() == Node::OBJECT_MESH_STATIC)
		checked_ptr_cast<ObjectMeshStatic>(piece)->setMeshPath(path.get());
}

////////////////////////////////////////////////////////////////////////////////
// The grid.
////////////////////////////////////////////////////////////////////////////////

// The curve measured: a dense polyline, the length along it to every sample, and
// how much it has turned up to every sample, in degrees. The turn is counted within
// the spans only: the bend at a corner point is not spread over the rows around it.
struct Measured
{
	QVector<Math::Vec3> samples;
	QVector<double> distances;
	QVector<double> turns;
	// The sample every point of the curve is at, the end of the last span included.
	QVector<int> point_samples;
};

Measured measure(const CurvePoints &points)
{
	Measured m;
	const int spans = points.spans();
	m.samples.reserve(spans * DENSE_SUBDIVISIONS + 1);
	m.samples.append(points.positions[0]);
	m.distances.append(0.0);
	m.turns.append(0.0);
	m.point_samples.append(0);
	for (int span = 0; span < spans; ++span)
	{
		Math::Vec3 previous_direction(0.0, 0.0, 0.0);
		for (int s = 1; s <= DENSE_SUBDIVISIONS; ++s)
		{
			const Math::Vec3 p = points.spanPoint(span, double(s) / DENSE_SUBDIVISIONS);
			const Math::Vec3 step = p - m.samples.last();
			const Math::Vec3 direction = unitOrZero(step);
			double turn = 0.0;
			if (s > 1 && Math::length2(previous_direction) > 0.0 && Math::length2(direction) > 0.0)
				turn = std::acos(Math::clamp(double(Math::dot(previous_direction, direction)), -1.0, 1.0)) * Math::Consts::RAD2DEG_D;
			if (Math::length2(direction) > 0.0)
				previous_direction = direction;

			m.samples.append(p);
			m.distances.append(m.distances.last() + double(Math::length(step)));
			m.turns.append(m.turns.last() + turn);
		}
		m.point_samples.append(int(m.samples.size()) - 1);
	}
	return m;
}

// The curve moved to the side by 'offset' (positive - to the right), as a curve of its
// own: every sample goes out square to the curve, at a bend along its bisector and
// farther, so that the moved curve keeps its distance on both sides of the bend; the
// lengths and the turns are measured along the moved curve.
void offset_measured(Measured &m, double offset, bool closed)
{
	const int count = int(m.samples.size());
	if (count < 2 || std::abs(offset) < 1e-9)
		return;

	QVector<Math::Vec3> moved(count);
	for (int k = 0; k < count; ++k)
	{
		int previous = k - 1;
		int next = k + 1;
		if (closed)
		{
			if (previous < 0)
				previous = count - 2;
			if (next >= count)
				next = 1;
		} else
		{
			previous = qMax(previous, 0);
			next = qMin(next, count - 1);
		}
		Math::Vec3 right_in = right_of(m.samples[k] - m.samples[previous]);
		Math::Vec3 right_out = right_of(m.samples[next] - m.samples[k]);
		if (Math::length2(right_in) == 0.0)
			right_in = right_out;
		if (Math::length2(right_out) == 0.0)
			right_out = right_in;
		Math::Vec3 right = unitOrZero(right_in + right_out);
		if (Math::length2(right) == 0.0)
			right = right_in;
		const double scale = 1.0 / qMax(double(Math::dot(right, right_in)), 1.0 / MITER_LIMIT);
		moved[k] = m.samples[k] + right * Math::Scalar(offset * scale);
	}

	// On the inner side of a bend the moved curve crosses itself: the samples before
	// the bend go on past the place where the two sides meet, those after it start
	// before that place, and the curve makes a little loop there. The loop is cut: every
	// sample of it goes to the place where the curve crosses itself (they keep their
	// count, so the points of the curve keep their samples).
	const double reach = std::abs(offset) * 4.0 + 1e-6;
	const auto cross_2d = [](const Math::Vec3 &a, const Math::Vec3 &b) { return double(a.x * b.y - a.y * b.x); };
	for (int i = 0; i + 1 < count; ++i)
	{
		const Math::Vec3 a0 = moved[i];
		const Math::Vec3 a1 = moved[i + 1];
		const Math::Vec3 da = a1 - a0;
		for (int j = i + 2; j + 1 < count && m.distances[j] - m.distances[i + 1] < reach; ++j)
		{
			const Math::Vec3 b0 = moved[j];
			const Math::Vec3 db = moved[j + 1] - b0;
			const double denominator = cross_2d(da, db);
			if (std::abs(denominator) < 1e-12)
				continue;
			const Math::Vec3 ab = b0 - a0;
			const double s = cross_2d(ab, db) / denominator;
			const double t = cross_2d(ab, da) / denominator;
			if (s < 0.0 || s > 1.0 || t < 0.0 || t > 1.0)
				continue;
			const Math::Vec3 meet = a0 + da * Math::Scalar(s);
			const Math::Vec3 meet_b = b0 + db * Math::Scalar(t);
			const Math::Vec3 place(meet.x, meet.y, (meet.z + meet_b.z) * Math::Scalar(0.5));
			for (int k = i + 1; k <= j; ++k)
				moved[k] = place;
			i = j - 1;
			break;
		}
	}

	m.samples = moved;
	m.distances[0] = 0.0;
	m.turns[0] = 0.0;
	const int spans = int(m.point_samples.size()) - 1;
	for (int span = 0; span < spans; ++span)
	{
		Math::Vec3 previous_direction(0.0, 0.0, 0.0);
		for (int k = m.point_samples[span] + 1; k <= m.point_samples[span + 1]; ++k)
		{
			const Math::Vec3 step = m.samples[k] - m.samples[k - 1];
			const Math::Vec3 direction = unitOrZero(step);
			double turn = 0.0;
			if (Math::length2(previous_direction) > 0.0 && Math::length2(direction) > 0.0)
				turn = std::acos(Math::clamp(double(Math::dot(previous_direction, direction)), -1.0, 1.0)) * Math::Consts::RAD2DEG_D;
			if (Math::length2(direction) > 0.0)
				previous_direction = direction;
			m.distances[k] = m.distances[k - 1] + double(Math::length(step));
			m.turns[k] = m.turns[k - 1] + turn;
		}
	}
}

// The direction of the dense polyline at its sample 'k', from the piece that leaves
// it (incoming = false) or the one that comes to it (incoming = true), within the
// samples [first, last] of one span.
Math::Vec3 piece_direction(const Measured &m, int k, bool incoming, int first, int last)
{
	if (incoming)
	{
		for (int i = k; i > first; --i)
		{
			const Math::Vec3 d = unitOrZero(m.samples[i] - m.samples[i - 1]);
			if (Math::length2(d) > 0.0)
				return d;
		}
	}
	for (int i = k; i < last; ++i)
	{
		const Math::Vec3 d = unitOrZero(m.samples[i + 1] - m.samples[i]);
		if (Math::length2(d) > 0.0)
			return d;
	}
	for (int i = k; i > first; --i)
	{
		const Math::Vec3 d = unitOrZero(m.samples[i] - m.samples[i - 1]);
		if (Math::length2(d) > 0.0)
			return d;
	}
	return Math::Vec3(0.0, 1.0, 0.0);
}

// A row of the grid: where it crosses the curve and the directions the curve comes to
// it and leaves it in (different at a corner).
struct Station
{
	Math::Vec3 position;
	Math::Vec3 incoming;
	Math::Vec3 outgoing;
	double distance{0.0};
	// How much the row lies on the ground: 1 - on it, 0 - at the height of the curve.
	// A point of the curve marked "off the ground" has 0, the rows between two points
	// go smoothly from the one to the other.
	double ground{1.0};
	// How much the road leans to the side about the curve there, in degrees: from
	// the roll of one point of the curve to that of the next one.
	double roll{0.0};
	// The width there, a multiplier of Width; from one point to the next the same way.
	double width{1.0};
};

// The values of the points of the curve the rows take between them.
struct PointValues
{
	QVector<double> widths;
};

// The rows along the curve. Every span gets rows of its own, spread by a measure that
// grows with the length and with the turn of the curve: on a straight the rows are
// 'step' apart, on a bend they come closer so that the road turns by at most
// 'max_angle' from one row to the next. Every point of the curve gets a row.
QVector<Station> place_stations(const Measured &m, const CurvePoints &points, const PointValues &values, float step,
	float max_angle, bool *too_many)
{
	QVector<Station> stations;
	*too_many = false;
	const int spans = int(m.point_samples.size()) - 1;

	// How many rows every span needs.
	QVector<int> counts(spans, 0);
	int total = 1;
	for (int span = 0; span < spans; ++span)
	{
		const int first = m.point_samples[span];
		const int last = m.point_samples[span + 1];
		const double length = m.distances[last] - m.distances[first];
		if (length < 1e-6)
			continue;
		const double need = length / double(step) + (m.turns[last] - m.turns[first]) / double(max_angle);
		counts[span] = qMax(1, int(std::ceil(need - 1e-6)));
		total += counts[span];
		if (total > MAX_STATIONS)
		{
			*too_many = true;
			return stations;
		}
	}

	for (int span = 0; span < spans; ++span)
	{
		if (counts[span] == 0)
			continue;
		const int first = m.point_samples[span];
		const int last = m.point_samples[span + 1];

		// The measure of every sample of the span, and the rows at equal steps of it.
		const auto need_at = [&](int k) {
			return (m.distances[k] - m.distances[first]) / double(step) + (m.turns[k] - m.turns[first]) / double(max_angle);
		};
		const double span_need = need_at(last);
		int k = first;
		for (int row = 0; row < counts[span]; ++row)
		{
			const double target = span_need * double(row) / double(counts[span]);
			while (k + 1 < last && need_at(k + 1) < target)
				++k;
			const double a = need_at(k);
			const double b = need_at(k + 1);
			const double t = row == 0 || b - a < 1e-12 ? 0.0 : Math::clamp((target - a) / (b - a), 0.0, 1.0);

			Station station;
			station.position = m.samples[k] + (m.samples[k + 1] - m.samples[k]) * Math::Scalar(t);
			station.distance = m.distances[k] + (m.distances[k + 1] - m.distances[k]) * t;
			station.outgoing = piece_direction(m, k, false, first, last);
			station.incoming = station.outgoing;
			{
				const double from = points.off_ground[span] ? 0.0 : 1.0;
				const double to = points.off_ground[(span + 1) % points.count()] ? 0.0 : 1.0;
				const double length = m.distances[last] - m.distances[first];
				const double f = length > 1e-9 ? Math::clamp((station.distance - m.distances[first]) / length, 0.0, 1.0) : 0.0;
				const double smooth = f * f * (3.0 - 2.0 * f);
				station.ground = from + (to - from) * smooth;
				const int next = (span + 1) % points.count();
				station.roll = points.rolls[span] + (points.rolls[next] - points.rolls[span]) * smooth;
				station.width = values.widths[span] + (values.widths[next] - values.widths[span]) * smooth;
			}
			if (row == 0)
			{
				// The row at a point of the curve: the curve comes from the previous span.
				// A span of no length (two points in one place) gives no direction.
				const int previous = span > 0 ? span - 1 : spans - 1;
				if ((span > 0 || points.closed) && counts[previous] > 0)
				{
					station.incoming = piece_direction(m, m.point_samples[previous + 1], true,
						m.point_samples[previous], m.point_samples[previous + 1]);
				}
			}
			stations.append(station);
		}
	}

	// The last row, at the end of the curve. On a closed curve it is the first one
	// again: the same place and the same directions, so the road closes without a gap.
	Station end;
	end.position = m.samples.last();
	end.distance = m.distances.last();
	if (points.closed && !stations.isEmpty())
	{
		end.position = stations.first().position;
		end.incoming = stations.first().incoming;
		end.outgoing = stations.first().outgoing;
		end.ground = stations.first().ground;
		end.roll = stations.first().roll;
		end.width = stations.first().width;
	} else
	{
		end.ground = points.off_ground.last() ? 0.0 : 1.0;
		end.roll = points.rolls.last();
		end.width = values.widths.last();
		const int first = m.point_samples[spans > 0 ? spans - 1 : 0];
		end.incoming = piece_direction(m, int(m.samples.size()) - 1, true, first, int(m.samples.size()) - 1);
		end.outgoing = end.incoming;
		if (!stations.isEmpty())
			stations.first().incoming = stations.first().outgoing;
	}
	stations.append(end);
	return stations;
}

// The road is cut into pieces of the same length, none longer than 'piece_length', at
// rows only. Where no row is close enough to a cut for the pieces to keep within that
// length (long straight rows, a large Step), a row is put there: between two rows the
// road is straight, so the new one changes nothing in its shape.
void add_cut_stations(QVector<Station> &stations, double piece_length)
{
	if (piece_length <= 0.0 || stations.size() < 2)
		return;
	const double length = stations.last().distance;
	if (length <= piece_length)
		return;
	const int pieces = int(std::ceil(length / piece_length));
	// How far a cut may be from its place: the pieces are length / pieces long, and
	// the room left to piece_length is shared by the two cuts of a piece.
	const double room = qMax(0.0, (piece_length - length / double(pieces)) * 0.5);
	for (int p = 1; p < pieces; ++p)
	{
		const double target = length * double(p) / double(pieces);
		int next = 1;
		while (next < stations.size() - 1 && stations[next].distance < target)
			++next;
		const Station &a = stations[next - 1];
		const Station &b = stations[next];
		if (std::abs(a.distance - target) <= room + 1e-9 || std::abs(b.distance - target) <= room + 1e-9)
			continue;
		if (stations.size() >= MAX_STATIONS)
			return;
		const double t = Math::clamp((target - a.distance) / qMax(b.distance - a.distance, 1e-12), 0.0, 1.0);
		Station station;
		station.position = a.position + (b.position - a.position) * Math::Scalar(t);
		station.distance = target;
		station.outgoing = unitOrZero(b.position - a.position);
		if (Math::length2(station.outgoing) == 0.0)
			station.outgoing = a.outgoing;
		station.incoming = station.outgoing;
		station.ground = a.ground + (b.ground - a.ground) * t;
		station.roll = a.roll + (b.roll - a.roll) * t;
		station.width = a.width + (b.width - a.width) * t;
		stations.insert(next, station);
	}
}

Math::Vec3 quadratic_bezier(const Math::Vec3 &p0, const Math::Vec3 &p1, const Math::Vec3 &p2, double t)
{
	const Math::Scalar k0 = Math::Scalar(t);
	const Math::Scalar k1 = Math::Scalar(1.0 - t);
	return p0 * (k1 * k1) + p1 * (Math::Scalar(2.0) * k0 * k1) + p2 * (k0 * k0);
}

// One edge of the road, a vertex per row. On the inner side of a tight bend the edge
// gets sharp: it makes a point where the bend is about as tight as half the road, and
// where it is tighter it goes back against the way the road goes and would fold over
// itself. Such a place is found - the vertices that go back, and the ones where the
// edge turns around less than 'round_length' - together with 'round_length' of the
// road before and after it, and that part of the edge is laid anew along a smooth
// arc. The arc leaves the edge and comes back to it along the edge itself, so there
// is no break where it starts and ends. The vertices are spread along the arc as the
// rows are spread along the road.
void round_edge(QVector<Math::Vec3> &edge, const QVector<Math::Vec3> &forwards, const QVector<double> &distances,
	double round_length)
{
	const int count = int(edge.size());
	if (count < 3)
		return;

	// The edge without its folds: a vertex that goes back stays where the edge was last
	// going forward.
	QVector<bool> sharp(count, false);
	for (int i = 1; i < count; ++i)
	{
		const double progress = double(Math::dot(level(edge[i] - edge[i - 1]), forwards[i]));
		if (progress < 1e-4)
		{
			edge[i] = edge[i - 1];
			sharp[i] = true;
		}
	}

	// The points: the edge turns around a radius smaller than round_length there.
	if (round_length > 0.0)
	{
		for (int i = 1; i + 1 < count; ++i)
		{
			const Math::Vec3 in = level(edge[i] - edge[i - 1]);
			const Math::Vec3 out = level(edge[i + 1] - edge[i]);
			const double in_length = double(Math::length(in));
			const double out_length = double(Math::length(out));
			if (in_length < 1e-9 || out_length < 1e-9)
				continue;
			const double cosine = Math::clamp(double(Math::dot(in, out)) / (in_length * out_length), -1.0, 1.0);
			const double turn = std::acos(cosine);
			// Only the inner side of a bend, where the edge is shorter than the middle
			// of the road: the outer corner of a corner point stays sharp.
			const bool inner = in_length + out_length < distances[i + 1] - distances[i - 1];
			if (inner && turn > 1e-3 && (in_length + out_length) * 0.5 / turn < round_length)
				sharp[i] = true;
		}
	}

	// The direction of the edge where it is fine, from the vertex 'i' back (or on).
	const auto direction_at = [&](int i, bool back) {
		for (int k = i; back ? k > 0 : k + 1 < count; back ? --k : ++k)
		{
			const Math::Vec3 d = unitOrZero(level(back ? edge[k] - edge[k - 1] : edge[k + 1] - edge[k]));
			if (Math::length2(d) > 0.0)
				return d;
		}
		return Math::Vec3(0.0, 0.0, 0.0);
	};

	const int ARC_SAMPLES = 32;
	int previous_end = 0;
	for (int i = 1; i < count; ++i)
	{
		if (!sharp[i])
			continue;

		// The sharp place, with the sharp places close after it: they make one arc.
		const int place_start = i;
		int place_end = i;
		for (int k = i + 1; k < count && distances[k] - distances[place_end] <= round_length; ++k)
		{
			if (sharp[k])
				place_end = k;
		}

		// The arc takes in some of the edge on both sides.
		int first = place_start - 1;
		while (first > previous_end && distances[place_start] - distances[first - 1] <= round_length)
			--first;
		int last = place_end;
		while (last + 1 < count && distances[last + 1] - distances[place_end] <= round_length)
			++last;
		if (last == place_end && last + 1 < count)
			++last;

		// The arc is pulled towards where the edge before it and the edge after it, kept
		// straight, would meet. If they do not meet ahead of the arc (the edge is nearly
		// straight there, or too short to tell), towards the sharpest place itself.
		const Math::Vec3 a = edge[first];
		const Math::Vec3 b = edge[last];
		Math::Vec3 corner = edge[place_start - 1];
		const Math::Vec3 da = direction_at(first, true);
		const Math::Vec3 db = direction_at(last, false);
		const double denominator = double(da.x * db.y - da.y * db.x);
		if (first > 0 && last + 1 < count && std::abs(denominator) > 1e-6)
		{
			const Math::Vec3 ab = level(b - a);
			const double s = double(ab.x * db.y - ab.y * db.x) / denominator;
			const double t = double(ab.x * da.y - ab.y * da.x) / denominator;
			const double chord = double(Math::length(ab));
			if (s > 0.0 && t < 0.0 && s < chord * 4.0 && -t < chord * 4.0)
			{
				corner = a + da * Math::Scalar(s);
				corner.z = (a.z + b.z) * Math::Scalar(0.5);
			}
		}

		// The arc measured, to spread the vertices along it by length.
		Math::Vec3 samples[ARC_SAMPLES + 1];
		double lengths[ARC_SAMPLES + 1];
		samples[0] = a;
		lengths[0] = 0.0;
		for (int k = 1; k <= ARC_SAMPLES; ++k)
		{
			samples[k] = quadratic_bezier(a, corner, b, double(k) / ARC_SAMPLES);
			lengths[k] = lengths[k - 1] + double(Math::length(samples[k] - samples[k - 1]));
		}

		const double road_span = distances[last] - distances[first];
		int k = 0;
		for (int v = first + 1; v < last; ++v)
		{
			const double wanted = road_span > 1e-9 ? lengths[ARC_SAMPLES] * (distances[v] - distances[first]) / road_span : 0.0;
			while (k + 1 < ARC_SAMPLES && lengths[k + 1] < wanted)
				++k;
			const double piece = lengths[k + 1] - lengths[k];
			const double t = piece > 1e-12 ? Math::clamp((wanted - lengths[k]) / piece, 0.0, 1.0) : 0.0;
			edge[v] = samples[k] + (samples[k + 1] - samples[k]) * Math::Scalar(t);
		}

		previous_end = last;
		i = last;
	}
}

} // namespace

RoadBuilder::RoadBuilder(QObject *parent)
	: QObject(parent)
{
	editor_.on_changed = [this]() { notify_changed(); };
	editor_.on_message = [this](int level, const QString &text) { report(level, text); };
}

RoadBuilder::~RoadBuilder()
{
	on_changed = nullptr;
	on_message = nullptr;
	on_roads_changed = nullptr;
	setActive(false);
	remove_preview();
	show_hidden_pieces();
	editor_.on_changed = nullptr;
	editor_.on_message = nullptr;
}

void RoadBuilder::setActive(bool active)
{
	if (active_ == active)
		return;

	active_ = active;
	if (active_)
	{
		CurveEditor::recoverManipulators();
		// The road is drawn with the Visualizer, once per frame.
		saved_visualizer_mode_ = Visualizer::getMode();
		if (saved_visualizer_mode_ == Visualizer::MODE_DISABLED)
			Visualizer::setMode(Visualizer::MODE_ENABLED_DEPTH_TEST_ENABLED);
		Engine::get()->getEventEndWorldUpdate().connect(connections_, this, &RoadBuilder::update);
		// Hidden pieces are saved shown; they are hidden again on the next frame.
		World::getEventPreWorldSave().connect(connections_, [this](const char *) { show_hidden_pieces(); });
		// Clicks on the points of the road are taken all the time the tool is active.
		qApp->installEventFilter(this);
		// The ground and the asset to bend may have changed while the tool was off.
		ground_cache_.clear();
		bend_sources_.clear();
		bend_errors_.clear();
		grid_state_.clear();
		updateFromSelection();
	} else
	{
		setAddingPoints(false);
		editor_.reset();
		remove_preview();
		show_hidden_pieces();
		qApp->removeEventFilter(this);
		connections_.disconnectAll();
		if (saved_visualizer_mode_ == Visualizer::MODE_DISABLED)
			Visualizer::setMode(Visualizer::MODE_DISABLED);
	}
}

void RoadBuilder::updateFromSelection()
{
	if (const SelectorNodes *selector = Selection::getSelectorNodes())
	{
		for (const NodePtr &node : selector->getNodes())
		{
			if (NodePtr root = find_road_root(node))
			{
				set_road(root);
				break;
			}
		}
	}
	notify_changed();
}

void RoadBuilder::set_road(const NodePtr &root)
{
	if (hasRoad() && root_->getID() == root->getID())
		return;

	setAddingPoints(false);
	show_hidden_pieces();
	root_ = root;
	editor_.setRoot(root_);
	// The heights were measured with the other road looked through.
	ground_cache_.clear();
	load_settings();
	editor_.setClosed(settings_.closed);
	grid_state_.clear();
	rebuild_grid();
}

QVector<RoadInfo> RoadBuilder::findRoads() const
{
	Vector<NodePtr> nodes;
	World::getNodes(nodes, false, false);

	QVector<RoadInfo> roads;
	for (const NodePtr &node : nodes)
	{
		if (!node || !is_road_root(node) || isEditorInternal(node))
			continue;

		RoadInfo info;
		info.id = node->getID();
		info.name = QString::fromUtf8(node->getName());
		info.points = findChildNode(node, CURVE_PATH_NODE_NAME)->getNumChildren();
		info.pieces = findChildNode(node, MESH_NODE_NAME)->getNumChildren();
		roads.append(info);
	}

	std::stable_sort(roads.begin(), roads.end(), [](const RoadInfo &a, const RoadInfo &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return roads;
}

bool RoadBuilder::selectRoad(int root_id)
{
	const NodePtr root = World::getNodeByID(root_id);
	if (!root || root.isDeleted() || !is_road_root(root))
		return false;

	set_road(root);

	Vector<NodePtr> nodes;
	nodes.append(root);
	::UnigineEditor::SelectionAction::applySelection(new SelectorNodes(nodes));

	report(MSG_INFO, QString());
	notify_changed();
	return true;
}

bool RoadBuilder::getSelectedPointsRoll(double &roll) const
{
	const NodePtr path = editor_.getPathNode();
	const Vector<NodePtr> selected = editor_.getSelectedPoints();
	if (!path || selected.empty())
		return false;
	const CurvePoints points = editor_.getPoints();
	for (int i = 0; i < points.count(); ++i)
	{
		if (path->getChild(i)->getID() == selected[0]->getID())
		{
			roll = points.rolls[i];
			return true;
		}
	}
	return false;
}

void RoadBuilder::setSelectedPointsRoll(double roll)
{
	const NodePtr path = editor_.getPathNode();
	const Vector<NodePtr> selected = editor_.getSelectedPoints();
	if (!path || selected.empty())
		return;

	// The point is turned about the way the curve goes through it; it keeps its place
	// and its scale. One undo step for all of them.
	const CurvePoints points = editor_.getPoints();
	using namespace ::UnigineEditor;
	Undo::begin();
	for (int i = 0; i < points.count(); ++i)
	{
		const NodePtr point = path->getChild(i);
		bool is_selected = false;
		for (const NodePtr &node : selected)
			is_selected = is_selected || node->getID() == point->getID();
		if (!is_selected)
			continue;

		const Math::Vec3 forward = unitOrZero(level(points.direction(i)));
		if (Math::length2(forward) == 0.0)
			continue;
		const Math::Mat4 transform = Math::translate(point->getWorldPosition())
			* Math::Mat4(curvePointBasis(forward, roll) * Math::scale(point->getWorldScale()));
		Undo::apply(new SetNodeTransformAction(point, transform));
	}
	Undo::commit();
	notify_changed();
}

RoadBuilder::GroundState RoadBuilder::getSelectedPointsGround() const
{
	int off = 0;
	int on = 0;
	editor_.countSelectedPointsFlag(CURVE_OFF_GROUND_DATA, off, on);
	if (off && on)
		return GROUND_MIXED;
	if (off)
		return GROUND_OFF;
	return on ? GROUND_ON : GROUND_NONE;
}

bool RoadBuilder::getSelectedPointsWidth(double &scale) const
{
	const Vector<NodePtr> selected = editor_.getSelectedPoints();
	if (selected.empty())
		return false;
	scale = point_width(selected[0]);
	return true;
}

void RoadBuilder::setSelectedPointsWidth(double scale)
{
	scale = Math::clamp(scale, MIN_POINT_WIDTH, MAX_POINT_WIDTH);
	SetNodeDataAction *action = new SetNodeDataAction;
	for (const NodePtr &point : editor_.getSelectedPoints())
		action->set(point, POINT_WIDTH_DATA, QByteArray::number(scale, 'g', 6));
	SetNodeDataAction::push(action);
	notify_changed();
}

int RoadBuilder::maxDecalTriangles()
{
	return MAX_DECAL_TRIANGLES;
}

int RoadBuilder::getRoadId() const
{
	return hasRoad() ? root_->getID() : 0;
}

bool RoadBuilder::hasRoad() const
{
	return isNodeAlive(root_) && editor_.getPathNode() && mesh_node();
}

QString RoadBuilder::getRoadName() const
{
	return hasRoad() ? QString::fromUtf8(root_->getName()) : QString();
}

int RoadBuilder::getNumPoints() const
{
	const NodePtr path = editor_.getPathNode();
	return path ? path->getNumChildren() : 0;
}

int RoadBuilder::getNumPieces() const
{
	const NodePtr mesh = mesh_node();
	return mesh ? mesh->getNumChildren() : 0;
}

NodePtr RoadBuilder::mesh_node() const
{
	return root_ && !root_.isDeleted() ? findChildNode(root_, MESH_NODE_NAME) : NodePtr();
}

bool RoadBuilder::setParentNode(const NodePtr &node)
{
	if (node && find_road_root(node))
	{
		report(MSG_WARNING, uiText("A part of a road can not be the parent of roads: select another node.",
			"Часть дороги не может быть родителем дорог: выделите другую ноду."));
		return false;
	}
	parent_ = node;
	report(MSG_INFO, QString());
	notify_changed();
	return true;
}

NodePtr RoadBuilder::getParentNode() const
{
	return isNodeAlive(parent_) ? parent_ : NodePtr();
}

QString RoadBuilder::unique_road_name(const QString &base) const
{
	QSet<QString> names;
	for (const RoadInfo &road : findRoads())
		names.insert(road.name);
	QString name = base;
	for (int i = 2; names.contains(name); ++i)
		name = QString::fromUtf8("%1_%2").arg(base).arg(i);
	return name;
}

void RoadBuilder::forget_removed_road(const NodePtr &root)
{
	if (hasRoad() && root_->getID() == root->getID())
	{
		root_ = NodePtr();
		editor_.setRoot(NodePtr());
		grid_ = RoadGrid();
		bend_ = RoadBend();
		grid_state_.clear();
		remove_preview();
	}
	removed_root_ = root;
	removed_root_frames_ = 0;
	notify_changed();
}

bool RoadBuilder::cloneRoad()
{
	if (!hasRoad())
		return false;
	setAddingPoints(false);
	show_hidden_pieces();

	// The points and the settings; the mesh is the copy's own, made by its Generate.
	const NodePtr source = root_;
	NodeDummyPtr root = NodeDummy::create();
	root->setName(unique_road_name(QString::fromUtf8(source->getName())).toUtf8().constData());
	root->setWorldTransform(source->getWorldTransform());
	if (const char *data = source->getData(SETTINGS_DATA))
		root->setData(SETTINGS_DATA, data);
	NodeDummyPtr path = NodeDummy::create();
	path->setName(CURVE_PATH_NODE_NAME);
	path->setWorldParent(root);
	const NodePtr source_path = editor_.getPathNode();
	path->setWorldTransform(source_path->getWorldTransform());
	for (int i = 0, num = source_path->getNumChildren(); i < num; ++i)
	{
		NodePtr point = source_path->getChild(i)->clone();
		point->setWorldParent(path);
		point->setWorldTransform(source_path->getChild(i)->getWorldTransform());
	}
	NodeDummyPtr mesh = NodeDummy::create();
	mesh->setName(MESH_NODE_NAME);
	mesh->setWorldParent(root);
	// Into the parent of the new roads, or next to the road.
	if (const NodePtr parent = getParentNode() ? getParentNode() : source->getParent())
		root->setWorldParent(parent);
	root->setShowInEditorEnabledRecursive(true);
	root->setSaveToWorldEnabledRecursive(true);
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(root));

	selectRoad(root->getID());
	if (on_roads_changed)
		on_roads_changed();
	report(MSG_INFO, uiText("The road is copied to %1: its points and settings, no mesh yet.",
		"Дорога скопирована в %1: точки и настройки, меша пока нет.").arg(getRoadName()));
	return true;
}

bool RoadBuilder::removeRoadCurve()
{
	if (!hasRoad())
		return false;
	setAddingPoints(false);
	show_hidden_pieces();

	// The pieces go to a node of the name of the road, where the road was; the road
	// itself with its curve is removed. One undo step.
	const NodePtr root = root_;
	const NodePtr holder = mesh_node();
	Vector<NodePtr> pieces;
	for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
		pieces.append(holder->getChild(i));

	using namespace ::UnigineEditor;
	Undo::begin();
	if (!pieces.empty())
	{
		NodeDummyPtr group = NodeDummy::create();
		group->setName(root->getName());
		if (const NodePtr parent = root->getParent())
			group->setWorldParent(parent);
		group->setWorldTransform(root->getWorldTransform());
		group->setShowInEditorEnabled(true);
		group->setSaveToWorldEnabled(true);
		Undo::apply(new CreateNodesAction(group));
		Undo::apply(new ReparentNodesAction(pieces, group));
	}
	Undo::apply(new RemoveNodesAction(root));
	Undo::commit();

	forget_removed_road(root);
	report(MSG_INFO, pieces.empty() ? uiText("The road is removed; it had no mesh.", "Дорога удалена; меша у неё не было.")
		: uiText("The curve is removed; the mesh stays as ordinary nodes.", "Кривая удалена; меш остался обычными нодами."));
	return true;
}

bool RoadBuilder::removeRoad(bool files)
{
	if (!hasRoad())
		return false;
	setAddingPoints(false);
	show_hidden_pieces();

	const NodePtr root = root_;
	// Only the files of this road: a copy made by the editor has the list of the
	// original.
	// Only its own files, and not those something else in the world still has as its mesh.
	const QStringList generated = files ? ownGeneratedFiles(root, road_generated_data()) : QStringList();
	::UnigineEditor::Undo::apply(new ::UnigineEditor::RemoveNodesAction(root));
	forget_removed_road(root);

	int removed = 0;
	if (files)
	{
		for (const QString &file : generated)
		{
			if (AssetManager::isAsset(file.toUtf8().constData()) && AssetManager::removeAssetSync(file.toUtf8().constData()))
				++removed;
		}
	}
	report(MSG_INFO, files ? uiText("The road is removed with its mesh and %1 mesh files.", "Дорога удалена вместе с мешем и файлами мешей: %1.").arg(removed)
		: uiText("The road is removed with its mesh.", "Дорога удалена вместе с мешем."));
	return true;
}

void RoadBuilder::createRoad(const RoadSettings &start_settings)
{
	setAddingPoints(false);

	// A name no other road has, so that the files of the roads do not mix.
	const QString name = unique_road_name(QString::fromUtf8(ROOT_NODE_NAME));

	NodeDummyPtr root = NodeDummy::create();
	root->setName(name.toUtf8().constData());
	NodeDummyPtr path = NodeDummy::create();
	path->setName(CURVE_PATH_NODE_NAME);
	path->setWorldParent(root);
	NodeDummyPtr mesh = NodeDummy::create();
	mesh->setName(MESH_NODE_NAME);
	mesh->setWorldParent(root);
	if (const NodePtr parent = getParentNode())
		root->setWorldParent(parent);

	root->setShowInEditorEnabledRecursive(true);
	root->setSaveToWorldEnabledRecursive(true);
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(root));

	// The new road starts with the settings it is given, in its own folder. They are a
	// part of making it, not a step of their own.
	root_ = root;
	settings_ = limited(start_settings);
	settings_.material.clear();
	settings_.folder.clear();
	editor_.setRoot(root_);
	editor_.setClosed(settings_.closed);
	editor_.setSurfaceFilter(settings_.ground);
	save_settings(false);
	grid_state_.clear();
	rebuild_grid();
	discard_if_empty_ = true;

	Vector<NodePtr> selection;
	selection.append(root);
	::UnigineEditor::SelectionAction::applySelection(new SelectorNodes(selection));

	notify_changed();
}

void RoadBuilder::setAddingPoints(bool adding)
{
	if (adding && !hasRoad())
	{
		report(MSG_WARNING, uiText("Create a road or select an existing one first.", "Сначала создайте дорогу или выделите существующую."));
		adding = false;
	}
	if (editor_.isAddingPoints() == adding)
		return;

	editor_.setAddingPoints(adding);

	// A road made by New Road is not kept if adding points to it is over before it
	// got a single one.
	if (!adding && discard_if_empty_)
	{
		discard_if_empty_ = false;
		if (hasRoad() && getNumPoints() == 0)
		{
			const NodePtr root = root_;
			root_ = NodePtr();
			editor_.setRoot(NodePtr());
			grid_ = RoadGrid();
			grid_state_.clear();
			::UnigineEditor::Undo::apply(new ::UnigineEditor::RemoveNodesAction(root));
			removed_root_ = root;
			removed_root_frames_ = 0;
			report(MSG_INFO, uiText("The new road got no points and was removed.",
				"У новой дороги не появилось ни одной точки - она удалена."));
		}
	}
	notify_changed();
}

void RoadBuilder::setSettings(const RoadSettings &settings)
{
	const SurfaceFilter old_ground = settings_.ground;
	const bool decal_changed = settings.decal_depth != settings_.decal_depth
		|| settings.decal_visible_distance != settings_.decal_visible_distance
		|| settings.decal_fade_distance != settings_.decal_fade_distance;
	settings_ = limited(settings);
	// The decals that are there get them at once: no Generate is needed.
	if (decal_changed && hasRoad())
		apply_decal_settings();
	editor_.setClosed(settings_.closed);
	editor_.setSurfaceFilter(settings_.ground);
	// Other surfaces to lie on: the heights found so far are not those.
	if (old_ground.surface_types != settings_.ground.surface_types || old_ground.only_immovable != settings_.ground.only_immovable
		|| old_ground.only_intersection != settings_.ground.only_intersection)
		ground_cache_.clear();

	if (hasRoad())
		save_settings();
	notify_changed();
}

void RoadBuilder::resetSettings()
{
	RoadSettings settings;
	settings.closed = settings_.closed;
	settings.material = settings_.material;
	settings.folder = settings_.folder;
	// What the road is made into and of, too.
	settings.output = settings_.output;
	settings.bend_parts = settings_.bend_parts;
	settings.bend_forward = settings_.bend_forward;
	settings.bend_up = settings_.bend_up;
	settings.bend_merge_materials = settings_.bend_merge_materials;
	settings.bend_mesh_angle = settings_.bend_mesh_angle;
	setSettings(settings);
	report(MSG_INFO, QString());
}

////////////////////////////////////////////////////////////////////////////////
// Settings, kept in the data of the root node as JSON.
////////////////////////////////////////////////////////////////////////////////
void RoadBuilder::load_settings()
{
	const char *data = root_->getData(SETTINGS_DATA);
	saved_settings_ = QByteArray(data ? data : "");
	++settings_revision_;
	const QJsonObject json = QJsonDocument::fromJson(saved_settings_).object();
	// A road the tool sees for the first time keeps what is on the page.
	if (json.isEmpty())
		return;

	const RoadSettings defaults;
	RoadSettings settings;
	settings.width = float(json.value("width").toDouble(defaults.width));
	settings.columns = json.value("columns").toInt(defaults.columns);
	settings.step = float(json.value("step").toDouble(defaults.step));
	settings.max_angle = float(json.value("max_angle").toDouble(defaults.max_angle));
	settings.closed = json.value("closed").toBool(defaults.closed);
	settings.snap_to_ground = json.value("snap_to_ground").toBool(defaults.snap_to_ground);
	settings.ground_offset = float(json.value("ground_offset").toDouble(defaults.ground_offset));
	settings.uv_length = float(json.value("uv_length").toDouble(defaults.uv_length));
	settings.uv_width = float(json.value("uv_width").toDouble(defaults.uv_width));
	settings.uv_rotation = float(json.value("uv_rotation").toDouble(defaults.uv_rotation));
	settings.piece_length = float(json.value("piece_length").toDouble(defaults.piece_length));
	settings.output = RoadOutput(qBound(int(ROAD_OUTPUT_MESH), json.value("output").toInt(defaults.output), int(ROAD_OUTPUT_BENT)));
	settings.bend_forward = AxisDirection(json.value("bend_forward").toInt(json.value("bend_axis").toInt(defaults.bend_forward)));
	settings.bend_up = AxisDirection(json.value("bend_up").toInt(defaults.bend_up));
	settings.bend_merge_materials = json.value("bend_merge_materials").toBool(defaults.bend_merge_materials);
	settings.bend_mesh_angle = float(json.value("bend_mesh_angle").toDouble(defaults.bend_mesh_angle));
	const QJsonValue parts = json.value("bend_parts");
	for (const QJsonValue &item : parts.toArray())
		settings.bend_parts.append(bend_part_from_json(item.toObject()));
	// The builds before took one pattern, one start and one end.
	if (parts.isObject())
	{
		for (int r = 0; r < 3; ++r)
		{
			const QJsonObject item = parts.toObject().value(QString::fromUtf8(BEND_ROLE_KEYS[r])).toObject();
			if (item.value("source").toString().isEmpty())
				continue;
			RoadBendPart part = bend_part_from_json(item);
			part.role = RoadBendRole(r);
			settings.bend_parts.append(part);
		}
	}
	if (parts.isUndefined() && !json.value("bend_source").toString().isEmpty())
	{
		RoadBendPart part;
		part.source = json.value("bend_source").toString();
		part.source_kind = json.value("bend_source_kind").toInt();
		part.source_name = json.value("bend_source_name").toString();
		settings.bend_parts.append(part);
	}
	settings.decal_depth = float(json.value("decal_depth").toDouble(defaults.decal_depth));
	settings.decal_lift = float(json.value("decal_lift").toDouble(defaults.decal_lift));
	settings.decal_visible_distance = float(json.value("decal_visible_distance").toDouble(defaults.decal_visible_distance));
	settings.decal_fade_distance = float(json.value("decal_fade_distance").toDouble(defaults.decal_fade_distance));
	settings.ground.surface_types = json.value("ground_types").toInt(defaults.ground.surface_types);
	settings.ground.only_immovable = json.value("ground_immovable").toBool(defaults.ground.only_immovable);
	settings.ground.only_intersection = json.value("ground_intersection").toBool(defaults.ground.only_intersection);
	settings.inner_round = float(json.value("inner_round").toDouble(defaults.inner_round));
	settings.roll = float(json.value("roll").toDouble(defaults.roll));
	settings.side_offset = float(json.value("side_offset").toDouble(defaults.side_offset));
	settings.material = json.value("material").toString();
	settings.folder = json.value("folder").toString();

	// The same limits as for the values set from the page: a damaged record must not
	// make a grid of millions of rows. Another ground to lie on (an undo of Lay On):
	// the heights found so far are not those.
	const SurfaceFilter old_ground = settings_.ground;
	settings_ = limited(settings);
	if (old_ground.surface_types != settings_.ground.surface_types || old_ground.only_immovable != settings_.ground.only_immovable
		|| old_ground.only_intersection != settings_.ground.only_intersection)
		ground_cache_.clear();
	editor_.setSurfaceFilter(settings_.ground);
}

void RoadBuilder::save_settings(bool undoable, bool merge)
{
	if (!root_ || root_.isDeleted())
		return;

	QJsonObject json;
	json.insert("width", double(settings_.width));
	json.insert("columns", settings_.columns);
	json.insert("step", double(settings_.step));
	json.insert("max_angle", double(settings_.max_angle));
	json.insert("closed", settings_.closed);
	json.insert("snap_to_ground", settings_.snap_to_ground);
	json.insert("ground_offset", double(settings_.ground_offset));
	json.insert("uv_length", double(settings_.uv_length));
	json.insert("uv_width", double(settings_.uv_width));
	json.insert("uv_rotation", double(settings_.uv_rotation));
	json.insert("piece_length", double(settings_.piece_length));
	json.insert("output", int(settings_.output));
	json.insert("bend_merge_materials", settings_.bend_merge_materials);
	json.insert("bend_mesh_angle", double(settings_.bend_mesh_angle));
	json.insert("bend_forward", int(settings_.bend_forward));
	json.insert("bend_up", int(settings_.bend_up));
	QJsonArray parts;
	for (const RoadBendPart &part : settings_.bend_parts)
	{
		QJsonObject item;
		item.insert("source", part.source);
		item.insert("kind", part.source_kind);
		item.insert("name", part.source_name);
		item.insert("role", QString::fromUtf8(BEND_ROLE_KEYS[part.role]));
		item.insert("scale_x", double(part.scale.x));
		item.insert("scale_y", double(part.scale.y));
		item.insert("scale_z", double(part.scale.z));
		item.insert("length", double(part.length));
		item.insert("auto_length", part.auto_length);
		item.insert("count", part.count);
		parts.append(item);
	}
	json.insert("bend_parts", parts);
	json.insert("decal_depth", double(settings_.decal_depth));
	json.insert("decal_lift", double(settings_.decal_lift));
	json.insert("decal_visible_distance", double(settings_.decal_visible_distance));
	json.insert("decal_fade_distance", double(settings_.decal_fade_distance));
	json.insert("ground_types", settings_.ground.surface_types);
	json.insert("ground_immovable", settings_.ground.only_immovable);
	json.insert("ground_intersection", settings_.ground.only_intersection);
	json.insert("inner_round", double(settings_.inner_round));
	json.insert("roll", double(settings_.roll));
	json.insert("side_offset", double(settings_.side_offset));
	json.insert("material", settings_.material);
	json.insert("folder", settings_.folder);
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
		SetNodeDataAction::push(action, merge);
	} else
		root_->setData(SETTINGS_DATA, saved_settings_.constData());
}

////////////////////////////////////////////////////////////////////////////////
// The grid.
////////////////////////////////////////////////////////////////////////////////
QByteArray RoadBuilder::snapshot() const
{
	QByteArray state;
	const NodePtr path = editor_.getPathNode();
	if (!path)
		return state;

	putBytes(state, double(isOldCurve()));
	putBytes(state, double(settings_.width));
	putBytes(state, double(settings_.columns));
	putBytes(state, double(settings_.step));
	putBytes(state, double(settings_.max_angle));
	putBytes(state, double(settings_.closed));
	putBytes(state, double(settings_.snap_to_ground));
	putBytes(state, double(settings_.ground_offset));
	putBytes(state, double(settings_.uv_length));
	putBytes(state, double(settings_.uv_width));
	putBytes(state, double(settings_.uv_rotation));
	putBytes(state, double(settings_.piece_length));
	putBytes(state, double(settings_.output));
	putBytes(state, double(settings_.decal_lift));
	putBytes(state, double(settings_.ground.surface_types));
	putBytes(state, double(settings_.ground.only_immovable));
	putBytes(state, double(settings_.ground.only_intersection));
	putBytes(state, double(settings_.inner_round));
	putBytes(state, double(settings_.roll));
	putBytes(state, double(settings_.side_offset));
	if (settings_.output == ROAD_OUTPUT_BENT)
	{
		putBytes(state, double(settings_.bend_merge_materials));
		putBytes(state, double(settings_.bend_mesh_angle));
		putBytes(state, double(settings_.bend_forward));
		putBytes(state, double(settings_.bend_up));
		for (const RoadBendPart &part : settings_.bend_parts)
		{
			state.append(part.source.toUtf8()).append('|');
			putBytes(state, double(part.role));
			putBytes(state, double(part.count));
			putBytes(state, Math::Vec3(part.scale));
			putBytes(state, part.auto_length ? -1.0 : double(part.length));
		}
	}
	for (int i = 0, num = path->getNumChildren(); i < num; ++i)
	{
		const NodePtr point = path->getChild(i);
		putBytes(state, point->getWorldPosition());
		putBytes(state, double(isCornerPoint(point)));
		putBytes(state, double(isPointFlag(point, CURVE_OFF_GROUND_DATA)));
		putBytes(state, Math::Vec3(point->getWorldTransform().getColumn3(2)));
		putBytes(state, point_width(point));
	}
	return state;
}

bool RoadBuilder::isGroundFromTerrain() const
{
	const SurfaceFilter &ground = settings_.ground;
	return ground.surface_types == SurfaceFilter::TERRAIN && !ground.only_immovable && !ground.only_intersection
		&& Landscape::getActiveTerrain();
}

void RoadBuilder::refreshGround()
{
	ground_cache_.clear();
	grid_state_.clear();
	notify_changed();
}

void RoadBuilder::ground_heights(const QVector<Math::Vec3> &points, QVector<double> &heights) const
{
	const int count = int(points.size());
	heights.resize(count);
	if (ground_cache_.size() > MAX_GROUND_CACHE)
		ground_cache_.clear();

	// What is known already.
	const bool from_terrain = isGroundFromTerrain();
	QVector<int> missing;
	for (int i = 0; i < count; ++i)
	{
		const auto it = ground_cache_.constFind(ground_key(points[i], !from_terrain));
		if (it != ground_cache_.constEnd())
			heights[i] = *it;
		else
			missing.append(i);
	}
	if (missing.isEmpty())
		return;

	if (from_terrain)
	{
		// Straight from the Landscape Terrain, a batch at a time: no raycasts.
		Vector<LandscapeFetchPtr> batch;
		for (int first = 0; first < missing.size(); first += FETCH_BATCH)
		{
			const int size = qMin(FETCH_BATCH, int(missing.size()) - first);
			while (fetches_.size() < size)
			{
				LandscapeFetchPtr fetch = LandscapeFetch::create();
				fetch->setUses(0);
				fetch->setUsesHeight(true);
				fetch->setHolesEnabled(true);
				fetches_.append(fetch);
			}
			batch.clear();
			for (int k = 0; k < size; ++k)
			{
				const Math::Vec3 &point = points[missing[first + k]];
				fetches_[k]->setFetchPosition(Math::Vec2(point.x, point.y));
				batch.append(fetches_[k]);
			}
			LandscapeFetch::fetchForce(batch);
			for (int k = 0; k < size; ++k)
			{
				heights[missing[first + k]] = batch[k]->isIntersection() ? double(batch[k]->getPosition().z)
					: NO_GROUND;
			}
		}
	} else
	{
		// A ray down at every point. The road itself, its old mesh included, is looked
		// through.
		Vector<NodePtr> ignore;
		ignore.append(root_);
		for (int i : missing)
		{
			const Math::Vec3 above = points[i] + Math::Vec3(0.0, 0.0, SNAP_UP);
			const Math::Vec3 below = points[i] - Math::Vec3(0.0, 0.0, SNAP_DOWN);
			const SurfaceHit hit = raycastSurface(above, below, settings_.ground, ignore);
			heights[i] = hit.found ? double(hit.point.z) : NO_GROUND;
		}
	}

	for (int i : missing)
		ground_cache_.insert(ground_key(points[i], !from_terrain), heights[i]);
}

void RoadBuilder::rebuild_grid(bool on_ground)
{
	grid_ = RoadGrid();
	bend_ = RoadBend();
	preview_dirty_ = true;
	grid_state_ = snapshot();
	grid_draft_ = !on_ground && settings_.snap_to_ground && settings_.output != ROAD_OUTPUT_BENT;

	const CurvePoints points = editor_.getPoints();
	if (points.spans() == 0)
		return;

	Measured measured = measure(points);
	offset_measured(measured, double(settings_.side_offset), points.closed);
	if (measured.distances.last() < 1e-6)
		return;

	PointValues values;
	const NodePtr path = editor_.getPathNode();
	for (int i = 0; i < points.count(); ++i)
	{
		values.widths.append(point_width(path->getChild(i)));
	}

	// A bent mesh follows the curve itself: no grid, no ground.
	if (settings_.output == ROAD_OUTPUT_BENT)
	{
		rebuild_bend(points, values.widths, measured.point_samples, measured.samples, measured.distances);
		return;
	}

	bool too_many = false;
	QVector<Station> stations = place_stations(measured, points, values, settings_.step, settings_.max_angle, &too_many);
	if (too_many)
	{
		report(MSG_WARNING, uiText("The road would get more than %1 rows: make the Step or the Bend Angle larger.",
			"У дороги получилось бы больше %1 рядов: увеличьте «Шаг» или «Угол на поворотах».").arg(MAX_STATIONS));
		return;
	}
	add_cut_stations(stations, double(settings_.piece_length));
	if (stations.size() < 2)
		return;

	RoadGrid &grid = grid_;
	grid.columns = settings_.columns;
	grid.stations = int(stations.size());
	grid.length = measured.distances.last();
	const int columns = grid.columns;
	const int row_size = columns + 1;
	grid.vertices.resize(grid.stations * row_size);
	grid.uvs.resize(grid.stations * row_size);
	grid.normals.resize(grid.stations * row_size);

	// Where every vertex of a row is across the road, from 0 on the left edge to 1 on
	// the right one.
	QVector<double> across(grid.stations * row_size);
	for (int i = 0; i < grid.stations; ++i)
	{
		for (int j = 0; j <= columns; ++j)
			across[grid.index(i, j)] = double(j) / columns;
	}

	// The texture repeats a whole number of times around a closed road, so that it
	// meets itself where the road closes.
	double v_scale = 1.0 / double(settings_.uv_length);
	if (points.closed)
		v_scale = qMax(1.0, std::round(grid.length * v_scale)) / grid.length;

	const double uv_angle = double(settings_.uv_rotation) * Math::Consts::DEG2RAD_D;
	const double uv_cos = std::cos(uv_angle);
	const double uv_sin = std::sin(uv_angle);

	// The rows across the road. At a point where the curve bends, the row goes along
	// the bisector of the bend and is longer, so that the road keeps its width on both
	// sides of it (a mitre joint).
	QVector<Math::Vec3> forwards(grid.stations);
	QVector<Math::Vec3> rights(grid.stations);
	Math::Vec3 last_right(1.0, 0.0, 0.0);
	for (int i = 0; i < grid.stations; ++i)
	{
		const Station &station = stations[i];
		Math::Vec3 right_in = right_of(station.incoming);
		Math::Vec3 right_out = right_of(station.outgoing);
		if (Math::length2(right_in) == 0.0)
			right_in = Math::length2(right_out) > 0.0 ? right_out : last_right;
		if (Math::length2(right_out) == 0.0)
			right_out = right_in;
		Math::Vec3 right = unitOrZero(right_in + right_out);
		if (Math::length2(right) == 0.0)
			right = right_in;	// the road turns right back
		last_right = right;
		rights[i] = right;
		const double scale = 1.0 / qMax(double(Math::dot(right, right_in)), 1.0 / MITER_LIMIT);

		forwards[i] = unitOrZero(level(station.incoming) + level(station.outgoing));
		if (Math::length2(forwards[i]) == 0.0)
			forwards[i] = Math::Vec3(-right.y, right.x, 0.0);

		for (int j = 0; j <= columns; ++j)
		{
			const double f = across[grid.index(i, j)];
			const double offset = (f - 0.5) * double(settings_.width) * station.width * scale;
			grid.vertices[grid.index(i, j)] = station.position + right * Math::Scalar(offset);
			// Across the road U, along it V, then turned by uv_rotation. V goes back
			// along the road: in the engine V runs down the image (its own plane has
			// v = 1 - y), so with V going forward the texture lay mirrored and its
			// normal map turned inside out - unseen near the camera, plain in the
			// distance, on decals most of all.
			const double u = f * double(settings_.uv_width);
			const double v = -station.distance * v_scale;
			grid.uvs[grid.index(i, j)] = Math::vec2(float(u * uv_cos - v * uv_sin), float(u * uv_sin + v * uv_cos));
		}
	}

	// On a tight bend the inner edge gets sharp or would even fold over itself. That
	// part of the edge is laid anew along a smooth arc (see round_edge()), and the rows
	// are straight lines between the two edges again, so the grid fans out evenly
	// around the inner side of the bend.
	QVector<double> distances(grid.stations);
	for (int i = 0; i < grid.stations; ++i)
		distances[i] = stations[i].distance;
	for (int j : {0, columns})
	{
		QVector<Math::Vec3> edge(grid.stations);
		for (int i = 0; i < grid.stations; ++i)
			edge[i] = grid.vertices[grid.index(i, j)];
		round_edge(edge, forwards, distances, double(settings_.inner_round));
		for (int i = 0; i < grid.stations; ++i)
			grid.vertices[grid.index(i, j)] = edge[i];
	}
	for (int i = 0; i < grid.stations; ++i)
	{
		const Math::Vec3 left = grid.vertices[grid.index(i, 0)];
		const Math::Vec3 right = grid.vertices[grid.index(i, columns)];
		for (int j = 1; j < columns; ++j)
			grid.vertices[grid.index(i, j)] = left + (right - left) * Math::Scalar(across[grid.index(i, j)]);
	}


	// The roll: every row leans about the curve, the right edge down for a positive
	// one. On the ground it shows only where the road keeps to the curve.
	for (int i = 0; i < grid.stations; ++i)
	{
		const double roll = (stations[i].roll + double(settings_.roll)) * Math::Consts::DEG2RAD_D;
		if (std::abs(roll) < 1e-6)
			continue;
		const double c = std::cos(roll);
		const double s = std::sin(roll);
		for (int j = 0; j <= columns; ++j)
		{
			Math::Vec3 &vertex = grid.vertices[grid.index(i, j)];
			const double offset = double(Math::dot(level(vertex - stations[i].position), rights[i]));
			vertex = vertex + rights[i] * Math::Scalar(offset * (c - 1.0)) - Math::Vec3(0.0, 0.0, Math::Scalar(offset * s));
		}
	}

	// Onto the ground. A decal mesh is lifted above it by half of its projection depth:
	// it is projected down from where it is, and the ground between its vertices may be
	// a little higher or lower than they are.
	// Near a point marked "off the ground" the road keeps to the height of the curve,
	// and goes over to the ground smoothly between such a point and an ordinary one.
	const bool decal = settings_.output == ROAD_OUTPUT_DECAL;
	const double ground_lift = decal ? double(settings_.decal_lift) : double(settings_.ground_offset);
	const double curve_lift = decal ? double(settings_.decal_lift) : 0.0;
	const bool snap = settings_.snap_to_ground && on_ground;
	QVector<Math::Vec3> wanted;
	if (snap)
	{
		for (int i = 0; i < grid.stations; ++i)
		{
			if (stations[i].ground <= 0.0)
				continue;
			for (int j = 0; j <= columns; ++j)
				wanted.append(grid.vertices[grid.index(i, j)]);
		}
	}
	QVector<double> heights;
	ground_heights(wanted, heights);
	int next_height = 0;
	for (int i = 0; i < grid.stations; ++i)
	{
		const double ground = snap ? stations[i].ground : 0.0;
		for (int j = 0; j <= columns; ++j)
		{
			Math::Vec3 &vertex = grid.vertices[grid.index(i, j)];
			const Math::Vec3 on_curve = vertex + Math::Vec3(0.0, 0.0, Math::Scalar(curve_lift));
			if (ground <= 0.0)
			{
				vertex = on_curve;
				continue;
			}
			// No ground under the vertex: it stays at the height of the curve.
			const double height = heights[next_height++];
			const Math::Vec3 on_ground(vertex.x, vertex.y,
				Math::Scalar((no_ground(height) ? double(vertex.z) : height) + ground_lift));
			vertex = on_curve + (on_ground - on_curve) * Math::Scalar(ground);
		}
	}

	// Normals from the neighbours in the grid, the whole road at once: the pieces it
	// is cut into join without a seam in the lighting.
	const bool closed = points.closed;
	for (int i = 0; i < grid.stations; ++i)
	{
		int previous = i - 1;
		int next = i + 1;
		if (closed)
		{
			// The last row is the first one again.
			if (previous < 0)
				previous = grid.stations - 2;
			if (next >= grid.stations)
				next = 1;
		} else
		{
			previous = qMax(previous, 0);
			next = qMin(next, grid.stations - 1);
		}
		for (int j = 0; j <= columns; ++j)
		{
			const Math::Vec3 along = grid.vertices[grid.index(next, j)] - grid.vertices[grid.index(previous, j)];
			const Math::Vec3 across = grid.vertices[grid.index(i, qMin(j + 1, columns))]
				- grid.vertices[grid.index(i, qMax(j - 1, 0))];
			Math::vec3 normal = Math::vec3(unitOrZero(Math::cross(across, along)));
			if (Math::length2(normal) < 0.5f)
				normal = Math::vec3(0.0f, 0.0f, 1.0f);
			grid.normals[grid.index(i, j)] = normal;
		}
	}

	// Pieces of about the same length, none longer than piece_length.
	grid.piece_starts.append(0);
	if (settings_.piece_length > 0.0f && grid.length > double(settings_.piece_length))
	{
		const int pieces = int(std::ceil(grid.length / double(settings_.piece_length)));
		for (int p = 1; p < pieces; ++p)
		{
			const double target = grid.length * double(p) / double(pieces);
			int best = grid.piece_starts.last() + 1;
			for (int i = best; i < grid.stations - 1; ++i)
			{
				if (std::abs(stations[i].distance - target) < std::abs(stations[best].distance - target))
					best = i;
			}
			if (best < grid.stations - 1 && best > grid.piece_starts.last())
				grid.piece_starts.append(best);
		}
	}

	// A decal piece of too many triangles is cut into equal parts.
	if (settings_.output == ROAD_OUTPUT_DECAL)
	{
		const int max_rows = qMax(1, MAX_DECAL_TRIANGLES / (2 * columns));
		QVector<int> starts;
		for (int piece = 0; piece < grid.piece_starts.size(); ++piece)
		{
			const int first = grid.piece_starts[piece];
			const int rows = grid.pieceEnd(piece) - first;
			const int parts = qMax(1, (rows + max_rows - 1) / max_rows);
			for (int k = 0; k < parts; ++k)
				starts.append(first + rows * k / parts);
		}
		grid.piece_starts = starts;
	}
}

bool RoadBuilder::isOutdated() const
{
	if (!hasRoad() || getNumPieces() == 0)
		return false;
	const QByteArray hash = QCryptographicHash::hash(snapshot(), QCryptographicHash::Md5).toHex();
	return dataString(root_, STATE_DATA).toUtf8() != hash;
}

////////////////////////////////////////////////////////////////////////////////
// Material and folder.
////////////////////////////////////////////////////////////////////////////////
bool RoadBuilder::takeMaterial(const Vector<UGUID> &guids)
{
	if (!hasRoad())
	{
		report(MSG_WARNING, uiText("Create a road or select an existing one first.", "Сначала создайте дорогу или выделите существующую."));
		return false;
	}
	for (const UGUID &guid : guids)
	{
		// A .mat asset, or a material made by the import of another asset, or a material
		// of the Materials window.
		UGUID file_guid;
		const String asset_path = AssetManager::getAssetPathFromGUID(guid);
		if (!asset_path.empty() && QFileInfo(QString::fromUtf8(asset_path.get())).suffix().toLower() == QLatin1String("mat"))
			file_guid = AssetManager::getAssetGUIDFromPath(asset_path);
		else if (extensionOf(guid) == QLatin1String("mat"))
			file_guid = guid;
		MaterialPtr material = file_guid.isValid() ? Materials::findMaterialByFileGUID(file_guid) : MaterialPtr();
		if (!material)
			material = Materials::findMaterialByGUID(guid);
		if (material)
			return take_material(material);
	}
	report(MSG_WARNING, guids.empty() ? uiText("Select a material in the Asset Browser first.", "Сначала выделите материал в Asset Browser.")
		: uiText("That is not a material: pick a material asset.", "Это не материал: выберите ассет материала."));
	return false;
}

bool RoadBuilder::showMaterial() const
{
	const UGUID file = guidOf(settings_.material);
	if (!hasRoad() || !file.isValid())
		return false;
	const String path = FileSystem::getVirtualPath(file);
	const UGUID asset = path.empty() ? UGUID() : AssetManager::getAssetGUIDFromPath(path);
	return showAssetInBrowser(asset.isValid() ? asset : file);
}

bool RoadBuilder::take_material(const MaterialPtr &material)
{
	// A decal shows only a decal material, a mesh only a mesh one.
	const bool decal = settings_.output == ROAD_OUTPUT_DECAL;
	if (!material_suits(material, decal ? Node::DECAL_MESH : Node::OBJECT_MESH_STATIC))
	{
		report(MSG_WARNING, decal
			? uiText("This material can not be put on a decal: pick a decal material (decal_base, its children or a decal Material Graph).",
				"Этот материал нельзя назначить декали: выберите материал декали (decal_base, его наследники или Material Graph для декали).")
			: uiText("This material can not be put on a mesh: pick a mesh material (mesh_base, its children or a mesh Material Graph).",
				"Этот материал нельзя назначить мешу: выберите материал меша (mesh_base, его наследники или Material Graph для меша)."));
		return false;
	}

	const UGUID file_guid = material->getFileGUID();
	if (!file_guid.isValid())
	{
		report(MSG_WARNING, uiText("This material has no file in the project; pick a material asset.",
			"У этого материала нет файла в проекте; выберите ассет материала."));
		return false;
	}

	// One step of the undo history: the material of the road and of its pieces.
	::UnigineEditor::Undo::begin();
	settings_.material = QString::fromUtf8(file_guid.makeString().get());
	save_settings(true, false);
	apply_material(material);
	::UnigineEditor::Undo::commit();
	report(MSG_INFO, QString());
	notify_changed();
	return true;
}

void RoadBuilder::clearMaterial()
{
	if (!hasRoad())
		return;
	::UnigineEditor::Undo::begin();
	settings_.material.clear();
	save_settings(true, false);
	apply_material(Materials::findManualMaterial(settings_.output == ROAD_OUTPUT_DECAL ? "Unigine::decal_base" : "Unigine::mesh_base"));
	::UnigineEditor::Undo::commit();
	notify_changed();
}

void RoadBuilder::apply_decal_settings()
{
	const NodePtr holder = mesh_node();
	if (!holder)
		return;
	for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
	{
		const NodePtr piece = holder->getChild(i);
		if (piece->getType() == Node::OBJECT_MESH_STATIC)
		{
			apply_distances(checked_ptr_cast<Object>(piece));
			continue;
		}
		if (piece->getType() != Node::DECAL_MESH)
			continue;
		DecalMeshPtr decal = checked_ptr_cast<DecalMesh>(piece);
		decal->setRadius(settings_.decal_depth);
		decal->setMaxVisibleDistance(settings_.decal_visible_distance > 0.0f ? settings_.decal_visible_distance : Math::Consts::INF);
		decal->setMaxFadeDistance(settings_.decal_fade_distance);
	}
	if (preview_ && !preview_.isDeleted())
		apply_distances(preview_);
}

void RoadBuilder::apply_distances(const ObjectPtr &object) const
{
	if (!object)
		return;
	const bool bent = settings_.output == ROAD_OUTPUT_BENT;
	const QHash<QString, BendSource::Surface> surfaces = bent ? bend_surfaces() : QHash<QString, BendSource::Surface>();
	for (int surface = 0; surface < object->getNumSurfaces(); ++surface)
	{
		// What the surface would be seen to without the setting: always for a grid, the
		// LOD distance of the asset for a bent mesh.
		float visible = Math::Consts::INF;
		float fade = 0.0f;
		const auto it = surfaces.constFind(QString::fromUtf8(object->getSurfaceName(surface)));
		if (it != surfaces.constEnd())
		{
			visible = it->max_visible;
			fade = it->max_fade;
		}
		if (settings_.decal_visible_distance > 0.0f && settings_.decal_visible_distance < visible)
		{
			visible = settings_.decal_visible_distance;
			fade = settings_.decal_fade_distance;
		}
		object->setMaxVisibleDistance(visible, surface);
		object->setMaxFadeDistance(fade, surface);
	}
}

// The pieces of the road get the material at once: no Generate is needed for it.
void RoadBuilder::apply_material(const MaterialPtr &material)
{
	const NodePtr holder = mesh_node();
	if (!holder || !material)
		return;
	SetMaterialsAction *action = new SetMaterialsAction;
	for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
	{
		const NodePtr piece = holder->getChild(i);
		if (is_piece(piece) && material_suits(material, piece->getType()))
			action->set(piece, material->getGUID());
	}
	if (action->isEmpty())
		delete action;
	else
		::UnigineEditor::Undo::apply(action);
}

QString RoadBuilder::getMaterialName() const
{
	// What the pieces have: the material may have been given to them by hand.
	if (const NodePtr holder = mesh_node())
	{
		for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
		{
			const NodePtr piece = holder->getChild(i);
			if (!is_piece(piece))
				continue;
			const MaterialPtr material = material_of(piece);
			if (!material)
				break;
			const QString path = QString::fromUtf8(material->getFilePath().get());
			return path.isEmpty() ? QString::fromUtf8(material->getManualName()) : QFileInfo(path).completeBaseName();
		}
	}

	const UGUID guid = guidOf(settings_.material);
	if (!guid.isValid())
		return QString();
	const QString path = QString::fromUtf8(FileSystem::getVirtualPath(guid).get());
	return path.isEmpty() ? uiText("(missing material)", "(материал не найден)") : QFileInfo(path).completeBaseName();
}

QString RoadBuilder::defaultFolder()
{
	// The folder of the world, as a path in the data of the project.
	QString world = QDir::fromNativeSeparators(QString::fromUtf8(FileSystem::getVirtualPath(World::getPath()).get()));
	QString folder = QFileInfo(world).path();
	if (world.isEmpty() || folder == QLatin1String(".") || QDir::isAbsolutePath(folder))
		folder.clear();
	return folder.isEmpty() ? QString::fromUtf8(DEFAULT_FOLDER_NAME) : folder + QLatin1Char('/') + QString::fromUtf8(DEFAULT_FOLDER_NAME);
}

QString RoadBuilder::getFolder() const
{
	return settings_.folder.isEmpty() ? defaultFolder() : settings_.folder;
}

bool RoadBuilder::hasGeneratedFiles() const
{
	return hasRoad() && !dataString(root_, FILES_DATA).isEmpty();
}

bool RoadBuilder::showGeneratedFiles() const
{
	if (!hasRoad())
		return false;
	for (const QString &file : dataString(root_, FILES_DATA).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
	{
		if (showAssetInBrowser(AssetManager::getAssetGUIDFromPath(file.toUtf8().constData())))
			return true;
	}
	return false;
}

bool RoadBuilder::setFolderFromDisk(const QString &absolute_path)
{
	const QString data = QDir::cleanPath(dataPath());
	const QString folder = QDir::cleanPath(QDir::fromNativeSeparators(absolute_path));
	if (!folder.startsWith(data + QLatin1Char('/'), Qt::CaseInsensitive) && folder.compare(data, Qt::CaseInsensitive) != 0)
	{
		report(MSG_WARNING, uiText("The folder must be inside the data folder of the project.",
			"Папка должна быть внутри папки data проекта."));
		return false;
	}

	RoadSettings settings = settings_;
	settings.folder = folder.mid(data.size() + 1);
	if (settings.folder.isEmpty())
		settings.folder = QString::fromUtf8(".");
	setSettings(settings);
	report(MSG_INFO, QString());
	return true;
}

////////////////////////////////////////////////////////////////////////////////
// Generate.
////////////////////////////////////////////////////////////////////////////////
MeshPtr RoadBuilder::make_road_mesh(int piece, Math::Vec3 &pivot) const
{
	const int first = grid_.piece_starts[piece];
	const int last = grid_.pieceEnd(piece);

	// The piece is put where the middle of its bounds is.
	Math::Vec3 minimum = grid_.vertices[grid_.index(first, 0)];
	Math::Vec3 maximum = minimum;
	for (int i = first; i <= last; ++i)
	{
		for (int j = 0; j <= grid_.columns; ++j)
		{
			minimum = Math::min(minimum, grid_.vertices[grid_.index(i, j)]);
			maximum = Math::max(maximum, grid_.vertices[grid_.index(i, j)]);
		}
	}
	pivot = (minimum + maximum) * Math::Scalar(0.5);

	MeshPtr mesh = Mesh::create();
	const int surface = mesh->addSurface(fileNameOf(getRoadName(), QString::fromUtf8(ROOT_NODE_NAME)).toUtf8().constData());
	for (int i = first; i <= last; ++i)
	{
		for (int j = 0; j <= grid_.columns; ++j)
		{
			const int index = grid_.index(i, j);
			mesh->addVertex(Math::vec3(grid_.vertices[index] - pivot), surface);
			mesh->addTexCoord0(grid_.uvs[index], surface);
			// The second UV channel and the vertex colors the way a mesh imported from
			// a 3D package has them: without them the engine gives every vertex UV1
			// (0, 0) and a black color of zero alpha, and a material that reads them
			// (decal materials do) goes wrong on the road.
			mesh->addTexCoord1(grid_.uvs[index], surface);
			mesh->addColor(Math::vec4(1.0f, 1.0f, 1.0f, 1.0f), surface);
			mesh->addNormal(grid_.normals[index], surface);
		}
	}
	// Two triangles per cell, their diagonals alternating like the cells of a chess
	// board, as in the planes of the engine: the grid bends evenly whichever way the
	// ground slopes.
	const int row = grid_.columns + 1;
	for (int i = 0; i < last - first; ++i)
	{
		for (int j = 0; j < grid_.columns; ++j)
		{
			const int a = i * row + j;
			const int b = a + 1;
			const int c = a + row;
			const int d = c + 1;
			const int indices[2][6] = {{a, b, d, d, c, a}, {a, b, c, d, c, b}};
			for (int k : indices[(i ^ j) & 1])
				mesh->addIndex(k, surface);
		}
	}
	mesh->createTangents(surface);
	mesh->createBounds(surface);
	mesh->createCollisionData(surface);
	return mesh;
}

bool RoadBuilder::generate()
{
	if (!hasRoad())
	{
		report(MSG_WARNING, uiText("Create a road or select an existing one first.", "Сначала создайте дорогу или выделите существующую."));
		return false;
	}

	// The pieces are generated as they are, shown.
	show_hidden_pieces();
	// The ground may have changed since the grid was drawn: it is measured anew.
	ground_cache_.clear();
	const bool bent = settings_.output == ROAD_OUTPUT_BENT;
	if (bent)
	{
		// The assets may have changed too.
		bend_sources_.clear();
		bend_errors_.clear();
	}
	rebuild_grid();
	if (bent && bend_.isEmpty())
	{
		report(MSG_WARNING, getBendError().isEmpty()
			? uiText("Add the meshes to bend, at least one of them a Pattern.",
				"Добавьте меши для изгиба, хотя бы один - «Паттерн».")
			: getBendError());
		return false;
	}
	if (!canGenerate())
	{
		report(MSG_WARNING, uiText("The road needs at least two points.", "Дороге нужно минимум две точки."));
		return false;
	}

	QString folder = getFolder();
	if (folder == QLatin1String("."))
		folder.clear();
	if (!folder.isEmpty() && !AssetManager::isDirectory(folder.toUtf8().constData())
		&& !AssetManager::createDirectory(folder.toUtf8().constData()))
	{
		report(MSG_ERROR, uiText("Could not create the folder %1.", "Не удалось создать папку %1.").arg(folder));
		return false;
	}

	// The files of a road keep the name they got the first time, in the same folder:
	// a road that is renamed or regenerated overwrites its own files. A copy of a road
	// made by the editor has the data of the original: it gets files of its own and
	// leaves those of the original.
	bool own_files = false;
	const QStringList old_files = ownGeneratedFiles(root_, road_generated_data(), &own_files);
	const int num_pieces = bent ? int(bend_.piece_starts.size()) : int(grid_.piece_starts.size());
	const QString base = pickBaseName(root_, road_generated_data(), own_files, folder,
		fileNameOf(getRoadName(), QString::fromUtf8(ROOT_NODE_NAME)), num_pieces, old_files);

	const bool decal = settings_.output == ROAD_OUTPUT_DECAL;
	const Node::TYPE piece_type = decal ? Node::DECAL_MESH : Node::OBJECT_MESH_STATIC;
	// The pieces of another kind than the last Generate made (a mesh road made into a
	// bent one, another asset bent) are not kept: they are of other surfaces.
	QString kind = QString::fromUtf8(decal ? "decal" : "mesh");
	if (bent)
	{
		kind = QString::fromUtf8("bent");
		for (const RoadBendPart &part : settings_.bend_parts)
			kind += QLatin1Char(':') + part.source;
	}
	const QString old_kind = dataString(root_, KIND_DATA);
	const bool same_kind = old_kind == kind || (old_kind.isEmpty() && !bent);
	// The material of a piece made from scratch: the one of the road if it suits the
	// kind of the pieces, else the base one.
	MaterialPtr material = Materials::findMaterialByFileGUID(guidOf(settings_.material));
	if (!material_suits(material, piece_type))
		material = Materials::findManualMaterial(decal ? "Unigine::decal_base" : "Unigine::mesh_base");

	// The pieces the road has are kept, in order: they only get the new geometry (their
	// files are written anew) and their new places, so whatever was set on them by hand
	// - the material first of all - stays. A piece added to a longer road is a copy of
	// the first one, with all of that too. Pieces of the other kind (Mesh <-> Decal)
	// are replaced.
	const NodePtr holder = mesh_node();
	QVector<NodePtr> old_pieces;
	NodePtr pattern;
	for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
	{
		const NodePtr child = holder->getChild(i);
		if (child->getType() != piece_type || !same_kind)
			continue;
		old_pieces.append(child);
		if (!pattern)
			pattern = child;
	}
	// What the pieces have now, before their files are written anew: the surfaces of the
	// old mesh are still there to be read.
	QVector<PieceState> old_states;
	for (const NodePtr &piece : old_pieces)
		old_states.append(PieceState::of(piece));
	Vector<NodePtr> created;
	QVector<NodePtr> kept;
	QVector<Math::Mat4> kept_transforms;
	QVector<PieceState> kept_states;
	QStringList files;

	// All the meshes are made first.
	QVector<MeshPtr> meshes;
	QVector<Math::Vec3> pivots;
	for (int piece = 0; piece < num_pieces; ++piece)
	{
		Math::Vec3 pivot;
		meshes.append(bent ? bend_mesh(bend_.piece_starts[piece], bend_.pieceEnd(piece), pivot) : make_road_mesh(piece, pivot));
		pivots.append(pivot);
	}

	// The files written over are a part of the undo step: Ctrl+Z gives them back what
	// they had. All the files are written first, and the scene is changed only when every
	// one of them is; the editor is kept from picking the files up on its own meanwhile.
	SetFilesAction *file_action = new SetFilesAction;
	AssetManager::blockAutoRefresh();
	QVector<bool> existed;
	{
		QString error;
		if (!writeMeshFiles(folder, base, meshes, file_action, files, existed, error))
		{
			AssetManager::unblockAutoRefresh();
			report(MSG_ERROR, error);
			return false;
		}
	}

	for (int piece = 0; piece < num_pieces; ++piece)
	{
		const QString &file = files[piece];
		const Math::Vec3 &pivot = pivots[piece];
		if (existed[piece])
			Render::reloadResource(file.toUtf8().constData());
		else
			AssetManager::importAssetSync(file.toUtf8().constData());

		const UGUID guid = AssetManager::getAssetGUIDFromPath(file.toUtf8().constData());
		const String mesh_path = guid.isValid() ? FileSystem::guidToPath(guid) : String(file.toUtf8().constData());

		const QString name = QFileInfo(file).completeBaseName();
		if (piece < old_pieces.size())
		{
			// A piece that is there: its file has just been written anew and the
			// engine reloads it; it is only moved to its new place below. The surfaces
			// it had keep what was set on them by hand; a bent piece that got a surface
			// it did not have gives it the settings of the asset.
			const NodePtr node = old_pieces[piece];
			const PieceState &before = old_states[piece];
			set_mesh_path(node, mesh_path);
			if (decal)
				checked_ptr_cast<DecalMesh>(node)->setRadius(settings_.decal_depth);
			else if (bent)
				apply_bend_materials(checked_ptr_cast<Object>(node), &before);
			else
			{
				ObjectMeshStaticPtr object = checked_ptr_cast<ObjectMeshStatic>(node);
				for (int s = 0; s < object->getNumSurfaces(); ++s)
				{
					if (const PieceState::Surface *settings = before.surface(object->getSurfaceName(s)))
						PieceState::putSurface(node, s, *settings);
				}
			}
			node->setName(name.toUtf8().constData());
			kept.append(node);
			kept_transforms.append(Math::translate(pivot));
			kept_states.append(before);
			continue;
		}

		NodePtr node;
		if (pattern)
		{
			node = pattern->clone();
			set_mesh_path(node, mesh_path);
			if (decal)
				checked_ptr_cast<DecalMesh>(node)->setRadius(settings_.decal_depth);
		} else if (decal)
		{
			DecalMeshPtr decal_mesh = DecalMesh::create(mesh_path.get());
			decal_mesh->setRadius(settings_.decal_depth);
			if (material)
				decal_mesh->setMaterial(material);
			node = decal_mesh;
		} else
		{
			ObjectMeshStaticPtr object = ObjectMeshStatic::create(mesh_path.get());
			if (material)
				object->setMaterial(material, "*");
			node = object;
		}
		node->setName(name.toUtf8().constData());
		node->setWorldParent(holder);
		node->setWorldTransform(Math::translate(pivot));
		node->setShowInEditorEnabledRecursive(true);
		node->setSaveToWorldEnabledRecursive(true);
		created.append(node);
	}
	AssetManager::unblockAutoRefresh();

	// The pieces of a bent road get the materials of the assets, surface by surface: a
	// piece has the surfaces of the meshes it holds.
	if (bent)
	{
		for (const NodePtr &node : created)
			apply_bend_materials(checked_ptr_cast<Object>(node));
	}

	// The depth and the distances of the decals are those of the settings, also on the
	// pieces that were there and on the new ones; the distances of the meshes too.
	if (!decal)
	{
		for (const NodePtr &node : created)
			apply_distances(checked_ptr_cast<Object>(node));
		for (const NodePtr &node : kept)
			apply_distances(checked_ptr_cast<Object>(node));
	} else
	{
		for (const NodePtr &node : created)
		{
			DecalMeshPtr decal_mesh = checked_ptr_cast<DecalMesh>(node);
			decal_mesh->setMaxVisibleDistance(settings_.decal_visible_distance > 0.0f ? settings_.decal_visible_distance : Math::Consts::INF);
			decal_mesh->setMaxFadeDistance(settings_.decal_fade_distance);
		}
		for (const NodePtr &node : kept)
		{
			DecalMeshPtr decal_mesh = checked_ptr_cast<DecalMesh>(node);
			decal_mesh->setMaxVisibleDistance(settings_.decal_visible_distance > 0.0f ? settings_.decal_visible_distance : Math::Consts::INF);
			decal_mesh->setMaxFadeDistance(settings_.decal_fade_distance);
		}
	}

	// What is in "mesh" and is not a piece of the new road: the pieces the road has
	// got shorter by, and the pieces of the other kind.
	Vector<NodePtr> removed;
	for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
	{
		const NodePtr child = holder->getChild(i);
		if (!is_piece(child))
			continue;
		bool stays = false;
		for (const NodePtr &node : kept)
			stays = stays || node->getID() == child->getID();
		for (const NodePtr &node : created)
			stays = stays || node->getID() == child->getID();
		if (!stays)
			removed.append(child);
	}

	// One undo step: the files written over with the pieces that stay, the pieces moved,
	// the new ones in, the extra ones out, and what the road remembers of its Generate.
	SetNodeDataAction *data_action = new SetNodeDataAction;
	setGeneratedData(data_action, root_, road_generated_data(), generatedFileList(files, old_files), base, folder);
	data_action->set(root_, STATE_DATA, QCryptographicHash::hash(snapshot(), QCryptographicHash::Md5).toHex());
	data_action->set(root_, KIND_DATA, kind.toUtf8());
	commitGenerate(file_action, kept, kept_states, kept_transforms, created, removed, data_action);

	report(MSG_INFO, uiText("Generated %1 pieces, %2 triangles, in %3.", "Создано кусков: %1, треугольников: %2, папка: %3.")
		.arg(kept.size() + created.size()).arg(bent ? bend_.triangles : grid_.numTriangles())
		.arg(folder.isEmpty() ? QString::fromUtf8("data") : folder));
	notify_changed();
	return true;
}

////////////////////////////////////////////////////////////////////////////////
// Every frame while the tool is active.
////////////////////////////////////////////////////////////////////////////////
bool RoadBuilder::eventFilter(QObject *watched, QEvent *event)
{
	if (!active_ || !hasRoad())
		return false;

	if (event->type() == QEvent::KeyPress && isAddingPoints()
		&& static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape && isEscapeFree(watched))
	{
		setAddingPoints(false);
		return true;
	}
	return editor_.handleEvent(watched, event);
}

void RoadBuilder::update()
{
	if (!active_)
		return;

	if (removed_root_ && (removed_root_.isDeleted() || ++removed_root_frames_ >= 3))
	{
		removed_root_ = NodePtr();
		if (on_roads_changed)
			on_roads_changed();
	}

	// The road was deleted or its world closed.
	if (root_ && !hasRoad())
	{
		root_ = NodePtr();
		editor_.setRoot(NodePtr());
		grid_ = RoadGrid();
		bend_ = RoadBend();
		remove_preview();
		grid_state_.clear();
		setAddingPoints(false);
		notify_changed();
	}
	if (!hasRoad())
	{
		remove_preview();
		return;
	}

	// The settings in the road are not those last written or read: an undo or a redo
	// has put others there.
	{
		const char *data = root_->getData(SETTINGS_DATA);
		if (QByteArray(data ? data : "") != saved_settings_)
		{
			load_settings();
			editor_.setClosed(settings_.closed);
			apply_decal_settings();
			notify_changed();
		}
	}

	editor_.update();

	const bool mouse_held = QApplication::mouseButtons() & Qt::LeftButton;
	// Not while a button is held: the point may be in the hands of the manipulator.
	if (!mouse_held)
		align_points();

	// With "snap after release" on, the road is not put on the ground while it is
	// changed with the mouse held, only when the button is let go.
	if (snapshot() != grid_state_)
	{
		rebuild_grid(!(snap_after_release_ && mouse_held));
		notify_changed();
	} else if (grid_draft_ && !mouse_held)
	{
		rebuild_grid();
		notify_changed();
	}
	update_preview();
	update_hidden_pieces();
	draw();
}

void RoadBuilder::align_points()
{
	const NodePtr path = editor_.getPathNode();
	if (!path)
		return;

	// Only the points that look elsewhere are turned: a turn about the vertical (or
	// a tilt along the road) means nothing to the road, the roll is kept. This is not
	// an undo step - the points only follow the curve, as the objects of a path do.
	const double aligned = std::cos(0.5 * Math::Consts::DEG2RAD_D);
	const CurvePoints points = editor_.getPoints();
	for (int i = 0; i < points.count(); ++i)
	{
		const NodePtr point = path->getChild(i);
		const Math::Vec3 forward = unitOrZero(level(points.direction(i)));
		if (Math::length2(forward) == 0.0)
			continue;
		const Math::Mat4 transform = point->getWorldTransform();
		const Math::Vec3 y_axis = unitOrZero(Math::Vec3(transform.getColumn3(1)));
		if (double(Math::dot(y_axis, forward)) >= aligned)
			continue;
		point->setWorldTransform(Math::translate(point->getWorldPosition())
			* Math::Mat4(curvePointBasis(forward, points.rolls[i]) * Math::scale(point->getWorldScale())));
	}
}

void RoadBuilder::draw() const
{
	// The curve itself, white.
	const CurvePolyline curve = buildPolyline(editor_.getPoints(), 24);
	const Math::vec4 curve_color(1.0f, 1.0f, 1.0f, 0.8f);
	for (int i = 0; i + 1 < curve.samples.size(); ++i)
		Visualizer::renderLine3D(curve.samples[i], curve.samples[i + 1], curve_color, 0.0f, false);

	// The grid the mesh is going to have: the edges of the road orange, the lines of
	// the grid along and across it dim blue, the joints of the pieces yellow.
	const RoadGrid &grid = grid_;
	// The joints of the copies of a bent mesh, dim blue lines; where it is cut into
	// pieces, a yellow section.
	if (view_ != ROAD_VIEW_CURVE && !bend_.isEmpty())
	{
		const Math::vec4 copy_color(0.45f, 0.65f, 0.9f, 0.8f);
		const Math::vec4 joint_color(1.0f, 0.9f, 0.2f, 1.0f);
		const Math::vec4 section_color(1.0f, 0.9f, 0.2f, 0.3f);
		for (int copy = 0; copy <= bend_.copies.size(); ++copy)
		{
			const double distance = copy < bend_.copies.size() ? bend_.copies[copy].start : bend_.length;
			const BendFrame f = bend_frame(distance);
			const Math::Vec3 right = Math::Vec3(f.right * f.width);
			const Math::Vec3 left_point = f.position - right * Math::Scalar(bend_.left_extent);
			const Math::Vec3 right_point = f.position + right * Math::Scalar(bend_.right_extent);
			if (!(bend_.piece_starts.contains(copy) && copy > 0))
			{
				Visualizer::renderLine3D(left_point, right_point, copy_color, 0.0f, false);
				continue;
			}
			// Where the mesh is cut into pieces: the section of the copies there, a
			// plane as wide and as high as they are.
			const Math::Vec3 low = Math::Vec3(f.up) * Math::Scalar(bend_.up_min);
			const Math::Vec3 high = Math::Vec3(f.up) * Math::Scalar(qMax(bend_.up_max, bend_.up_min + 0.01));
			const Math::Vec3 corners[4] = {left_point + low, right_point + low, right_point + high, left_point + high};
			Visualizer::renderQuad3D(corners[0], corners[1], corners[2], corners[3], section_color, 0.0f, false);
			for (int k = 0; k < 4; ++k)
				Visualizer::renderLine3D(corners[k], corners[(k + 1) % 4], joint_color, 0.0f, false);
		}
	}
	if (view_ != ROAD_VIEW_CURVE && !grid.isEmpty())
	{
		const Math::vec4 edge_color(1.0f, 0.55f, 0.1f, 1.0f);
		const Math::vec4 grid_color(0.45f, 0.65f, 0.9f, 0.55f);
		const Math::vec4 joint_color(1.0f, 0.9f, 0.2f, 1.0f);

		for (int j = 0; j <= grid.columns; ++j)
		{
			const bool edge = j == 0 || j == grid.columns;
			for (int i = 0; i + 1 < grid.stations; ++i)
			{
				Visualizer::renderLine3D(grid.vertices[grid.index(i, j)], grid.vertices[grid.index(i + 1, j)],
					edge ? edge_color : grid_color, 0.0f, false);
			}
		}
		for (int i = 0; i < grid.stations; ++i)
		{
			Math::vec4 color = grid_color;
			if (i == 0 || i == grid.stations - 1)
				color = edge_color;
			else if (grid.piece_starts.contains(i))
				color = joint_color;
			for (int j = 0; j < grid.columns; ++j)
				Visualizer::renderLine3D(grid.vertices[grid.index(i, j)], grid.vertices[grid.index(i, j + 1)], color, 0.0f, false);
		}
	}

	// A point the road keeps to the curve at, while the rest lies on the ground: a
	// line down to the ground under it shows how high it is.
	if (settings_.snap_to_ground)
	{
		const CurvePoints points = editor_.getPoints();
		const Math::vec4 height_color(0.35f, 0.7f, 1.0f, 0.8f);
		for (int i = 0; i < points.count(); ++i)
		{
			if (!points.off_ground[i])
				continue;
			QVector<Math::Vec3> point(1, points.positions[i]);
			QVector<double> height;
			ground_heights(point, height);
			if (!no_ground(height[0]))
			{
				Visualizer::renderLine3D(points.positions[i],
					Math::Vec3(points.positions[i].x, points.positions[i].y, Math::Scalar(height[0])), height_color, 0.0f, false);
			}
		}
	}

	editor_.drawPoints();
}

////////////////////////////////////////////////////////////////////////////////
// Bent mesh.
////////////////////////////////////////////////////////////////////////////////
bool RoadBuilder::addSelectedBendParts()
{
	if (!hasRoad())
	{
		report(MSG_WARNING, uiText("Create a road or select an existing one first.", "Сначала создайте дорогу или выделите существующую."));
		return false;
	}
	const QVector<PlaceableAsset> assets = resolveSelectedAssets();
	if (assets.isEmpty())
	{
		report(MSG_WARNING, uiText("Select meshes, nodes or models in the Asset Browser first.",
			"Сначала выделите меши, ноды или модели в Asset Browser."));
		return false;
	}

	RoadSettings settings = settings_;
	QStringList failed;
	for (const PlaceableAsset &asset : assets)
	{
		RoadBendPart part;
		part.source = QString::fromUtf8(asset.path.get());
		part.source_kind = int(asset.kind);
		part.source_name = QFileInfo(asset.name).completeBaseName();
		if (!bend_sources_.contains(part.source))
		{
			BendSource source;
			QString error;
			if (!BendSource::load(asset, source, error))
			{
				failed.append(QString::fromUtf8("%1: %2").arg(part.source_name, error));
				continue;
			}
			if (!source.note.isEmpty())
				failed.append(QString::fromUtf8("%1: %2").arg(part.source_name, source.note));
			bend_sources_.insert(part.source, source);
		}
		settings.bend_parts.append(part);
	}
	if (settings.bend_parts.size() != settings_.bend_parts.size())
		setSettings(settings);
	report(failed.isEmpty() ? MSG_INFO : MSG_WARNING, failed.join(QLatin1Char('\n')));
	return failed.isEmpty();
}

bool RoadBuilder::replaceBendPart(int part, const Vector<UGUID> &guids)
{
	if (part < 0 || part >= settings_.bend_parts.size())
		return false;
	PlaceableAsset asset;
	if (!resolveSelection(guids, asset))
	{
		report(MSG_WARNING, uiText("That is not a mesh: pick a .mesh, a .node or a model.", "Это не меш: выберите .mesh, .node или модель."));
		return false;
	}
	RoadSettings settings = settings_;
	RoadBendPart &bend_part = settings.bend_parts[part];
	bend_part.source = QString::fromUtf8(asset.path.get());
	bend_part.source_kind = int(asset.kind);
	bend_part.source_name = QFileInfo(asset.name).completeBaseName();
	bend_part.auto_length = true;
	QString note;
	if (!bend_sources_.contains(bend_part.source))
	{
		BendSource source;
		QString error;
		if (!BendSource::load(asset, source, error))
		{
			report(MSG_WARNING, QString::fromUtf8("%1: %2").arg(bend_part.source_name, error));
			return false;
		}
		if (!source.note.isEmpty())
			note = QString::fromUtf8("%1: %2").arg(bend_part.source_name, source.note);
		bend_sources_.insert(bend_part.source, source);
	}
	setSettings(settings);
	report(note.isEmpty() ? MSG_INFO : MSG_WARNING, note);
	return true;
}

void RoadBuilder::removeBendPart(int part)
{
	if (part < 0 || part >= settings_.bend_parts.size())
		return;
	RoadSettings settings = settings_;
	settings.bend_parts.removeAt(part);
	setSettings(settings);
}

void RoadBuilder::moveBendPart(int part, int offset)
{
	const int other = part + offset;
	if (part < 0 || part >= settings_.bend_parts.size() || other < 0 || other >= settings_.bend_parts.size())
		return;
	RoadSettings settings = settings_;
	settings.bend_parts.swapItemsAt(part, other);
	setSettings(settings);
}

void RoadBuilder::clearBendParts()
{
	RoadSettings settings = settings_;
	settings.bend_parts.clear();
	setSettings(settings);
}

bool RoadBuilder::showBendPart(int part) const
{
	if (part < 0 || part >= settings_.bend_parts.size())
		return false;
	PlaceableAsset asset;
	asset.kind = PlaceableAsset::Kind(settings_.bend_parts[part].source_kind);
	asset.path = String(settings_.bend_parts[part].source.toUtf8().constData());
	asset.name = settings_.bend_parts[part].source_name;
	return showInAssetBrowser(asset);
}

BendAxes RoadBuilder::bend_axes(int part) const
{
	BendAxes axes;
	axes.forward = settings_.bend_forward;
	axes.up = settings_.bend_up;
	axes.scale = settings_.bend_parts[part].scale;
	return axes;
}

double RoadBuilder::getBendPartLength(int part) const
{
	if (part < 0 || part >= settings_.bend_parts.size())
		return 0.0;
	const RoadBendPart &item = settings_.bend_parts[part];
	if (!item.auto_length)
		return double(item.length);
	const auto it = bend_sources_.constFind(item.source);
	return it == bend_sources_.constEnd() || it->isEmpty() ? 0.0 : double(bendBounds(*it, bend_axes(part)).length());
}

QString RoadBuilder::getBendError() const
{
	QStringList errors;
	for (const QString &error : bend_errors_)
	{
		if (!error.isEmpty())
			errors.append(error);
	}
	return errors.join(QLatin1Char('\n'));
}

void RoadBuilder::load_bend_parts()
{
	for (const RoadBendPart &part : settings_.bend_parts)
	{
		if (part.source.isEmpty() || bend_sources_.contains(part.source) || bend_errors_.contains(part.source))
			continue;
		PlaceableAsset asset;
		asset.kind = PlaceableAsset::Kind(part.source_kind);
		asset.path = String(part.source.toUtf8().constData());
		asset.name = part.source_name;
		BendSource source;
		QString error;
		if (BendSource::load(asset, source, error))
		{
			if (!source.note.isEmpty())
				report(MSG_INFO, QString::fromUtf8("%1: %2").arg(part.source_name, source.note));
			bend_sources_.insert(part.source, source);
		}
		else
			bend_errors_.insert(part.source, QString::fromUtf8("%1: %2").arg(part.source_name, error));
	}
}

void RoadBuilder::bend_names(QVector<QStringList> &names, QHash<QString, BendSource::Surface> &surfaces) const
{
	const QVector<RoadBendPart> &parts = settings_.bend_parts;
	names = QVector<QStringList>(parts.size());
	surfaces.clear();

	// What a surface of the result gets: not the geometry.
	const auto settings_of = [](const BendSource::Surface &surface) {
		BendSource::Surface info;
		info.material = surface.material;
		info.min_visible = surface.min_visible;
		info.max_visible = surface.max_visible;
		info.min_fade = surface.min_fade;
		info.max_fade = surface.max_fade;
		info.viewport_mask = surface.viewport_mask;
		info.shadow_mask = surface.shadow_mask;
		info.cast_shadow = surface.cast_shadow;
		info.cast_world_shadow = surface.cast_world_shadow;
		return info;
	};

	// Merged: the pattern meshes are looked through first, so that a merged surface
	// takes its settings from the first mesh of the pattern that has its material.
	QVector<int> order;
	for (int k = 0; k < parts.size(); ++k)
	{
		if (parts[k].role == ROAD_BEND_PATTERN)
			order.append(k);
	}
	for (int k = 0; k < parts.size(); ++k)
	{
		if (parts[k].role != ROAD_BEND_PATTERN)
			order.append(k);
	}

	QStringList sources;
	QHash<QString, QString> merged;
	QSet<QString> used;
	for (int k : order)
	{
		const auto source = bend_sources_.constFind(parts[k].source);
		if (source == bend_sources_.constEnd())
			continue;
		if (!sources.contains(parts[k].source))
			sources.append(parts[k].source);
		for (const BendSource::Surface &surface : source->surfaces)
		{
			QString name;
			if (settings_.bend_merge_materials)
			{
				// One surface per material, per LOD level and for the shadow LOD.
				const int lod = qMax(surface.lod, 0);
				const QString key = QString::fromUtf8(surface.material.makeString().get()) + QLatin1Char('|')
					+ QString::number(lod) + QLatin1Char('|') + QString::number(int(surface.shadow_lod));
				name = merged.value(key);
				if (name.isEmpty())
				{
					// The name of the .mat file of the project; a material that has no such
					// file of its own (made by the import of a model: its file is named by a
					// GUID) - the name of the surface without its LOD.
					static const QRegularExpression lod_suffix(QString::fromUtf8("_(shadow_?)?lod_?\\d*$"),
						QRegularExpression::CaseInsensitiveOption);
					static const QRegularExpression hash_name(QString::fromUtf8("^[0-9a-f]{32,}$"),
						QRegularExpression::CaseInsensitiveOption);
					QString base = surface.name;
					base.remove(lod_suffix);
					if (const MaterialPtr material = Materials::findMaterialByGUID(surface.material))
					{
						const UGUID file = material->getFileGUID();
						const QString path = file.isValid() ? QString::fromUtf8(FileSystem::getVirtualPath(file).get()) : QString();
						const QString file_name = QFileInfo(path).completeBaseName();
						if (!file_name.isEmpty() && !hash_name.match(file_name).hasMatch() && !path.contains(QLatin1String(".runtimes")))
							base = file_name;
						else if (base.isEmpty() && material->getManualName())
							base = QString::fromUtf8(material->getManualName());
					}
					if (base.isEmpty())
						base = QString::fromUtf8("material");
					if (surface.shadow_lod)
						base += QString::fromUtf8("_shadow_lod");
					else if (surface.lod >= 0)
						base += QString::fromUtf8("_lod%1").arg(surface.lod);
					name = base;
					for (int n = 2; used.contains(name); ++n)
						name = QString::fromUtf8("%1_%2").arg(base).arg(n);
					used.insert(name);
					merged.insert(key, name);
					surfaces.insert(name, settings_of(surface));
				}
			} else
			{
				// Apart: the surfaces of different assets may be named the same, so every
				// asset adds its number among the different assets of the list.
				name = QString::fromUtf8("m%1_").arg(sources.indexOf(parts[k].source)) + surface.name;
				surfaces.insert(name, settings_of(surface));
			}
			names[k].append(name);
		}
	}
}

QHash<QString, BendSource::Surface> RoadBuilder::bend_surfaces() const
{
	QVector<QStringList> names;
	QHash<QString, BendSource::Surface> surfaces;
	bend_names(names, surfaces);
	return surfaces;
}

void RoadBuilder::apply_bend_materials(const ObjectPtr &object, const PieceState *keep) const
{
	if (!object)
		return;
	const QHash<QString, BendSource::Surface> surfaces = bend_surfaces();
	for (int surface = 0; surface < object->getNumSurfaces(); ++surface)
	{
		if (keep)
		{
			if (const PieceState::Surface *settings = keep->surface(object->getSurfaceName(surface)))
			{
				PieceState::putSurface(object, surface, *settings);
				continue;
			}
		}
		const auto it = surfaces.constFind(QString::fromUtf8(object->getSurfaceName(surface)));
		if (it != surfaces.constEnd())
			applySurface(object, surface, *it);
	}
}

void RoadBuilder::add_bend_copies(int first, int end, const Math::Vec3 &pivot, const MeshPtr &mesh) const
{
	const auto frame = [this](double distance) { return bend_frame(distance); };
	QVector<QStringList> names;
	QHash<QString, BendSource::Surface> surfaces;
	bend_names(names, surfaces);
	QHash<int, BendBounds> bounds;
	for (int c = first; c < end; ++c)
	{
		const RoadBendCopy &copy = bend_.copies[c];
		const auto source = bend_sources_.constFind(settings_.bend_parts[copy.part].source);
		if (source == bend_sources_.constEnd())
			continue;
		if (!bounds.contains(copy.part))
			bounds.insert(copy.part, bendBounds(*source, bend_axes(copy.part)));
		bendCopy(*source, bend_axes(copy.part), bounds.value(copy.part), names[copy.part], copy.start, copy.fit, frame,
			pivot, mesh, settings_.bend_mesh_angle);
	}
}

void RoadBuilder::rebuild_bend(const CurvePoints &points, const QVector<double> &widths, const QVector<int> &point_samples,
	const QVector<Math::Vec3> &samples, const QVector<double> &distances)
{
	load_bend_parts();

	RoadBend &bend = bend_;
	bend.samples = samples;
	bend.distances = distances;
	bend.point_samples = point_samples;
	bend.rolls = points.rolls;
	bend.widths = widths;
	bend.roll = double(settings_.roll);
	bend.length = distances.last();

	// The way the curve goes at every sample: between the pieces of the polyline on
	// both sides of it, so that the copies turn smoothly. A closed curve goes on past
	// its end to its start.
	const int count = int(samples.size());
	bend.tangents.resize(count);
	for (int k = 0; k < count; ++k)
	{
		int previous = k - 1;
		int next = k + 1;
		if (points.closed)
		{
			if (previous < 0)
				previous = count - 2;
			if (next >= count)
				next = 1;
		} else
		{
			previous = qMax(previous, 0);
			next = qMin(next, count - 1);
		}
		Math::Vec3 way = unitOrZero(samples[next] - samples[previous]);
		if (Math::length2(way) == 0.0)
			way = k > 0 ? Math::Vec3(bend.tangents[k - 1]) : Math::Vec3(0.0, 1.0, 0.0);
		bend.tangents[k] = Math::vec3(way);
	}

	// The lengths and the bounds of the meshes, measured once: every copy takes them.
	QVector<double> part_lengths(settings_.bend_parts.size(), 0.0);
	QVector<BendBounds> part_bounds(settings_.bend_parts.size());
	for (int k = 0; k < settings_.bend_parts.size(); ++k)
	{
		const auto source = bend_sources_.constFind(settings_.bend_parts[k].source);
		if (source == bend_sources_.constEnd())
			continue;
		part_bounds[k] = bendBounds(*source, bend_axes(k));
		part_lengths[k] = settings_.bend_parts[k].auto_length ? double(part_bounds[k].length()) : double(settings_.bend_parts[k].length);
	}

	// The meshes that are loaded: the pattern ones in order, the first start and the
	// first end (a closed curve has no ends). Nothing without a pattern.
	QVector<int> pattern;
	int start = -1;
	int end = -1;
	for (int k = 0; k < settings_.bend_parts.size(); ++k)
	{
		const RoadBendPart &part = settings_.bend_parts[k];
		if (!bend_sources_.contains(part.source))
			continue;
		// A pattern mesh as many times in a row as its count.
		if (part.role == ROAD_BEND_PATTERN)
		{
			for (int n = 0; n < part.count; ++n)
				pattern.append(k);
		}
		else if (part.role == ROAD_BEND_START && start < 0 && !points.closed)
			start = k;
		else if (part.role == ROAD_BEND_END && end < 0 && !points.closed)
			end = k;
	}
	if (pattern.isEmpty())
		return;
	double cycle = 0.0;
	for (int k : pattern)
		cycle += part_lengths[k];
	if (cycle < 1e-4)
	{
		report(MSG_WARNING, uiText("The pattern takes no length of the curve: set the lengths of its meshes.",
			"Паттерн не занимает длины кривой: задайте длины его мешей."));
		return;
	}

	// The start and the end take their lengths at the ends of the curve; the pattern
	// is repeated between them as many times as fits best, and then all of it is
	// stretched or squeezed a little to fill the curve exactly. A curve too short for
	// them gets one mesh of the pattern, everything squeezed together.
	const double start_length = start >= 0 ? part_lengths[start] : 0.0;
	const double end_length = end >= 0 ? part_lengths[end] : 0.0;
	const double middle = bend.length - start_length - end_length;
	QVector<int> sequence;
	double nominal = 0.0;
	for (int i = 0; sequence.size() < MAX_BEND_COPIES; ++i)
	{
		const int k = pattern[i % pattern.size()];
		const double length = part_lengths[k];
		if (!sequence.isEmpty() && nominal + length * 0.5 > middle)
			break;
		sequence.append(k);
		nominal += length;
	}
	double ends_fit = 1.0;
	double middle_fit = nominal > 1e-9 ? middle / nominal : 1.0;
	if (middle < part_lengths[pattern.first()] * 0.5)
	{
		ends_fit = bend.length / (start_length + end_length + nominal);
		middle_fit = ends_fit;
	}

	qint64 triangles = 0;
	double position = 0.0;
	const auto add = [&](int k, double fit) {
		const double length = part_lengths[k] * fit;
		bend.copies.append(RoadBendCopy{k, position, length, fit});
		position += length;
		triangles += bend_sources_.value(settings_.bend_parts[k].source).numTriangles();
	};
	if (start >= 0)
		add(start, ends_fit);
	for (int k : sequence)
		add(k, middle_fit);
	if (end >= 0)
		add(end, ends_fit);
	// Mesh Angle cuts the copies where the curve turns: their share of its turn adds the
	// triangles of the cuts.
	if (settings_.bend_mesh_angle > 0.0f && bend.length > 1e-9)
	{
		double turn = 0.0;
		for (int i = 0; i + 1 < bend.tangents.size(); ++i)
		{
			const double cosine = qBound(-1.0, double(Math::dot(bend.tangents[i], bend.tangents[i + 1])), 1.0);
			turn += std::acos(cosine) * Math::Consts::RAD2DEG_D;
		}
		QHash<int, QPair<int, double>> by_part;
		for (const RoadBendCopy &copy : bend.copies)
		{
			by_part[copy.part].first += 1;
			by_part[copy.part].second += copy.length;
		}
		triangles = 0;
		for (auto it = by_part.constBegin(); it != by_part.constEnd(); ++it)
		{
			const int k = it.key();
			triangles += bentTriangles(bend_sources_.value(settings_.bend_parts[k].source), bend_axes(k), it.value().first,
				turn * it.value().second / bend.length, settings_.bend_mesh_angle);
		}
	}
	if (triangles > MAX_BEND_TRIANGLES)
	{
		bend.copies.clear();
		report(MSG_WARNING, uiText("The copies of the meshes would have more than %1 triangles: make the pattern longer, the curve shorter or Mesh Angle larger.",
			"У копий мешей было бы больше %1 треугольников: сделайте паттерн длиннее, кривую короче или «Угол меша» больше.").arg(MAX_BEND_TRIANGLES));
		return;
	}
	bend.pattern_copies = int(sequence.size());
	bend.pattern_fit = middle_fit;
	bend.triangles = int(triangles);
	bool first_bounds = true;
	for (const RoadBendCopy &copy : bend.copies)
	{
		const BendBounds &bounds = part_bounds[copy.part];
		bend.right_extent = qMax(bend.right_extent, double(bounds.right_max));
		bend.left_extent = qMax(bend.left_extent, -double(bounds.right_min));
		bend.up_min = first_bounds ? double(bounds.up_min) : qMin(bend.up_min, double(bounds.up_min));
		bend.up_max = first_bounds ? double(bounds.up_max) : qMax(bend.up_max, double(bounds.up_max));
		first_bounds = false;
	}

	// Pieces of whole copies, none longer than piece_length (but at least one copy).
	double piece = 0.0;
	for (int c = 0; c < bend.copies.size(); ++c)
	{
		if (c == 0 || (settings_.piece_length > 0.0f && piece + bend.copies[c].length > double(settings_.piece_length) + 1e-6))
		{
			bend.piece_starts.append(c);
			piece = 0.0;
		}
		piece += bend.copies[c].length;
	}
}

BendFrame RoadBuilder::bend_frame(double distance) const
{
	const RoadBend &bend = bend_;
	BendFrame frame;
	const int count = int(bend.samples.size());
	if (count < 2)
		return frame;

	distance = Math::clamp(distance, 0.0, bend.length);
	int k = int(std::upper_bound(bend.distances.begin(), bend.distances.end(), distance) - bend.distances.begin()) - 1;
	k = qBound(0, k, count - 2);
	const double piece = bend.distances[k + 1] - bend.distances[k];
	const double t = piece > 1e-12 ? Math::clamp((distance - bend.distances[k]) / piece, 0.0, 1.0) : 0.0;
	frame.position = bend.samples[k] + (bend.samples[k + 1] - bend.samples[k]) * Math::Scalar(t);
	Math::vec3 forward = bend.tangents[k] + (bend.tangents[k + 1] - bend.tangents[k]) * float(t);
	forward = Math::length2(forward) > 1e-12f ? Math::normalize(forward) : Math::vec3(0.0f, 1.0f, 0.0f);

	// The roll and the width go from one point of the curve to the next one smoothly,
	// as on a road.
	const int spans = int(bend.point_samples.size()) - 1;
	int span = int(std::upper_bound(bend.point_samples.begin(), bend.point_samples.end(), k) - bend.point_samples.begin()) - 1;
	span = qBound(0, span, qMax(spans - 1, 0));
	const int points = int(bend.rolls.size());
	const int next = (span + 1) % qMax(points, 1);
	const double span_start = bend.distances[bend.point_samples[span]];
	const double span_length = bend.distances[bend.point_samples[span + 1]] - span_start;
	const double f = span_length > 1e-9 ? Math::clamp((distance - span_start) / span_length, 0.0, 1.0) : 0.0;
	const double smooth = f * f * (3.0 - 2.0 * f);
	const double roll = (bend.rolls[span] + (bend.rolls[next] - bend.rolls[span]) * smooth + bend.roll) * Math::Consts::DEG2RAD_D;
	frame.width = float(bend.widths[span] + (bend.widths[next] - bend.widths[span]) * smooth);
	if (span_length > 1e-9 && f > 0.0 && f < 1.0)
		frame.width_slope = float((bend.widths[next] - bend.widths[span]) * 6.0 * f * (1.0 - f) / span_length);

	// Right is level, up leans with the slope of the curve; then both turn by the roll,
	// the right side down for a positive one.
	Math::vec3 right(forward.y, -forward.x, 0.0f);
	right = Math::length2(right) > 1e-12f ? Math::normalize(right) : Math::vec3(1.0f, 0.0f, 0.0f);
	const Math::vec3 up = Math::normalize(Math::cross(right, forward));
	const float c = float(std::cos(roll));
	const float s = float(std::sin(roll));
	frame.forward = forward;
	frame.right = right * c - up * s;
	frame.up = up * c + right * s;
	return frame;
}

MeshPtr RoadBuilder::bend_mesh(int first_copy, int end_copy, Math::Vec3 &pivot) const
{
	// The piece is put where the middle of its curve is.
	const double start = bend_.copies[first_copy].start;
	const double end = bend_.copies[end_copy - 1].start + bend_.copies[end_copy - 1].length;
	pivot = bend_frame((start + end) * 0.5).position;

	MeshPtr mesh = Mesh::create();
	add_bend_copies(first_copy, end_copy, pivot, mesh);
	for (int surface = 0; surface < mesh->getNumSurfaces(); ++surface)
	{
		mesh->createTangents(surface);
		mesh->createBounds(surface);
		mesh->createCollisionData(surface);
	}
	return mesh;
}

void RoadBuilder::update_preview()
{
	// Shown while the road is a bent mesh that is not generated as it is now: the
	// pieces that are there show it once they are up to date.
	bool show = active_ && hasRoad() && settings_.output == ROAD_OUTPUT_BENT && !bend_.isEmpty();
	if (show && getNumPieces() > 0)
		show = !is_up_to_date();
	if (!show)
	{
		if (preview_ && !preview_.isDeleted() && preview_->isEnabled())
			preview_->setEnabled(false);
		return;
	}

	if (!preview_ || preview_.isDeleted())
	{
		preview_ = ObjectMeshDynamic::create();
		preview_->setName("artist_tool_road_preview");
		preview_->setShowInEditorEnabled(false);
		preview_->setSaveToWorldEnabled(false);
		preview_dirty_ = true;
		// The points are not put on it: it moves with them.
		editor_.setLookThrough(preview_);
	}
	if (preview_dirty_)
	{
		preview_dirty_ = false;
		const Math::Vec3 pivot = bend_.samples.first();
		MeshPtr mesh = Mesh::create();
		add_bend_copies(0, int(bend_.copies.size()), pivot, mesh);
		for (int surface = 0; surface < mesh->getNumSurfaces(); ++surface)
		{
			mesh->createTangents(surface);
			mesh->createBounds(surface);
		}
		preview_->setMesh(mesh);
		preview_->setWorldTransform(Math::translate(pivot));
		// The materials of the assets; the preview is not picked by the clicks.
		apply_bend_materials(preview_);
		apply_distances(preview_);
		for (int surface = 0; surface < preview_->getNumSurfaces(); ++surface)
		{
			preview_->setIntersection(false, surface);
			preview_->setCollision(false, surface);
			preview_->setPhysicsIntersection(false, surface);
		}
	}
	if (!preview_->isEnabled())
		preview_->setEnabled(true);
}

bool RoadBuilder::is_up_to_date() const
{
	return dataString(root_, STATE_DATA).toUtf8() == QCryptographicHash::hash(grid_state_, QCryptographicHash::Md5).toHex();
}

void RoadBuilder::setHideOutdated(bool hide)
{
	hide_outdated_ = hide;
	if (!hide)
		show_hidden_pieces();
}

void RoadBuilder::update_hidden_pieces()
{
	// Only a bent mesh: its preview and the old pieces would overlap.
	const NodePtr holder = mesh_node();
	if (!hide_outdated_ || settings_.output != ROAD_OUTPUT_BENT || !holder || holder->getNumChildren() == 0 || is_up_to_date())
	{
		show_hidden_pieces();
		return;
	}
	// Not an undo step: the pieces only get out of the way while the road is edited.
	for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
	{
		const NodePtr piece = holder->getChild(i);
		if (is_piece(piece) && piece->isEnabled())
		{
			piece->setEnabled(false);
			piece->setData(HIDDEN_DATA, "1");
			hidden_pieces_.append(piece->getID());
		}
	}
}

void RoadBuilder::show_hidden_pieces()
{
	const auto show = [](const NodePtr &piece) {
		if (piece && !piece.isDeleted() && piece->hasData(HIDDEN_DATA))
		{
			piece->setEnabled(true);
			piece->removeData(HIDDEN_DATA);
		}
	};
	for (int id : hidden_pieces_)
		show(World::getNodeByID(id));
	hidden_pieces_.clear();
	// Also the pieces hidden by the tool that it does not know: of a copy of the road
	// made by the editor while they were hidden.
	if (const NodePtr holder = hasRoad() ? mesh_node() : NodePtr())
	{
		for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
			show(holder->getChild(i));
	}
}

void RoadBuilder::remove_preview()
{
	if (preview_ && !preview_.isDeleted())
		preview_.deleteLater();
	preview_ = ObjectMeshDynamicPtr();
	editor_.setLookThrough(NodePtr());
	preview_dirty_ = true;
}

void RoadBuilder::notify_changed() const
{
	if (on_changed)
		on_changed();
}

void RoadBuilder::report(int level, const QString &text) const
{
	if (on_message)
		on_message(level, text);
}

} // namespace ArtistTool
