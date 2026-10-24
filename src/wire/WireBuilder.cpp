#include "WireBuilder.h"
#include "UiHelpers.h"
#include "CurveEditor.h"
#include "Localization.h"
#include "GeneratedFiles.h"
#include "NodeData.h"
#include "PlaceableAsset.h"
#include "UndoActions.h"
#include "ViewportPicking.h"

#include <editor/UnigineActions.h>
#include <editor/UnigineAssetManager.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>
#include <editor/UnigineUndo.h>

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
#include <QHash>
#include <QJsonArray>
#include <QJsonDocument>
#include <QJsonObject>
#include <QKeyEvent>
#include <QRandomGenerator>
#include <QMap>
#include <QPair>
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

const char ROOT_NODE_NAME[] = "wires";
const char MESH_NODE_NAME[] = "mesh";
const char DEFAULT_FOLDER_NAME[] = "wires";

// Names of the node data the tool keeps its state in: the settings, the poles, and
// what the last Generate did - the files it wrote, the name and the folder they got,
// and what the line was like then.
const char SETTINGS_DATA[] = "artist_tool_wire";
const char POLES_DATA[] = "artist_tool_wire_poles";
const char FILES_DATA[] = "artist_tool_wire_files";
// The GUIDs of those files, line by line: a file moved away and another one put at its
// path is not taken for one of them.
const char FILE_GUIDS_DATA[] = "artist_tool_wire_file_guids";
const char BASE_NAME_DATA[] = "artist_tool_wire_base";
const char FOLDER_DATA[] = "artist_tool_wire_folder";
const char STATE_DATA[] = "artist_tool_wire_state";
// What the pieces made by the last Generate are: "wire" for strips and tubes, "mesh:"
// and the asset for a mesh. Pieces of another kind are not kept by the next Generate.
const char KIND_DATA[] = "artist_tool_wire_kind";
// The ID of the line whose files these are: a line copied by the editor takes the data
// of the original and must not write or remove its files.
const char OWNER_DATA[] = "artist_tool_wire_owner";

// The data of the last Generate, for the helpers of common/GeneratedFiles.
GeneratedData line_generated_data()
{
	return GeneratedData{FILES_DATA, FILE_GUIDS_DATA, BASE_NAME_DATA, FOLDER_DATA, OWNER_DATA};
}

// "1" on a piece hidden while it is out of date.
const char HIDDEN_DATA[] = "artist_tool_hidden";
// The surface of the strips and the tubes, and of their LOD.
const char WIRE_SURFACE_NAME[] = "wire";
const char WIRE_LOD_SURFACE_NAME[] = "wire_lod1";
// What a point of a curve sets for the span from it to the next point: a multiplier of
// the sag of the line (none - 1), and a random of its own in percent (none - that of the
// line).
const char POINT_SAG_DATA[] = "artist_tool_wire_sag";
const char POINT_RANDOM_DATA[] = "artist_tool_wire_random";
// And for the shape of the span: a multiplier of the thickness (none - 1), turns added to
// the twist of the line (none - 0).
const char POINT_THICKNESS_DATA[] = "artist_tool_wire_thickness";
const char POINT_TWIST_DATA[] = "artist_tool_wire_twist";

// The node of a line drawn as a curve its poles are in, and the data of a pole: the
// asset it is made of.
const char POLES_NODE_NAME[] = "poles";
// The poles the line has no place for any more, kept off and out of sight instead of being
// removed: when Ctrl+Z gives the line its points back, they come back with what was set
// on them by hand. None of them is ever removed - the undo history can go back to any of
// them - and they are not saved with the world, which ends the history too.
const char SPARE_POLES_NODE_NAME[] = "spare_poles";
const char POLE_SOURCE_DATA[] = "artist_tool_wire_pole";
// The rotation a point of the curve was given with the manipulator on top of looking along
// the curve, and its pole with it: a quaternion "x y z w".
const char POINT_TURN_DATA[] = "artist_tool_wire_turn";
// The data of an insulator on a pole: the asset it is made of, the number of the attach
// point it stands on.
const char INSULATOR_SOURCE_DATA[] = "artist_tool_wire_insulator";
const char INSULATOR_NUMBER_DATA[] = "artist_tool_wire_insulator_point";
// "1" - an insulator of a point the pole has no more, kept off and out of sight for when
// the point comes back; never removed, as the spare poles.
const char INSULATOR_SPARE_DATA[] = "artist_tool_wire_insulator_spare";
// The roles of the assets of the poles in the settings.
const int POLE_ROLES = 5;
const char *const POLE_ROLE_KEYS[POLE_ROLES] = {"pattern", "start", "end", "corner", "insulator"};
// The most poles a line drawn as a curve may have.
const int MAX_POLES = 5000;
// How far above and below the curve the ground under a pole is searched for.
const double GROUND_UP = 5.0;
// When that finds nothing (it started inside the ground, under a hill), the ray is cast
// again from this high above.
const double GROUND_FAR_UP = 1000.0;
const double GROUND_DOWN = 100.0;

// The most wires a line may have, and the most triangles all of them together.
const int MAX_WIRES = 20000;
const int MAX_TRIANGLES = 1000000;
// The most copies of a mesh one wire may get.
const int MAX_MESH_COPIES = 2000;
// The wire a sag set with WIRE_SAG_TENSION is for.
const double TENSION_SPAN = 10.0;
// The most joints a wire gets for its twist.
const int MAX_TWIST_SAMPLES = 4096;
// The most samples a wire may get where it lies on the ground.
const int MAX_GROUND_SAMPLES = 4096;
// How deep into a pole its attach points are searched for.
const int MAX_ATTACH_DEPTH = 16;
// How many levels over a selected node its pole is searched for.
const int MAX_POLE_LEVELS = 2;
// How many of the last selections are remembered while poles are picked: an undo of the
// editor puts one of them back, and that is not a pole picked.
const int REMEMBERED_SELECTIONS = 32;

bool is_line_root(const NodePtr &node)
{
	return node && node->hasData(SETTINGS_DATA) && node->findChild(MESH_NODE_NAME) >= 0;
}

// The root of the line the node is a part of.
NodePtr find_line_root(const NodePtr &node)
{
	for (NodePtr n = node; n; n = n->getParent())
	{
		if (is_line_root(n))
			return n;
	}
	return NodePtr();
}

// The materials that come with the plugin (data/zloy_pingvin/artist_tool/materials), the
// default ones of the wires: crossed strips with alpha, and a ribbon or a tube.
const char CROSS_MATERIAL_GUID[] = "1b923e266540a4cffb2e31fed6892161844c2f89"; // wire_alpha_blend
const char SOLID_MATERIAL_GUID[] = "e8105cac207eefb516f037923dd087dfbc2d2962"; // wire_solid

// A material of a mesh by the GUID of its file; null if there is none, or not one a mesh
// can have.
MaterialPtr mesh_material(const QString &guid)
{
	const MaterialPtr material = Materials::findMaterialByFileGUID(guidOf(guid));
	return material && material->isNodeTypeSupported(Node::OBJECT_MESH_STATIC) ? material : MaterialPtr();
}

// The material of the strips and the tubes when none is picked: the one that comes with the
// plugin for the shape, or the base one if it is gone from the project.
MaterialPtr default_wire_material(WireShapeKind shape)
{
	if (const MaterialPtr material = mesh_material(QString::fromUtf8(shape == WIRE_SHAPE_CROSS ? CROSS_MATERIAL_GUID : SOLID_MATERIAL_GUID)))
		return material;
	return Materials::findManualMaterial("Unigine::mesh_base");
}

// The material of the strips and the tubes: the one of the settings if it suits a mesh,
// the default one of the shape if none is picked, else the base one.
MaterialPtr wire_material(const QString &material, WireShapeKind shape)
{
	if (material.isEmpty())
		return default_wire_material(shape);
	if (const MaterialPtr result = mesh_material(material))
		return result;
	return Materials::findManualMaterial("Unigine::mesh_base");
}

// Whether the material is one a wire gets by default (of any shape): such a material of a
// piece follows the shape, a material given by hand stays.
bool is_default_wire_material(const UGUID &material)
{
	for (const MaterialPtr &candidate : {mesh_material(QString::fromUtf8(CROSS_MATERIAL_GUID)),
			mesh_material(QString::fromUtf8(SOLID_MATERIAL_GUID)), Materials::findManualMaterial("Unigine::mesh_base")})
	{
		if (candidate && candidate->getGUID() == material)
			return true;
	}
	return false;
}

// The rotation a point was given with the manipulator (see POINT_TURN_DATA); none - none.
Math::quat point_turn(const NodePtr &point)
{
	const char *data = point->getData(POINT_TURN_DATA);
	const QList<QByteArray> parts = QByteArray(data ? data : "").split(' ');
	if (parts.size() == 4)
	{
		// quat(x, y, z, w) of four floats would be an axis and an angle.
		Math::quat turn(Math::vec4(parts[0].toFloat(), parts[1].toFloat(), parts[2].toFloat(), parts[3].toFloat()));
		return Math::length2(Math::vec4(turn.x, turn.y, turn.z, turn.w)) > 1e-12f ? Math::normalize(turn) : Math::quat_identity;
	}
	return Math::quat_identity;
}

// The vector of an axis of an asset.
Math::vec3 axis_vector(AxisDirection axis)
{
	static const Math::vec3 vectors[6] = {Math::vec3(1.0f, 0.0f, 0.0f), Math::vec3(-1.0f, 0.0f, 0.0f), Math::vec3(0.0f, 1.0f, 0.0f),
		Math::vec3(0.0f, -1.0f, 0.0f), Math::vec3(0.0f, 0.0f, 1.0f), Math::vec3(0.0f, 0.0f, -1.0f)};
	return vectors[qBound(0, int(axis), 5)];
}

// Whether two rotations are the same, to about a twentieth of a degree: what a rotation
// read back from a transform differs by. In doubles - 1 - 1e-8 is 1 as a float.
bool same_rotation(const Math::quat &a, const Math::quat &b)
{
	const double dot = double(a.x) * double(b.x) + double(a.y) * double(b.y) + double(a.z) * double(b.z)
		+ double(a.w) * double(b.w);
	return std::abs(dot) > 1.0 - 1e-7;
}

Math::quat rotation_of(const Math::Mat4 &transform)
{
	Math::vec3 position, scale;
	Math::quat rotation;
	Math::decomposeTransform(Math::mat4(transform), position, rotation, scale);
	return rotation;
}

// A number from 0 to 1 made from the numbers, always the same for the same ones.
float random_of(quint64 a, quint64 b, quint64 c, quint64 d)
{
	quint64 x = a * 0x9E3779B97F4A7C15ull ^ (b + 0x632BE59BD9B4E019ull) * 0xBF58476D1CE4E5B9ull
		^ (c + 0x8CB92BA72F3D8DD7ull) * 0x94D049BB133111EBull ^ (d + 0x2545F4914F6CDD1Dull);
	x ^= x >> 30;
	x *= 0xBF58476D1CE4E5B9ull;
	x ^= x >> 27;
	x *= 0x94D049BB133111EBull;
	x ^= x >> 31;
	return float(double(x >> 11) / double(1ull << 53));
}

WireSettings limited(WireSettings settings)
{
	settings.attach_name = settings.attach_name.trimmed();
	if (settings.attach_name.isEmpty())
		settings.attach_name = WireSettings().attach_name;
	settings.pole_spacing = qMax(settings.pole_spacing, 0.1f);
	settings.ground.look_through = true;
	settings.pole_forward = AxisDirection(qBound(int(AXIS_X_POSITIVE), int(settings.pole_forward), int(AXIS_Z_NEGATIVE)));
	settings.pole_up = AxisDirection(qBound(int(AXIS_X_POSITIVE), int(settings.pole_up), int(AXIS_Z_NEGATIVE)));
	// The two can not be the same axis.
	if (int(settings.pole_up) / 2 == int(settings.pole_forward) / 2)
		settings.pole_up = AxisDirection((int(settings.pole_forward) / 2 * 2 + 2) % 6);
	for (WirePolePart &part : settings.pole_parts)
		part.role = WirePoleRole(qBound(int(WIRE_POLE_PATTERN), int(part.role), int(WIRE_POLE_INSULATOR)));
	settings.sag_mode = WireSagMode(qBound(int(WIRE_SAG_UNITS), int(settings.sag_mode), int(WIRE_SAG_TENSION)));
	settings.sag = qMax(settings.sag, 0.0f);
	settings.sag_random = qBound(0.0f, settings.sag_random, 1000.0f);
	settings.segment_angle = qBound(0.1f, settings.segment_angle, 90.0f);
	settings.min_segments = qBound(1, settings.min_segments, 256);
	settings.max_segment_length = qMax(settings.max_segment_length, 0.0f);
	settings.shape = WireShapeKind(qBound(int(WIRE_SHAPE_CROSS), int(settings.shape), int(WIRE_SHAPE_MESH)));
	settings.thickness = qMax(settings.thickness, 0.0001f);
	settings.sides = qBound(3, settings.sides, 64);
	settings.twist_mode = WireTwistMode(qBound(int(WIRE_TWIST_PER_UNIT), int(settings.twist_mode), int(WIRE_TWIST_PER_SPAN)));
	settings.twist = qBound(-1000.0f, settings.twist, 1000.0f);
	settings.uv_length = qMax(settings.uv_length, 0.001f);
	settings.uv_width = qBound(-1000.0f, settings.uv_width, 1000.0f);
	settings.uv_rotation = qBound(-180.0f, settings.uv_rotation, 180.0f);
	settings.mesh_forward = AxisDirection(qBound(int(AXIS_X_POSITIVE), int(settings.mesh_forward), int(AXIS_Z_NEGATIVE)));
	settings.mesh_up = AxisDirection(qBound(int(AXIS_X_POSITIVE), int(settings.mesh_up), int(AXIS_Z_NEGATIVE)));
	settings.mesh_scale = qMax(settings.mesh_scale, 0.001f);
	settings.mesh_angle = qBound(0.0f, settings.mesh_angle, 90.0f);
	settings.twist_step = qBound(0.0f, settings.twist_step, 180.0f);
	settings.piece_size = qMax(settings.piece_size, 0.0f);
	settings.visible_distance = qMax(settings.visible_distance, 0.0f);
	settings.lod_factor = qBound(2, settings.lod_factor, 64);
	settings.lod_distance = qMax(settings.lod_distance, 0.0f);
	settings.lod_fade = qMax(settings.lod_fade, 0.0f);
	settings.fade_distance = qMax(settings.fade_distance, 0.0f);
	return settings;
}

QJsonObject to_json(const WireSettings &s)
{
	QJsonObject json;
	json.insert("attach_name", s.attach_name);
	json.insert("closed", s.closed);
	QJsonArray parts;
	for (const WirePolePart &part : s.pole_parts)
	{
		QJsonObject item;
		item.insert("source", part.source);
		item.insert("kind", part.source_kind);
		item.insert("name", part.source_name);
		item.insert("role", QString::fromUtf8(POLE_ROLE_KEYS[part.role]));
		parts.append(item);
	}
	json.insert("pole_parts", parts);
	json.insert("pole_every", s.pole_every);
	json.insert("pole_spacing", double(s.pole_spacing));
	json.insert("pole_forward", int(s.pole_forward));
	json.insert("pole_up", int(s.pole_up));
	json.insert("pole_on_ground", s.pole_on_ground);
	json.insert("ground_types", s.ground.surface_types);
	json.insert("ground_immovable", s.ground.only_immovable);
	json.insert("ground_intersection", s.ground.only_intersection);
	json.insert("sag_mode", int(s.sag_mode));
	json.insert("sag", double(s.sag));
	json.insert("sag_random", double(s.sag_random));
	json.insert("segment_angle", double(s.segment_angle));
	json.insert("above_ground", s.above_ground);
	json.insert("min_segments", s.min_segments);
	json.insert("max_segment_length", double(s.max_segment_length));
	json.insert("shape", int(s.shape));
	json.insert("thickness", double(s.thickness));
	json.insert("sides", s.sides);
	json.insert("rotation", double(s.rotation));
	json.insert("random_rotation", s.random_rotation);
	json.insert("twist_mode", int(s.twist_mode));
	json.insert("twist", double(s.twist));
	json.insert("uv_length", double(s.uv_length));
	json.insert("uv_width", double(s.uv_width));
	json.insert("uv_rotation", double(s.uv_rotation));
	json.insert("material", s.material);
	json.insert("mesh_source", s.mesh_source);
	json.insert("mesh_kind", s.mesh_kind);
	json.insert("mesh_name", s.mesh_name);
	json.insert("mesh_forward", int(s.mesh_forward));
	json.insert("mesh_up", int(s.mesh_up));
	json.insert("mesh_scale", double(s.mesh_scale));
	json.insert("mesh_angle", double(s.mesh_angle));
	json.insert("twist_step", double(s.twist_step));
	json.insert("piece_size", double(s.piece_size));
	json.insert("visible_distance", double(s.visible_distance));
	json.insert("lod", s.lod);
	json.insert("lod_factor", s.lod_factor);
	json.insert("lod_distance", double(s.lod_distance));
	json.insert("lod_fade", double(s.lod_fade));
	json.insert("fade_distance", double(s.fade_distance));
	json.insert("seed", s.seed);
	json.insert("folder", s.folder);
	return json;
}

