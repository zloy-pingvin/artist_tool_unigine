#pragma once

#include "PhysicsShapeFit.h"
#include "PlaceableAsset.h"
#include "ViewportPicking.h"

#include <UnigineCallback.h>
#include <UnigineMathLib.h>
#include <UnigineNode.h>
#include <UnigineObjects.h>
#include <UniginePhysics.h>

#include <QElapsedTimer>
#include <QHash>
#include <QMenuBar>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>
#include <memory>
#include <vector>

class QToolButton;

namespace ArtistTool
{

// Lets objects fall and settle with the physics of the engine, and leaves them where they
// came to rest. The physics is there only while they fall.
//
// Every object gets an invisible stand-in with a body and a shape measured from its mesh
// (it does not need shapes of its own), and follows it frame by frame. The object itself
// gets no body: the engine would drop its scale. When it is over the stand-ins are removed,
// all that was changed in the world is put back, and the new places of the objects are one
// step of the undo history.
class PhysicsDrop final : public QObject
{
public:
	enum ShapeKind
	{
		SHAPE_AUTO = 0,
		SHAPE_HULL,
		SHAPE_PIECES,
		SHAPE_BOX,
		SHAPE_SPHERE,
		SHAPE_CAPSULE,
		SHAPE_CYLINDER,
		// The shapes the object came with. Not a choice of the page: Auto takes them.
		SHAPE_READY,
	};

	enum MessageLevel
	{
		MSG_INFO = 0,
		MSG_SUCCESS,
		MSG_WARNING,
		MSG_ERROR,
	};

	struct Options
	{
		ShapeKind shape{SHAPE_AUTO};
		// With SHAPE_AUTO: the shapes an object came with go before anything measured.
		bool use_own_shapes{true};
		// What the objects fall on. An object of these kinds with no collision surfaces gets
		// them for the time of the fall; the other kinds are passed through.
		SurfaceFilter filter;
		// Of the shapes; the engine multiplies those of the two things that touch. Measured on
		// piles of 300: at 1 objects lean on each other at odd angles and take 7-11 s, at 0.2
		// they slide to where they are steady and take 3-4 s.
		float friction{0.4f};
		bool show_shapes{true};
		// Seconds of the fall: what has not settled by then stays where it is at that moment.
		double max_time{60.0};
		// The objects do not turn, whatever hits them.
		bool lock_rotation{false};
		// New objects of a session end up baked into clusters, a cluster per mesh, instead of
		// staying separate nodes.
		bool as_clusters{false};
		// New objects seen less than this, on average from all around and above, are gone on applying:
		// the pile looks the same with fewer objects. 0 - all stay. See PhysicsVisibility.h.
		float least_seen{0.0f};
		// The same for what was in the world before and took part in the session: it is removed from
		// the world, as a part of the step of the history. Never in a drop.
		float least_seen_existing{0.0f};
		// The physics goes on only inside a sphere in front of the camera, in units.
		float dome_radius{30.0f};
		bool show_dome{true};
	};

	explicit PhysicsDrop(QObject *parent = nullptr);
	~PhysicsDrop() override;

	// The nodes fall, each as one solid thing with all that is inside it, and it ends when
	// they have settled. False if it did not start: on_message has told why.
	bool start(const Unigine::Vector<Unigine::NodePtr> &nodes, const Options &options);
	// A session: the physics is there until finish() or cancel(), with no objects at first.
	// Objects are added to it and taken from it while it goes, and all it did is one step of
	// the history at the end. When everything has settled the physics is on pause by itself.
	bool beginSession(const Options &options);
	// Leaves the objects where they are now, as a step of the history.
	void finish();
	// Puts the objects back where they were and removes the new ones.
	void cancel();
	// The same, not waiting for the next frame: the plugin is being unloaded.
	void cancelNow();
	bool isRunning() const { return running_; }
	bool isSession() const { return session_; }

