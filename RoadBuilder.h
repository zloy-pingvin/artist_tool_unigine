#pragma once

#include "CurveEditor.h"
#include "MeshBender.h"

#include <UnigineCallback.h>
#include <UnigineMaterial.h>
#include <UnigineObjects.h>
#include <UnigineMathLib.h>
#include <UnigineNode.h>
#include <UnigineVisualizer.h>

#include <QByteArray>
#include <QHash>
#include <QPair>
#include <QObject>
#include <QString>
#include <QStringList>
#include <QVector>

#include <functional>

namespace ArtistTool
{

struct PieceState;

// Where a height of the ground was measured: x, y and, for a raycast, z, in mm.
using GroundKey = QPair<QPair<qint64, qint64>, qint64>;

// What the road is made into.
enum RoadOutput
{
	// An ordinary mesh (ObjectMeshStatic): real geometry with its own material.
	ROAD_OUTPUT_MESH = 0,
	// A mesh decal (DecalMesh): the road is projected onto the ground under it.
	ROAD_OUTPUT_DECAL,
	// A ready mesh of the project (a kerb, a fence, a guard rail) laid along the curve
	// again and again, every copy bent to follow it (ObjectMeshStatic).
	ROAD_OUTPUT_BENT,
};

// What a mesh of a bent road is: a part of the pattern repeated along the curve, or
// the one put once at its start or at its end.
enum RoadBendRole
{
	ROAD_BEND_PATTERN = 0,
	ROAD_BEND_START,
	ROAD_BEND_END,
};

// One mesh of a bent road: the asset it is made of, its scale and the length of the
// curve it takes.
struct RoadBendPart
{
	// The file the node of the asset is made of (a GUID path), what kind of asset it is
	// (PlaceableAsset::Kind) and its name.
	QString source;
	int source_kind{0};
	QString source_name;
	RoadBendRole role{ROAD_BEND_PATTERN};
	// How many times in a row a pattern mesh is put before the next one of the pattern,
	// as in the Path Placer. The start and the end are put once.
	int count{1};
	// The scale of the asset along its own X, Y and Z: the size of the bent mesh.
	Unigine::Math::vec3 scale{1.0f, 1.0f, 1.0f};
	// The length of the curve from the start of this mesh to the start of the next
	// one, in units, as in the Path Placer: more than the mesh - a gap after it, less -
	// the next one overlaps it. With auto_length it is the length of the scaled mesh
	// along the forward axis, measured.
	float length{1.0f};
	bool auto_length{true};
};

// What is drawn over the road in the viewport.
enum RoadView
{
	// The grid the mesh is going to have.
	ROAD_VIEW_GRID = 0,
	// The curve and its points only.
	ROAD_VIEW_CURVE,
};

// How the mesh of a road is built. Stored in the root node of the road.
struct RoadSettings
{
	// Width of the road, in units.
	float width{4.0f};
	// Strips of the grid across the road. More strips let the road follow the bumps
	// of the ground under it.
	int columns{4};
	// The longest a row of the grid may be along the road, in units.
	float step{2.0f};
	// On a bend the rows get shorter: the road turns by at most this angle from one
	// row to the next, in degrees.
	float max_angle{5.0f};
	// The last point is joined to the first one.
	bool closed{false};
	// Every vertex of the grid is put on the surface under it.
	bool snap_to_ground{false};
	// How far above that surface, in units.
	float ground_offset{0.05f};
	// What the road and its points are laid on; everything else is looked through
	// (a road over rocks lies on the terrain under them).
	SurfaceFilter ground;
	// The inner edge of a tight bend is laid along a smooth arc where it gets sharper
	// than this radius or folds over itself; the arc takes in this much of the road
	// before and after that place, in units. 0 - only the folds are rounded.
	float inner_round{2.0f};
	// How much the whole road leans to the side about the curve, in degrees, on top
	// of the roll of its points; positive - the right edge down.
	float roll{0.0f};
	// The road goes along a curve this far to the side of the one drawn, in units:
	// positive - to the right, negative - to the left. The curve is moved, not the
	// mesh: on the outer side of a bend it is longer and takes more (a kerb along the
	// edge of a road drawn by its middle).
	float side_offset{0.0f};
	// Length of the road one repeat of the texture takes, in units.
	float uv_length{4.0f};
	// Repeats of the texture across the road.
	float uv_width{1.0f};
	// The texture turned on the road, in degrees: for a texture drawn with its road
	// going across it, 90.
	float uv_rotation{0.0f};
	// The longest one piece of the road may be, in units; a longer road is cut into
	// pieces, each a node and a mesh of its own. 0 - the road is one piece.
	float piece_length{50.0f};
	RoadOutput output{ROAD_OUTPUT_MESH};
	// The depth a decal is projected to, in units: how far below its mesh it still
	// lies on what is there.
	float decal_depth{2.0f};
	// How high above the ground the decal mesh is put, in units. A decal is projected
	// only down from its mesh, so the ground between the vertices that bulges above
	// it would get no road; a small lift covers that.
	float decal_lift{0.3f};
	// From what distance to the camera the decal starts to fade out, and over what
	// distance it fades out then, in units. 0 - always seen / no fading.
	float decal_visible_distance{0.0f};
	float decal_fade_distance{0.0f};
	// The material of the road: GUID of the .mat file, empty - the base one.
	QString material;
	// The meshes bent along the curve for the Bent Mesh output, in order; their
	// materials go with them. The pattern meshes are repeated one after another;
	// nothing is made without one. At most one start, always the first, and one end,
	// always the last.
	QVector<RoadBendPart> bend_parts;
	// The surfaces of all the meshes with the same material are merged into one surface
	// per piece (a LOD level and the shadow LOD each apart), with the LOD distances and
	// the shadows of the first mesh of the pattern that has it.
	bool bend_merge_materials{true};
	// Which axis of the meshes goes along the curve and which one up.
	AxisDirection bend_forward{AXIS_Y_POSITIVE};
	AxisDirection bend_up{AXIS_Z_POSITIVE};
	// Folder of the project the meshes of the road are saved to, empty - the default
	// one (see RoadBuilder::defaultFolder()).
	QString folder;
};

// A road found in the world.
struct RoadInfo
{
	// ID of its root node.
	int id{0};
	QString name;
	int points{0};
	int pieces{0};
};

// The grid of the mesh a road is built from: rows across the road at "stations"
// along it, every row 'columns + 1' vertices from the left edge to the right one.
struct RoadGrid
{
	int columns{0};
	int stations{0};
	// World positions, row after row.
	QVector<Unigine::Math::Vec3> vertices;
	QVector<Unigine::Math::vec3> normals;
	QVector<Unigine::Math::vec2> uvs;
	// The station each piece of the road starts at; it ends at the start of the next
	// one, the last one at the last station.
	QVector<int> piece_starts;
	double length{0.0};