WireSettings from_json(const QJsonObject &json)
{
	const WireSettings d;
	WireSettings s;
	s.attach_name = json.value("attach_name").toString(d.attach_name);
	s.closed = json.value("closed").toBool(d.closed);
	for (const QJsonValue &value : json.value("pole_parts").toArray())
	{
		const QJsonObject item = value.toObject();
		WirePolePart part;
		part.source = item.value("source").toString();
		part.source_kind = item.value("kind").toInt();
		part.source_name = item.value("name").toString();
		const QString role = item.value("role").toString();
		for (int r = 0; r < POLE_ROLES; ++r)
		{
			if (role == QString::fromUtf8(POLE_ROLE_KEYS[r]))
				part.role = WirePoleRole(r);
		}
		s.pole_parts.append(part);
	}
	s.pole_spacing = float(json.value("pole_spacing").toDouble(d.pole_spacing));
	// The builds before had no switch: a spacing of 0 was "only at the points".
	s.pole_every = json.value("pole_every").toBool(s.pole_spacing > 0.0f && json.contains("pole_spacing"));
	if (s.pole_spacing <= 0.0f)
		s.pole_spacing = d.pole_spacing;
	s.pole_forward = AxisDirection(json.value("pole_forward").toInt(d.pole_forward));
	s.pole_up = AxisDirection(json.value("pole_up").toInt(d.pole_up));
	s.pole_on_ground = json.value("pole_on_ground").toBool(d.pole_on_ground);
	s.ground.surface_types = json.value("ground_types").toInt(d.ground.surface_types);
	s.ground.only_immovable = json.value("ground_immovable").toBool(d.ground.only_immovable);
	s.ground.only_intersection = json.value("ground_intersection").toBool(d.ground.only_intersection);
	s.sag_mode = WireSagMode(json.value("sag_mode").toInt(d.sag_mode));
	s.sag = float(json.value("sag").toDouble(d.sag));
	s.sag_random = float(json.value("sag_random").toDouble(d.sag_random));
	s.segment_angle = float(json.value("segment_angle").toDouble(d.segment_angle));
	s.above_ground = json.value("above_ground").toBool(d.above_ground);
	s.min_segments = json.value("min_segments").toInt(d.min_segments);
	s.max_segment_length = float(json.value("max_segment_length").toDouble(d.max_segment_length));
	s.shape = WireShapeKind(json.value("shape").toInt(d.shape));
	s.thickness = float(json.value("thickness").toDouble(d.thickness));
	s.sides = json.value("sides").toInt(d.sides);
	s.rotation = float(json.value("rotation").toDouble(d.rotation));
	s.random_rotation = json.value("random_rotation").toBool(d.random_rotation);
	s.twist_mode = WireTwistMode(json.value("twist_mode").toInt(d.twist_mode));
	s.twist = float(json.value("twist").toDouble(d.twist));
	s.uv_length = float(json.value("uv_length").toDouble(d.uv_length));
	s.uv_width = float(json.value("uv_width").toDouble(d.uv_width));
	s.uv_rotation = float(json.value("uv_rotation").toDouble(d.uv_rotation));
	s.material = json.value("material").toString();
	s.mesh_source = json.value("mesh_source").toString();
	s.mesh_kind = json.value("mesh_kind").toInt();
	s.mesh_name = json.value("mesh_name").toString();
	s.mesh_forward = AxisDirection(json.value("mesh_forward").toInt(d.mesh_forward));
	s.mesh_up = AxisDirection(json.value("mesh_up").toInt(d.mesh_up));
	s.mesh_scale = float(json.value("mesh_scale").toDouble(d.mesh_scale));
	s.mesh_angle = float(json.value("mesh_angle").toDouble(d.mesh_angle));
	s.twist_step = float(json.value("twist_step").toDouble(d.twist_step));
	s.piece_size = float(json.value("piece_size").toDouble(d.piece_size));
	s.visible_distance = float(json.value("visible_distance").toDouble(d.visible_distance));
	s.lod = json.value("lod").toBool(d.lod);
	s.lod_factor = json.value("lod_factor").toInt(d.lod_factor);
	s.lod_distance = float(json.value("lod_distance").toDouble(d.lod_distance));
	s.lod_fade = float(json.value("lod_fade").toDouble(d.lod_fade));
	s.fade_distance = float(json.value("fade_distance").toDouble(d.fade_distance));
	s.seed = json.value("seed").toInt(d.seed);
	s.folder = json.value("folder").toString();
	return limited(s);
}

} // namespace

int WireBuilder::randomSeed()
{
	return int(QRandomGenerator::global()->bounded(std::numeric_limits<int>::max()));
}

WireBuilder::WireBuilder(QObject *parent)
	: QObject(parent)
{
	editor_.on_changed = [this]() { notify_changed(); };
	editor_.on_message = [this](int level, const QString &text) { report(level, text); };
	// A point put into a span by Subdivide keeps its sag.
	editor_.setSpanData({QByteArray(POINT_SAG_DATA), QByteArray(POINT_RANDOM_DATA), QByteArray(POINT_THICKNESS_DATA),
		QByteArray(POINT_TWIST_DATA)});
}

WireBuilder::~WireBuilder()
{
	on_changed = nullptr;
	on_message = nullptr;
	on_lines_changed = nullptr;
	setActive(false);
	remove_preview();
	show_hidden_pieces();
	editor_.on_changed = nullptr;
	editor_.on_message = nullptr;
}

void WireBuilder::setActive(bool active)
{
	if (active_ == active)
		return;

	active_ = active;
	if (active_)
	{
		CurveEditor::recoverManipulators();
		// The wires are drawn with the Visualizer too, once per frame.
		requestVisualizer();
		Engine::get()->getEventEndWorldUpdate().connect(connections_, this, &WireBuilder::update);
		// Hidden pieces are saved shown; they are hidden again on the next frame.
		World::getEventPreWorldSave().connect(connections_, [this](const char *) { show_hidden_pieces(); });
		// Esc ends adding poles; Ctrl+Z is told from a pole selected.
		qApp->installEventFilter(this);
		// The poles and the mesh may have changed while the tool was off.
		mesh_source_path_.clear();
		state_.clear();
		poles_state_.clear();
		ground_heights_.clear();
		pole_ground_.clear();
		selectionChanged();
	} else
	{
		setAddingPoles(false);
		setAddingPoints(false);
		editor_.reset();
		remove_preview();
		show_hidden_pieces();
		qApp->removeEventFilter(this);
		connections_.disconnectAll();
		releaseVisualizer();
	}
}

void WireBuilder::selectionChanged()
{
	Vector<NodePtr> nodes;
	if (const SelectorNodes *selector = Selection::getSelectorNodes())
		nodes = selector->getNodes();
	QSet<int> selected;
	for (const NodePtr &node : nodes)
	{
		if (node)
			selected.insert(node->getID());
	}

	// A selection that was there before is the one an undo (or a redo) of the editor has
	// put back, not a pole picked: an undo from the menu, or a slow one, is told too.
	const bool undoing = recent_selections_.contains(selected);
	if (adding_poles_ && hasLine() && !undoing)
	{
		QVector<int> ids = pole_ids();
		bool added = false;
		QStringList skipped;
		for (const NodePtr &node : nodes)
		{
			if (!node || selected_before_.contains(node->getID()))
				continue;
			const NodePtr pole = pole_of(node);
			if (!pole && isNodeAlive(node) && !find_line_root(node))
				skipped.append(QString::fromUtf8(node->getName()));
			if (!pole || (!ids.isEmpty() && ids.last() == pole->getID()))
				continue;
			ids.append(pole->getID());
			added = true;
		}
		if (added)
			set_pole_ids(ids);
		report_skipped(skipped);
	} else if (!adding_poles_)
	{
		for (const NodePtr &node : nodes)
		{
			if (const NodePtr root = node ? find_line_root(node) : NodePtr())
			{
				set_line(root);
				break;
			}
		}
	}
	selected_before_ = selected;
	remember_selection(selected);
	notify_changed();
}

void WireBuilder::remember_selection(const QSet<int> &selected)
{
	if (!recent_selections_.isEmpty() && recent_selections_.last() == selected)
		return;
	recent_selections_.append(selected);
	while (recent_selections_.size() > REMEMBERED_SELECTIONS)
		recent_selections_.removeFirst();
}

QVector<WireLineInfo> WireBuilder::findLines() const
{
	Vector<NodePtr> nodes;
	World::getNodes(nodes, false, false);

	QVector<WireLineInfo> lines;
	for (const NodePtr &node : nodes)
	{
		if (!node || !is_line_root(node) || !isNodeAlive(node) || isEditorInternal(node))
			continue;
		WireLineInfo info;
		info.id = node->getID();
		info.name = QString::fromUtf8(node->getName());
		const NodePtr path = findChildNode(node, CURVE_PATH_NODE_NAME);
		info.poles = path ? path->getNumChildren() : dataString(node, POLES_DATA).split(QLatin1Char(' '), Qt::SkipEmptyParts).size();
		info.pieces = findChildNode(node, MESH_NODE_NAME)->getNumChildren();
		lines.append(info);
	}
	std::stable_sort(lines.begin(), lines.end(), [](const WireLineInfo &a, const WireLineInfo &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return lines;
}

bool WireBuilder::selectLine(int root_id)
{
	const NodePtr root = World::getNodeByID(root_id);
	if (!isNodeAlive(root) || !is_line_root(root))
		return false;

	setAddingPoles(false);
	setAddingPoints(false);
	set_line(root);
	Vector<NodePtr> nodes;
	nodes.append(root);
	::UnigineEditor::SelectionAction::applySelection(new SelectorNodes(nodes));
	report(MSG_INFO, QString());
	notify_changed();
	return true;
}

int WireBuilder::getLineId() const
{
	return hasLine() ? root_->getID() : 0;
}

bool WireBuilder::hasLine() const
{
	return isNodeAlive(root_) && mesh_node();
}

QString WireBuilder::getLineName() const
{
	return hasLine() ? QString::fromUtf8(root_->getName()) : QString();
}

int WireBuilder::getNumPoles() const
{
	if (!isCurveLine())
		return pole_ids().size();
	const NodePtr holder = poles_node();
	return settings_.pole_parts.isEmpty() || !holder ? getNumPoints() : holder->getNumChildren();
}

int WireBuilder::getNumPoints() const
{
	const NodePtr path = editor_.getPathNode();
	return path ? path->getNumChildren() : 0;
}

bool WireBuilder::isCurveLine() const
{
	return hasLine() && editor_.getPathNode() && poles_node();
}

int WireBuilder::getNumPieces() const
{
	const NodePtr mesh = mesh_node();
	return mesh ? mesh->getNumChildren() : 0;
}

double WireBuilder::getLength() const
{
	double length = 0.0;
	for (const WireSpan &wire : wires_)
		length += wire.length;
	return length;
}

NodePtr WireBuilder::mesh_node() const
{
	return root_ && !root_.isDeleted() ? findChildNode(root_, MESH_NODE_NAME) : NodePtr();
}

NodePtr WireBuilder::poles_node() const
{
	return root_ && !root_.isDeleted() ? findChildNode(root_, POLES_NODE_NAME) : NodePtr();
}

void WireBuilder::set_line(const NodePtr &root)
{
	if (hasLine() && root_->getID() == root->getID())
		return;
	setAddingPoles(false);
	setAddingPoints(false);
	show_hidden_pieces();
	root_ = root;
	load_settings();
	refresh_other_lines();
	// A line that connects poles of the world has no curve.
	editor_.setRoot(findChildNode(root_, CURVE_PATH_NODE_NAME) ? root_ : NodePtr());
	editor_.setClosed(settings_.closed);
	editor_.setSurfaceFilter(settings_.ground);
	state_.clear();
	poles_state_.clear();
	placed_points_.clear();
	remove_preview();
}

void WireBuilder::forget_line()
{
	root_ = NodePtr();
	poles_cached_ = false;
	editor_.setRoot(NodePtr());
	poles_state_.clear();
	placed_points_.clear();
	wires_.clear();
	wire_poles_.clear();
	cuts_.clear();
	triangles_ = 0;
	state_.clear();
	remove_preview();
}

QString WireBuilder::unique_line_name(const QString &base) const
{
	QSet<QString> names;
	for (const WireLineInfo &line : findLines())
		names.insert(line.name);
	QString name = base;
	for (int i = 2; names.contains(name); ++i)
		name = QString::fromUtf8("%1_%2").arg(base).arg(i);
	return name;
}

void WireBuilder::create_root(const WireSettings &start_settings, bool curve)
{
	setAddingPoles(false);
	setAddingPoints(false);

	NodeDummyPtr root = NodeDummy::create();
	root->setName(unique_line_name(QString::fromUtf8(ROOT_NODE_NAME)).toUtf8().constData());
	if (curve)
	{
		NodeDummyPtr path = NodeDummy::create();
		path->setName(CURVE_PATH_NODE_NAME);
		path->setWorldParent(root);
		NodeDummyPtr poles = NodeDummy::create();
		poles->setName(POLES_NODE_NAME);
		poles->setWorldParent(root);
	}
	NodeDummyPtr mesh = NodeDummy::create();
	mesh->setName(MESH_NODE_NAME);
	mesh->setWorldParent(root);
	if (const NodePtr parent = getParentNode())
		root->setWorldParent(parent);
	root->setShowInEditorEnabledRecursive(true);
	root->setSaveToWorldEnabledRecursive(true);
	// The settings are a part of making it, not a step of their own.
	root_ = root;
	settings_ = limited(start_settings);
	// Every new line its own random sags and turns.
	settings_.seed = randomSeed();
	settings_changed();
	settings_.folder.clear();
	save_settings(false);
	root_->setData(POLES_DATA, "");
	saved_poles_.clear();
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(root));
	editor_.setRoot(curve ? root_ : NodePtr());
	editor_.setClosed(settings_.closed);
	editor_.setSurfaceFilter(settings_.ground);
	state_.clear();
	poles_state_.clear();
	wires_.clear();

	Vector<NodePtr> selection;
	selection.append(root);
	::UnigineEditor::SelectionAction::applySelection(new SelectorNodes(selection));
}

void WireBuilder::createLine(const WireSettings &start_settings)
{
	create_root(start_settings, true);
	setAddingPoints(true);
	discard_if_empty_ = true;
	lines_changed();
	notify_changed();
}

void WireBuilder::createLinkLine(const WireSettings &start_settings)
{
	create_root(start_settings, false);
	setAddingPoles(true);
	discard_if_empty_ = true;
	lines_changed();
	notify_changed();
}

void WireBuilder::setAddingPoints(bool adding)
{
	if (adding && !isCurveLine())
	{
		report(MSG_WARNING, uiText("Create a line or select a line drawn as a curve first.",
			"Сначала создайте линию или выделите линию, нарисованную кривой."));
		adding = false;
	}
	if (editor_.isAddingPoints() == adding)
		return;
	editor_.setAddingPoints(adding);

	if (!adding)
		discard_empty_line(false);
	notify_changed();
}

bool WireBuilder::setParentNode(const NodePtr &node)
{
	if (node && find_line_root(node))
	{
		report(MSG_WARNING, uiText("A part of a line can not be the parent of lines: select another node.",
			"Часть линии не может быть родителем линий: выделите другую ноду."));
		return false;
	}
	parent_ = node;
	report(MSG_INFO, QString());
	notify_changed();
	return true;
}

NodePtr WireBuilder::getParentNode() const
{
	return isNodeAlive(parent_) ? parent_ : NodePtr();
}

bool WireBuilder::cloneLine()
{
	if (!hasLine())
		return false;
	setAddingPoles(false);
	setAddingPoints(false);
	show_hidden_pieces();

	// The points (or the poles of the world) and the settings; the poles of a curve are
	// made anew by the copy, the mesh by its Generate.
	const NodePtr source = root_;
	NodeDummyPtr root = NodeDummy::create();
	root->setName(unique_line_name(QString::fromUtf8(source->getName())).toUtf8().constData());
	root->setWorldTransform(source->getWorldTransform());
	for (const char *name : {SETTINGS_DATA, POLES_DATA})
	{
		if (const char *data = source->getData(name))
			root->setData(name, data);
	}
	if (const NodePtr source_path = editor_.getPathNode())
	{
		NodeDummyPtr path = NodeDummy::create();
		path->setName(CURVE_PATH_NODE_NAME);
		path->setWorldParent(root);
		path->setWorldTransform(source_path->getWorldTransform());
		for (int i = 0, num = source_path->getNumChildren(); i < num; ++i)
		{
			NodePtr point = source_path->getChild(i)->clone();
			point->setWorldParent(path);
			point->setWorldTransform(source_path->getChild(i)->getWorldTransform());
		}
		NodeDummyPtr poles = NodeDummy::create();
		poles->setName(POLES_NODE_NAME);
		poles->setWorldParent(root);
	}
	NodeDummyPtr mesh = NodeDummy::create();
	mesh->setName(MESH_NODE_NAME);
	mesh->setWorldParent(root);
	// Into the parent of the new lines, or next to the line.
	if (const NodePtr parent = getParentNode() ? getParentNode() : source->getParent())
		root->setWorldParent(parent);
	root->setShowInEditorEnabledRecursive(true);
	root->setSaveToWorldEnabledRecursive(true);
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(root));

	selectLine(root->getID());
	lines_changed();
	report(MSG_INFO, uiText("The line is copied to %1: its points and settings, no mesh yet.",
		"Линия скопирована в %1: точки и настройки, меша пока нет.").arg(getLineName()));
	return true;
}

