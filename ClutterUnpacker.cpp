#include "ClutterUnpacker.h"
#include "Localization.h"

#include <editor/UnigineActions.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>
#include <editor/UnigineUndo.h>

#include <UnigineEngine.h>
#include <UnigineFileSystem.h>
#include <UnigineLog.h>
#include <UnigineNodes.h>
#include <UnigineObjects.h>
#include <UniginePtr.h>
#include <UnigineRender.h>
#include <UnigineString.h>
#include <UnigineWorld.h>

#include <QTimer>

#include <cmath>

using namespace Unigine;

namespace ArtistTool
{

namespace
{

// Frames to wait in each bake phase. Spawning is a fixed heuristic: with the spawn
// rate raised to the cell count the engine fills the area in a few frames, the rest
// is a safety margin.
constexpr int SPAWN_FRAMES = 30;
constexpr int FLUSH_FRAMES = 2;

QString node_name(const NodePtr &node)
{
	return QString::fromUtf8(node->getName());
}

// The created nodes refer to their files by GUID, the way the editor does it: such a
// reference survives renaming and moving the asset. A clutter may hold a plain path.
String guid_path(const char *path)
{
	if (!path || !*path || FileSystem::isGUIDPath(path))
		return String(path);

	const UGUID guid = FileSystem::getGUID(path);
	return guid.isValid() ? FileSystem::guidToPath(guid) : String(path);
}

// The clutter renders every instance with its own per-surface settings, so carry
// them over - otherwise the unpacked meshes come out with default materials.
void copy_surfaces(const ObjectPtr &from, const ObjectPtr &to)
{
	const int num = Math::min(from->getNumSurfaces(), to->getNumSurfaces());
	for (int s = 0; s < num; ++s)
	{
		to->setMaterial(from->getMaterial(s), s);
		if (PropertyPtr property = from->getSurfaceProperty(s))
			to->setSurfaceProperty(property, s);

		to->setEnabled(from->isEnabled(s), s);
		to->setViewportMask(from->getViewportMask(s), s);
		to->setShadowMask(from->getShadowMask(s), s);
		to->setCastShadow(from->getCastShadow(s), s);
		to->setCastWorldShadow(from->getCastWorldShadow(s), s);
		to->setShadowMode(from->getShadowMode(s), s);

		to->setIntersection(from->getIntersection(s), s);
		to->setIntersectionMask(from->getIntersectionMask(s), s);
		to->setCollision(from->getCollision(s), s);
		to->setCollisionMask(from->getCollisionMask(s), s);
		to->setPhysicsIntersection(from->getPhysicsIntersection(s), s);
		to->setPhysicsIntersectionMask(from->getPhysicsIntersectionMask(s), s);

		to->setMinVisibleDistance(from->getMinVisibleDistance(s), s);
		to->setMinFadeDistance(from->getMinFadeDistance(s), s);
		to->setMaxVisibleDistance(from->getMaxVisibleDistance(s), s);
		to->setMaxFadeDistance(from->getMaxFadeDistance(s), s);
	}
}

} // namespace

ClutterUnpacker::ClutterUnpacker(QObject *parent)
	: QObject(parent)
{
}

ClutterUnpacker::~ClutterUnpacker()
{
	// No signals from the destructor: only put the clutter being baked back.
	if (bake_.phase != BakeState::PHASE_IDLE)
		restore_bake_state(true);
}

bool ClutterUnpacker::isSupported(const NodePtr &node)
{
	if (!node || node.isDeleted())
		return false;

	switch (node->getType())
	{
		case Node::WORLD_CLUTTER:
		case Node::OBJECT_MESH_CLUTTER:
		case Node::OBJECT_MESH_CLUSTER:
			return true;
		default:
			return false;
	}
}

QString ClutterUnpacker::typeLabel(const NodePtr &node)
{
	switch (node->getType())
	{
		case Node::WORLD_CLUTTER:       return QString::fromUtf8("World Clutter");
		case Node::OBJECT_MESH_CLUTTER: return QString::fromUtf8("Mesh Clutter");
		case Node::OBJECT_MESH_CLUSTER: return QString::fromUtf8("Mesh Cluster");
		default:                        return QString::fromUtf8(node->getTypeName());
	}
}

QString ClutterUnpacker::contentLabel(const NodePtr &node)
{
	switch (node->getType())
	{
		case Node::WORLD_CLUTTER:
		{
			const int refs = checked_ptr_cast<WorldClutter>(node)->getNumReferences();
			return refs == 1 ? uiText("1 asset", "1 ассет") : uiText("%1 assets", "ассетов: %1").arg(refs);
		}
		case Node::OBJECT_MESH_CLUSTER:
		{
			const int meshes = checked_ptr_cast<ObjectMeshCluster>(node)->getNumMeshes();
			return meshes == 1 ? uiText("1 mesh", "1 меш") : uiText("%1 meshes", "мешей: %1").arg(meshes);
		}
		default:
			// Generating the transforms just to count them is too expensive here.
			return uiText("generated", "генерируется");
	}
}

void ClutterUnpacker::start(const Vector<NodePtr> &nodes, const UnpackOptions &options)
{
	if (busy_)
		return;

	queue_.clear();
	for (const NodePtr &node : nodes)
	{
		if (isSupported(node))
			queue_.append(node);
	}

	if (queue_.empty())
	{
		emit message(MSG_WARNING, uiText(
			"Nothing to unpack: select a World Clutter, Mesh Clutter or Mesh Cluster node.",
			"Распаковывать нечего: выделите ноду World Clutter, Mesh Clutter или Mesh Cluster."));
		return;
	}

	options_ = options;
	results_.clear();
	next_ = 0;
	succeeded_ = 0;
	failed_ = 0;
	instances_ = 0;
	busy_ = true;

	emit started(queue_.size());
	QTimer::singleShot(0, this, &ClutterUnpacker::process_next);
}

void ClutterUnpacker::cancel()
{
	if (!busy_)
		return;

	if (bake_.phase != BakeState::PHASE_IDLE)
		restore_bake_state(true);

	finish_all(true);
}

void ClutterUnpacker::process_next()
{
	if (!busy_)
		return;

	if (next_ >= queue_.size())
	{
		finish_all(false);
		return;
	}

	const NodePtr node = queue_[next_++];
	if (!isSupported(node))
	{
		emit message(MSG_ERROR, uiText("A queued node no longer exists, skipped.", "Нода из очереди больше не существует, пропущена."));
		node_done(false, 0);
		return;
	}

	report_progress(0.0f, uiText("Unpacking '%1'...", "Распаковка '%1'...").arg(node_name(node)));

	switch (node->getType())
	{
		case Node::WORLD_CLUTTER:
			// Asynchronous: node_done() is called when the bake finishes.
			if (!begin_world_clutter_bake(node))
				node_done(false, 0);
			break;
		case Node::OBJECT_MESH_CLUTTER:
		{
			const int count = unpack_mesh_clutter(node);
			node_done(count > 0, count);
			break;
		}
		case Node::OBJECT_MESH_CLUSTER:
		{
			const int count = unpack_mesh_cluster(node);
			node_done(count > 0, count);
			break;
		}
		default:
			node_done(false, 0);
			break;
	}
}

void ClutterUnpacker::node_done(bool ok, int instances)
{
	if (ok)
	{
		++succeeded_;
		instances_ += instances;
	} else
		++failed_;

	QTimer::singleShot(0, this, &ClutterUnpacker::process_next);
}

void ClutterUnpacker::finish_all(bool canceled)
{
	busy_ = false;

	if (options_.select_result && results_.size())
	{
		::UnigineEditor::SelectionAction::applySelection(
			new ::UnigineEditor::SelectorNodes(results_));
	}

	queue_.clear();
	results_.clear();

	emit finished(succeeded_, failed_, instances_, canceled);
}

void ClutterUnpacker::report_progress(float current_fraction, const QString &status)
{
	const int total = Math::max(queue_.size(), 1);
	const float done = float(Math::max(next_ - 1, 0)) + Math::clamp(current_fraction, 0.0f, 1.0f);
	emit progress(Math::ftoi(done * 100.0f / float(total)), status);
}

bool ClutterUnpacker::confirm_count(const NodePtr &node, int instances)
{
	if (options_.confirm_threshold <= 0 || instances <= options_.confirm_threshold || !confirm_)
		return true;

	if (confirm_(node_name(node), instances))
		return true;

	emit message(MSG_WARNING, uiText("'%1' skipped (%2 instances).", "'%1' пропущен (экземпляров: %2).")
		.arg(node_name(node)).arg(instances));
	return false;
}

////////////////////////////////////////////////////////////////////////////////
// ObjectMeshClutter / ObjectMeshCluster (synchronous).
////////////////////////////////////////////////////////////////////////////////
int ClutterUnpacker::unpack_mesh_clutter(const NodePtr &node)
{
	ObjectMeshClutterPtr clutter = checked_ptr_cast<ObjectMeshClutter>(node);
	if (!clutter)
		return 0;

	clutter->createClutterTransforms();

	Vector<Math::Mat4> transforms;
	clutter->getClutterWorldTransforms(transforms);

	return build_mesh_instances(node, clutter->getMeshPath(), transforms);
}

int ClutterUnpacker::unpack_mesh_cluster(const NodePtr &node)
{
	ObjectMeshClusterPtr cluster = checked_ptr_cast<ObjectMeshCluster>(node);
	if (!cluster)
		return 0;

	const Math::Mat4 world = node->getWorldTransform();
	const int num = cluster->getNumMeshes();

	Vector<Math::Mat4> transforms;
	transforms.reserve(num);
	for (int i = 0; i < num; ++i)
		transforms.append(world * Math::Mat4(cluster->getMeshTransform(i)));

	return build_mesh_instances(node, cluster->getMeshPath(), transforms);
}

int ClutterUnpacker::build_mesh_instances(const NodePtr &node, const char *mesh_path,
	const Vector<Math::Mat4> &transforms)
{
	if (transforms.empty())
	{
		emit message(MSG_ERROR, uiText("'%1' has no instances to unpack.", "В '%1' нет экземпляров для распаковки.")
			.arg(node_name(node)));
		return 0;
	}
	if (!confirm_count(node, transforms.size()))
		return 0;

	// One configured template, cloned per instance.
	ObjectMeshStaticPtr instance_template = ObjectMeshStatic::create(guid_path(mesh_path).get());
	instance_template->setName(node->getName());
	copy_surfaces(checked_ptr_cast<Object>(node), instance_template);

	NodePtr root = make_root(node);
	for (const Math::Mat4 &transform : transforms)
	{
		NodePtr instance = instance_template->clone();
		instance->setWorldParent(root);
		instance->setWorldTransform(transform);
	}
	instance_template.deleteLater();

	commit_result(node, root);

	emit message(MSG_SUCCESS, uiText("'%1' -> '%2': %3 meshes.", "'%1' -> '%2': мешей: %3.")
		.arg(node_name(node), node_name(root)).arg(transforms.size()));
	return transforms.size();
}

////////////////////////////////////////////////////////////////////////////////
// WorldClutter (asynchronous bake).
//
// WorldClutter exposes only its scatter parameters, generation happens inside the
// engine, per cell, lazily, around the camera. So instead of reimplementing the
// scatter we drive the engine's own generation and read the result:
//   1. force the whole area to spawn (streaming, spawn rate, visible distance);
//   2. disable the clutter - this moves every active instance into its recycle
//      pool, disabled but with the transform intact, where getNodes() can see it;
//   3. recreate a NodeReference per captured instance.
////////////////////////////////////////////////////////////////////////////////
bool ClutterUnpacker::begin_world_clutter_bake(const NodePtr &node)
{
	WorldClutterPtr clutter = checked_ptr_cast<WorldClutter>(node);
	if (!clutter)
		return false;

	if (clutter->getNumReferences() == 0)
	{
		emit message(MSG_ERROR, uiText("'%1' has no node references assigned.", "В '%1' не назначено ни одной node reference.")
			.arg(node_name(node)));
		return false;
	}
	if (!clutter->isEnabled())
	{
		emit message(MSG_ERROR, uiText(
			"'%1' is disabled (or its parent is) - enable it to unpack.",
			"'%1' выключен (или выключен его родитель) - включите его, чтобы распаковать.")
			.arg(node_name(node)));
		return false;
	}

	bake_ = BakeState{};
	bake_.clutter = clutter;
	bake_.saved_force_streaming = Render::isForceStreaming();
	bake_.saved_spawn_rate = clutter->getSpawnRate();
	bake_.saved_visible_distance = clutter->getVisibleDistance();
	bake_.phase = BakeState::PHASE_SPAWN;

	const Math::Vec3 camera_position = attach_bake_camera(node);

	// The spawn radius is measured from the camera, so size the visible distance to
	// reach the whole clutter from it. The user's camera is covered as well, in case
	// a viewport could not be switched to the bake camera.
	const float scale = Math::max(Render::getDistanceScale(), 0.01f);
	const Math::Vec3 clutter_position = clutter->getWorldPosition();
	const double cam_dist = Math::max(Math::length(camera_position - clutter_position),
		Math::length(Renderer::getCameraPosition() - clutter_position));
	const float reach = (float(cam_dist) + clutter->getSizeX() + clutter->getSizeY() + 1000.0f) / scale;

	// Spawn every cell in a single pass; bound the rate to the cell count so the
	// per-frame nearest-cell search does not degenerate on huge grids.
	const float step = Math::max(clutter->getStep(), 0.001f);
	const long long csx = Math::max(1, Math::ftoi(Math::ceil(clutter->getSizeX() / step)));
	const long long csy = Math::max(1, Math::ftoi(Math::ceil(clutter->getSizeY() / step)));
	const long long cells_total = csx * csy;
	const int spawn_rate = int(cells_total < 200000 ? cells_total : 200000);

	Render::setForceStreaming(true);
	clutter->setSpawnRate(spawn_rate);
	clutter->setVisibleDistance(reach); // calls invalidate() internally

	Engine::get()->getEventEndWorldUpdate().connect(bake_connections_, this,
		&ClutterUnpacker::bake_tick);

	Log::message("ArtistTool: baking world clutter '%s' (references: %d)\n",
		node->getName(), clutter->getNumReferences());
	return true;
}

// The engine spawns clutter cells only for clutters inside a rendered frustum. Rather
// than move the user's camera, switch every editor viewport to a temporary camera
// that looks straight down at the clutter; detach_bake_camera() switches them back.
Math::Vec3 ClutterUnpacker::attach_bake_camera(const NodePtr &node)
{
	using ::UnigineEditor::ViewportManager;
	using ::UnigineEditor::ViewportWindowPtr;

	// Just above the top of the clutter bounds, at their center.
	Math::Vec3 position = node->getWorldPosition();
	const Math::WorldBoundBox bounds = node->getWorldBoundBox();
	const Math::Vec3 center = Math::Vec3(bounds.getCenter());
	if (std::isfinite(center.x) && std::isfinite(center.y) && std::isfinite(bounds.maximum.z))
	{
		position = center;
		position.z = bounds.maximum.z;
	}
	position.z += 2.0f;

	// A player with no rotation looks along its -Z axis, i.e. straight down.
	PlayerDummyPtr camera = PlayerDummy::create();
	camera->setName("artist_tool_bake_camera");
	camera->setShowInEditorEnabled(false);
	camera->setSaveToWorldEnabled(false);
	camera->setWorldPosition(position);
	bake_.camera = camera;

	Vector<ViewportWindowPtr> viewports;
	ViewportManager::getViewports(viewports);
	for (const ViewportWindowPtr &viewport : viewports)
	{
		PlayerPtr player = viewport->getPlayer();
		if (!player)
			continue;

		bake_.saved_cameras.append({viewport, player});
		viewport->setPlayer(camera);
	}

	return position;
}

void ClutterUnpacker::detach_bake_camera()
{
	for (const BakeState::SavedCamera &saved : bake_.saved_cameras)
	{
		if (saved.viewport && !saved.player.isDeleted())
			saved.viewport->setPlayer(saved.player);
	}
	bake_.saved_cameras.clear();

	if (bake_.camera)
	{
		bake_.camera.deleteLater();
		bake_.camera = PlayerPtr();
	}
}

void ClutterUnpacker::bake_tick()
{
	if (bake_.phase == BakeState::PHASE_IDLE || bake_.phase == BakeState::PHASE_FINISH)
		return;

	if (bake_.clutter.isDeleted())
	{
		bake_.clutter = WorldClutterPtr();
		restore_bake_state(false);
		emit message(MSG_ERROR, uiText("The clutter was removed during the bake.", "Клаттер был удалён во время распаковки."));
		node_done(false, 0);
		return;
	}

	++bake_.frames;

	const int total_frames = SPAWN_FRAMES + FLUSH_FRAMES;
	const QString status = uiText("Baking '%1'...", "Генерация '%1'...").arg(node_name(bake_.clutter));

	switch (bake_.phase)
	{
		case BakeState::PHASE_SPAWN:
			report_progress(float(bake_.frames) / float(total_frames), status);
			if (bake_.frames >= SPAWN_FRAMES)
			{
				// getNodes() can only see a clutter's RECYCLE POOL, not its active cell
				// nodes. Snapshot the pre-existing (stale) pool first, so that only the
				// instances flushed by the disable below are captured.
				const int id = bake_.clutter->getID();
				Vector<NodePtr> nodes;
				World::getNodes(nodes, true, true);
				for (const NodePtr &n : nodes)
				{
					NodePtr possessor = n->getPossessor();
					if (possessor && possessor->getID() == id)
						bake_.stale_ids.append(n->getID());
				}

				bake_.clutter->setEnabled(false);
				bake_.disabled_by_bake = true;
				bake_.phase = BakeState::PHASE_FLUSH;
				bake_.frames = 0;
			}
			break;

		case BakeState::PHASE_FLUSH:
			report_progress(float(SPAWN_FRAMES + bake_.frames) / float(total_frames), status);
			if (bake_.frames >= FLUSH_FRAMES)
			{
				// Build the result from the Qt event loop rather than from inside the
				// engine update: it registers undo actions and may show a dialog.
				bake_.phase = BakeState::PHASE_FINISH;
				bake_connections_.disconnectAll();
				QTimer::singleShot(0, this, &ClutterUnpacker::finish_world_clutter_bake);
			}
			break;

		default:
			break;
	}
}

void ClutterUnpacker::finish_world_clutter_bake()
{
	if (!busy_ || bake_.phase != BakeState::PHASE_FINISH)
		return;

	if (bake_.clutter.isDeleted())
	{
		bake_.clutter = WorldClutterPtr();
		restore_bake_state(false);
		emit message(MSG_ERROR, uiText("The clutter was removed during the bake.", "Клаттер был удалён во время распаковки."));
		node_done(false, 0);
		return;
	}

	WorldClutterPtr clutter = bake_.clutter;
	const int clutter_id = clutter->getID();

	// The clutter is disabled, so all its instances are in the recycle pool now.
	// Capture pool nodes owned by THIS clutter (possessor filter = isolation +
	// instance roots), excluding the pre-existing stale ones.
	struct Captured
	{
		String name;
		Math::Mat4 transform;
	};
	Vector<Captured> captured;
	{
		Vector<NodePtr> nodes;
		World::getNodes(nodes, true, true);
		for (const NodePtr &n : nodes)
		{
			NodePtr possessor = n->getPossessor();
			if (!possessor || possessor->getID() != clutter_id)
				continue;
			if (bake_.stale_ids.contains(n->getID()))
				continue;
			captured.append({n->getName(), n->getWorldTransform()});
		}
	}

	if (captured.empty())
	{
		restore_bake_state(true);
		emit message(MSG_ERROR, uiText(
			"'%1': the clutter generated no nodes. Check that it shows nodes in the "
			"viewport (density, mask, assets).",
			"'%1': клаттер не создал ни одной ноды. Проверьте, что он показывает ноды во "
			"вьюпорте (плотность, маска, ассеты).").arg(node_name(clutter)));
		node_done(false, 0);
		return;
	}

	if (!confirm_count(clutter, captured.size()))
	{
		restore_bake_state(true);
		node_done(false, 0);
		return;
	}

	// Map each reference's content-root name -> (asset path, inverse content-local
	// transform). The clutter places the content root at
	// clutter * scatter * content_local, so a NodeReference must be placed at
	// captured * inverse(content_local) for its content to land at the same spot.
	Vector<String> ref_names;
	Vector<String> ref_paths;
	Vector<Math::Mat4> ref_icontent;
	for (int r = 0, n = clutter->getNumReferences(); r < n; ++r)
	{
		const char *path = clutter->getReferenceName(r);
		NodeReferencePtr tmp = NodeReference::create(path);
		NodePtr content = tmp->getReference();
		ref_names.append(content ? String(content->getName()) : String());
		ref_paths.append(guid_path(path));
		ref_icontent.append(content ? Math::inverse(content->getTransform()) : Math::Mat4());
		tmp.deleteLater();
	}

	// The instances are told apart by the name of the root node of their asset. Two
	// assets whose root nodes are named the same can not be: all their instances come
	// out as the first of the two, and the user is told so.
	for (int a = 0; a < ref_names.size(); ++a)
	{
		for (int b = a + 1; b < ref_names.size(); ++b)
		{
			if (ref_names[a].empty() || ref_names[a] != ref_names[b])
				continue;
			emit message(MSG_WARNING, uiText(
				"'%1': node references %2 and %3 have root nodes of the same name ('%4'). Their "
				"instances can not be told apart and are all unpacked as number %2. Rename the "
				"root node of one of the assets to unpack them right.",
				"'%1': у node reference %2 и %3 корневые ноды называются одинаково ('%4'). Их "
				"экземпляры не различить, все распакованы как номер %2. Переименуйте корневую "
				"ноду одного из ассетов, чтобы распаковка была верной.")
				.arg(node_name(clutter)).arg(a + 1).arg(b + 1).arg(QString::fromUtf8(ref_names[a].get())));
		}
	}

	// Build the hierarchy: root -> (per-asset group) -> instances.
	NodePtr root = make_root(clutter);
	Vector<NodeDummyPtr> groups;
	groups.resize(ref_paths.size());
	int unmatched = 0;
	for (const Captured &c : captured)
	{
		int idx = -1;
		for (int k = 0; k < ref_names.size(); ++k)
		{
			if (ref_names[k] == c.name)
			{
				idx = k;
				break;
			}
		}
		if (idx < 0)
		{
			idx = 0;
			++unmatched;
		}

		NodePtr parent = root;
		if (options_.group_by_asset)
		{
			if (!groups[idx])
			{
				// Name the group after the node reference used under it (its content
				// root name); fall back to the asset file name if that is empty.
				const String group_name = ref_names[idx].empty()
					? String(String::filename(ref_paths[idx].get()))
					: ref_names[idx];

				NodeDummyPtr group = NodeDummy::create();
				group->setName(group_name.get());
				group->setWorldParent(root);
				group->setWorldTransform(clutter->getWorldTransform());
				groups[idx] = group;
			}
			parent = groups[idx];
		}

		NodeReferencePtr ref = NodeReference::create(ref_paths[idx].get());
		ref->setWorldParent(parent);
		ref->setWorldTransform(c.transform * ref_icontent[idx]);
		ref->setName(c.name.get());
	}

	// Re-enable the source first, so that the undo action recorded by
	// commit_result() restores it to the state it had before the bake.
	restore_bake_state(true);
	commit_result(clutter, root);

	emit message(MSG_SUCCESS, uiText("'%1' -> '%2': %3 node references.", "'%1' -> '%2': node reference: %3.")
		.arg(node_name(clutter), node_name(root)).arg(captured.size()));
	if (unmatched)
	{
		emit message(MSG_WARNING, uiText(
			"'%1': %2 instances could not be matched to an asset by name and were "
			"assigned to the first one.",
			"'%1': экземпляры (%2) не удалось сопоставить с ассетом по имени, они отнесены "
			"к первому.").arg(node_name(clutter)).arg(unmatched));
	}

	node_done(true, captured.size());
}

void ClutterUnpacker::restore_bake_state(bool restore_enabled)
{
	bake_connections_.disconnectAll();
	detach_bake_camera();

	if (bake_.clutter && !bake_.clutter.isDeleted())
	{
		bake_.clutter->setSpawnRate(bake_.saved_spawn_rate);
		bake_.clutter->setVisibleDistance(bake_.saved_visible_distance);
		if (restore_enabled && bake_.disabled_by_bake)
			bake_.clutter->setEnabled(true);
	}
	Render::setForceStreaming(bake_.saved_force_streaming);

	bake_ = BakeState{};
}

////////////////////////////////////////////////////////////////////////////////
// Result.
////////////////////////////////////////////////////////////////////////////////
NodePtr ClutterUnpacker::make_root(const NodePtr &source) const
{
	const String name = String(source->getName()) + options_.suffix.toUtf8().constData();

	NodeDummyPtr root = NodeDummy::create();
	root->setName(name.get());
	root->setWorldTransform(source->getWorldTransform());
	return root;
}

void ClutterUnpacker::commit_result(const NodePtr &source, const NodePtr &root)
{
	// A source owned by a node reference (possessor) lives in a hierarchy the
	// editor does not let you add nodes to, so the result goes to the world root.
	if (options_.keep_parent && !source->getPossessor())
	{
		// Right below the source: in its parent, or in the world root list if the
		// source has no parent.
		if (NodePtr parent = source->getParent())
		{
			root->setWorldParent(parent);
			parent->setChildIndex(root, parent->getChildIndex(source) + 1);
		} else
			World::setRootNodeIndex(root, World::getRootNodeIndex(source) + 1);
	}

	// Make the generated hierarchy visible in the World Hierarchy and saveable with
	// the world (the user still decides when to actually save the world).
	root->setShowInEditorEnabledRecursive(true);
	root->setSaveToWorldEnabledRecursive(true);

	// Nodes created via ::create() exist in the engine world, but are unknown to the
	// editor until a CreateNodesAction is applied. One transaction = one Ctrl+Z.
	using namespace ::UnigineEditor;
	Undo::begin();
	Undo::apply(new CreateNodesAction(root));
	switch (options_.source_mode)
	{
		case UnpackOptions::SOURCE_DISABLE:
			Undo::apply(new EnableNodeAction(source, false));
			break;
		case UnpackOptions::SOURCE_DELETE:
			Undo::apply(new RemoveNodesAction(source));
			break;
		default:
			break;
	}
	Undo::commit();

	results_.append(root);
}

} // namespace ArtistTool