	bool isEmpty() const { return stations < 2 || columns < 1; }
	int index(int station, int column) const { return station * (columns + 1) + column; }
	int pieceEnd(int piece) const { return piece + 1 < piece_starts.size() ? piece_starts[piece + 1] : stations - 1; }
	int numTriangles() const { return isEmpty() ? 0 : (stations - 1) * columns * 2; }
};

// One bent copy: which mesh (an index into RoadSettings::bend_parts), where along the
// curve, the length of the curve it takes and how much it is stretched to fit.
struct RoadBendCopy
{
	int part{0};
	double start{0.0};
	double length{0.0};
	double fit{1.0};
};

// The copies of ready meshes along the curve, for the Bent Mesh output.
struct RoadBend
{
	// The curve as a dense polyline: its samples, the length along it to every one of
	// them and the way it goes there.
	QVector<Unigine::Math::Vec3> samples;
	QVector<double> distances;
	QVector<Unigine::Math::vec3> tangents;
	// The sample every point of the curve is at (the end of the last span included),
	// and the roll and the width at every point.
	QVector<int> point_samples;
	QVector<double> rolls;
	QVector<double> widths;
	// The roll of the whole road, on top of that of the points.
	double roll{0.0};
	double length{0.0};
	// The copies in order along the curve: the start, the pattern, the end.
	QVector<RoadBendCopy> copies;
	// How many meshes of the pattern there are and how much they are stretched to fill
	// the curve (1 - not at all).
	int pattern_copies{0};
	double pattern_fit{1.0};
	// The half width of the copies to the right and to the left, for drawing their
	// joints.
	double right_extent{0.0};
	double left_extent{0.0};
	// How low and how high the copies go, for drawing the cuts between the pieces.
	double up_min{0.0};
	double up_max{0.0};
	// The copy each piece of the road starts at.
	QVector<int> piece_starts;
	int triangles{0};