bool WireBuilder::removeLineCurve()
{
	if (!hasLine())
		return false;
	setAddingPoles(false);
	setAddingPoints(false);
	show_hidden_pieces();

	// The poles and the pieces go to a node of the name of the line, where the line was;
	// the line itself with its curve is removed. One undo step.
	const NodePtr root = root_;
	Vector<NodePtr> kept;
	for (const NodePtr &holder : {poles_node(), mesh_node()})
	{
		for (int i = 0, num = holder ? holder->getNumChildren() : 0; i < num; ++i)
			kept.append(holder->getChild(i));
	}

	using namespace ::UnigineEditor;
	Undo::begin();
	if (!kept.empty())
	{
		NodeDummyPtr group = NodeDummy::create();
		group->setName(root->getName());
		if (const NodePtr parent = root->getParent())
			group->setWorldParent(parent);
		group->setWorldTransform(root->getWorldTransform());
		group->setShowInEditorEnabled(true);
		group->setSaveToWorldEnabled(true);
		Undo::apply(new CreateNodesAction(group));
		Undo::apply(new ReparentNodesAction(kept, group));
	}
	Undo::apply(new RemoveNodesAction(root));
	Undo::commit();

	forget_line();
	lines_changed();
	report(MSG_INFO, kept.empty() ? uiText("The line is removed; it had no poles or mesh.", "Линия удалена; столбов и меша у неё не было.")
		: uiText("The curve is removed; the poles and the mesh stay as ordinary nodes.",
			"Кривая удалена; столбы и меш остались обычными нодами."));
	notify_changed();
	return true;
}

bool WireBuilder::removeLine(bool files)
{
	if (!hasLine())
		return false;
	setAddingPoles(false);
	setAddingPoints(false);
	show_hidden_pieces();

	const NodePtr root = root_;
	// Only its own files, and not those something else in the world still has as its mesh.
	const QStringList generated = files ? ownGeneratedFiles(root, line_generated_data()) : QStringList();
	::UnigineEditor::Undo::apply(new ::UnigineEditor::RemoveNodesAction(root));
	forget_line();

	int removed = 0;
	if (files)
	{
		for (const QString &file : generated)
		{
			if (AssetManager::isAsset(file.toUtf8().constData()) && AssetManager::removeAssetSync(file.toUtf8().constData()))
				++removed;
		}
	}
	lines_changed();
	report(MSG_INFO, files ? uiText("The line is removed with its mesh and %1 mesh files.", "Линия удалена вместе с мешем и файлами мешей: %1.").arg(removed)
		: uiText("The line is removed with its mesh.", "Линия удалена вместе с мешем."));
	notify_changed();
	return true;
}

////////////////////////////////////////////////////////////////////////////////
// Poles.
////////////////////////////////////////////////////////////////////////////////
// A line made by New Line is not kept if adding points (or poles) to it is over before it
// got a single one.
void WireBuilder::discard_empty_line(bool poles)
{
	if (!discard_if_empty_)
		return;
	discard_if_empty_ = false;
	if (!hasLine() || (poles ? getNumPoles() : getNumPoints()) != 0)
		return;
	const NodePtr root = root_;
	forget_line();
	::UnigineEditor::Undo::apply(new ::UnigineEditor::RemoveNodesAction(root));
	lines_changed();
	report(MSG_INFO, poles ? uiText("The new line got no poles and was removed.", "У новой линии не появилось ни одного столба - она удалена.")
		: uiText("The new line got no points and was removed.", "У новой линии не появилось ни одной точки - она удалена."));
}

void WireBuilder::setAddingPoles(bool adding)
{
	if (adding && (!hasLine() || isCurveLine()))
	{
		report(MSG_WARNING, uiText("Create a line of poles or select an existing one first.",
			"Сначала создайте линию из столбов или выделите существующую."));
		adding = false;
	}
	if (adding_poles_ == adding)
		return;
	adding_poles_ = adding;

	// What is selected now is not a pole picked: only what is selected after it.
	selected_before_.clear();
	if (const SelectorNodes *selector = Selection::getSelectorNodes())
	{
		for (const NodePtr &node : selector->getNodes())
		{
			if (node)
				selected_before_.insert(node->getID());
		}
	}
	recent_selections_.clear();
	remember_selection(selected_before_);

	if (!adding)
		discard_empty_line(true);
	notify_changed();
}

bool WireBuilder::addSelectedPoles()
{
	if (!hasLine() || isCurveLine())
	{
		report(MSG_WARNING, uiText("Create a line of poles or select an existing one first.",
			"Сначала создайте линию из столбов или выделите существующую."));
		return false;
	}
	QVector<int> ids = pole_ids();
	const int before = ids.size();
	QStringList skipped;
	if (const SelectorNodes *selector = Selection::getSelectorNodes())
	{
		for (const NodePtr &node : selector->getNodes())
		{
			const NodePtr pole = pole_of(node);
			if (!pole && isNodeAlive(node) && !find_line_root(node))
				skipped.append(QString::fromUtf8(node->getName()));
			if (pole && (ids.isEmpty() || ids.last() != pole->getID()))
				ids.append(pole->getID());
		}
	}
	if (ids.size() == before)
	{
		if (!skipped.isEmpty())
			report_skipped(skipped);
		else
		{
			report(MSG_WARNING, uiText("Select the poles in the viewport or in the World Hierarchy first.",
				"Сначала выделите столбы во вьюпорте или в World Hierarchy."));
		}
		return false;
	}
	set_pole_ids(ids);
	report(MSG_INFO, QString());
	report_skipped(skipped);
	return true;
}

void WireBuilder::removeLastPole()
{
	QVector<int> ids = pole_ids();
	if (ids.isEmpty())
		return;
	ids.removeLast();
	set_pole_ids(ids);
}

void WireBuilder::clearPoles()
{
	if (!pole_ids().isEmpty())
		set_pole_ids({});
}

bool WireBuilder::addSelectedPoleParts()
{
	if (!isCurveLine())
	{
		report(MSG_WARNING, uiText("Create a line or select a line drawn as a curve first.",
			"Сначала создайте линию или выделите линию, нарисованную кривой."));
		return false;
	}
	const QVector<PlaceableAsset> assets = resolveSelectedAssets();
	if (assets.isEmpty())
	{
		report(MSG_WARNING, uiText("Select the assets of the poles in the Asset Browser first: .node, .mesh or models.",
			"Сначала выделите ассеты столбов в Asset Browser: .node, .mesh или модели."));
		return false;
	}
	WireSettings settings = settings_;
	for (const PlaceableAsset &asset : assets)
	{
		WirePolePart part;
		part.source = QString::fromUtf8(asset.path.get());
		part.source_kind = int(asset.kind);
		part.source_name = QFileInfo(asset.name).completeBaseName();
		settings.pole_parts.append(part);
	}
	setSettings(settings);
	report(MSG_INFO, QString());
	return true;
}

bool WireBuilder::replacePolePart(int part, const Vector<UGUID> &guids)
{
	if (part < 0 || part >= settings_.pole_parts.size())
		return false;
	PlaceableAsset asset;
	if (!resolveSelection(guids, asset))
	{
		report(MSG_WARNING, uiText("That is not a pole: pick a .node, a .mesh or a model.", "Это не столб: выберите .node, .mesh или модель."));
		return false;
	}
	WireSettings settings = settings_;
	settings.pole_parts[part].source = QString::fromUtf8(asset.path.get());
	settings.pole_parts[part].source_kind = int(asset.kind);
	settings.pole_parts[part].source_name = QFileInfo(asset.name).completeBaseName();
	setSettings(settings);
	report(MSG_INFO, QString());
	return true;
}

void WireBuilder::removePolePart(int part)
{
	WireSettings settings = settings_;
	if (part < 0 || part >= settings.pole_parts.size())
		return;
	settings.pole_parts.removeAt(part);
	setSettings(settings);
}

void WireBuilder::setPolePartRole(int part, WirePoleRole role)
{
	WireSettings settings = settings_;
	if (part < 0 || part >= settings.pole_parts.size())
		return;
	settings.pole_parts[part].role = role;
	setSettings(settings);
}

void WireBuilder::clearPoleParts()
{
	WireSettings settings = settings_;
	settings.pole_parts.clear();
	setSettings(settings);
}

PlaceableAsset WireBuilder::asset_of(const WirePolePart &part)
{
	PlaceableAsset asset;
	asset.kind = PlaceableAsset::Kind(part.source_kind);
	asset.path = String(part.source.toUtf8().constData());
	asset.name = part.source_name;
	return asset;
}

PlaceableAsset WireBuilder::mesh_asset() const
{
	PlaceableAsset asset;
	asset.kind = PlaceableAsset::Kind(settings_.mesh_kind);
	asset.path = String(settings_.mesh_source.toUtf8().constData());
	asset.name = settings_.mesh_name;
	return asset;
}

bool WireBuilder::showPolePart(int part) const
{
	if (part < 0 || part >= settings_.pole_parts.size())
		return false;
	return showInAssetBrowser(asset_of(settings_.pole_parts[part]));
}

void WireBuilder::resetPoleTurns()
{
	const NodePtr holder = isCurveLine() ? poles_node() : NodePtr();
	const NodePtr path = editor_.getPathNode();
	if (!holder || !path)
		return;
	// No turn, every point looking along the curve again; one step of the undo history:
	// Ctrl+Z gives the points their turns back, and the poles follow them.
	const CurvePoints points = editor_.getPoints();
	SetNodeDataAction *data = new SetNodeDataAction;
	Vector<NodePtr> turned;
	Vector<Math::Mat4> straight;
	for (int i = 0; i < points.count() && i < path->getNumChildren(); ++i)
	{
		const NodePtr point = path->getChild(i);
		data->set(point, POINT_TURN_DATA, QByteArray());
		const Math::quat basis(Math::mat3(curvePointBasis(points.direction(i), 0.0)));
		const Math::Mat4 target = Math::translate(point->getWorldPosition()) * Math::Mat4(Math::rotate(basis))
			* Math::Mat4(Math::scale(point->getWorldScale()));
		if (!same_rotation(rotation_of(point->getWorldTransform()), basis))
		{
			turned.append(point);
			straight.append(target);
		}
	}
	::UnigineEditor::Undo::begin();
	SetNodeDataAction::push(data, false);
	for (int i = 0; i < turned.size(); ++i)
		::UnigineEditor::Undo::apply(new ::UnigineEditor::SetNodeTransformAction(turned[i], straight[i]));
	::UnigineEditor::Undo::commit();
	// The rotations they have now are not taken for new turns.
	for (int i = 0, num = path->getNumChildren(); i < num; ++i)
		placed_points_.insert(path->getChild(i)->getID(), rotation_of(path->getChild(i)->getWorldTransform()));
	poles_state_.clear();
	notify_changed();
}

bool WireBuilder::hasPolesWithoutPoints() const
{
	if (!isCurveLine() || !has_pole_assets())
		return false;
	for (const Pole &pole : poles())
	{
		if (pole.points.isEmpty() && !find_tie(pole.node))
			return true;
	}
	return false;
}

void WireBuilder::report_skipped(const QStringList &names) const
{
	if (names.isEmpty())
		return;
	report(MSG_WARNING, uiText("Not a pole - no attach points %1_1, %1_2... in it: %2",
		"Не столб - в нём нет точек крепления %1_1, %1_2...: %2").arg(settings_.attach_name, names.join(QString::fromUtf8(", "))));
}

QVector<int> WireBuilder::pole_ids() const
{
	QVector<int> ids;
	if (!hasLine())
		return ids;
	for (const QString &id : dataString(root_, POLES_DATA).split(QLatin1Char(' '), Qt::SkipEmptyParts))
		ids.append(id.toInt());
	return ids;
}

void WireBuilder::set_pole_ids(const QVector<int> &ids, bool undoable)
{
	poles_cached_ = false;
	if (!hasLine())
		return;
	QStringList list;
	for (int id : ids)
		list.append(QString::number(id));
	const QByteArray value = list.join(QLatin1Char(' ')).toUtf8();
	saved_poles_ = value;
	if (undoable)
	{
		SetNodeDataAction *action = new SetNodeDataAction;
		action->set(root_, POLES_DATA, value);
		SetNodeDataAction::push(action, false);
	} else
		root_->setData(POLES_DATA, value.constData());
	notify_changed();
}

NodePtr WireBuilder::pole_of(const NodePtr &node) const
{
	if (!isNodeAlive(node) || isEditorInternal(node) || find_line_root(node))
		return NodePtr();
	// A mesh of a pole made of several nodes is picked in the viewport: the pole is the
	// node over it that has the attach points. Two levels up only a node that has them as
	// its own children: a node that holds many things, one of them a pole, is not the
	// pole. A node with none is not a pole.
	NodePtr n = node;
	for (int level = 0; n && level <= MAX_POLE_LEVELS; ++level, n = n->getParent())
	{
		if (level < 2 ? !attach_points(n).isEmpty() : has_own_attach_point(n))
			return n;
	}
	return NodePtr();
}

