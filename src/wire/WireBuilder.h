#pragma once

#include "CurveEditor.h"
#include "MeshBender.h"
#include "WireShape.h"

#include <UnigineCallback.h>
#include <UnigineMaterial.h>
#include <UnigineNode.h>
#include <UnigineObjects.h>
#include <UnigineVisualizer.h>

#include <QByteArray>
#include <QRegularExpression>
#include <QHash>
#include <QObject>
#include <QPair>
#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace ArtistTool
{

struct PieceState;

// How the sag of a wire is set.
enum WireSagMode
{
	// The same number of units for every wire, whatever its length.
	WIRE_SAG_UNITS = 0,
	// A part of the length of the wire, in percent: a twice longer wire hangs twice as
	// low.
	WIRE_SAG_PERCENT,
	// As wires pulled with the same force: the sag of a wire 10 units long, and a twice
	// longer one hangs four times as low - the way real wires between poles do.
	WIRE_SAG_TENSION,
};

// How the twist of the shape about the wire is set.
enum WireTwistMode
{
	// Turns per unit of length: the same twist on every wire.
	WIRE_TWIST_PER_UNIT = 0,
	// Turns from one end of a wire to the other, whatever its length.
	WIRE_TWIST_PER_SPAN,
};

// What is drawn over the wires in the viewport.
enum WireView
{
	// The segments: a line along every wire and a short one across it at every joint.
	WIRE_VIEW_SEGMENTS = 0,
	// Nothing: only the wires themselves and the attach points.
	WIRE_VIEW_NONE,
};

// Which poles of a line drawn as a curve an asset of the list is for.
enum WirePoleRole
{
	// The poles along the line; several such assets take turns.
	WIRE_POLE_PATTERN = 0,
	// The first pole of the line, the last one, the poles at the corner points.
	WIRE_POLE_START,
	WIRE_POLE_END,
	WIRE_POLE_CORNER,
	// Not a pole: put on every attach point of every pole, turned as the point is; the wire
	// is tied to the attach point inside it. Several such assets - one of them at random.
	WIRE_POLE_INSULATOR,
};

// An asset the poles of a line drawn as a curve are made of.
struct WirePolePart
{
	// The file the node of the asset is made of (a GUID path), what kind of asset it is
	// (PlaceableAsset::Kind) and its name.
	QString source;
	int source_kind{0};
	QString source_name;
	WirePoleRole role{WIRE_POLE_PATTERN};
};

// How the wires of a line are made. Stored in the root node of the line.
struct WireSettings
{
	// The attach points of a pole are the nodes inside it named this and a number:
	// "wire_1", "wire2", "wire.03"... The wire of number N goes from the point N of one
	// pole to the point N of the next one. A pole with no such points is one point
	// itself, all the wires go to it.
	QString attach_name{QString::fromUtf8("wire")};
	// The last pole is joined to the first one.
	bool closed{false};
	// The poles of a line drawn as a curve: the assets they are made of, a pole at every
	// point of the curve and, with pole_every, between the points every pole_spacing units
	// of it, the axes of the assets that go along the line and up, and whether they stand
	// on the ground under the curve.
	QVector<WirePolePart> pole_parts;
	bool pole_every{false};
	float pole_spacing{30.0f};
	AxisDirection pole_forward{AXIS_Y_POSITIVE};
	AxisDirection pole_up{AXIS_Z_POSITIVE};
	bool pole_on_ground{true};
	// What the points and the poles are put on; the rest is looked through (poles on the
	// terrain under the rocks).
	SurfaceFilter ground;
	WireSagMode sag_mode{WIRE_SAG_TENSION};
	// Units, percent or the sag of a 10 units long wire, by the mode.
	float sag{0.15f};
	// How much more than 'sag' a wire may hang, in percent of it: every wire gets a sag
	// from 'sag' to 'sag' plus this part, never less.
	float sag_random{25.0f};
	// How the wires are cut into segments: a wire turns by at most this many degrees from
	// one segment to the next (more segments where it bends more), has at least
	// min_segments of them, and none longer than max_segment_length units (0 - no limit).
	float segment_angle{2.0f};
	// A wire that would hang below the ground lies on it there instead.
	bool above_ground{false};
	int min_segments{4};
	float max_segment_length{0.0f};
	WireShapeKind shape{WIRE_SHAPE_CROSS};
	// The width of a strip, the diameter of a tube, in units.
	float thickness{0.04f};
	int sides{4};
	// The turn of the shape about the wire at its start, in degrees; with
	// random_rotation, every wire gets a turn of its own instead.
	float rotation{0.0f};
	bool random_rotation{false};
	WireTwistMode twist_mode{WIRE_TWIST_PER_SPAN};
	// Turns by the mode; negative - the other way.
	float twist{0.0f};
	// The most a strip or a tube turns by the twist from one joint to the next, in degrees:
	// a long segment gets joints in between. 0 - no joints for the twist.
	float twist_step{90.0f};
	// The length of the wire one repeat of the texture takes, in units.
	float uv_length{1.0f};
	// Repeats of the texture across a strip, around a tube.
	float uv_width{1.0f};
	// How the texture is turned on the wire, in degrees: 0 - the wire goes along V, 90 -
	// along U (a wire drawn in the texture from left to right).
	float uv_rotation{0.0f};
	// The material of a strip or a tube: GUID of the .mat file, empty - the base one.
	QString material;
	// The ready mesh laid along the wires (WIRE_SHAPE_MESH): the file the node of the
	// asset is made of (a GUID path), what kind of asset it is and its name.
	QString mesh_source;
	int mesh_kind{0};
	QString mesh_name;
	AxisDirection mesh_forward{AXIS_Y_POSITIVE};
	AxisDirection mesh_up{AXIS_Z_POSITIVE};
	// The scale of the mesh, all its axes.
	float mesh_scale{1.0f};
	// The mesh is cut where the wire bends or twists it by more than this many degrees
	// across one of its edges: it bends and twists smoothly, and the straight parts get no
	// more triangles. 0 - as it is.
	float mesh_angle{0.0f};
	// The wires are cut into pieces of the world grid of this size, in units: every
	// piece is a node and a mesh of its own, with the wires whose middle is in its
	// square. 0 - the line is one piece.
	float piece_size{100.0f};
	// From what distance to the camera the wires start to fade out, and over what distance
	// they fade out then, in units, as on a decal. 0 - always seen / no fading.
	float visible_distance{0.0f};
	float fade_distance{0.0f};
	// A LOD of the strips and the tubes: the same wires with lod_factor times fewer
	// segments, seen from lod_distance on, faded over lod_fade.
	bool lod{false};
	int lod_factor{3};
	float lod_distance{50.0f};
	float lod_fade{5.0f};
	// Another number - other random sags and turns.
	int seed{1};
	// Folder of the project the meshes of the line are saved to, empty - the default
	// one (see WireBuilder::defaultFolder()).
	QString folder;
};

// A line of wires found in the world.
struct WireLineInfo
{
	// ID of its root node.
	int id{0};
	QString name;
	int poles{0};
	int pieces{0};
};

// Strings poles with wires: power lines, cables between houses, garlands.
//
// A line lives in the world as ordinary nodes, so it is saved with it. Most lines are
// drawn as a curve, the poles are made by the tool:
//   <root>   - the settings are in its data
//     path   - its children are the points of the curve, in order
//     poles  - the poles, a pole at every point and between them (made by the tool)
//     mesh   - the pieces of the generated wires
// A line that connects poles already in the world has no "path" and "poles": its poles
// are nodes of the world anywhere, told by their IDs in the data of the root.
// Every wire hangs between two attach points as a real wire does under its weight (a
// catenary). While the tool is active the wires are shown in the viewport and follow the
// poles when they are moved; Generate saves them to the project and puts them into "mesh".
class WireBuilder final : public QObject
{
public:
	enum MessageLevel
	{
		MSG_INFO = 0,
		MSG_WARNING,
		MSG_ERROR,
	};

	explicit WireBuilder(QObject *parent = nullptr);
	~WireBuilder() override;

	// The tool shows the wires and takes the poles only while active.
	void setActive(bool active);
	// The editor selection has changed: while poles are added, the newly selected nodes
	// become poles; else the line the selected node belongs to becomes the current one.
	void selectionChanged();

	// All the lines of the world, sorted by name.
	QVector<WireLineInfo> findLines() const;
	// Makes the line with this root node the current one and selects the root in the
	// editor. False if there is no such line any more.
	bool selectLine(int root_id);
	int getLineId() const;

	bool hasLine() const;
	QString getLineName() const;
	int getNumPoles() const;
	int getNumPieces() const;
	const QVector<WireSpan> &getWires() const { return wires_; }
	double getLength() const;
	int getTriangles() const { return triangles_; }
	// Whether any wire of the line is twisted: by the line or by a point of its curve.
	bool hasTwist() const;
	// The line has a generated mesh that was made for other poles or settings.
	bool isOutdated() const;

	// A new line drawn as a curve, with these settings, becomes the current one, adding
	// points.
	void createLine(const WireSettings &settings);
	// A new line that connects poles already in the world becomes the current one,
	// adding poles.
	void createLinkLine(const WireSettings &settings);
	// Removes the current line with its mesh and the poles it has made; with 'files', the
	// mesh files the line has generated too (they are not brought back by Ctrl+Z).
	bool removeLine(bool files);
	// The current line is drawn as a curve (else it connects poles of the world).
	bool isCurveLine() const;
	// A copy of the current line - its points or poles and its settings, no mesh -
	// becomes the current one.
	bool cloneLine();
	// Removes the curve of the current line and keeps its poles and its generated mesh:
	// they go on as ordinary nodes, in a node of the name of the line.
	bool removeLineCurve();
	// The node new lines (and the copies of lines) are put into; none - the root of the
	// world. It can not be a part of a line.
	bool setParentNode(const Unigine::NodePtr &node);
	Unigine::NodePtr getParentNode() const;

	// A line drawn as a curve: while on, a left click on a surface in a viewport adds a
	// point at the end of the curve, a pole stands there; with Ctrl it is a corner. A
	// line made by createLine() that is left without points is removed.
	void setAddingPoints(bool adding);
	bool isAddingPoints() const { return editor_.isAddingPoints(); }
	int getNumPoints() const;
	CurvePointsState getSelectedPointsState() const { return editor_.getSelectedPointsState(); }
	// What the selected points set for the span from them to the next point: a multiplier
	// of the sag of the line, and a random of their own in percent (negative - that of
	// the line). False if no point is selected; the values are those of the first one.
	bool getSelectedPointsSag(double &scale, double &random) const;
	void setSelectedPointsSagScale(double scale);
	void setSelectedPointsSagRandom(double random);
	// The same for the shape of the span: a multiplier of the thickness of the line (1 - as
	// the line), and turns of the twist added to those of the line, by its mode (0 - as the
	// line).
	bool getSelectedPointsShape(double &thickness, double &twist) const;
	void setSelectedPointsThickness(double thickness);
	void setSelectedPointsTwist(double twist);
	void setSelectedPointsCorner(bool corner) { editor_.setSelectedPointsCorner(corner); }
	bool canSubdivide() const { return editor_.canSubdivide(); }
	void subdivide() { editor_.subdivide(); }

	// The assets of the poles of a line drawn as a curve: the ones selected in the Asset
	// Browser are added to the end of the list.
	bool addSelectedPoleParts();
	// Puts another asset into the part (picked in the asset dialog or dropped onto it); its
	// role stays.
	bool replacePolePart(int part, const Unigine::Vector<Unigine::UGUID> &guids);
	void removePolePart(int part);
	void setPolePartRole(int part, WirePoleRole role);
	void clearPoleParts();
	bool showPolePart(int part) const;
	// The points turned with the manipulator, and their poles, look along the curve again.
	void resetPoleTurns();
	// Some poles made of an asset have no attach points: their wires hang from their
	// bottom.
	bool hasPolesWithoutPoints() const;

	// A line that connects poles of the world: while on, every node selected in the
	// editor is added to the end of the line as a pole. A line made by createLinkLine()
	// that is left without poles is removed.
	void setAddingPoles(bool adding);
	bool isAddingPoles() const { return adding_poles_; }
	// Adds the nodes selected now to the end of the line, in the order of selecting.
	bool addSelectedPoles();
	void removeLastPole();
	void clearPoles();

	const WireSettings &getSettings() const { return settings_; }
	// Grows every time the settings are read from a line: the page shows them then even
	// in the field being typed in.
	int getSettingsRevision() const { return settings_revision_; }
	void setSettings(const WireSettings &settings);
	// Puts the settings of the line back to their defaults. What it is made of (the
	// shape, the material, the mesh), its folder and Closed stay.
	void resetSettings();

	// Takes the first material among the GUIDs (picked in the asset dialog or dropped:
	// assets, runtimes, materials) for the strips or the tube and gives it to the pieces
	// right away.
	bool takeMaterial(const Unigine::Vector<Unigine::UGUID> &guids);
	bool showMaterial() const;
	void clearMaterial();
	QString getMaterialName() const;
	// Takes the asset among the GUIDs as the mesh laid along the wires.
	bool takeMesh(const Unigine::Vector<Unigine::UGUID> &guids);
	// Selects that asset in the Asset Browser.
	bool showMesh() const;
	// Why the mesh can not be laid along the wires; empty if it can.
	QString getMeshError() const;
	// Why there are no wires although there are poles (too many triangles); empty if not that.
	QString getRebuildError() const { return rebuild_error_; }

	// The folder the meshes go to when the line has none of its own: "wires" next to
	// the world.
	static QString defaultFolder();
	QString getFolder() const;
	bool hasGeneratedFiles() const;
	bool showGeneratedFiles() const;
	// Takes a folder picked on the disk; it must be inside the data of the project.
	bool setFolderFromDisk(const QString &absolute_path);

	// What is drawn over the wires in the viewport.
	void setView(WireView view);
	WireView getView() const { return view_; }

	// Makes the wires, saves them to the project and replaces the pieces that are in
	// "mesh" with the new ones. Can be undone (the files stay).
	bool generate();

	// Called when the line, its poles or the settings change.
	std::function<void()> on_changed;
	std::function<void(int level, const QString &text)> on_message;
	// Called when a line the tool has made or removed appears in the world or is gone.
	std::function<void()> on_lines_changed;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	// An attach point found on a pole.
	struct AttachPoint
	{
		int number{0};
		Unigine::Math::Vec3 position;
		Unigine::Math::quat rotation;
	};
	// One wire to make: between which poles (their IDs) and which points.
	struct WireEnds
	{
		int pole_a{0};
		int pole_b{0};
		int number{0};
		Unigine::Math::Vec3 from;
		Unigine::Math::Vec3 to;
		// What the point of the curve the wire goes from sets for its span: a multiplier
		// of the sag, and a random of its own in percent (negative - that of the line).
		double sag_scale{1.0};
		double sag_random{-1.0};
		// And for its shape: a multiplier of the thickness, turns added to the twist.
		double thickness_scale{1.0};
		double twist_add{0.0};
	};

	// A pole of the line: the node (none - a point of the curve with no pole asset, the
	// wires hang from the point itself), a number that tells it from the others (its ID,
	// or the number of the point) and its attach points.
	struct Pole
	{
		Unigine::NodePtr node;
		int id{0};
		Unigine::Math::Vec3 pivot;
		QVector<AttachPoint> points;
		// The point of the curve whose span the pole is in; -1 - a line of poles.
		int span{-1};
	};
	// Where a pole of a line drawn as a curve stands, and of which asset.
	struct PoleSlot
	{
		Unigine::Math::Vec3 position;
		Unigine::Math::Vec3 direction;
		// An index into WireSettings::pole_parts, -1 - none.
		int part{-1};
		// The point of the curve the pole stands at; -1 - one between the points.
		int point{-1};
		// The point of the curve whose span the pole is in: the one it stands at, or the one
		// before it.
		int span{-1};
	};

	Unigine::NodePtr mesh_node() const;
	Unigine::NodePtr poles_node() const;
	// The root of the line a new one is: its "mesh" node, and for a curve "path" and
	// "poles" too.
	void create_root(const WireSettings &settings, bool curve);
	void set_line(const Unigine::NodePtr &root);
	void forget_line();
	QString unique_line_name(const QString &base) const;

	void load_settings();
	void save_settings(bool undoable = true, bool merge = true);
	QVector<int> pole_ids() const;
	void set_pole_ids(const QVector<int> &ids, bool undoable = true);
	// The node of the world a selected node stands for as a pole: the nearest node over
	// it that has attach points. Null for a node with none and for a part of a line.
	Unigine::NodePtr pole_of(const Unigine::NodePtr &node) const;
	// What the point sets for its span (see getSelectedPointsSag()).
	void point_sag(const Unigine::NodePtr &point, double &scale, double &random) const;
	void point_shape(const Unigine::NodePtr &point, double &thickness, double &twist) const;
	// Tells which selected nodes were not taken as poles: they have no attach points.
	void report_skipped(const QStringList &names) const;
	// The attach points of a pole sorted by number; none - the pole is one point. Where an
	// insulator stands on a point, the point is the one inside the insulator, unless
	// 'own' - the points of the pole itself.
	QVector<AttachPoint> attach_points(const Unigine::NodePtr &pole, bool own = false) const;
	// Whether an attach point is a child of the node itself (or of its reference).
	bool has_own_attach_point(const Unigine::NodePtr &node) const;
	// The names of the attach points, and of the tie of an insulator, as patterns.
	const QRegularExpression &attach_pattern() const;
	const QRegularExpression &tie_pattern() const;
	// The poles of the line found anew (see poles()).
	QVector<Pole> compute_poles() const;
	// What the rays to the ground look through.
	const Unigine::Vector<Unigine::NodePtr> &ground_ignore() const;
	// The other lines of the world, for ground_ignore(); and what to do when the lines of
	// the world change.
	void refresh_other_lines();
	void lines_changed();
	// What is found from the settings is found again.
	void settings_changed();
	// The selections seen while poles are picked (see selectionChanged()).
	void remember_selection(const QSet<int> &selected);
	void discard_empty_line(bool poles);
	static PlaceableAsset asset_of(const WirePolePart &part);
	PlaceableAsset mesh_asset() const;
	WireLook look() const;
	// Whether the list has assets of poles, not only insulators: with only insulators, they
	// stand at the points of the curve themselves, as poles.
	bool has_pole_assets() const;
	// The node inside an insulator (or a pole that is one point) the wire is tied to: named
	// as the attach points, with a number or without ("wire", "wire_1"). Null if none.
	Unigine::NodePtr find_tie(const Unigine::NodePtr &node) const;
	// Where the wires of a pole that is one point go from: its tie node, or the pole itself.
	Unigine::Math::Vec3 pole_pivot(const Unigine::NodePtr &pole) const;
	// Puts the insulators on the attach points of the pole; 'key' - a number of the pole that
	// stays the same while it stands, for the random pick of the insulators.
	void sync_insulators(const Unigine::NodePtr &pole, quint64 key);

	// The poles of the line in order, with their attach points.
	QVector<Pole> poles() const;
	// Where the poles of a line drawn as a curve stand.
	QVector<PoleSlot> pole_slots() const;
	// Everything the places of the poles depend on.
	QByteArray pole_snapshot() const;
	// Puts the poles of a line drawn as a curve where they stand: the ones that are there
	// are moved, the missing ones made, the extra ones removed. Not a step of the undo
	// history: the poles follow the curve, as the objects of a path do.
	void sync_poles();
	Unigine::Math::Vec3 on_ground(const Unigine::Math::Vec3 &position) const;
	// Lifts the part of a wire that goes below the ground onto it (see
	// WireSettings::above_ground).
	void keep_above_ground(WireSpan &span) const;

	// The wires the line has now, from its poles.
	QVector<WireEnds> wire_ends() const;
	// Everything the wires depend on.
	QByteArray snapshot(const QVector<WireEnds> &ends) const;
	void rebuild(const QVector<WireEnds> &ends);
	// The poles the line is cut into pieces at (see cuts_).
	void find_cuts();

	// The material of the strips and the tubes, for the line and its pieces at once.
	void set_material(const QString &material);
	void load_mesh_source();
	BendAxes mesh_axes() const;
	// The names of the surfaces of the mesh laid along the wires, one per surface of
	// the source.
	QStringList mesh_surface_names() const;
	// Gives the surfaces of a piece (or of the preview) their materials: the one of the
	// settings for strips and tubes, those of the asset for a mesh. A surface found in
	// 'keep' gets the settings it had there instead.
	void apply_materials(const Unigine::ObjectPtr &object, const PieceState *keep = nullptr) const;
	// The visible and the fade distance of the settings, to the surfaces of a piece (or of
	// the preview); a mesh keeps the LOD distances of its asset within them.
	void apply_distances(const Unigine::ObjectPtr &object) const;
	// The same to all the pieces of the line: they get the distances at once.
	void apply_distances_to_pieces() const;
	// Adds the wires to the mesh, the positions relative to 'pivot'.
	void add_wires(const QVector<int> &wires, const Unigine::Math::Vec3 &pivot, const Unigine::MeshPtr &mesh) const;
	// The wires of every piece: those whose middle is in one square of the grid.
	QVector<QVector<int>> pieces() const;

	void update_preview();
	void remove_preview();
	bool is_up_to_date() const;
	void update_hidden_pieces();
	void show_hidden_pieces();

	void update();
	// Turns the points of a curve to look along it, each with the turn it was given with
	// the manipulator on top (see sync_poles()).
	void align_points();
	void draw() const;

	void notify_changed() const;
	void report(int level, const QString &text) const;

	Unigine::NodePtr root_;
	Unigine::NodePtr parent_;
	// The points of a line drawn as a curve.
	CurveEditor editor_;
	// What the poles were last put for, and where they stand then.
	QByteArray poles_state_;
	QVector<PoleSlot> slots_;
	// The span of the curve every pole of the 'poles' node is in, in their order: a pole
	// whose asset is gone leaves its slot without one.
	QVector<int> pole_spans_;
	// The rotations the points of the curve were last given or found with, by ID: a point
	// that has another one now was turned with the manipulator.
	QHash<int, Unigine::Math::quat> placed_points_;
	// The heights of the ground under the wires found so far, by the place (a 5 cm grid);
	// NO_GROUND - no ground there (see no_ground() in WireBuilder.cpp: not a NaN).
	mutable QHash<QPair<QPair<qint64, qint64>, qint64>, double> ground_heights_;
	// A pole could not be made the last time: told once, not every frame.
	bool pole_error_reported_{false};
	WireSettings settings_;
	QByteArray saved_settings_;
	QByteArray saved_poles_;
	int settings_revision_{0};

	QVector<WireSpan> wires_;
	// The poles every wire goes between (their IDs), from - to.
	QVector<QPair<int, int>> wire_poles_;
	// Where the line is cut into pieces: a plane across it at a pole, drawn yellow.
	struct CutQuad
	{
		Unigine::Math::Vec3 corners[4];
	};
	QVector<CutQuad> cuts_;
	int triangles_{0};
	// What the wires were last made for.
	QByteArray state_;

	BendSource mesh_source_;
	QString mesh_source_path_;
	QString mesh_error_;

	Unigine::ObjectMeshDynamicPtr preview_;
	bool preview_dirty_{true};
	QVector<int> hidden_pieces_;

	WireView view_{WIRE_VIEW_SEGMENTS};
	bool active_{false};
	bool adding_poles_{false};
	// The nodes that were selected the last time the selection changed: only the ones
	// selected after that are added as poles.
	QSet<int> selected_before_;
	// Started by Ctrl+Z / Ctrl+Y: the selection the undo puts back is not a pole picked.
	QVector<QSet<int>> recent_selections_;
	// The frame of update(), and what is found once a frame.
	int frame_{0};
	mutable bool poles_cached_{false};
	mutable QVector<Pole> poles_cache_;
	mutable int ground_ignore_frame_{-1};
	mutable Unigine::Vector<Unigine::NodePtr> ground_ignore_;
	QVector<int> other_line_ids_;
	// The patterns of the attach names, for the name they were made for.
	mutable QString patterns_name_;
	mutable QRegularExpression attach_pattern_;
	mutable QRegularExpression tie_pattern_;
	// The settings as the snapshot of the wires has them.
	mutable QByteArray snapshot_settings_;
	mutable bool snapshot_settings_valid_{false};
	// The heights of the ground under the poles, as ground_heights_.
	mutable QHash<QPair<QPair<qint64, qint64>, qint64>, double> pole_ground_;
	// What the insulators were last made of.
	QByteArray insulator_signature_;
	QString rebuild_error_;
	// The current line has just been made by createLine(): if adding poles to it ends
	// with no poles, it is removed.
	bool discard_if_empty_{false};

	Unigine::EventConnections connections_;
	Unigine::Visualizer::MODE saved_visualizer_mode_{Unigine::Visualizer::MODE_DISABLED};
};

} // namespace ArtistTool