	bool isEmpty() const { return copies.isEmpty(); }
	int pieceEnd(int piece) const { return piece + 1 < piece_starts.size() ? piece_starts[piece + 1] : int(copies.size()); }
};

// Builds a mesh along a curve drawn through points: roads, paths, tracks.
//
// The road lives in the world as ordinary nodes, so it is saved with it and can be
// returned to later:
//   <root>
//     path   - its children are the points of the curve, in order
//     mesh   - the pieces of the generated road
// The settings are kept in the data of the root node. While the tool is active the
// grid the mesh is going to have is drawn over the curve; Generate builds the mesh,
// saves it to the project and puts it into "mesh".
class RoadBuilder final : public QObject
{
public:
	enum MessageLevel
	{
		MSG_INFO = 0,
		MSG_WARNING,
		MSG_ERROR,
	};

	explicit RoadBuilder(QObject *parent = nullptr);
	~RoadBuilder() override;

	// The tool draws the road and takes the clicks only while active.
	void setActive(bool active);
	// Makes the road the selected node belongs to the current one. A selection
	// outside of any road leaves the current road as it is.
	void updateFromSelection();

	// All the roads of the world, sorted by name.
	QVector<RoadInfo> findRoads() const;
	// Makes the road with this root node the current one and selects the root in the
	// editor. False if there is no such road any more.
	bool selectRoad(int root_id);
	int getRoadId() const;

	bool hasRoad() const;
	QString getRoadName() const;
	int getNumPoints() const;
	int getNumPieces() const;
	double getLength() const { return grid_.length; }
	const RoadGrid &getGrid() const { return grid_; }
	const RoadBend &getBend() const { return bend_; }
	// The road has something to make: a grid, or copies of the bent mesh.
	bool canGenerate() const { return !grid_.isEmpty() || !bend_.isEmpty(); }
	// The road has a generated mesh that was made for other points or settings.
	bool isOutdated() const;

	// The node new roads (and the copies of roads) are put into; none - the root of the
	// world. It can not be a part of a road.
	bool setParentNode(const Unigine::NodePtr &node);
	Unigine::NodePtr getParentNode() const;

	// A new road with these settings becomes the current one.
	void createRoad(const RoadSettings &settings);
	// A copy of the current road - its points and its settings, no mesh - becomes the
	// current one: e.g. a kerb along a road, moved to the side.
	bool cloneRoad();
	// Removes the curve of the current road and keeps its generated mesh: the pieces go
	// on as ordinary nodes, in a node of the name of the road.
	bool removeRoadCurve();
	// Removes the current road with its mesh; with 'files', the mesh files the road has
	// generated too (they are not brought back by Ctrl+Z).
	bool removeRoad(bool files);
	// While on, a left click on a surface in a viewport adds a point at the end of
	// the road; with Ctrl it is a corner. A road made by createRoad() that is left
	// without points when this is turned off is removed.
	void setAddingPoints(bool adding);
	bool isAddingPoints() const { return editor_.isAddingPoints(); }

	const RoadSettings &getSettings() const { return settings_; }
	// Grows every time the settings are read from a road (another road becomes the
	// current one, an undo puts old ones back): the page shows them then even in the
	// field being typed in.
	int getSettingsRevision() const { return settings_revision_; }
	void setSettings(const RoadSettings &settings);
	// Puts the settings of the road back to their defaults. Its material, folder and
	// "closed" stay: they are not how the road looks but what it is.
	void resetSettings();

	CurvePointsState getSelectedPointsState() const { return editor_.getSelectedPointsState(); }
	void setSelectedPointsCorner(bool corner) { editor_.setSelectedPointsCorner(corner); }
	bool canSubdivide() const { return editor_.canSubdivide(); }
	void subdivide() { editor_.subdivide(); }
	// Whether the road lies on the ground at the selected points or keeps to the
	// height of the curve there, and setting it.
	enum GroundState
	{
		GROUND_NONE = 0,	// no points selected
		GROUND_ON,
		GROUND_OFF,
		GROUND_MIXED,
	};
	GroundState getSelectedPointsGround() const;
	void setSelectedPointsOffGround(bool off_ground) { editor_.setSelectedPointsFlag(CURVE_OFF_GROUND_DATA, off_ground); }
	// The roll of the road at the selected points: how much it leans to the side about
	// the curve, in degrees, positive - the right edge down. It is the turn of the point
	// nodes themselves, so it is set with the rotation manipulator of the editor too.
	// False if no point is selected; 'roll' is that of the first one.
	bool getSelectedPointsRoll(double &roll) const;
	void setSelectedPointsRoll(double roll);
	// The width of the road at the selected points, a multiplier of Width: 1 - Width
	// itself. Between the points it changes smoothly. False if no point is selected;
	// 'scale' is that of the first one.
	bool getSelectedPointsWidth(double &scale) const;
	void setSelectedPointsWidth(double scale);