	// Whether a new object may appear there: not too near the other objects that take part.
	bool hasRoom(const Unigine::Math::Vec3 &spot) const;
	// In a session: a new object of the asset, with its middle at 'center', or above that if it is
	// deep in something there. 'stroke' tells the objects put down at one go from the others.
	bool spawn(const PlaceableAsset &asset, const Unigine::Math::Vec3 &center, const Unigine::Math::quat &rotation, float scale, int stroke);
	// How far above where they were asked to the new objects have had to appear since the last call.
	float takeSpawnRise();
	// The size of the biggest new object so far, from its middle to its farthest point.
	float getLargestNewRadius() const { return largest_new_radius_; }
	// What is around becomes solid before anything gets there.
	void prepareArea(const Unigine::Math::Vec3 &center, float reach);
	// Removes the objects of the last stroke; false if there are no new objects left.
	bool removeLastStroke();
	// Removes the new objects with their middle within the radius; the ones that were in the
	// world before are not touched. Returns how many.
	int eraseNew(const Unigine::Math::Vec3 &center, float radius);
	void clearNew();
	bool hasNewObjects() const;

	// In a session: the objects of the world within the radius start taking part: the ones
	// with a mesh, not bigger than 'max_size' across, without the Immovable flag; with
	// 'only' given, only what is inside those nodes. All the others stay as solid as they were:
	// this is what keeps the physics to the place of work. 'clicked' takes part however far
	// its middle is from the center, if the other rules let it.
	void activateZone(const Unigine::Math::Vec3 &center, float radius, float max_size, const QVector<Unigine::NodePtr> *only,
		const Unigine::NodePtr &clicked = Unigine::NodePtr());
	// The object a node is or is inside of, -1 if it does not take part.
	int findItem(const Unigine::NodePtr &node) const;
	// The node an object of the world belongs to as a whole: the node reference it is inside
	// of, or itself.
	static Unigine::NodePtr wholeNode(const Unigine::NodePtr &node);

	// Whether something that far around the point has a part in the dome.
	bool isInDome(const Unigine::Math::Vec3 &point, float reach = 0.0f) const;
	void setDomeRadius(float radius);
	void setShowDome(bool show) { options_.show_dome = show; }

	// An object held by the mouse flies to the target and pushes what is on its way.
	void grab(int item, const Unigine::Math::Vec3 &point);
	void setGrabTarget(const Unigine::Math::Vec3 &target);
	void release();

	// In a session: the manipulator of the editor moves the selected objects. They fly where it
	// tells them, pushing what is on the way, turn the way it turns them, and fall when it is let go.
	void setGizmoEnabled(bool enabled) { gizmo_enabled_ = enabled; }
	// Where the manipulator is taking objects now; none if it is not in use.
	QVector<Unigine::Math::Vec3> getGizmoTargets() const;
	bool isGizmoHolding() const { return gizmo_holding_; }
	// Everything within the radius gets pushed away from the center or pulled to it.
	void applyForce(const Unigine::Math::Vec3 &center, float radius, float strength, bool pull, float ifps);
	// Everything within the radius goes sideways, at that speed in the middle and smoothly slower to
	// nothing at the edge, and gravity stays: a whole pile can be moved over the ground.
	void carryZone(const Unigine::Math::Vec3 &center, float radius, const Unigine::Math::vec3 &velocity, float ifps);
	// The same way everything within the radius stops going sideways: with low friction it would go on
	// and on.
	void stopZone(const Unigine::Math::Vec3 &center, float radius);
	// A random push to all the objects, to unstick what has got stuck.
	void jitter();

	void setAsClusters(bool as_clusters) { options_.as_clusters = as_clusters; }
	void setLeastSeen(float least_seen) { options_.least_seen = least_seen; }
	void setLeastSeenExisting(float least_seen) { options_.least_seen_existing = least_seen; }
	const Options &getOptions() const { return options_; }
	void setUserPaused(bool paused);
	bool isUserPaused() const { return user_paused_; }
	bool isIdlePaused() const { return idle_paused_; }