bool WireBuilder::has_own_attach_point(const NodePtr &node) const
{
	if (!node)
		return false;
	const QRegularExpression &pattern = attach_pattern();
	NodePtr content;
	if (node->getType() == Node::NODE_REFERENCE)
		content = checked_ptr_cast<NodeReference>(node)->getReference();
	for (const NodePtr &n : {node, content})
	{
		for (int i = 0, num = n ? n->getNumChildren() : 0; i < num; ++i)
		{
			if (pattern.match(QString::fromUtf8(n->getChild(i)->getName())).hasMatch())
				return true;
		}
	}
	return false;
}

// "wire_1", "wire1", "Wire.01", "wire-1": the name, maybe a separator, a number; the tie
// of an insulator also without the number. Made once for a name: they are asked for
// pole by pole, frame by frame.
const QRegularExpression &WireBuilder::attach_pattern() const
{
	if (patterns_name_ != settings_.attach_name || !attach_pattern_.isValid())
	{
		patterns_name_ = settings_.attach_name;
		const QString name = QRegularExpression::escape(settings_.attach_name);
		attach_pattern_ = QRegularExpression(QString::fromUtf8("^%1[ _.\\-]?(\\d+)$").arg(name), QRegularExpression::CaseInsensitiveOption);
		tie_pattern_ = QRegularExpression(QString::fromUtf8("^%1([ _.\\-]?\\d+)?$").arg(name), QRegularExpression::CaseInsensitiveOption);
	}
	return attach_pattern_;
}

const QRegularExpression &WireBuilder::tie_pattern() const
{
	attach_pattern();
	return tie_pattern_;
}

QVector<WireBuilder::AttachPoint> WireBuilder::attach_points(const NodePtr &pole, bool own) const
{
	QVector<AttachPoint> points;
	if (!pole)
		return points;
	const QRegularExpression &pattern = attach_pattern();

	QSet<int> numbers;
	QVector<NodePtr> insulators;
	const std::function<void(const NodePtr &, int)> visit = [&](const NodePtr &node, int depth) {
		if (!node || depth > MAX_ATTACH_DEPTH)
			return;
		// The points of an insulator on the pole are not those of the pole; a spare one is
		// not there at all.
		if (depth > 0 && node->hasData(INSULATOR_NUMBER_DATA))
		{
			if (dataString(node, INSULATOR_SPARE_DATA) != QLatin1String("1"))
				insulators.append(node);
			return;
		}
		if (depth > 0)
		{
			const QRegularExpressionMatch match = pattern.match(QString::fromUtf8(node->getName()));
			if (match.hasMatch())
			{
				const int number = match.captured(1).toInt();
				if (!numbers.contains(number))
				{
					numbers.insert(number);
					points.append(AttachPoint{number, node->getWorldPosition(), rotation_of(node->getWorldTransform())});
				}
			}
		}
		// The content of a node reference is inside it too.
		if (node->getType() == Node::NODE_REFERENCE)
			visit(checked_ptr_cast<NodeReference>(node)->getReference(), depth + 1);
		for (int i = 0, num = node->getNumChildren(); i < num; ++i)
			visit(node->getChild(i), depth + 1);
	};
	visit(pole, 0);
	std::sort(points.begin(), points.end(), [](const AttachPoint &a, const AttachPoint &b) { return a.number < b.number; });
	if (own)
		return points;

	// The wire is tied to the insulator: to its tie node, none - to the insulator itself.
	for (const NodePtr &insulator : insulators)
	{
		const int number = dataString(insulator, INSULATOR_NUMBER_DATA).toInt();
		const Math::Vec3 position = pole_pivot(insulator);
		for (AttachPoint &point : points)
		{
			if (point.number == number)
				point.position = position;
		}
	}
	return points;
}

NodePtr WireBuilder::find_tie(const NodePtr &node) const
{
	const QRegularExpression &pattern = tie_pattern();
	NodePtr tie;
	const std::function<void(const NodePtr &, int)> visit = [&](const NodePtr &n, int depth) {
		if (!n || tie || depth > MAX_ATTACH_DEPTH)
			return;
		if (depth > 0 && pattern.match(QString::fromUtf8(n->getName())).hasMatch())
		{
			tie = n;
			return;
		}
		if (n->getType() == Node::NODE_REFERENCE)
			visit(checked_ptr_cast<NodeReference>(n)->getReference(), depth + 1);
		for (int i = 0, num = n->getNumChildren(); i < num; ++i)
			visit(n->getChild(i), depth + 1);
	};
	visit(node, 0);
	return tie;
}

Math::Vec3 WireBuilder::pole_pivot(const NodePtr &pole) const
{
	const NodePtr tie = find_tie(pole);
	return (tie ? tie : pole)->getWorldPosition();
}

bool WireBuilder::has_pole_assets() const
{
	for (const WirePolePart &part : settings_.pole_parts)
	{
		if (part.role != WIRE_POLE_INSULATOR)
			return true;
	}
	return false;
}

void WireBuilder::sync_insulators(const NodePtr &pole, quint64 key)
{
	// Only insulators in the list: they are the poles themselves.
	QVector<int> parts;
	for (int k = 0; k < settings_.pole_parts.size() && has_pole_assets(); ++k)
	{
		if (settings_.pole_parts[k].role == WIRE_POLE_INSULATOR)
			parts.append(k);
	}

	// The insulators the pole has, by the number of their point, and the spare ones.
	QHash<int, NodePtr> standing;
	QVector<NodePtr> spares;
	for (int i = 0, num = pole->getNumChildren(); i < num; ++i)
	{
		const NodePtr child = pole->getChild(i);
		if (!child->hasData(INSULATOR_NUMBER_DATA))
			continue;
		if (dataString(child, INSULATOR_SPARE_DATA) == QLatin1String("1"))
			spares.append(child);
		else
			standing.insert(dataString(child, INSULATOR_NUMBER_DATA).toInt(), child);
	}
	// Off and out of sight, not removed: what was set on it by hand comes back with it.
	const auto retire = [&spares](const NodePtr &insulator) {
		if (const SelectorNodes *selector = Selection::getSelectorNodes())
		{
			for (const NodePtr &node : selector->getNodes())
			{
				if (node && node->getID() == insulator->getID())
				{
					Selection::clear();
					break;
				}
			}
		}
		insulator->setEnabled(false);
		insulator->setShowInEditorEnabled(false);
		insulator->setSaveToWorldEnabled(false);
		insulator->setData(INSULATOR_SPARE_DATA, "1");
		spares.append(insulator);
	};

	if (!parts.isEmpty())
	{
		for (const AttachPoint &point : attach_points(pole, true))
		{
			// Several assets: one of them for every point, at random by the seed.
			const int pick = parts.size() == 1 ? 0
				: qMin(int(parts.size()) - 1, int(random_of(quint64(settings_.seed), key, quint64(point.number), 0x1D5) * float(parts.size())));
			const WirePolePart &part = settings_.pole_parts[parts[pick]];
			NodePtr insulator = standing.take(point.number);
			if (insulator && dataString(insulator, INSULATOR_SOURCE_DATA) != part.source)
			{
				retire(insulator);
				insulator = NodePtr();
			}
			// A spare one of this point and asset.
			for (int k = 0; !insulator && k < spares.size(); ++k)
			{
				if (dataString(spares[k], INSULATOR_NUMBER_DATA).toInt() == point.number
					&& dataString(spares[k], INSULATOR_SOURCE_DATA) == part.source)
				{
					insulator = spares.takeAt(k);
					insulator->setEnabled(true);
					insulator->setShowInEditorEnabled(true);
					insulator->setSaveToWorldEnabled(true);
					insulator->setData(INSULATOR_SPARE_DATA, "");
				}
			}
			if (!insulator)
			{
				insulator = createNode(asset_of(part));
				if (!insulator)
					continue;
				insulator->setData(INSULATOR_SOURCE_DATA, part.source.toUtf8().constData());
				insulator->setData(INSULATOR_NUMBER_DATA, QByteArray::number(point.number).constData());
				insulator->setWorldParent(pole);
				insulator->setShowInEditorEnabledRecursive(true);
				insulator->setSaveToWorldEnabledRecursive(true);
			}
			// Turned as the attach point is.
			const Math::Mat4 transform = Math::translate(point.position) * Math::Mat4(Math::rotate(point.rotation));
			if (insulator->getWorldTransform() != transform)
				insulator->setWorldTransform(transform);
		}
	}

	// The insulators of the points that are gone are kept spare.
	for (auto it = standing.begin(); it != standing.end(); ++it)
		retire(it.value());
}

QVector<WireBuilder::Pole> WireBuilder::poles() const
{
	// Asked for several times a frame (the wires, the drawing, the page): found once.
	if (!poles_cached_)
	{
		poles_cache_ = compute_poles();
		poles_cached_ = true;
	}
	return poles_cache_;
}

QVector<WireBuilder::Pole> WireBuilder::compute_poles() const
{
	QVector<Pole> result;
	if (!hasLine())
		return result;

	if (isCurveLine())
	{
		// No assets of the poles: the wires hang from the places of the poles themselves -
		// a garland between the points.
		if (settings_.pole_parts.isEmpty())
		{
			for (int i = 0; i < slots_.size(); ++i)
				result.append(Pole{NodePtr(), i + 1, slots_[i].position, {}, slots_[i].span});
			return result;
		}
		const NodePtr holder = poles_node();
		for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
		{
			const NodePtr pole = holder->getChild(i);
			const QVector<AttachPoint> points = attach_points(pole);
			result.append(Pole{pole, pole->getID(), points.isEmpty() ? pole_pivot(pole) : pole->getWorldPosition(), points,
				i < pole_spans_.size() ? pole_spans_[i] : -1});
		}
		return result;
	}

	// The poles of the world that are still there.
	for (int id : pole_ids())
	{
		const NodePtr pole = World::getNodeByID(id);
		if (isNodeAlive(pole))
			result.append(Pole{pole, id, pole->getWorldPosition(), attach_points(pole)});
	}
	return result;
}

QVector<WireBuilder::PoleSlot> WireBuilder::pole_slots() const
{
	QVector<PoleSlot> places;
	const CurvePoints points = editor_.getPoints();
	const int count = points.count();
	if (count == 0)
		return places;

	// A pole at every point; with a spacing, also between the points, as many as fit the
	// span best, evenly along it.
	QVector<bool> corners;
	for (int i = 0; i < count && places.size() < MAX_POLES; ++i)
	{
		places.append(PoleSlot{points.positions[i], points.direction(i), -1, i, i});
		corners.append(points.corners[i]);
		if (!settings_.pole_every || i >= points.spans())
			continue;

		const int samples = 64;
		QVector<Math::Vec3> along;
		QVector<double> lengths;
		for (int k = 0; k <= samples; ++k)
		{
			along.append(points.spanPoint(i, double(k) / double(samples)));
			lengths.append(k == 0 ? 0.0 : lengths.last() + double(Math::length(along[k] - along[k - 1])));
		}
		const int parts = qMax(1, int(std::lround(lengths.last() / double(settings_.pole_spacing))));
		for (int j = 1; j < parts && places.size() < MAX_POLES; ++j)
		{
			const double target = lengths.last() * double(j) / double(parts);
			int k = int(std::upper_bound(lengths.begin(), lengths.end(), target) - lengths.begin()) - 1;
			k = qBound(0, k, samples - 1);
			const double piece = lengths[k + 1] - lengths[k];
			const double t = piece > 1e-12 ? (target - lengths[k]) / piece : 0.0;
			places.append(PoleSlot{along[k] + (along[k + 1] - along[k]) * Math::Scalar(t), unitOrZero(along[k + 1] - along[k]), -1, -1, i});
			corners.append(false);
		}
	}

	// The asset of every pole: the start one first, the end one last (an open line), the
	// corner one at the corner points, the pattern ones in turn everywhere else. A role
	// with no asset of its own takes the pattern.
	const QVector<WirePolePart> &parts = settings_.pole_parts;
	if (parts.isEmpty())
		return places;
	QVector<int> by_role[POLE_ROLES];
	for (int k = 0; k < parts.size(); ++k)
		by_role[parts[k].role].append(k);
	if (by_role[WIRE_POLE_PATTERN].isEmpty())
	{
		for (int k = 0; k < parts.size() && by_role[WIRE_POLE_PATTERN].isEmpty(); ++k)
		{
			if (parts[k].role != WIRE_POLE_INSULATOR)
				by_role[WIRE_POLE_PATTERN].append(k);
		}
	}
	// Only insulators: they stand at the points of the curve, in turn.
	if (by_role[WIRE_POLE_PATTERN].isEmpty())
		by_role[WIRE_POLE_PATTERN] = by_role[WIRE_POLE_INSULATOR];
	int next_pattern = 0;
	for (int s = 0; s < places.size(); ++s)
	{
		WirePoleRole role = WIRE_POLE_PATTERN;
		if (!points.closed && s == 0 && !by_role[WIRE_POLE_START].isEmpty())
			role = WIRE_POLE_START;
		else if (!points.closed && s == places.size() - 1 && !by_role[WIRE_POLE_END].isEmpty())
			role = WIRE_POLE_END;
		else if (corners[s] && !by_role[WIRE_POLE_CORNER].isEmpty())
			role = WIRE_POLE_CORNER;
		if (role == WIRE_POLE_PATTERN)
			places[s].part = by_role[role][next_pattern++ % by_role[role].size()];
		else
			places[s].part = by_role[role].first();
		// The poles stand on the ground under the curve; insulators that stand instead of
		// poles (on a wall), at the points themselves.
		if (settings_.pole_on_ground && has_pole_assets())
			places[s].position = on_ground(places[s].position);
	}
	return places;
}

Math::Vec3 WireBuilder::on_ground(const Math::Vec3 &position) const
{
	// On a grid of 5 cm (and half a unit up): the poles that move with a dragged point
	// find most of the heights found before.
	const QPair<QPair<qint64, qint64>, qint64> key(
		QPair<qint64, qint64>(qint64(std::llround(double(position.x) * 20.0)), qint64(std::llround(double(position.y) * 20.0))),
		qint64(std::llround(double(position.z) * 2.0)));
	auto it = pole_ground_.constFind(key);
	if (it == pole_ground_.constEnd())
	{
		if (pole_ground_.size() > 1000000)
			pole_ground_.clear();
		SurfaceHit hit = raycastSurface(position + Math::Vec3(0.0, 0.0, GROUND_UP), position - Math::Vec3(0.0, 0.0, GROUND_DOWN),
			settings_.ground, ground_ignore());
		// The curve under a hill (between points in two valleys): the ray starts inside
		// the ground and finds nothing; again from high above.
		if (!hit.found)
			hit = raycastSurface(position + Math::Vec3(0.0, 0.0, GROUND_FAR_UP), position - Math::Vec3(0.0, 0.0, GROUND_DOWN),
				settings_.ground, ground_ignore());
		it = pole_ground_.insert(key, hit.found ? double(hit.point.z) : NO_GROUND);
	}
	return no_ground(it.value()) ? position : Math::Vec3(position.x, position.y, Math::Scalar(it.value()));
}

const Vector<NodePtr> &WireBuilder::ground_ignore() const
{
	// What the rays to the ground look through: the line itself (its poles, its wires)
	// and the preview; the poles of the world a line of poles is strung on (their
	// crossarms, their insulators); the other lines (their wires are not ground). Found
	// once a frame.
	if (ground_ignore_frame_ == frame_ && !ground_ignore_.empty())
		return ground_ignore_;
	ground_ignore_frame_ = frame_;
	ground_ignore_.clear();
	ground_ignore_.append(root_);
	if (preview_ && !preview_.isDeleted())
		ground_ignore_.append(preview_);
	if (!isCurveLine())
	{
		for (int id : pole_ids())
		{
			if (const NodePtr pole = World::getNodeByID(id))
				ground_ignore_.append(pole);
		}
	}
	for (int id : other_line_ids_)
	{
		if (const NodePtr other = World::getNodeByID(id))
			ground_ignore_.append(other);
	}
	return ground_ignore_;
}

