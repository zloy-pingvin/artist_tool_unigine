#pragma once

#include "PlaceableAsset.h"
#include "ViewportPicking.h"

#include <UnigineCallback.h>
#include <UnigineGUID.h>
#include <UnigineMathLib.h>
#include <UnigineNode.h>
#include <UnigineObjects.h>
#include <UnigineString.h>

#include <QElapsedTimer>
#include <QObject>
#include <QPoint>
#include <QPointF>
#include <QPointer>
#include <QString>

#include <functional>

class QWidget;

namespace ArtistTool
{

struct PlacerOptions
{
	// Which axis of the asset points away from the surface ("up").
	enum UpAxis
	{
		UP_AXIS_Z_POSITIVE = 0,
		UP_AXIS_Z_NEGATIVE,
		UP_AXIS_X_POSITIVE,
		UP_AXIS_X_NEGATIVE,
		UP_AXIS_Y_POSITIVE,
		UP_AXIS_Y_NEGATIVE,
	};

	// Kinds of objects that can be placed on (same set as the Cluster Paint filter).
	enum SurfaceType
	{
		SURFACE_MESH_STATIC = SurfaceFilter::MESH_STATIC,
		SURFACE_MESH_SKINNED = SurfaceFilter::MESH_SKINNED,
		SURFACE_MESH_DYNAMIC = SurfaceFilter::MESH_DYNAMIC,
		SURFACE_MESH_CLUSTER = SurfaceFilter::MESH_CLUSTER,
		SURFACE_MESH_CLUTTER = SurfaceFilter::MESH_CLUTTER,
		SURFACE_TERRAIN = SurfaceFilter::TERRAIN,
		SURFACE_WATER = SurfaceFilter::WATER,
		SURFACE_CLOUDS = SurfaceFilter::CLOUDS,
		SURFACE_ALL = SurfaceFilter::ALL,
	};

	int surface_types{SURFACE_ALL};
	bool only_immovable{false};
	// Only surfaces with the Intersection flag and a non-zero Intersection Mask.
	bool only_intersection{false};

	UpAxis up_axis{UP_AXIS_Z_POSITIVE};
	// Align the "up" of the object with the normal of the clicked surface.
	bool normal_orientation{true};
	// How far from the surface the object is put, in units, along its up axis: the
	// normal of the surface, or straight up without the normal orientation - like
	// Offset of the editor's Snap to Surface. The same for every object.
	float surface_offset{0.0f};
	// While the mouse button that placed an object is still held, the object follows
	// the mouse over the surfaces; releasing the button leaves it there.
	bool move_while_held{true};

	// Every placed object gets a random value between min and max. "Local" values are
	// applied relative to the surface (when oriented by its normal), "world" ones in
	// world coordinates. Offsets are in units, rotations in degrees.
	Unigine::Math::vec3 local_offset_min{0.0f, 0.0f, 0.0f};
	Unigine::Math::vec3 local_offset_max{0.0f, 0.0f, 0.0f};
	Unigine::Math::vec3 world_offset_min{0.0f, 0.0f, 0.0f};
	Unigine::Math::vec3 world_offset_max{0.0f, 0.0f, 0.0f};
	Unigine::Math::vec3 local_rotation_min{0.0f, 0.0f, 0.0f};
	Unigine::Math::vec3 local_rotation_max{0.0f, 0.0f, 0.0f};
	Unigine::Math::vec3 world_rotation_min{0.0f, 0.0f, 0.0f};
	Unigine::Math::vec3 world_rotation_max{0.0f, 0.0f, 0.0f};
	float scale_min{1.0f};
	float scale_max{1.0f};
};

// Click-to-place tool: while enabled, every left click in an editor viewport drops
// the current asset onto the geometry under the cursor (the same way the "Single
// Object" tool of the Cluster Paint mode places a mesh). A disc under the cursor
// shows where the object will go; it turns red where placing is not allowed.
//
// Clicks are taken at the Qt level, before the viewport gets them, so the editor
// does not also handle them as a selection.
class ObjectPlacer final : public QObject
{
public:
	// What is placed and what node it becomes, see PlaceableAsset.
	using Asset = PlaceableAsset;
	using AssetKind = PlaceableAsset::Kind;
	static constexpr AssetKind ASSET_NONE = PlaceableAsset::KIND_NONE;
	static constexpr AssetKind ASSET_NODE_REFERENCE = PlaceableAsset::KIND_NODE_REFERENCE;
	static constexpr AssetKind ASSET_MODEL = PlaceableAsset::KIND_MODEL;
	static constexpr AssetKind ASSET_MESH = PlaceableAsset::KIND_MESH;