	void setShowShapes(bool show);
	double getElapsed() const { return elapsed_; }
	int getNumMoving() const { return moving_; }
	int getNumObjects() const { return alive_; }
	// Out of the dome: they stand still.
	int getNumParked() const { return num_parked_; }

	// Whether the nodes can fall at all: they have an enabled object inside.
	static bool canFall(const Unigine::NodePtr &node);

	// Called when it starts and ends, and a few times a second while it goes.
	std::function<void()> on_changed;
	std::function<void(int level, const QString &text)> on_message;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	// What falls.
	struct Item
	{
		Unigine::NodePtr node;
		QString name;
		Unigine::Math::Mat4 start_transform;
		// From the stand-in to the node: the scale of the node and all it has.
		Unigine::Math::Mat4 relative;
		Unigine::ObjectDummyPtr stand_in;
		Unigine::BodyRigidPtr body;
		// Distance from the stand-in to the farthest point of the object.
		float radius{0.0f};
		// Its shapes have continuous collision now.
		bool continuous{true};
		// Its smallest size along its own axes.
		float thinnest{0.0f};
		int shape_kind{SHAPE_HULL};
		// Where it has been since a moment: it has settled when it stays there.
		Unigine::Math::Vec3 place;
		double place_since{0.0};
		// Distance from the middle of the object to its farthest point.
		float size_radius{0.0f};
		// Made by the session, and the stroke it was made by; -1 - it was in the world before.
		bool is_new{false};
		int stroke{-1};
		// Fell far below where it was: there is nothing under it. What was in the world before is hidden
		// and removed at the end, 'was_enabled' for when it is back. And whether it went through a surface.
		bool lost{false};
		bool was_enabled{true};
		bool through_something{false};
		// Its hull, around the stand-in, and the triangles of it: what it hides of the others. None if
		// it is flat.
		std::vector<Unigine::Math::vec3> outline;
		std::shared_ptr<const std::vector<int>> outline_triangles;
		// Where it was at the end of the last frame.
		bool has_last_place{false};
		Unigine::Math::Vec3 last_center;
		Unigine::Math::Mat4 last_stand_in;
		// Not simulated: the object was too far under its start, or was removed.
		bool gone{false};
		// Out of the dome: it stands still, as solid as the rest of the world.
		bool parked{false};
		// Parked from the very start: it has yet to fall.
		bool waiting{false};
		// What the node was last given: anything else in it is the work of the manipulator.
		Unigine::Math::Mat4 written;
		// In the hands of the manipulator, and where it wants the stand-in.
		bool gizmo_held{false};
		Unigine::Math::Mat4 gizmo_target;
	};

	// A mesh measured once: many objects use the same one.
	struct MeshShape
	{
		std::vector<Unigine::Math::vec3> hull;
		// False: 'hull' is only its points or corners, it is flat.
		bool has_hull{false};
		double hull_volume{0.0};
		std::shared_ptr<const std::vector<int>> hull_triangles;
		// The convex parts it is split into, made when they are first asked for.
		std::vector<std::vector<Unigine::Math::vec3>> pieces;
		bool has_pieces{false};
		std::vector<Unigine::Math::vec3> vertices;
		// The best simple shape around the hull at scale 1, found when it is first asked for: copies
		// differ only by their scale. Not found: the hull is to be used.
		bool has_best_fit{false};
		bool best_fit_found{false};
		PrimitiveFit best_fit;
	};

	// What the shape of an object was chosen to be, for the log.
	struct ShapeResult
	{
		int kind{SHAPE_HULL};
		double extra{0.0};
		bool flat{false};
	};

	// A flag of a surface as it was, to be put back. Surface -1 - the flag of a Mesh Clutter
	// as a whole.
	struct SurfaceFlags
	{
		Unigine::ObjectPtr object;
		int surface{0};
		bool collision{false};
		int mask{0};
	};

	struct BodyFlags
	{
		Unigine::ObjectPtr object;
		bool enabled{true};
		bool immovable{false};
	};