void WireBuilder::refresh_other_lines()
{
	other_line_ids_.clear();
	for (const WireLineInfo &line : findLines())
	{
		if (!root_ || root_.isDeleted() || line.id != root_->getID())
			other_line_ids_.append(line.id);
	}
}

void WireBuilder::lines_changed()
{
	refresh_other_lines();
	if (on_lines_changed)
		on_lines_changed();
}

void WireBuilder::keep_above_ground(WireSpan &span) const
{
	// The rays start above the higher end of the wire: the wire is below it everywhere.
	const double top = qMax(double(span.from.z), double(span.to.z)) + GROUND_UP;
	const Vector<NodePtr> &ignore = ground_ignore();
	// Over the ground by half the thickness (of this span), so that a lying wire is not cut
	// by it.
	const double clearance = qMax(double(settings_.thickness) * double(span.thickness_scale) * 0.5, 0.01);
	if (ground_heights_.size() > 1000000)
		ground_heights_.clear();

	// The height of the ground under a place; NO_GROUND - none. On a grid of 5 cm, and of half a
	// unit for where the rays start: the wires that move with a dragged pole find most
	// heights there.
	const qint64 top_key = qint64(std::llround(top * 2.0));
	const auto ground_at = [&](const Math::Vec3 &place) {
		const QPair<QPair<qint64, qint64>, qint64> key(
			QPair<qint64, qint64>(qint64(std::llround(double(place.x) * 20.0)), qint64(std::llround(double(place.y) * 20.0))), top_key);
		auto it = ground_heights_.constFind(key);
		if (it == ground_heights_.constEnd())
		{
			SurfaceHit hit = raycastSurface(Math::Vec3(place.x, place.y, Math::Scalar(top)),
				Math::Vec3(place.x, place.y, Math::Scalar(top - GROUND_DOWN - 1000.0)), settings_.ground, ignore);
			// A crest higher than both ends of the wire: the ray starts inside it.
			if (!hit.found)
				hit = raycastSurface(Math::Vec3(place.x, place.y, Math::Scalar(top + GROUND_FAR_UP)),
					Math::Vec3(place.x, place.y, Math::Scalar(top - GROUND_DOWN - 1000.0)), settings_.ground, ignore);
			it = ground_heights_.insert(key, hit.found ? double(hit.point.z) : NO_GROUND);
		}
		return it.value();
	};

	bool moved = false;
	QVector<bool> lying(span.samples.size(), false);
	for (int i = 1; i + 1 < span.samples.size(); ++i)
	{
		Math::Vec3 &sample = span.samples[i];
		const double ground = ground_at(sample);
		if (!no_ground(ground) && double(sample.z) < ground + clearance)
		{
			sample.z = Math::Scalar(ground + clearance);
			moved = true;
			lying[i] = true;
		}
	}
	// A straight segment between two samples may still go through the ground (over a crest):
	// it gets a sample in its middle, on the ground, again and again while it is long enough.
	const double shortest = qMax(0.25, double(settings_.thickness) * 4.0);
	for (int i = 0; i + 1 < span.samples.size() && span.samples.size() < MAX_GROUND_SAMPLES;)
	{
		const Math::Vec3 middle = (span.samples[i] + span.samples[i + 1]) * Math::Scalar(0.5);
		const double ground = double(Math::length(span.samples[i + 1] - span.samples[i])) > shortest ? ground_at(middle)
			: NO_GROUND;
		if (no_ground(ground) || double(middle.z) >= ground + clearance)
		{
			++i;
			continue;
		}
		span.samples.insert(i + 1, Math::Vec3(middle.x, middle.y, Math::Scalar(ground + clearance)));
		lying.insert(i + 1, true);
		moved = true;
	}
	if (!moved)
		return;
	measureWire(span);
	// Where it lies, by the distances along the wire measured anew: it does not sway there.
	span.rests.clear();
	for (int i = 0; i < lying.size(); ++i)
	{
		if (!lying[i])
			continue;
		int last = i;
		while (last + 1 < lying.size() && lying[last + 1])
			++last;
		span.rests.append(qMakePair(span.distances[i], span.distances[last]));
		i = last;
	}
}

QByteArray WireBuilder::pole_snapshot() const
{
	QByteArray state;
	putBytes(state, double(isOldCurve()));
	putBytes(state, double(settings_.closed));
	putBytes(state, double(settings_.pole_every));
	putBytes(state, double(settings_.pole_spacing));
	putBytes(state, double(settings_.pole_forward));
	putBytes(state, double(settings_.pole_up));
	putBytes(state, double(settings_.pole_on_ground));
	putBytes(state, double(settings_.ground.surface_types));
	putBytes(state, double(settings_.ground.only_immovable));
	putBytes(state, double(settings_.ground.only_intersection));
	putBytes(state, double(settings_.seed));
	state.append(settings_.attach_name.toUtf8());
	for (const WirePolePart &part : settings_.pole_parts)
	{
		state.append(part.source.toUtf8());
		putBytes(state, double(part.role));
	}
	if (const NodePtr path = editor_.getPathNode())
	{
		for (int i = 0, num = path->getNumChildren(); i < num; ++i)
		{
			// Its turn too: the pole there takes it.
			putBytes(state, path->getChild(i)->getWorldTransform());
			putBytes(state, double(isCornerPoint(path->getChild(i))));
		}
	}
	// A pole moved by hand or removed is put back.
	if (const NodePtr holder = poles_node())
	{
		for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
		{
			putBytes(state, double(holder->getChild(i)->getID()));
			putBytes(state, holder->getChild(i)->getWorldTransform());
		}
	}
	return state;
}

void WireBuilder::sync_poles()
{
	const NodePtr holder = poles_node();
	if (!holder)
		return;
	slots_ = pole_slots();

	const NodePtr path = editor_.getPathNode();
	// The axes of the assets of the poles to those of a point of the curve: forward to its
	// Y (along the curve), up to its Z (up), and the third one to its X.
	const Math::vec3 forward = axis_vector(settings_.pole_forward);
	const Math::vec3 up = axis_vector(settings_.pole_up);
	Math::mat3 asset_axes;
	asset_axes.setRow(0, Math::cross(forward, up));
	asset_axes.setRow(1, forward);
	asset_axes.setRow(2, up);
	const Math::mat3 pole_axes = asset_axes;

	// The poles that are there, by their assets, in order: a pole keeps its node while
	// its asset stays the same, so what is set on it by hand is not lost every time.
	QHash<QString, QVector<NodePtr>> unused;
	for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
	{
		const NodePtr pole = holder->getChild(i);
		// Only the poles the tool made (they know their asset): a node the user put among
		// them is left where it is, not put away as a spare (spares are not saved).
		const QString source = dataString(pole, POLE_SOURCE_DATA);
		if (!source.isEmpty())
			unused[source].append(pole);
	}
	// Then the spare ones: a pole that comes back is the one that was there. They are a
	// stack - the last one put away comes back first (see below how they are put away).
	NodePtr spare = findChildNode(root_, SPARE_POLES_NODE_NAME);
	QSet<int> spare_ids;
	if (spare)
	{
		for (int i = spare->getNumChildren() - 1; i >= 0; --i)
		{
			const NodePtr pole = spare->getChild(i);
			unused[dataString(pole, POLE_SOURCE_DATA)].append(pole);
			spare_ids.insert(pole->getID());
		}
	}

	// The insulators of every pole are put anew when what they are made of changes; else
	// only those of a pole that moved or is new.
	QByteArray insulator_signature;
	putBytes(insulator_signature, double(settings_.seed));
	insulator_signature.append(settings_.attach_name.toUtf8());
	for (const WirePolePart &part : settings_.pole_parts)
	{
		insulator_signature.append(part.source.toUtf8());
		putBytes(insulator_signature, double(part.role));
	}
	const bool insulators_all = insulator_signature != insulator_signature_;
	insulator_signature_ = insulator_signature;

	QVector<NodePtr> ordered;
	pole_spans_.clear();
	bool missing = false;
	// The poles in the same span so far: with the point of the span, a number of the pole
	// that stays when poles are added or removed elsewhere on the line.
	int in_span = 0;
	for (int s = 0; s < slots_.size(); ++s)
	{
		const PoleSlot &slot = slots_[s];
		in_span = s > 0 && slots_[s - 1].span == slot.span ? in_span + 1 : 0;
		if (slot.part < 0)
			continue;
		const WirePolePart &part = settings_.pole_parts[slot.part];
		NodePtr pole;
		bool created = false;
		QVector<NodePtr> &same = unused[part.source];
		if (!same.isEmpty())
			pole = same.takeFirst();
		else
		{
			created = true;
			pole = createNode(asset_of(part));
			if (!pole)
			{
				missing = true;
				continue;
			}
			pole->setData(POLE_SOURCE_DATA, part.source.toUtf8().constData());
			pole->setWorldParent(holder);
			pole->setShowInEditorEnabledRecursive(true);
			pole->setSaveToWorldEnabledRecursive(true);
		}

		if (!pole->getParent() || pole->getParent()->getID() != holder->getID())
			pole->setWorldParent(holder);
		ordered.append(pole);
		pole_spans_.append(slot.span);

		// Turned so that its forward axis goes along the line and its up axis up. The pole
		// at a point takes the turn the point is given with the manipulator on top of
		// looking along the curve, as the point of a road does: a pole turned, leaning, a
		// broken one. The point as it is now: the pole follows it while it is being turned.
		Math::quat rotation(Math::mat3(curvePointBasis(slot.direction, 0.0)) * pole_axes);
		if (path && slot.point >= 0 && slot.point < path->getNumChildren())
			rotation = rotation_of(path->getChild(slot.point)->getWorldTransform()) * Math::quat(pole_axes);

		// The scale the pole has, rounded: read back from a turned transform it is a little
		// off every time, and would creep away.
		Math::vec3 scale = pole->getWorldScale();
		for (int axis = 0; axis < 3; ++axis)
			scale[axis] = float(std::round(double(scale[axis]) * 10000.0) / 10000.0);
		const Math::Mat4 transform = Math::translate(slot.position) * Math::Mat4(Math::rotate(rotation))
			* Math::Mat4(Math::scale(scale));
		const bool moved = pole->getWorldTransform() != transform;
		if (moved)
			pole->setWorldTransform(transform);
		if (moved || created || insulators_all)
		{
			const quint64 span_id = path && slot.span >= 0 && slot.span < path->getNumChildren() ? quint64(path->getChild(slot.span)->getID()) : quint64(slot.span);
			sync_insulators(pole, span_id * 1024ull + quint64(in_span));
		}
	}

	// The poles the line has no place for any more are removed; a selected one is not
	// left selected.
	bool selected = false;
	if (const SelectorNodes *selector = Selection::getSelectorNodes())
	{
		for (auto it = unused.constBegin(); it != unused.constEnd(); ++it)
		{
			for (const NodePtr &pole : it.value())
			{
				for (const NodePtr &node : selector->getNodes())
					selected = selected || (node && node->getID() == pole->getID());
			}
		}
	}
	if (selected)
		Selection::clear();
	// Kept spare, off and out of sight, not saved with the world.
	bool any_unused = false;
	for (auto it = unused.constBegin(); it != unused.constEnd() && !any_unused; ++it)
		any_unused = !it.value().isEmpty();
	if (any_unused && !spare)
	{
		NodeDummyPtr node = NodeDummy::create();
		node->setName(SPARE_POLES_NODE_NAME);
		node->setWorldParent(root_);
		node->setEnabled(false);
		node->setShowInEditorEnabled(false);
		node->setSaveToWorldEnabled(false);
		spare = node;
	}
	// Put away last first: on the top of the stack is the first of them along the line, the
	// one that comes back first when the line gets longer again (A B C D -> A B -> A
	// leaves D C B; back to A B takes B, to A B C D then C and D).
	for (auto it = unused.constBegin(); it != unused.constEnd(); ++it)
	{
		const QVector<NodePtr> &poles = it.value();
		for (int i = int(poles.size()) - 1; i >= 0; --i)
		{
			const NodePtr &pole = poles[i];
			if (!spare_ids.contains(pole->getID()))
				pole->setWorldParent(spare);
		}
	}
	for (int i = 0; i < ordered.size(); ++i)
		holder->setChildIndex(ordered[i], i);

	if (missing && !pole_error_reported_)
	{
		report(MSG_WARNING, uiText("A pole could not be made: an asset of the list is gone from the project.",
			"Не удалось поставить столб: ассета из списка больше нет в проекте."));
	}
	pole_error_reported_ = missing;
	poles_state_ = pole_snapshot();
	poles_cached_ = false;
}

QVector<WireBuilder::WireEnds> WireBuilder::wire_ends() const
{
	QVector<WireEnds> ends;
	if (!hasLine())
		return ends;

	const QVector<Pole> poles = this->poles();
	const int count = int(poles.size());
	const int spans = settings_.closed && count > 2 ? count : count - 1;
	for (int i = 0; i < spans && ends.size() < MAX_WIRES; ++i)
	{
		const int j = (i + 1) % count;
		const QVector<AttachPoint> &a = poles[i].points;
		const QVector<AttachPoint> &b = poles[j].points;
		const Math::Vec3 pivot_a = poles[i].pivot;
		const Math::Vec3 pivot_b = poles[j].pivot;
		const int ids_a = poles[i].id;
		const int ids_b = poles[j].id;
		const int first_end = int(ends.size());
		if (a.isEmpty() && b.isEmpty())
			ends.append(WireEnds{ids_a, ids_b, 0, pivot_a, pivot_b});
		else if (a.isEmpty())
		{
			// A pole that is one point (a hook on a wall): all the wires come to it.
			for (const AttachPoint &p : b)
				ends.append(WireEnds{ids_a, ids_b, p.number, pivot_a, p.position});
		} else if (b.isEmpty())
		{
			for (const AttachPoint &p : a)
				ends.append(WireEnds{ids_a, ids_b, p.number, p.position, pivot_b});
		} else
		{
			for (const AttachPoint &p : a)
			{
				for (const AttachPoint &q : b)
				{
					if (q.number == p.number)
						ends.append(WireEnds{ids_a, ids_b, p.number, p.position, q.position});
				}
			}
		}

		// The sag and the random the point of the curve sets for its span.
		const NodePtr path = editor_.getPathNode();
		if (path && poles[i].span >= 0 && poles[i].span < path->getNumChildren())
		{
			double scale = 1.0;
			double random = -1.0;
			double thickness = 1.0;
			double twist = 0.0;
			point_sag(path->getChild(poles[i].span), scale, random);
			point_shape(path->getChild(poles[i].span), thickness, twist);
			for (int k = first_end; k < ends.size(); ++k)
			{
				ends[k].sag_scale = scale;
				ends[k].sag_random = random;
				ends[k].thickness_scale = thickness;
				ends[k].twist_add = twist;
			}
		}
	}
	return ends;
}

void WireBuilder::point_sag(const NodePtr &point, double &scale, double &random) const
{
	bool ok = false;
	const double value = dataString(point, POINT_SAG_DATA).toDouble(&ok);
	scale = ok ? qBound(0.0, value, 1000.0) : 1.0;
	const double percent = dataString(point, POINT_RANDOM_DATA).toDouble(&ok);
	random = ok ? qBound(-1.0, percent, 1000.0) : -1.0;
}

void WireBuilder::point_shape(const NodePtr &point, double &thickness, double &twist) const
{
	bool ok = false;
	const double value = dataString(point, POINT_THICKNESS_DATA).toDouble(&ok);
	thickness = ok ? qBound(0.0, value, 1000.0) : 1.0;
	const double turns = dataString(point, POINT_TWIST_DATA).toDouble(&ok);
	twist = ok ? qBound(-1000.0, turns, 1000.0) : 0.0;
}

bool WireBuilder::hasTwist() const
{
	if (std::abs(settings_.twist) > 1e-6f)
		return true;
	// What the points of the curve add, as they are now: the wires may not be built anew
	// yet after a change.
	if (const NodePtr path = editor_.getPathNode())
	{
		for (int i = 0, num = path->getNumChildren(); i < num; ++i)
		{
			double thickness = 1.0;
			double twist = 0.0;
			point_shape(path->getChild(i), thickness, twist);
			if (std::abs(twist) > 1e-6)
				return true;
		}
	}
	return false;
}