	enum MessageLevel
	{
		MSG_INFO = 0,
		MSG_WARNING,
		MSG_ERROR,
	};

	// Point of the surface under the mouse.
	using Hit = SurfaceHit;

	explicit ObjectPlacer(QObject *parent = nullptr);
	~ObjectPlacer() override;

	// Takes the asset selected in the Asset Browser, if it is a mesh or a node.
	// Any other selection leaves the current asset as it is.
	void updateAssetFromSelection();
	const Asset &getAsset() const { return asset_; }
	static QString kindLabel(AssetKind kind);

	// Place empty Node Dummy nodes instead of the asset.
	void setPlaceDummy(bool dummy);
	bool isPlaceDummy() const { return place_dummy_; }
	// Whether there is something to place: a dummy needs no asset.
	bool hasObject() const { return place_dummy_ || asset_.kind != ASSET_NONE; }
	// Name of what is placed, for messages.
	QString getObjectName() const;

	// Placed nodes become children of this node (world root if null).
	void setParentNode(const Unigine::NodePtr &node);
	Unigine::NodePtr getParentNode() const;

	void setOptions(const PlacerOptions &options) { options_ = options; }

	void setEnabled(bool enabled);
	bool isEnabled() const { return enabled_; }
	int getPlacedCount() const { return placed_count_; }

	// Called when the asset, the enabled state or the placed count changes.
	std::function<void()> on_changed;
	std::function<void(int level, const QString &text)> on_message;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	// The random part of where and how an object stands.
	struct Placement
	{
		Unigine::Math::mat4 local_rotation;
		Unigine::Math::mat4 world_rotation;
		Unigine::Math::vec3 local_offset;
		Unigine::Math::vec3 world_offset;
		float scale{1.0f};
	};

	Placement random_placement() const;
	Unigine::Math::Mat4 make_transform(const Hit &hit, const Placement &placement) const;
	void turn_around_up(float angle);
	void switch_asset(int direction);
	void register_node(const Unigine::NodePtr &node);
	void end_drag();

	QWidget *find_viewport_widget(QObject *watched) const;
	void log_ignored_click(QObject *watched);
	Hit pick(QWidget *viewport, const QPointF &position) const;
	void update_cursor();
	void show_cursor(const Hit &hit);
	void set_cursor_color(bool allowed);
	void hide_cursor();
	void remove_cursor();
	bool place(QWidget *viewport, const QPointF &position);
	Unigine::NodePtr create_node() const;
	void notify_changed() const;
	void report(int level, const QString &text) const;

	Asset asset_;
	bool place_dummy_{false};
	PlacerOptions options_;
	Unigine::NodePtr parent_;

	bool enabled_{false};
	bool swallow_release_{false};
	int placed_count_{0};
	int ignored_clicks_logged_{0};
	bool cursor_logged_{false};

	// The object placed by the press that is still held, and its random part.
	Unigine::NodePtr drag_node_;
	Placement drag_placement_;
	// The surface point it stands on and how far the wheel has turned it, in degrees.
	Hit drag_hit_;
	float drag_turn_{0.0f};
	// The object starts to follow the mouse only after the button has been held for a
	// moment and the mouse has moved away from where it was pressed - an ordinary
	// click, with the hand shaking a little, must not shift it.
	QElapsedTimer drag_timer_;
	QPoint drag_press_position_;
	bool drag_moving_{false};

	// Viewport the mouse is over, tracked from the mouse events.
	QPointer<QWidget> hover_viewport_;
	// The disc shown under the mouse: a helper object that is never saved to the
	// world and is not listed in the World Hierarchy.
	Unigine::ObjectMeshDynamicPtr cursor_;
	// Color the disc currently has: blue (placing allowed) or red (not allowed).
	bool cursor_allowed_{true};
	Unigine::EventConnections connections_;
};

} // namespace ArtistTool