	// A body taken off its object for the time of the fall.
	struct DetachedBody
	{
		Unigine::ObjectPtr object;
		Unigine::BodyPtr body;
	};

	std::shared_ptr<MeshShape> mesh_shape(const Unigine::ObjectMeshStaticPtr &mesh);
	// All that a session and a drop need before the first object.
	bool can_begin();
	void begin_world();
	// A node starts falling: its own bodies and surfaces are out of the way of its stand-in.
	void take_over_node(const Unigine::NodePtr &root);
	// The stand-in of the object. With 'center_at' the object goes there first.
	bool build_item(const Unigine::NodePtr &node, Item &item, ShapeResult &result, const Unigine::Math::Vec3 *center_at = nullptr);
	// A new object deep in what is there goes up into the free air.
	void rise_out_of_things(Item &item);
	void remove_item(Item &item);
	// The object takes part from now on; returns its index.
	int register_item(const Item &item);
	// An object of the world starts taking part; -1 if it can not.
	int add_existing(const Unigine::NodePtr &root);
	// What the manipulator of the editor has done since the last frame.
	void update_gizmo();
	void steer_gizmo_items();
	void release_gizmo_items();
	// The manipulator leaves a step in the history of the editor, which is not to be there: what the
	// session has done is one step of its own.
	void close_gizmo_undo();
	// The dome goes after the camera, calmly; with 'jump' it is at its place at once.
	void update_dome(bool jump);
	// How high the ground is in the dome: what its wall rises from.
	void measure_ground();
	void draw_dome() const;
	void apply_show_distance() const;
	// What has settled out of the dome stands still, what is in the dome goes on.
	void update_parking();
	void set_parked(Item &item, bool parked);
	bool is_in_dome(const Item &item) const;
	// What the engine is to step: it asks before every step, and does not look for bodies itself.
	void before_step();
	void rebuild_stepped();
	void update_hold();
	void wake();
	void update_pause();
	// What is hardly seen does not stay: the new objects go at once, the others are told for the history.
	void remove_unseen(const QVector<Unigine::Math::Mat4> &results, Unigine::Vector<Unigine::NodePtr> &existing);
	// The new objects find their final places, under one node, as nodes or clusters.
	void commit_new(const QVector<Unigine::Math::Mat4> &results);
	int make_clusters(const QVector<Unigine::Math::Mat4> &results, const Unigine::NodePtr &group, QVector<bool> &baked);
	void add_convex(const Unigine::BodyRigidPtr &body, const std::vector<Unigine::Math::vec3> &points, ShapeResult &result);
	void add_primitive(const Unigine::BodyRigidPtr &body, const PrimitiveFit &fit);
	void set_up_shape(const Unigine::ShapePtr &shape, float volume) const;

	void hold_other_bodies();
	void prepare_surroundings();
	void prepare_object(const Unigine::ObjectPtr &object);
	void take_area(const Unigine::Math::WorldBoundBox &area);
	void take_item_area(const Item &item);
	// The area around the objects in the dome: all of them, or those that still move.
	void take_moving_area(bool only_moving);
	void remember_surface(const Unigine::ObjectPtr &object, int surface);
	void remember_body(const Unigine::ObjectPtr &object);
	void apply_show_shapes(bool show);

	void update();
	void follow_stand_ins();
	// Whether it is falling into nothing: there is no thing to land on anywhere below it.
	bool has_nothing_under(const Item &item) const;
	// It has fallen through: it takes no more part, and does not stay.
	void lose_item(Item &item);
	// If it has gone further in this frame than anything but a glitch of the engine could take it, it
	// goes back where it was. 'frame_time' - the seconds of physics in this frame.
	void take_back_jump(Item &item, float frame_time);
	// Ends the fall right now: mode says what becomes of the objects.
	enum EndMode
	{
		END_KEEP = 0,
		END_RETURN,
		// The world goes away: nothing is put anywhere.
		END_WORLD_GONE,
	};
	void end(EndMode mode, const QString &summary, int level);
	void restore_world();
	void report(int level, const QString &text) const;
	// For the log of the editor, not for the page: figures of no use to the user of the tool.
	void log_only(const QString &text) const;
	void notify() const;