bool WireBuilder::getSelectedPointsShape(double &thickness, double &twist) const
{
	const Vector<NodePtr> selected = editor_.getSelectedPoints();
	if (selected.empty())
		return false;
	point_shape(selected[0], thickness, twist);
	return true;
}

void WireBuilder::setSelectedPointsThickness(double thickness)
{
	SetNodeDataAction *action = new SetNodeDataAction;
	for (const NodePtr &point : editor_.getSelectedPoints())
		action->set(point, POINT_THICKNESS_DATA, std::abs(thickness - 1.0) < 1e-6 ? QByteArray() : QByteArray::number(qMax(thickness, 0.0), 'g', 6));
	SetNodeDataAction::push(action);
	notify_changed();
}

void WireBuilder::setSelectedPointsTwist(double twist)
{
	SetNodeDataAction *action = new SetNodeDataAction;
	for (const NodePtr &point : editor_.getSelectedPoints())
		action->set(point, POINT_TWIST_DATA, std::abs(twist) < 1e-9 ? QByteArray() : QByteArray::number(twist, 'g', 6));
	SetNodeDataAction::push(action);
	notify_changed();
}

bool WireBuilder::getSelectedPointsSag(double &scale, double &random) const
{
	const Vector<NodePtr> selected = editor_.getSelectedPoints();
	if (selected.empty())
		return false;
	point_sag(selected[0], scale, random);
	return true;
}

void WireBuilder::setSelectedPointsSagScale(double scale)
{
	SetNodeDataAction *action = new SetNodeDataAction;
	for (const NodePtr &point : editor_.getSelectedPoints())
		action->set(point, POINT_SAG_DATA, std::abs(scale - 1.0) < 1e-6 ? QByteArray() : QByteArray::number(qMax(scale, 0.0), 'g', 6));
	SetNodeDataAction::push(action);
	notify_changed();
}

void WireBuilder::setSelectedPointsSagRandom(double random)
{
	SetNodeDataAction *action = new SetNodeDataAction;
	for (const NodePtr &point : editor_.getSelectedPoints())
		action->set(point, POINT_RANDOM_DATA, random < 0.0 ? QByteArray() : QByteArray::number(random, 'g', 6));
	SetNodeDataAction::push(action);
	notify_changed();
}

////////////////////////////////////////////////////////////////////////////////
// Settings, kept in the data of the root node as JSON.
////////////////////////////////////////////////////////////////////////////////
void WireBuilder::setSettings(const WireSettings &settings)
{
	// Another ground to lie on: the heights found so far are not those.
	if (settings.ground.surface_types != settings_.ground.surface_types || settings.ground.only_immovable != settings_.ground.only_immovable
		|| settings.ground.only_intersection != settings_.ground.only_intersection)
		ground_heights_.clear();
		pole_ground_.clear();
	const bool distances = settings.visible_distance != settings_.visible_distance
		|| settings.fade_distance != settings_.fade_distance || settings.lod_distance != settings_.lod_distance
		|| settings.lod_fade != settings_.lod_fade;
	settings_ = limited(settings);
	settings_changed();
	// The pieces get the distances at once: no Generate is needed.
	if (distances)
		apply_distances_to_pieces();
	editor_.setClosed(settings_.closed);
	editor_.setSurfaceFilter(settings_.ground);
	if (hasLine())
		save_settings();
	notify_changed();
}

void WireBuilder::resetSettings()
{
	// All as a new line has them; only Closed (the shape of the curve) and the folder of its
	// files stay.
	WireSettings settings;
	settings.closed = settings_.closed;
	settings.folder = settings_.folder;
	settings.seed = randomSeed();
	setSettings(settings);
	report(MSG_INFO, QString());
}

void WireBuilder::load_settings()
{
	const char *data = root_->getData(SETTINGS_DATA);
	saved_settings_ = QByteArray(data ? data : "");
	saved_poles_ = dataString(root_, POLES_DATA).toUtf8();
	++settings_revision_;
	const QJsonObject json = QJsonDocument::fromJson(saved_settings_).object();
	if (!json.isEmpty())
		settings_ = from_json(json);
	settings_changed();
	// Another line, or an undo: the ground may be another one.
	ground_heights_.clear();
	pole_ground_.clear();
}

void WireBuilder::settings_changed()
{
	poles_cached_ = false;
	snapshot_settings_valid_ = false;
}

void WireBuilder::save_settings(bool undoable, bool merge)
{
	if (!root_ || root_.isDeleted())
		return;

	const QJsonObject json = to_json(settings_);
	// Changes of the same settings made one right after another (a value dragged in a
	// field) are one step of the undo history, of other ones not.
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
// The wires.
////////////////////////////////////////////////////////////////////////////////
QByteArray WireBuilder::snapshot(const QVector<WireEnds> &ends) const
{
	// The material and the folder are not the shape of the wires: the pieces get the
	// material at once, the folder is for the next Generate. The settings are written out
	// once, not every frame.
	if (!snapshot_settings_valid_)
	{
		QJsonObject json = to_json(settings_);
		json.remove("material");
		json.remove("folder");
		json.remove("visible_distance");
		json.remove("fade_distance");
		json.remove("lod_distance");
		json.remove("lod_fade");
		snapshot_settings_ = QJsonDocument(json).toJson(QJsonDocument::Compact);
		snapshot_settings_valid_ = true;
	}
	QByteArray state = snapshot_settings_;
	for (const WireEnds &end : ends)
	{
		putBytes(state, double(end.pole_a));
		putBytes(state, double(end.pole_b));
		putBytes(state, double(end.number));
		putBytes(state, end.from);
		putBytes(state, end.to);
		putBytes(state, end.sag_scale);
		putBytes(state, end.sag_random);
		putBytes(state, end.thickness_scale);
		putBytes(state, end.twist_add);
	}
	return state;
}

void WireBuilder::rebuild(const QVector<WireEnds> &ends)
{
	wires_.clear();
	wire_poles_.clear();
	cuts_.clear();
	triangles_ = 0;
	rebuild_error_.clear();
	state_ = snapshot(ends);
	preview_dirty_ = true;

	const bool mesh = settings_.shape == WIRE_SHAPE_MESH;
	double copy_length = 0.0;
	if (mesh)
	{
		load_mesh_source();
		if (mesh_source_.isEmpty())
			return;
		copy_length = double(bendBounds(mesh_source_, mesh_axes()).length());
		// The source loaded (it stays loaded while its file is the same): the error of an
		// axis without length is of the axes as they are now.
		if (mesh_error_ == axis_length_error())
			mesh_error_.clear();
		if (copy_length < 1e-4)
		{
			mesh_error_ = axis_length_error();
			return;
		}
	}

	const WireLook look = this->look();
	qint64 triangles = 0;
	for (const WireEnds &end : ends)
	{
		// The random numbers of a wire are told by its poles and its number, so they stay
		// the same when the poles are moved.
		const quint64 pole_a = quint64(end.pole_a);
		const quint64 pole_b = quint64(end.pole_b);
		const quint64 key = quint64(settings_.seed) * 1000003ull + quint64(end.number);
		const float random_sag = random_of(pole_a, pole_b, key, 1);
		const float random_turn = random_of(pole_a, pole_b, key, 2);

		const double chord = double(Math::length(end.to - end.from));
		double sag = double(settings_.sag);
		if (settings_.sag_mode == WIRE_SAG_PERCENT)
			sag = chord * double(settings_.sag) * 0.01;
		else if (settings_.sag_mode == WIRE_SAG_TENSION)
			sag = double(settings_.sag) * (chord / TENSION_SPAN) * (chord / TENSION_SPAN);
		// The random part only adds to the sag: none of the wires hangs higher than Sag, so
		// none of them gets pulled tight.
		const double random_percent = end.sag_random >= 0.0 ? end.sag_random : double(settings_.sag_random);
		sag *= end.sag_scale * (1.0 + random_percent * 0.01 * double(random_sag));

		WireSpan span;
		hangWire(end.from, end.to, sag, double(settings_.segment_angle), settings_.min_segments,
			double(settings_.max_segment_length), span);
		if (span.isEmpty())
			continue;
		span.thickness_scale = float(end.thickness_scale);
		if (settings_.above_ground)
			keep_above_ground(span);
		span.random = random_of(pole_a, pole_b, key, 3);
		const double rotation = double(settings_.rotation) + (settings_.random_rotation ? double(random_turn) * 360.0 : 0.0);
		span.twist_start = rotation * Math::Consts::DEG2RAD_D;
		// The twist of the line and that the point adds for its span, by the same mode.
		const double twist = double(settings_.twist) + end.twist_add;
		const double turns = settings_.twist_mode == WIRE_TWIST_PER_SPAN && span.length > 1e-9 ? twist / span.length : twist;
		span.thickness_scale = float(end.thickness_scale);
		span.twist_rate = turns * 2.0 * Math::Consts::PI_D;
		// A strip or a tube turns at its joints only: they are made as dense as the twist
		// needs. A mesh is cut by Mesh Angle instead.
		if (!mesh)
			stepTwist(span, double(settings_.twist_step), MAX_TWIST_SAMPLES);

		if (mesh)
		{
			const int copies = qBound(1, int(std::lround(span.length / copy_length)), MAX_MESH_COPIES);
			// How much the wire turns the copies in all: its bends and its twist.
			double turn = std::abs(span.twist_rate) * span.length * Math::Consts::RAD2DEG_D;
			for (int i = 0; i + 1 < span.tangents.size(); ++i)
			{
				const double cosine = qBound(-1.0, double(Math::dot(span.tangents[i], span.tangents[i + 1])), 1.0);
				turn += std::acos(cosine) * Math::Consts::RAD2DEG_D;
			}
			triangles += bentTriangles(mesh_source_, mesh_axes(), copies, turn, settings_.mesh_angle);
		} else
		{
			triangles += wireTriangles(span, look);
			if (settings_.lod)
				triangles += wireTriangles(simplifyWire(span, settings_.lod_factor), look);
		}
		wires_.append(span);
		wire_poles_.append(qMakePair(end.pole_a, end.pole_b));
	}
	if (triangles > MAX_TRIANGLES)
	{
		wires_.clear();
		wire_poles_.clear();
		rebuild_error_ = uiText("The wires would have more than %1 triangles: make Bend Angle or Mesh Angle larger, or the mesh lighter.",
			"У проводов было бы больше %1 треугольников: увеличьте «Угол изгиба» или «Угол меша», или возьмите меш полегче.").arg(MAX_TRIANGLES);
		report(MSG_WARNING, rebuild_error_);
		return;
	}
	triangles_ = int(triangles);
	find_cuts();
}

void WireBuilder::find_cuts()
{
	cuts_.clear();
	const QVector<QVector<int>> piece_wires = pieces();
	if (piece_wires.size() < 2)
		return;
	QVector<int> piece_of(wires_.size(), 0);
	for (int p = 0; p < piece_wires.size(); ++p)
	{
		for (int w : piece_wires[p])
			piece_of[w] = p;
	}

	// The wires that end at every pole: the line is cut there when they are in different
	// pieces.
	QMap<int, QVector<int>> pole_wires;
	for (int w = 0; w < wires_.size(); ++w)
	{
		pole_wires[wire_poles_[w].first].append(w);
		pole_wires[wire_poles_[w].second].append(w);
	}
	for (auto it = pole_wires.constBegin(); it != pole_wires.constEnd(); ++it)
	{
		const QVector<int> &wires = it.value();
		bool cut = false;
		for (int w : wires)
			cut = cut || piece_of[w] != piece_of[wires.first()];
		if (!cut)
			continue;

		// A plane across the line through the ends of its wires at the pole, a little
		// larger than they are.
		QVector<Math::Vec3> points;
		Math::Vec3 along(0.0);
		for (int w : wires)
		{
			const bool starts = wire_poles_[w].first == it.key();
			points.append(starts ? wires_[w].from : wires_[w].to);
			// Every wire goes the way of the line, from its pole to the next one.
			const Math::Vec3 way = unitOrZero(wires_[w].to - wires_[w].from);
			along += Math::Vec3(way.x, way.y, 0.0);
		}
		Math::Vec3 forward = unitOrZero(along);
		if (Math::length2(forward) == 0.0)
			forward = Math::Vec3(0.0, 1.0, 0.0);
		const Math::Vec3 right(forward.y, -forward.x, 0.0);
		const Math::Vec3 centre = points.first();
		double right_min = 0.0, right_max = 0.0, up_min = 0.0, up_max = 0.0;
		for (const Math::Vec3 &point : points)
		{
			const Math::Vec3 offset = point - centre;
			right_min = qMin(right_min, double(Math::dot(offset, right)));
			right_max = qMax(right_max, double(Math::dot(offset, right)));
			up_min = qMin(up_min, double(offset.z));
			up_max = qMax(up_max, double(offset.z));
		}
		const double margin = qMax(0.3, 0.15 * qMax(right_max - right_min, up_max - up_min));
		right_min -= margin;
		right_max += margin;
		up_min -= margin;
		up_max += margin;
		const Math::Vec3 up(0.0, 0.0, 1.0);
		const auto corner = [&](double r, double u) { return centre + right * Math::Scalar(r) + up * Math::Scalar(u); };
		CutQuad quad;
		quad.corners[0] = corner(right_min, up_min);
		quad.corners[1] = corner(right_max, up_min);
		quad.corners[2] = corner(right_max, up_max);
		quad.corners[3] = corner(right_min, up_max);
		cuts_.append(quad);
	}
}

QString WireBuilder::axis_length_error()
{
	return uiText("The mesh has no length along its forward axis: pick another axis.",
		"У меша нет длины вдоль «Оси вперёд»: выберите другую ось.");
}

void WireBuilder::load_mesh_source()
{
	if (mesh_source_path_ == settings_.mesh_source && !mesh_source_path_.isEmpty())
		return;
	mesh_source_ = BendSource();
	mesh_error_.clear();
	mesh_source_path_ = settings_.mesh_source;
	if (settings_.mesh_source.isEmpty())
	{
		mesh_error_ = uiText("Pick the mesh to lay along the wires in the Mesh field.",
			"Выберите меш, который ляжет вдоль проводов, в поле «Меш».");
		return;
	}
	QString error;
	if (!BendSource::load(mesh_asset(), mesh_source_, error))
		mesh_error_ = QString::fromUtf8("%1: %2").arg(settings_.mesh_name, error);
	else if (!mesh_source_.note.isEmpty())
		report(MSG_INFO, QString::fromUtf8("%1: %2").arg(settings_.mesh_name, mesh_source_.note));
}

BendAxes WireBuilder::mesh_axes() const
{
	BendAxes axes;
	axes.forward = settings_.mesh_forward;
	axes.up = settings_.mesh_up;
	axes.scale = Math::vec3(settings_.mesh_scale);
	return axes;
}

QStringList WireBuilder::mesh_surface_names() const
{
	// The names of the asset; surfaces of the same name in different objects of it are
	// told apart by a number.
	QStringList names;
	for (const BendSource::Surface &surface : mesh_source_.surfaces)
	{
		QString name = surface.name.isEmpty() ? QString::fromUtf8("surface") : surface.name;
		const QString base = name;
		for (int n = 2; names.contains(name); ++n)
			name = QString::fromUtf8("%1_%2").arg(base).arg(n);
		names.append(name);
	}
	return names;
}

void WireBuilder::apply_materials(const ObjectPtr &object, const PieceState *keep) const
{
	if (!object)
		return;
	const bool mesh = settings_.shape == WIRE_SHAPE_MESH;
	const QStringList names = mesh ? mesh_surface_names() : QStringList();
	const MaterialPtr material = mesh ? MaterialPtr() : wire_material(settings_.material, settings_.shape);
	for (int surface = 0; surface < object->getNumSurfaces(); ++surface)
	{
		if (keep)
		{
			if (const PieceState::Surface *settings = keep->surface(object->getSurfaceName(surface)))
			{
				PieceState::putSurface(object, surface, *settings);
				// A default material follows the shape (alpha for a cross, solid for a
				// ribbon or a tube); one given by hand stays.
				if (!mesh && material && settings_.material.isEmpty() && is_default_wire_material(settings->material))
					object->setMaterial(material, surface);
				continue;
			}
		}
		if (!mesh)
		{
			if (material)
				object->setMaterial(material, surface);
			continue;
		}
		const int index = names.indexOf(QString::fromUtf8(object->getSurfaceName(surface)));
		if (index >= 0)
			applySurface(object, surface, mesh_source_.surfaces[index]);
	}
	// The LOD has the material the full wires have, also one given to them by hand.
	const int full = object->findSurface(WIRE_SURFACE_NAME);
	const int lod = object->findSurface(WIRE_LOD_SURFACE_NAME);
	if (!mesh && full >= 0 && lod >= 0 && object->getMaterial(full))
		object->setMaterial(object->getMaterial(full), lod);
	apply_distances(object);
}

void WireBuilder::apply_distances(const ObjectPtr &object) const
{
	if (!object)
		return;
	const bool mesh = settings_.shape == WIRE_SHAPE_MESH;
	const QStringList names = mesh ? mesh_surface_names() : QStringList();
	for (int surface = 0; surface < object->getNumSurfaces(); ++surface)
	{
		// What the surface would be seen to without the setting: always for a strip or a
		// tube, the LOD distance of the asset for a mesh.
		float visible = Math::Consts::INF;
		float fade = 0.0f;
		const int index = mesh ? names.indexOf(QString::fromUtf8(object->getSurfaceName(surface))) : -1;
		if (index >= 0)
		{
			visible = mesh_source_.surfaces[index].max_visible;
			fade = mesh_source_.surfaces[index].max_fade;
		}
		if (settings_.visible_distance > 0.0f && settings_.visible_distance < visible)
		{
			visible = settings_.visible_distance;
			fade = settings_.fade_distance;
		}
		// The LOD of the strips and the tubes: the full wires up to its distance, the
		// simple ones from it on, faded into each other over its fade.
		float min_visible = -Math::Consts::INF;
		float min_fade = 0.0f;
		if (!mesh && settings_.lod)
		{
			const bool lod_surface = QByteArray(object->getSurfaceName(surface)) == WIRE_LOD_SURFACE_NAME;
			if (lod_surface)
			{
				min_visible = settings_.lod_distance;
				min_fade = settings_.lod_fade;
			} else if (settings_.lod_distance < visible)
			{
				visible = settings_.lod_distance;
				fade = settings_.lod_fade;
			}
		}
		// A mesh keeps where the LODs of its asset start.
		if (!mesh)
		{
			object->setMinVisibleDistance(min_visible, surface);
			object->setMinFadeDistance(min_fade, surface);
		}
		object->setMaxVisibleDistance(visible, surface);
		object->setMaxFadeDistance(fade, surface);
	}
}

void WireBuilder::apply_distances_to_pieces() const
{
	const NodePtr holder = mesh_node();
	for (int i = 0, num = holder ? holder->getNumChildren() : 0; i < num; ++i)
	{
		if (holder->getChild(i)->getType() == Node::OBJECT_MESH_STATIC)
			apply_distances(checked_ptr_cast<Object>(holder->getChild(i)));
	}
	if (preview_ && !preview_.isDeleted())
		apply_distances(preview_);
}

WireLook WireBuilder::look() const
{
	WireLook look;
	look.shape = settings_.shape;
	look.thickness = settings_.thickness;
	look.sides = settings_.sides;
	look.uv_length = settings_.uv_length;
	look.uv_width = settings_.uv_width;
	look.uv_rotation = settings_.uv_rotation;
	return look;
}

void WireBuilder::add_wires(const QVector<int> &wires, const Math::Vec3 &pivot, const MeshPtr &mesh) const
{
	if (settings_.shape != WIRE_SHAPE_MESH)
	{
		const WireLook look = this->look();
		int surface = mesh->findSurface(WIRE_SURFACE_NAME);
		if (surface < 0)
			surface = mesh->addSurface(WIRE_SURFACE_NAME);
		for (int w : wires)
			addWireGeometry(wires_[w], look, pivot, mesh, surface);
		// The LOD: the same wires with fewer segments, a surface of its own.
		if (settings_.lod)
		{
			int lod_surface = mesh->findSurface(WIRE_LOD_SURFACE_NAME);
			if (lod_surface < 0)
				lod_surface = mesh->addSurface(WIRE_LOD_SURFACE_NAME);
			for (int w : wires)
				addWireGeometry(simplifyWire(wires_[w], settings_.lod_factor), look, pivot, mesh, lod_surface);
		}
		return;
	}

	if (mesh_source_.isEmpty())
		return;
	// The mesh is laid along every wire a whole number of times, all the copies
	// stretched a little to fill it.
	const BendAxes axes = mesh_axes();
	const BendBounds bounds = bendBounds(mesh_source_, axes);
	const QStringList names = mesh_surface_names();
	const double copy_length = qMax(double(bounds.length()), 1e-4);
	for (int w : wires)
	{
		const WireSpan &span = wires_[w];
		// Thicker or thinner across the wire, as long along it.
		BendAxes span_axes = axes;
		for (int axis = 0; axis < 3; ++axis)
		{
			if (axis != int(settings_.mesh_forward) / 2)
				span_axes.scale[axis] *= span.thickness_scale;
		}
		const int copies = qBound(1, int(std::lround(span.length / copy_length)), MAX_MESH_COPIES);
		const double fit = span.length / (copy_length * copies);
		const auto frame = [&span](double distance) { return wireFrame(span, distance); };
		// The vertices the surfaces have before this wire: those after them are its own.
		QHash<QString, int> first;
		for (const QString &name : names)
		{
			const int s = mesh->findSurface(name.toUtf8().constData());
			first.insert(name, s >= 0 ? mesh->getNumVertex(s) : 0);
		}
		for (int c = 0; c < copies; ++c)
			bendCopy(mesh_source_, span_axes, bounds, names, double(c) * copy_length * fit, fit, frame, pivot, mesh,
				settings_.mesh_angle);
		// The colors of the vertex animation, the same as on strips and tubes.
		for (auto it = first.constBegin(); it != first.constEnd(); ++it)
		{
			const int s = mesh->findSurface(it.key().toUtf8().constData());
			if (s >= 0)
				paintWireColors(span, pivot, mesh, s, it.value());
		}
	}
}

QVector<QVector<int>> WireBuilder::pieces() const
{
	// Ordered by the square, so that the pieces keep their order (and their files) while
	// the wires change.
	QMap<QPair<qint64, qint64>, QVector<int>> squares;
	const double size = double(settings_.piece_size);
	for (int w = 0; w < wires_.size(); ++w)
	{
		const Math::Vec3 middle = (wires_[w].from + wires_[w].to) * Math::Scalar(0.5);
		const QPair<qint64, qint64> square = size > 0.0
			? qMakePair(qint64(std::floor(double(middle.x) / size)), qint64(std::floor(double(middle.y) / size)))
			: qMakePair(qint64(0), qint64(0));
		squares[square].append(w);
	}
	return squares.values();
}

bool WireBuilder::isOutdated() const
{
	return hasLine() && getNumPieces() > 0 && !is_up_to_date();
}

bool WireBuilder::is_up_to_date() const
{
	return dataString(root_, STATE_DATA).toUtf8() == QCryptographicHash::hash(state_, QCryptographicHash::Md5).toHex();
}

////////////////////////////////////////////////////////////////////////////////
// Material, mesh and folder.
////////////////////////////////////////////////////////////////////////////////
bool WireBuilder::takeMaterial(const Vector<UGUID> &guids)
{
	if (!hasLine())
	{
		report(MSG_WARNING, uiText("Create a line or select an existing one first.", "Сначала создайте линию или выделите существующую."));
		return false;
	}

	MaterialPtr material;
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
		material = file_guid.isValid() ? Materials::findMaterialByFileGUID(file_guid) : MaterialPtr();
		if (!material)
			material = Materials::findMaterialByGUID(guid);
		if (material)
			break;
	}
	if (!material)
	{
		report(MSG_WARNING, guids.empty() ? uiText("Select a material in the Asset Browser first.", "Сначала выделите материал в Asset Browser.")
			: uiText("That is not a material: pick a material asset.", "Это не материал: выберите ассет материала."));
		return false;
	}
	if (!material->isNodeTypeSupported(Node::OBJECT_MESH_STATIC))
	{
		report(MSG_WARNING, uiText("This material can not be put on a mesh: pick a mesh material (mesh_base, its children or a mesh Material Graph).",
			"Этот материал нельзя назначить мешу: выберите материал меша (mesh_base, его наследники или Material Graph для меша)."));
		return false;
	}
	if (!material->getFileGUID().isValid())
	{
		report(MSG_WARNING, uiText("This material has no file in the project; pick a material asset.",
			"У этого материала нет файла в проекте; выберите ассет материала."));
		return false;
	}
	set_material(QString::fromUtf8(material->getFileGUID().makeString().get()));
	report(MSG_INFO, QString());
	return true;
}