	// Takes the material selected in the Asset Browser for the road and gives it to
	// its pieces right away. It must suit them: a decal material for decals, a mesh
	// one for meshes.
	bool takeSelectedMaterial();
	// Back to the base material, for the pieces too.
	void clearMaterial();
	// The name of the material the pieces of the road have (it may have been set on
	// them by hand), or of the one the road gives its pieces; empty for the base one.
	QString getMaterialName() const;

	// Adds the assets selected in the Asset Browser to the end of the meshes bent along
	// the curve (Bent Mesh): .mesh, .node or imported models, with their materials.
	bool addSelectedBendParts();
	void removeBendPart(int part);
	// Moves a mesh up (offset -1) or down (+1) the list.
	void moveBendPart(int part, int offset);
	void clearBendParts();
	// Selects the asset of the mesh in the Asset Browser.
	bool showBendPart(int part) const;
	// The length of the curve a copy of the mesh takes: the one set, or the measured
	// one; 0 while the mesh is not loaded.
	double getBendPartLength(int part) const;
	// Why the meshes could not be bent; empty if they can.
	QString getBendError() const;

	// The folder the meshes go to when the road has none of its own: "roads" next to
	// the world.
	static QString defaultFolder();
	QString getFolder() const;
	// Whether the road has files made by Generate, and selecting the first of them in the
	// Asset Browser, which opens their folder there.
	bool hasGeneratedFiles() const;
	bool showGeneratedFiles() const;
	// Takes a folder picked on the disk; it must be inside the data of the project.
	bool setFolderFromDisk(const QString &absolute_path);

	// Forgets the heights of the ground found so far and measures the whole road
	// anew: after the ground under it has changed (the terrain was sculpted, objects
	// moved). While the road is edited, the heights are measured only where the grid
	// has moved.
	void refreshGround();
	// While on, a road that is being changed with the mouse (a point dragged) is not
	// put on the ground until the button is let go: the fastest way to edit a long
	// road over a heavy scene.
	void setSnapAfterRelease(bool on) { snap_after_release_ = on; }
	// What is drawn over the road in the viewport.
	void setView(RoadView view) { view_ = view; }
	// While on, the generated pieces of the road are hidden while they are out of date
	// (the curve or the settings have changed since Generate): the grid or the preview
	// of the new mesh is not mixed up with the old one. They are shown again when the
	// road is generated anew, when the tool is off, and while the world is saved.
	void setHideOutdated(bool hide);
	RoadView getView() const { return view_; }
	// The most triangles a piece of a decal road gets; a longer piece is cut.
	static int maxDecalTriangles();
	bool isSnapAfterRelease() const { return snap_after_release_; }
	// Whether the heights of the ground are read from the Landscape Terrain directly,
	// which is much faster than raycasts: when only Terrain is checked in "Lay On".
	bool isGroundFromTerrain() const;

	// Builds the mesh of the road, saves it to the project and replaces the pieces
	// that are in "mesh" with the new ones. Can be undone (the files stay).
	bool generate();

	// Called when the road, the selection of points or the settings change.
	std::function<void()> on_changed;
	std::function<void(int level, const QString &text)> on_message;
	// Called when a road the tool has removed is gone from the world.
	std::function<void()> on_roads_changed;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	Unigine::NodePtr mesh_node() const;
	bool take_material(const Unigine::MaterialPtr &material);
	void apply_material(const Unigine::MaterialPtr &material);
	// The projection depth and the distances of the settings, to the decal pieces.
	void apply_decal_settings();

	void set_road(const Unigine::NodePtr &root);
	// A name no road of the world has, made from 'base'.
	QString unique_road_name(const QString &base) const;
	// The current road is gone from the world by the tool: it is forgotten and the list
	// of the roads is told once it is gone.
	void forget_removed_road(const Unigine::NodePtr &root);
	void load_settings();
	// Writes the settings into the road as a step of the undo history ('undoable'),
	// or right away for a road being made.
	void save_settings(bool undoable = true, bool merge = true);