	bool running_{false};
	bool session_{false};
	Options options_;
	QVector<Item> items_;
	int alive_{0};
	float spawn_rise_{0.0f};
	float largest_new_radius_{0.0f};
	// Already told what shape the objects of this name fall in.
	QSet<QString> told_shapes_;
	// The item held by the mouse, -1 if none, and where it is taken by.
	int held_{-1};
	Unigine::Math::vec3 held_offset_{0.0f, 0.0f, 0.0f};
	Unigine::Math::Vec3 held_target_;
	float held_damping_{0.0f};
	bool gizmo_enabled_{false};
	bool gizmo_holding_{false};
	bool gizmo_undo_open_{false};
	bool gizmo_action_pushed_{false};
	int gizmo_undo_wait_{0};
	// Where the selected objects that do not take part yet are, before the manipulator takes them.
	QHash<int, Unigine::Math::Mat4> gizmo_seen_;
	// The item of a node, by the ID of the node.
	QHash<int, int> item_of_node_;
	Unigine::Math::Vec3 dome_center_;
	bool dome_moving_{false};
	Unigine::Math::Vec3 dome_measured_at_;
	double dome_ground_{0.0};
	double dome_ground_target_{0.0};
	int num_parked_{0};
	// How many times an object was thrown by the physics and put back, for the log.
	int jumps_taken_back_{0};
	// A drop: the manipulator of the editor is off while it goes, to be put back.
	bool manipulators_were_on_{true};
	// The manipulator has moved what can not take part: it goes back, and the user is told.
	bool gizmo_refused_{false};
	int last_followed_frame_{0};
	Unigine::Vector<Unigine::NodePtr> stepped_nodes_;
	bool stepped_dirty_{true};
	// The physics stands still: asked for, or nothing moves and nothing is being done.
	bool user_paused_{false};
	bool idle_paused_{false};
	float saved_scale_{1.0f};
	float running_scale_{1.0f};
	double last_activity_{0.0};
	// The IDs of all the nodes inside the falling objects.
	QSet<int> falling_nodes_;
	// The objects whose surfaces have been seen to: every one only once.
	QSet<int> prepared_objects_;
	// The pieces of the world looked through for them: every one only once.
	QSet<quint64> prepared_cells_;
	QVector<SurfaceFlags> surfaces_;
	QVector<BodyFlags> bodies_;
	QVector<DetachedBody> detached_bodies_;
	QHash<QString, std::shared_ptr<MeshShape>> mesh_shapes_;
	// Asked for by the buttons, done in the next update.
	bool finish_requested_{false};
	bool cancel_requested_{false};

	float saved_physics_distance_{0.0f};
	float saved_show_distance_{0.0f};
	float saved_ifps_{0.0f};
	int saved_iterations_{1};
	int saved_frozen_frames_{0};
	float saved_penetration_tolerance_{0.0f};
	float saved_penetration_factor_{0.0f};
	bool saved_game_enabled_{false};
	Unigine::Physics::SHOW_TYPE saved_show_shapes_{Unigine::Physics::SHOW_TYPE_DISABLED};
	bool visualizer_requested_{false};
	// What of the editor must not be used while it goes.
	QPointer<QToolButton> physics_button_;
	QPointer<QToolButton> logic_button_;
	QPointer<QMenuBar> main_menu_;

	// Used for making convex shapes: the engine takes their points from an object.
	Unigine::ObjectMeshDynamicPtr convex_source_;

	QElapsedTimer timer_;
	double elapsed_{0.0};
	double last_notified_{0.0};
	int moving_{0};
	int frame_{0};
	int first_physics_frame_{0};
	// Time of the physics per frame, for the log.
	double physics_time_sum_{0.0};
	double physics_time_peak_{0.0};
	int physics_time_frames_{0};

	Unigine::EventConnections connections_;
};

} // namespace ArtistTool