void WireBuilder::clearMaterial()
{
	if (hasLine())
		set_material(QString());
}

void WireBuilder::set_material(const QString &material)
{
	// One step of the undo history: the material of the line and of its pieces, which
	// get it at once - Generate keeps the material a piece has.
	::UnigineEditor::Undo::begin();
	settings_.material = material;
	save_settings(true, false);
	if (settings_.shape != WIRE_SHAPE_MESH)
	{
		const MaterialPtr wire = wire_material(material, settings_.shape);
		SetMaterialsAction *action = new SetMaterialsAction;
		const NodePtr holder = mesh_node();
		for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
		{
			if (wire && holder->getChild(i)->getType() == Node::OBJECT_MESH_STATIC)
				action->set(holder->getChild(i), wire->getGUID());
		}
		if (action->isEmpty())
			delete action;
		else
			::UnigineEditor::Undo::apply(action);
	}
	::UnigineEditor::Undo::commit();
	preview_dirty_ = true;
	notify_changed();
}

QString WireBuilder::getDefaultMaterialName() const
{
	const MaterialPtr material = default_wire_material(settings_.shape);
	if (!material || !material->getFileGUID().isValid())
		return QString::fromUtf8("mesh_base");
	// The path of the asset, not the one of the material (the runtime file, named by GUID).
	const QString path = QString::fromUtf8(FileSystem::getVirtualPath(material->getFileGUID()).get());
	return path.isEmpty() ? QString::fromUtf8("mesh_base") : QFileInfo(path).completeBaseName();
}

QString WireBuilder::getMaterialName() const
{
	const UGUID guid = guidOf(settings_.material);
	if (!guid.isValid())
		return QString();
	const QString path = QString::fromUtf8(FileSystem::getVirtualPath(guid).get());
	return path.isEmpty() ? uiText("(missing material)", "(материал не найден)") : QFileInfo(path).completeBaseName();
}

bool WireBuilder::showMaterial() const
{
	// None picked: the default one of the shape, if it is a file of the project.
	QString picked = settings_.material;
	if (picked.isEmpty())
	{
		if (const MaterialPtr material = default_wire_material(settings_.shape))
			picked = QString::fromUtf8(material->getFileGUID().makeString().get());
	}
	const UGUID file = guidOf(picked);
	if (!hasLine() || !file.isValid())
		return false;
	const String path = FileSystem::getVirtualPath(file);
	const UGUID asset = path.empty() ? UGUID() : AssetManager::getAssetGUIDFromPath(path);
	return showAssetInBrowser(asset.isValid() ? asset : file);
}

bool WireBuilder::takeMesh(const Vector<UGUID> &guids)
{
	if (!hasLine())
	{
		report(MSG_WARNING, uiText("Create a line or select an existing one first.", "Сначала создайте линию или выделите существующую."));
		return false;
	}
	PlaceableAsset asset;
	if (!resolveSelection(guids, asset))
	{
		report(MSG_WARNING, guids.empty() ? uiText("Select a mesh, a node or a model in the Asset Browser first.",
				"Сначала выделите меш, ноду или модель в Asset Browser.")
			: uiText("That is not a mesh: pick a .mesh, a .node or a model.", "Это не меш: выберите .mesh, .node или модель."));
		return false;
	}
	WireSettings settings = settings_;
	settings.mesh_source = QString::fromUtf8(asset.path.get());
	settings.mesh_kind = int(asset.kind);
	settings.mesh_name = QFileInfo(asset.name).completeBaseName();
	settings.shape = WIRE_SHAPE_MESH;
	mesh_source_path_.clear();
	setSettings(settings);
	report(MSG_INFO, QString());
	return true;
}

bool WireBuilder::showMesh() const
{
	return !settings_.mesh_source.isEmpty() && showInAssetBrowser(mesh_asset());
}

QString WireBuilder::getMeshError() const
{
	return settings_.shape == WIRE_SHAPE_MESH ? mesh_error_ : QString();
}

QString WireBuilder::defaultFolder()
{
	// The folder of the world, as a path in the data of the project.
	QString world = QDir::fromNativeSeparators(QString::fromUtf8(FileSystem::getVirtualPath(World::getPath()).get()));
	QString folder = QFileInfo(world).path();
	if (world.isEmpty() || folder == QLatin1String(".") || QDir::isAbsolutePath(folder))
		folder.clear();
	return folder.isEmpty() ? QString::fromUtf8(DEFAULT_FOLDER_NAME) : folder + QLatin1Char('/') + QString::fromUtf8(DEFAULT_FOLDER_NAME);
}

QString WireBuilder::getFolder() const
{
	return settings_.folder.isEmpty() ? defaultFolder() : settings_.folder;
}

bool WireBuilder::hasGeneratedFiles() const
{
	return hasLine() && !dataString(root_, FILES_DATA).isEmpty();
}

bool WireBuilder::showGeneratedFiles() const
{
	if (!hasLine())
		return false;
	for (const QString &file : dataString(root_, FILES_DATA).split(QLatin1Char('\n'), Qt::SkipEmptyParts))
	{
		if (showAssetInBrowser(AssetManager::getAssetGUIDFromPath(file.toUtf8().constData())))
			return true;
	}
	return false;
}