	// Everything the grid depends on.
	QByteArray snapshot() const;
	// 'on_ground' off: the grid is not put on the ground, which is much faster - for
	// the frames a point is being moved in.
	void rebuild_grid(bool on_ground = true);
	// The height of the ground under each of the points, NaN where there is none.
	// What was found once is remembered (see ground_cache_).
	void ground_heights(const QVector<Unigine::Math::Vec3> &points, QVector<double> &heights) const;

	// The copies of the bent mesh along the curve (the Bent Mesh output).
	void rebuild_bend(const CurvePoints &points, const QVector<double> &widths, const QVector<int> &point_samples,
		const QVector<Unigine::Math::Vec3> &samples, const QVector<double> &distances);
	// Loads the assets to bend that are not loaded yet.
	void load_bend_parts();
	BendAxes bend_axes(int part) const;
	// The names of the surfaces of the result every surface of every mesh of the list
	// goes to, and what each such surface of the result gets: its material, LOD
	// distances and shadows.
	void bend_names(QVector<QStringList> &names, QHash<QString, BendSource::Surface> &surfaces) const;
	QHash<QString, BendSource::Surface> bend_surfaces() const;
	// A surface found in 'keep' gets the settings it had there instead.
	void apply_bend_materials(const Unigine::ObjectPtr &object, const PieceState *keep = nullptr) const;
	// Adds the bent copies [first, end) to the mesh, relative to 'pivot'.
	void add_bend_copies(int first, int end, const Unigine::Math::Vec3 &pivot, const Unigine::MeshPtr &mesh) const;
	// Where on the curve the bent mesh is at a distance along it, and how it is turned.
	BendFrame bend_frame(double distance) const;
	// The mesh of one piece of the road grid, the positions relative to 'pivot'.
	Unigine::MeshPtr make_road_mesh(int piece, Unigine::Math::Vec3 &pivot) const;
	// The bent copies of one piece, the positions relative to 'pivot'.
	Unigine::MeshPtr bend_mesh(int first_copy, int end_copy, Unigine::Math::Vec3 &pivot) const;
	// The bent mesh shown in the viewport while it is not generated: made anew when the
	// road changes, hidden when its pieces are up to date.
	void update_preview();
	void remove_preview();
	// The pieces were generated for the road as it is now.
	bool is_up_to_date() const;
	// Hides the pieces of an out of date road (see setHideOutdated()) or shows them.
	void update_hidden_pieces();
	void show_hidden_pieces();

	void update();
	// Turns the points to look along the curve, keeping their roll (see
	// curvePointBasis()), so that the local Y ring of the manipulator rolls them.
	void align_points();
	void draw() const;

	void notify_changed() const;
	void report(int level, const QString &text) const;

	Unigine::NodePtr root_;
	Unigine::NodePtr parent_;
	RoadSettings settings_;
	CurveEditor editor_;
	RoadGrid grid_;
	RoadBend bend_;
	// The meshes to bend by the files they are loaded from, and the errors of those
	// that could not be.
	QHash<QString, BendSource> bend_sources_;
	QHash<QString, QString> bend_errors_;
	Unigine::ObjectMeshDynamicPtr preview_;
	// The preview is made anew only when the copies change.
	bool preview_dirty_{false};
	bool hide_outdated_{true};
	// The pieces hidden by update_hidden_pieces(), by ID.
	QVector<int> hidden_pieces_;
	// What the grid was last built for.
	QByteArray grid_state_;
	// The grid was built without putting it on the ground while a point was dragged
	// (snap_after_release_); it is put there when the mouse button is let go.
	bool grid_draft_{false};
	bool snap_after_release_{false};
	RoadView view_{ROAD_VIEW_GRID};
	// The settings as last written into the road or read from it: when the road has
	// others (an undo has put the old ones back), they are read again.
	QByteArray saved_settings_;
	int settings_revision_{0};
	// The heights of the ground found so far, by the place they were measured at (in
	// millimetres). The ground under the road does not change while its points are
	// moved, so only the rows that have moved need measuring.
	mutable QHash<GroundKey, double> ground_cache_;
	mutable Unigine::Vector<Unigine::LandscapeFetchPtr> fetches_;

	bool active_{false};
	// The current road has just been made by createRoad(): if adding points to it ends
	// with no points, it is removed.
	bool discard_if_empty_{false};
	// Such a road after it has been removed, until it is gone from the world.
	Unigine::NodePtr removed_root_;
	int removed_root_frames_{0};

	Unigine::EventConnections connections_;
	Unigine::Visualizer::MODE saved_visualizer_mode_{Unigine::Visualizer::MODE_DISABLED};
};

} // namespace ArtistTool
