#pragma once

#include <editor/UnigineViewportManager.h>

#include <UnigineCallback.h>
#include <UnigineHashSet.h>
#include <UnigineNode.h>
#include <UniginePlayers.h>
#include <UnigineVector.h>
#include <UnigineWorlds.h>

#include <QObject>
#include <QString>

#include <functional>

namespace ArtistTool
{

struct UnpackOptions
{
	enum SourceMode
	{
		SOURCE_DISABLE = 0,
		SOURCE_KEEP,
		SOURCE_DELETE,
	};

	SourceMode source_mode{SOURCE_DISABLE};
	QString suffix{"_unpacked"};
	// WorldClutter only: root -> one dummy per used asset -> instances.
	bool group_by_asset{true};
	// Put the result next to the source node instead of the world root.
	bool keep_parent{true};
	bool select_result{true};
	// Ask before creating more nodes than this (0 = never ask).
	int confirm_threshold{20000};
};

// Unpacks procedural clutters (WorldClutter / ObjectMeshClutter / ObjectMeshCluster)
// into explicit, saveable nodes - one per generated instance.
//
// ObjectMeshClutter / ObjectMeshCluster are unpacked synchronously. WorldClutter has
// no public instance enumeration, so it is baked by a frame-ticked state machine that
// drives the engine's own generation and reads the result back. The engine spawns
// cells only for clutters inside a rendered frustum, so for the time of the bake the
// editor viewports are switched to a temporary camera placed over the clutter; the
// user's own camera is not moved and is put back afterwards.
class ClutterUnpacker final : public QObject
{
	Q_OBJECT
public:
	enum MessageLevel
	{
		MSG_INFO = 0,
		MSG_SUCCESS,
		MSG_WARNING,
		MSG_ERROR,
	};

	using ConfirmFunc = std::function<bool(const QString &node_name, int instances)>;

	explicit ClutterUnpacker(QObject *parent = nullptr);
	~ClutterUnpacker() override;

	static bool isSupported(const Unigine::NodePtr &node);
	static QString typeLabel(const Unigine::NodePtr &node);
	// Short human-readable description of what the node will produce.
	static QString contentLabel(const Unigine::NodePtr &node);

	// Called when a node is about to produce more instances than
	// UnpackOptions::confirm_threshold; return false to skip that node.
	void setConfirmFunc(ConfirmFunc func) { confirm_ = std::move(func); }

	bool isBusy() const { return busy_; }
	void start(const Unigine::Vector<Unigine::NodePtr> &nodes, const UnpackOptions &options);
	void cancel();

signals:
	void started(int total_nodes);
	void progress(int percent, const QString &status);
	void message(int level, const QString &text);
	void finished(int succeeded, int failed, int instances, bool canceled);

private:
	void process_next();
	void node_done(bool ok, int instances);
	void finish_all(bool canceled);
	void report_progress(float current_fraction, const QString &status);
	bool confirm_count(const Unigine::NodePtr &node, int instances);

	int unpack_mesh_clutter(const Unigine::NodePtr &node);
	int unpack_mesh_cluster(const Unigine::NodePtr &node);
	int build_mesh_instances(const Unigine::NodePtr &node, const char *mesh_path,
		const Unigine::Vector<Unigine::Math::Mat4> &transforms);

	bool begin_world_clutter_bake(const Unigine::NodePtr &node);
	Unigine::Math::Vec3 attach_bake_camera(const Unigine::NodePtr &node);
	void detach_bake_camera();
	void bake_tick();
	void finish_world_clutter_bake();
	void restore_bake_state(bool restore_enabled);

	Unigine::NodePtr make_root(const Unigine::NodePtr &source) const;
	void commit_result(const Unigine::NodePtr &source, const Unigine::NodePtr &root);

	bool busy_{false};
	UnpackOptions options_;
	ConfirmFunc confirm_;

	Unigine::Vector<Unigine::NodePtr> queue_;
	Unigine::Vector<Unigine::NodePtr> results_;
	int next_{0};
	int succeeded_{0};
	int failed_{0};
	int instances_{0};

	Unigine::EventConnections bake_connections_;

	struct BakeState
	{
		enum Phase
		{
			PHASE_IDLE = 0,
			PHASE_SPAWN,	// wait for the engine to spawn every cell
			PHASE_FLUSH,	// clutter disabled: wait for instances to land in the pool
			PHASE_FINISH,	// captured on the next Qt event loop iteration
		};

		Phase phase{PHASE_IDLE};
		int frames{0};
		bool disabled_by_bake{false};
		bool saved_force_streaming{false};
		int saved_spawn_rate{1};
		float saved_visible_distance{0.0f};
		Unigine::WorldClutterPtr clutter;
		Unigine::HashSet<int> stale_ids;

		// Temporary camera looking at the clutter and the cameras it replaced.
		struct SavedCamera
		{
			::UnigineEditor::ViewportWindowPtr viewport;
			Unigine::PlayerPtr player;
		};
		Unigine::PlayerPtr camera;
		Unigine::Vector<SavedCamera> saved_cameras;
	} bake_;
};

} // namespace ArtistTool