bool WireBuilder::setFolderFromDisk(const QString &absolute_path)
{
	const QString data = QDir::cleanPath(dataPath());
	const QString folder = QDir::cleanPath(QDir::fromNativeSeparators(absolute_path));
	if (!folder.startsWith(data + QLatin1Char('/'), Qt::CaseInsensitive) && folder.compare(data, Qt::CaseInsensitive) != 0)
	{
		report(MSG_WARNING, uiText("The folder must be inside the data folder of the project.",
			"Папка должна быть внутри папки data проекта."));
		return false;
	}
	WireSettings settings = settings_;
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
bool WireBuilder::generate()
{
	if (!hasLine())
	{
		report(MSG_WARNING, uiText("Create a line or select an existing one first.", "Сначала создайте линию или выделите существующую."));
		return false;
	}

	show_hidden_pieces();
	// The mesh asset and the ground may have changed since they were looked at.
	mesh_source_path_.clear();
	ground_heights_.clear();
	pole_ground_.clear();
	rebuild(wire_ends());
	if (wires_.isEmpty())
	{
		report(MSG_WARNING, !rebuild_error_.isEmpty() ? rebuild_error_ : !getMeshError().isEmpty() ? getMeshError()
			: uiText("No wires: the line needs at least two poles with attach points of the same numbers.",
				"Проводов нет: линии нужно минимум два столба с точками крепления одинаковых номеров."));
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

	// The files of a line keep the name they got the first time, in the same folder; a
	// copy made by the editor gets files of its own and leaves those of the original.
	bool own_files = false;
	const QStringList old_files = ownGeneratedFiles(root_, line_generated_data(), &own_files);
	const QVector<QVector<int>> piece_wires = pieces();
	const int num_pieces = int(piece_wires.size());
	const QString base = pickBaseName(root_, line_generated_data(), own_files, folder,
		fileNameOf(getLineName(), QString::fromUtf8(ROOT_NODE_NAME)), num_pieces, old_files);

	// Pieces of another kind than the last Generate made have other surfaces: they are
	// not kept.
	const bool mesh = settings_.shape == WIRE_SHAPE_MESH;
	const QString kind = mesh ? QString::fromUtf8("mesh:") + settings_.mesh_source : QString::fromUtf8("wire");
	const bool same_kind = dataString(root_, KIND_DATA) == kind;
	const NodePtr holder = mesh_node();
	QVector<NodePtr> old_pieces;
	NodePtr pattern;
	for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
	{
		const NodePtr child = holder->getChild(i);
		if (child->getType() != Node::OBJECT_MESH_STATIC || !same_kind)
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

	// All the meshes are made first; a piece is put where the middle of its wires is.
	QVector<MeshPtr> meshes;
	QVector<Math::Vec3> pivots;
	for (const QVector<int> &wires : piece_wires)
	{
		Math::Vec3 minimum = wires_[wires.first()].samples.first();
		Math::Vec3 maximum = minimum;
		for (int w : wires)
		{
			for (const Math::Vec3 &sample : wires_[w].samples)
			{
				minimum = Math::min(minimum, sample);
				maximum = Math::max(maximum, sample);
			}
		}
		const Math::Vec3 pivot = (minimum + maximum) * Math::Scalar(0.5);
		MeshPtr piece_mesh = Mesh::create();
		add_wires(wires, pivot, piece_mesh);
		for (int surface = 0; surface < piece_mesh->getNumSurfaces(); ++surface)
		{
			piece_mesh->createTangents(surface);
			piece_mesh->createBounds(surface);
			piece_mesh->createCollisionData(surface);
		}
		meshes.append(piece_mesh);
		pivots.append(pivot);
	}

	// The files written over are a part of the undo step: Ctrl+Z gives them back what
	// they had. All the files are written first, and the scene is changed only when every
	// one of them is; the editor is kept from picking the files up on its own meanwhile.
	SetFilesAction *file_action = new SetFilesAction;
	AssetManager::blockAutoRefresh();
	QStringList files;
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

	Vector<NodePtr> created;
	QVector<NodePtr> kept;
	QVector<Math::Mat4> kept_transforms;
	QVector<PieceState> kept_states;
	for (int piece = 0; piece < num_pieces; ++piece)
	{
		const QString &file = files[piece];
		if (existed[piece])
			Render::reloadResource(file.toUtf8().constData());
		else
			AssetManager::importAssetSync(file.toUtf8().constData());
		const UGUID guid = AssetManager::getAssetGUIDFromPath(file.toUtf8().constData());
		const String mesh_path = guid.isValid() ? FileSystem::guidToPath(guid) : String(file.toUtf8().constData());
		const QString name = QFileInfo(file).completeBaseName();

		if (piece < old_pieces.size())
		{
			// A piece that is there keeps what was set on it by hand; it only gets the new
			// file and its new place.
			const ObjectMeshStaticPtr object = checked_ptr_cast<ObjectMeshStatic>(old_pieces[piece]);
			const PieceState &before = old_states[piece];
			if (String(object->getMeshPath()) != mesh_path)
				object->setMeshPath(mesh_path.get());
			apply_materials(object, &before);
			object->setName(name.toUtf8().constData());
			kept.append(object);
			kept_transforms.append(Math::translate(pivots[piece]));
			kept_states.append(before);
			continue;
		}

		NodePtr node;
		if (pattern)
		{
			// A copy of the first piece, with what was set on it by hand; a surface the
			// first piece lacks (the LOD, another shape) gets the settings of the line.
			const PieceState first = PieceState::of(pattern);
			node = pattern->clone();
			ObjectMeshStaticPtr object = checked_ptr_cast<ObjectMeshStatic>(node);
			object->setMeshPath(mesh_path.get());
			apply_materials(object, &first);
		} else
		{
			ObjectMeshStaticPtr object = ObjectMeshStatic::create(mesh_path.get());
			apply_materials(object);
			node = object;
		}
		node->setName(name.toUtf8().constData());
		node->setWorldParent(holder);
		node->setWorldTransform(Math::translate(pivots[piece]));
		node->setShowInEditorEnabledRecursive(true);
		node->setSaveToWorldEnabledRecursive(true);
		created.append(node);
	}
	AssetManager::unblockAutoRefresh();

	// What is in "mesh" and is not a piece of the new wires.
	Vector<NodePtr> removed;
	for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
	{
		const NodePtr child = holder->getChild(i);
		bool stays = false;
		for (const NodePtr &node : kept)
			stays = stays || node->getID() == child->getID();
		for (const NodePtr &node : created)
			stays = stays || node->getID() == child->getID();
		if (!stays && child->getType() == Node::OBJECT_MESH_STATIC)
			removed.append(child);
	}

	// One undo step: the files written over with the pieces that stay, the pieces moved,
	// the new ones in, the extra ones out, and what the line remembers of its Generate.
	SetNodeDataAction *data_action = new SetNodeDataAction;
	setGeneratedData(data_action, root_, line_generated_data(), generatedFileList(files, old_files), base, folder);
	data_action->set(root_, STATE_DATA, QCryptographicHash::hash(state_, QCryptographicHash::Md5).toHex());
	data_action->set(root_, KIND_DATA, kind.toUtf8());
	commitGenerate(file_action, kept, kept_states, kept_transforms, created, removed, data_action);

	report(MSG_INFO, uiText("Generated %1 wires in %2 pieces, %3 triangles, in %4.",
		"Создано проводов: %1, кусков: %2, треугольников: %3, папка: %4.")
		.arg(wires_.size()).arg(num_pieces).arg(triangles_).arg(folder.isEmpty() ? QString::fromUtf8("data") : folder));
	lines_changed();
	notify_changed();
	return true;
}

////////////////////////////////////////////////////////////////////////////////
// Every frame while the tool is active.
////////////////////////////////////////////////////////////////////////////////
bool WireBuilder::eventFilter(QObject *watched, QEvent *event)
{
	if (active_ && event->type() == QEvent::KeyPress)
	{
		const QKeyEvent *key = static_cast<QKeyEvent *>(event);
		if (key->key() == Qt::Key_Escape && (adding_poles_ || isAddingPoints()) && isEscapeFree(watched))
		{
			setAddingPoles(false);
			setAddingPoints(false);
			return true;
		}
	}
	// The clicks on the points of a curve, and on the surfaces while points are added.
	if (active_ && isCurveLine() && editor_.handleEvent(watched, event))
		return true;
	return QObject::eventFilter(watched, event);
}

void WireBuilder::update()
{
	if (!active_)
		return;
	// The poles may have moved since the last frame.
	++frame_;
	poles_cached_ = false;

	// The line was deleted or its world closed.
	if (root_ && !hasLine())
	{
		forget_line();
		adding_poles_ = false;
		discard_if_empty_ = false;
		editor_.setAddingPoints(false);
		lines_changed();
		notify_changed();
	}
	if (!hasLine())
		return;

	// The settings or the poles in the line are not those last written or read: an undo
	// or a redo has put others there.
	{
		const char *data = root_->getData(SETTINGS_DATA);
		if (QByteArray(data ? data : "") != saved_settings_ || dataString(root_, POLES_DATA).toUtf8() != saved_poles_)
		{
			load_settings();
			editor_.setClosed(settings_.closed);
			editor_.setSurfaceFilter(settings_.ground);
			apply_distances_to_pieces();
			// The material may be another one.
			preview_dirty_ = true;
			notify_changed();
		}
	}

	// A line drawn as a curve: the point held follows the mouse, the poles follow the
	// points.
	if (isCurveLine())
	{
		editor_.update();
		// Not while a button is held: the point may be in the hands of the manipulator.
		if (!(QApplication::mouseButtons() & Qt::LeftButton))
			align_points();
		if (pole_snapshot() != poles_state_)
		{
			sync_poles();
			notify_changed();
		}
	}

	const QVector<WireEnds> ends = wire_ends();
	if (snapshot(ends) != state_)
	{
		rebuild(ends);
		notify_changed();
	}
	update_preview();
	update_hidden_pieces();
	draw();
}

void WireBuilder::align_points()
{
	const NodePtr path = editor_.getPathNode();
	if (!path)
		return;

	// Every point looks along the curve, as the points of a road do, with the turn it was
	// given with the manipulator on top (kept in the point). A point turned since the last
	// time (with the manipulator, or back by Ctrl+Z) gets that turn; one the curve turns
	// under (another point moved) is turned with it. Not an undo step: the points only
	// follow the curve.
	const CurvePoints points = editor_.getPoints();
	for (int i = 0; i < points.count() && i < path->getNumChildren(); ++i)
	{
		const NodePtr point = path->getChild(i);
		const Math::quat basis(Math::mat3(curvePointBasis(points.direction(i), 0.0)));
		const Math::Mat4 current = point->getWorldTransform();
		const Math::quat rotation = rotation_of(current);
		// A point seen for the first time (the tool was off, the plugin reloaded) keeps the
		// rotation it has, unless it is a new one that has never been turned along the
		// curve (it has no turn recorded at all).
		const auto placed = placed_points_.constFind(point->getID());
		const bool first = placed == placed_points_.constEnd();
		if (first && !point->hasData(POINT_TURN_DATA))
			point->setData(POINT_TURN_DATA, "");
		else if (first || !same_rotation(rotation, placed.value()))
		{
			const Math::quat turn = Math::inverse(basis) * rotation;
			const bool none = std::abs(std::abs(turn.w) - 1.0f) < 1e-7f;
			point->setData(POINT_TURN_DATA, none ? "" : QString::fromUtf8("%1 %2 %3 %4")
				.arg(double(turn.x), 0, 'g', 9).arg(double(turn.y), 0, 'g', 9)
				.arg(double(turn.z), 0, 'g', 9).arg(double(turn.w), 0, 'g', 9).toUtf8().constData());
		}
		const Math::quat expected = basis * point_turn(point);
		if (!same_rotation(rotation, expected))
		{
			point->setWorldTransform(Math::translate(point->getWorldPosition()) * Math::Mat4(Math::rotate(expected))
				* Math::Mat4(Math::scale(point->getWorldScale())));
		}
		placed_points_.insert(point->getID(), rotation_of(point->getWorldTransform()));
	}
}

void WireBuilder::draw() const
{
	// A line drawn as a curve: the curve, white, and its points.
	if (isCurveLine())
	{
		const CurvePolyline curve = buildPolyline(editor_.getPoints(), 24);
		const Math::vec4 curve_color(1.0f, 1.0f, 1.0f, 0.8f);
		for (int i = 0; i + 1 < curve.samples.size(); ++i)
			Visualizer::renderLine3D(curve.samples[i], curve.samples[i + 1], curve_color, 0.0f, false);
		editor_.drawPoints();
	}

	// The poles in order, a thin line from one to the next; their attach points as dots.
	const QVector<Pole> poles = this->poles();
	const Math::vec4 order_color(0.45f, 0.65f, 0.9f, 0.6f);
	const Math::vec4 point_color(1.0f, 0.85f, 0.2f, 1.0f);
	for (int i = 0; i < poles.size(); ++i)
	{
		if (i > 0 && !isCurveLine())
			Visualizer::renderLine3D(poles[i - 1].pivot, poles[i].pivot, order_color, 0.0f, false);
		for (const AttachPoint &point : poles[i].points)
			Visualizer::renderPoint3D(point.position, 0.012f, point_color, true, 0.0f, false);
	}
	// The segments of the wires: a line along every wire and a short one across it where
	// one segment ends and the next one starts, over everything.
	if (view_ != WIRE_VIEW_SEGMENTS)
		return;
	// Where the line is cut into pieces: a yellow plane across it at the pole, as on a road.
	const Math::vec4 cut_color(1.0f, 0.9f, 0.2f, 0.3f);
	const Math::vec4 cut_edge_color(1.0f, 0.9f, 0.2f, 1.0f);
	for (const CutQuad &cut : cuts_)
	{
		Visualizer::renderQuad3D(cut.corners[0], cut.corners[1], cut.corners[2], cut.corners[3], cut_color, 0.0f, false);
		for (int k = 0; k < 4; ++k)
			Visualizer::renderLine3D(cut.corners[k], cut.corners[(k + 1) % 4], cut_edge_color, 0.0f, false);
	}
	const Math::vec4 wire_color(1.0f, 0.55f, 0.1f, 0.8f);
	const Math::vec4 joint_color(0.45f, 0.65f, 0.9f, 0.9f);
	for (const WireSpan &wire : wires_)
	{
		for (int i = 0; i + 1 < wire.samples.size(); ++i)
			Visualizer::renderLine3D(wire.samples[i], wire.samples[i + 1], wire_color, 0.0f, false);
		for (int i = 0; i < wire.samples.size(); ++i)
		{
			// About the same size on the screen wherever the wire is.
			const BendFrame f = wireFrame(wire, wire.distances[i]);
			const Math::Vec3 half = Math::Vec3(f.right) * Math::Scalar(viewportSizeAt(f.position, 0.006f));
			Visualizer::renderLine3D(f.position - half, f.position + half, joint_color, 0.0f, false);
		}
	}
}

void WireBuilder::setView(WireView view)
{
	view_ = view;
}

void WireBuilder::update_preview()
{
	// Shown while the wires are not generated as they are now.
	bool show = active_ && hasLine() && !wires_.isEmpty();
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
		preview_->setName("artist_tool_wire_preview");
		preview_->setShowInEditorEnabled(false);
		preview_->setSaveToWorldEnabled(false);
		preview_dirty_ = true;
		// The points of the curve are not put on it: it moves with them.
		editor_.setLookThrough(preview_);
	}
	if (preview_dirty_)
	{
		preview_dirty_ = false;
		const Math::Vec3 pivot = wires_.first().from;
		QVector<int> all(wires_.size());
		for (int w = 0; w < wires_.size(); ++w)
			all[w] = w;
		MeshPtr mesh = Mesh::create();
		add_wires(all, pivot, mesh);
		for (int surface = 0; surface < mesh->getNumSurfaces(); ++surface)
		{
			mesh->createTangents(surface);
			mesh->createBounds(surface);
		}
		preview_->setMesh(mesh);
		preview_->setWorldTransform(Math::translate(pivot));
		apply_materials(preview_);
		// The preview is not picked by the clicks: the poles behind it are.
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

void WireBuilder::remove_preview()
{
	if (preview_ && !preview_.isDeleted())
		preview_.deleteLater();
	preview_ = ObjectMeshDynamicPtr();
	editor_.setLookThrough(NodePtr());
	preview_dirty_ = true;
}

void WireBuilder::update_hidden_pieces()
{
	// The preview and the old pieces would overlap.
	const NodePtr holder = mesh_node();
	// Nothing to show instead (no wires at all): the pieces stay in sight.
	if (!holder || holder->getNumChildren() == 0 || wires_.isEmpty() || is_up_to_date())
	{
		show_hidden_pieces();
		return;
	}
	// Not an undo step: the pieces only get out of the way while the line is edited.
	for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
	{
		const NodePtr piece = holder->getChild(i);
		if (piece->getType() == Node::OBJECT_MESH_STATIC && piece->isEnabled())
		{
			piece->setEnabled(false);
			piece->setData(HIDDEN_DATA, "1");
			hidden_pieces_.append(piece->getID());
		}
	}
}

void WireBuilder::show_hidden_pieces()
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
	if (const NodePtr holder = hasLine() ? mesh_node() : NodePtr())
	{
		for (int i = 0, num = holder->getNumChildren(); i < num; ++i)
			show(holder->getChild(i));
	}
}

void WireBuilder::notify_changed() const
{
	if (on_changed)
		on_changed();
}

void WireBuilder::report(int level, const QString &text) const
{
	if (on_message)
		on_message(level, text);
}

} // namespace ArtistTool
