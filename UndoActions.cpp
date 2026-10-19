#include "UndoActions.h"

#include <editor/UnigineAssetManager.h>

#include <UnigineDecals.h>
#include <UnigineEngine.h>
#include <UnigineLog.h>
#include <UnigineMaterials.h>
#include <UnigineObjects.h>
#include <UnigineRender.h>
#include <UnigineWorld.h>

#include <QDir>
#include <QElapsedTimer>
#include <QFile>
#include <QObject>

using namespace Unigine;
using ::UnigineEditor::Undo;

namespace ArtistTool
{

namespace
{

// How many steps of the plugin exist: in the history, or being made.
int live_actions = 0;

// A change that comes this soon after the previous one joins its step.
const qint64 MERGE_MILLISECONDS = 1500;

NodePtr node_of(int id)
{
	const NodePtr node = World::getNodeByID(id);
	return node && !node.isDeleted() ? node : NodePtr();
}

} // namespace

////////////////////////////////////////////////////////////////////////////////
// SetNodeDataAction
////////////////////////////////////////////////////////////////////////////////
SetNodeDataAction::SetNodeDataAction()
	: entries_(std::make_shared<QVector<Entry>>())
{
	++live_actions;
}

SetNodeDataAction::~SetNodeDataAction()
{
	--live_actions;
}

void SetNodeDataAction::set(const NodePtr &node, const char *name, const QByteArray &value)
{
	if (!node)
		return;
	for (Entry &entry : *entries_)
	{
		if (entry.node == node->getID() && entry.name == name)
		{
			entry.after = value;
			return;
		}
	}
	Entry entry;
	entry.node = node->getID();
	entry.name = name;
	const char *before = node->getData(name);
	entry.before = before ? QByteArray(before) : QByteArray();
	entry.after = value;
	entries_->append(entry);
}

bool SetNodeDataAction::isEmpty() const
{
	for (const Entry &entry : *entries_)
	{
		if (entry.before != entry.after)
			return false;
	}
	return true;
}

void SetNodeDataAction::put(bool after) const
{
	for (const Entry &entry : *entries_)
	{
		if (const NodePtr node = node_of(entry.node))
			node->setData(entry.name.constData(), (after ? entry.after : entry.before).constData());
	}
}

void SetNodeDataAction::apply()
{
	put(true);
}

void SetNodeDataAction::undo()
{
	put(false);
}

void SetNodeDataAction::redo()
{
	put(true);
}

void SetNodeDataAction::push(SetNodeDataAction *action, bool merge)
{
	// The last step pushed here, while it is the last one of the history: anything
	// else that happens to the history (another step, an undo, a redo) ends it.
	static std::weak_ptr<QVector<Entry>> last;
	static QString last_key;
	static QElapsedTimer last_time;
	static bool pushing = false;
	static QObject context;
	static bool connected = false;
	if (!connected)
	{
		connected = true;
		QObject::connect(Undo::instance(), &Undo::activity, &context, []() {
			if (!pushing)
				last.reset();
		});
	}

	if (action->isEmpty())
	{
		delete action;
		return;
	}

	// The same data of the same nodes as the last step, right after it: that step
	// gets the new values.
	const std::shared_ptr<QVector<Entry>> previous = last.lock();
	if (merge && previous && last_time.isValid() && last_time.elapsed() < MERGE_MILLISECONDS
		&& last_key == action->merge_key_ && previous->size() == action->entries_->size())
	{
		bool same = true;
		for (int i = 0; i < previous->size() && same; ++i)
			same = (*previous)[i].node == (*action->entries_)[i].node && (*previous)[i].name == (*action->entries_)[i].name;
		if (same)
		{
			for (int i = 0; i < previous->size(); ++i)
				(*previous)[i].after = (*action->entries_)[i].after;
			action->apply();
			delete action;
			last_time.restart();
			// The step the editor has is changed behind its back: the world is told to be
			// modified, so that it is not taken for saved.
			emit Undo::instance()->worldModified();
			return;
		}
	}

	last = action->entries_;
	last_key = action->merge_key_;
	pushing = true;
	Undo::apply(action);
	pushing = false;
	last_time.restart();
}

////////////////////////////////////////////////////////////////////////////////
// SetMaterialsAction
////////////////////////////////////////////////////////////////////////////////
SetMaterialsAction::SetMaterialsAction()
{
	++live_actions;
}

SetMaterialsAction::~SetMaterialsAction()
{
	--live_actions;
}

UGUID SetMaterialsAction::materialOf(const NodePtr &node, int surface)
{
	MaterialPtr material;
	if (node->getType() == Node::DECAL_MESH)
		material = checked_ptr_cast<DecalMesh>(node)->getMaterial();
	else if (node->getType() == Node::OBJECT_MESH_STATIC)
	{
		ObjectMeshStaticPtr object = checked_ptr_cast<ObjectMeshStatic>(node);
		const int index = surface < 0 ? 0 : surface;
		if (index < object->getNumSurfaces())
			material = object->getMaterial(index);
	}
	return material ? material->getGUID() : UGUID();
}

void SetMaterialsAction::setMaterial(const NodePtr &node, const UGUID &guid, int surface)
{
	const MaterialPtr material = Materials::findMaterialByGUID(guid);
	if (!material)
		return;
	if (node->getType() == Node::DECAL_MESH)
		checked_ptr_cast<DecalMesh>(node)->setMaterial(material);
	else if (node->getType() == Node::OBJECT_MESH_STATIC)
	{
		ObjectMeshStaticPtr object = checked_ptr_cast<ObjectMeshStatic>(node);
		if (surface < 0)
			object->setMaterial(material, "*");
		else if (surface < object->getNumSurfaces())
			object->setMaterial(material, surface);
	}
}

void SetMaterialsAction::set(const NodePtr &node, const UGUID &material)
{
	if (!node)
		return;
	Entry entry;
	entry.node = node->getID();
	// Every surface of a mesh has its own material to come back to.
	const int surfaces = node->getType() == Node::OBJECT_MESH_STATIC ? checked_ptr_cast<ObjectMeshStatic>(node)->getNumSurfaces() : 1;
	for (int s = 0; s < surfaces; ++s)
		entry.before.append(materialOf(node, node->getType() == Node::OBJECT_MESH_STATIC ? s : -1));
	entry.after = material;
	entries_.append(entry);
}

void SetMaterialsAction::put(bool after) const
{
	for (const Entry &entry : entries_)
	{
		const NodePtr node = node_of(entry.node);
		if (!node)
			continue;
		if (after)
			setMaterial(node, entry.after);
		else if (node->getType() == Node::OBJECT_MESH_STATIC)
		{
			for (int s = 0; s < entry.before.size(); ++s)
				setMaterial(node, entry.before[s], s);
		} else if (!entry.before.isEmpty())
			setMaterial(node, entry.before[0]);
	}
}

void SetMaterialsAction::apply()
{
	put(true);
}

void SetMaterialsAction::undo()
{
	put(false);
}

void SetMaterialsAction::redo()
{
	put(true);
}

////////////////////////////////////////////////////////////////////////////////
// SetFilesAction
////////////////////////////////////////////////////////////////////////////////
namespace
{

QString disk_path(const QString &file)
{
	QString data = QDir::fromNativeSeparators(QString::fromUtf8(Engine::get()->getDataPath()));
	if (!data.endsWith(QLatin1Char('/')))
		data += QLatin1Char('/');
	return data + file;
}

QByteArray read_file(const QString &file)
{
	QFile in(disk_path(file));
	return in.open(QIODevice::ReadOnly) ? in.readAll() : QByteArray();
}

} // namespace

SetFilesAction::SetFilesAction()
{
	++live_actions;
}

SetFilesAction::~SetFilesAction()
{
	--live_actions;
}

void SetFilesAction::before(const QString &file)
{
	// Only a file that is there: a new one stays when the step is undone (the nodes
	// that use it go away with the undo of their own steps).
	if (!QFile::exists(disk_path(file)))
		return;
	Entry entry;
	entry.file = file;
	entry.guid = ::UnigineEditor::AssetManager::getAssetGUIDFromPath(file.toUtf8().constData());
	entry.before = read_file(file);
	entries_.append(entry);
}

void SetFilesAction::after(const QString &file)
{
	for (Entry &entry : entries_)
	{
		if (entry.file == file)
			entry.after = read_file(file);
	}
}

void SetFilesAction::put(bool after) const
{
	for (const Entry &entry : entries_)
	{
		// Where the asset is now; an asset that is gone is not made again.
		QString file = entry.file;
		if (entry.guid.isValid())
		{
			const String path = ::UnigineEditor::AssetManager::getAssetPathFromGUID(entry.guid);
			if (path.empty())
			{
				Log::warning("Artist Tools: %s is gone, it was not restored\n", entry.file.toUtf8().constData());
				continue;
			}
			file = QString::fromUtf8(path.get());
		}
		QFile out(disk_path(file));
		const QByteArray &bytes = after ? entry.after : entry.before;
		if (!out.open(QIODevice::WriteOnly | QIODevice::Truncate) || out.write(bytes) != bytes.size())
		{
			Log::error("Artist Tools: could not write %s\n", file.toUtf8().constData());
			continue;
		}
		out.close();
		Render::reloadResource(file.toUtf8().constData());
	}
}

void SetFilesAction::undo()
{
	put(false);
}

void SetFilesAction::redo()
{
	put(true);
}

////////////////////////////////////////////////////////////////////////////////
// SetPiecesAction
////////////////////////////////////////////////////////////////////////////////
PieceState PieceState::of(const NodePtr &node)
{
	PieceState state;
	state.name = node->getName();
	if (node->getType() == Node::DECAL_MESH)
	{
		DecalMeshPtr decal = checked_ptr_cast<DecalMesh>(node);
		state.mesh = decal->getMeshPath();
		state.radius = decal->getRadius();
		Surface surface;
		const MaterialPtr material = decal->getMaterial();
		surface.material = material ? material->getGUID() : UGUID();
		surface.max_visible = decal->getMaxVisibleDistance();
		surface.max_fade = decal->getMaxFadeDistance();
		state.surfaces.append(surface);
	} else if (node->getType() == Node::OBJECT_MESH_STATIC)
	{
		ObjectMeshStaticPtr object = checked_ptr_cast<ObjectMeshStatic>(node);
		state.mesh = object->getMeshPath();
		for (int s = 0; s < object->getNumSurfaces(); ++s)
		{
			Surface surface;
			surface.name = object->getSurfaceName(s);
			const MaterialPtr material = object->getMaterial(s);
			surface.material = material ? material->getGUID() : UGUID();
			surface.min_visible = object->getMinVisibleDistance(s);
			surface.max_visible = object->getMaxVisibleDistance(s);
			surface.min_fade = object->getMinFadeDistance(s);
			surface.max_fade = object->getMaxFadeDistance(s);
			surface.viewport_mask = object->getViewportMask(s);
			surface.shadow_mask = object->getShadowMask(s);
			surface.cast_shadow = object->getCastShadow(s);
			surface.cast_world_shadow = object->getCastWorldShadow(s);
			state.surfaces.append(surface);
		}
	}
	return state;
}

const PieceState::Surface *PieceState::surface(const char *name) const
{
	for (const Surface &surface : surfaces)
	{
		if (surface.name == name)
			return &surface;
	}
	return nullptr;
}

void PieceState::putSurface(const NodePtr &node, int surface, const Surface &settings)
{
	if (node->getType() != Node::OBJECT_MESH_STATIC)
		return;
	ObjectMeshStaticPtr object = checked_ptr_cast<ObjectMeshStatic>(node);
	if (surface < 0 || surface >= object->getNumSurfaces())
		return;
	if (const MaterialPtr material = Materials::findMaterialByGUID(settings.material))
		object->setMaterial(material, surface);
	object->setMinVisibleDistance(settings.min_visible, surface);
	object->setMaxVisibleDistance(settings.max_visible, surface);
	object->setMinFadeDistance(settings.min_fade, surface);
	object->setMaxFadeDistance(settings.max_fade, surface);
	object->setViewportMask(settings.viewport_mask, surface);
	object->setShadowMask(settings.shadow_mask, surface);
	object->setCastShadow(settings.cast_shadow, surface);
	object->setCastWorldShadow(settings.cast_world_shadow, surface);
}

void PieceState::put(const NodePtr &node) const
{
	node->setName(name.constData());
	if (node->getType() == Node::DECAL_MESH)
	{
		DecalMeshPtr decal = checked_ptr_cast<DecalMesh>(node);
		if (mesh != decal->getMeshPath())
			decal->setMeshPath(mesh.constData());
		decal->setRadius(radius);
		if (!surfaces.isEmpty())
		{
			if (const MaterialPtr material = Materials::findMaterialByGUID(surfaces[0].material))
				decal->setMaterial(material);
			decal->setMaxVisibleDistance(surfaces[0].max_visible);
			decal->setMaxFadeDistance(surfaces[0].max_fade);
		}
	} else if (node->getType() == Node::OBJECT_MESH_STATIC)
	{
		ObjectMeshStaticPtr object = checked_ptr_cast<ObjectMeshStatic>(node);
		if (mesh != object->getMeshPath())
			object->setMeshPath(mesh.constData());
		for (int s = 0; s < object->getNumSurfaces(); ++s)
		{
			if (const Surface *settings = surface(object->getSurfaceName(s)))
				putSurface(node, s, *settings);
		}
	}
}

SetPiecesAction::SetPiecesAction()
{
	++live_actions;
}

SetPiecesAction::~SetPiecesAction()
{
	--live_actions;
}

void SetPiecesAction::add(const NodePtr &node, const PieceState &before)
{
	if (!node)
		return;
	Entry entry;
	entry.node = node->getID();
	entry.before = before;
	entry.after = PieceState::of(node);
	entries_.append(entry);
}

void SetPiecesAction::put(bool after) const
{
	for (const Entry &entry : entries_)
	{
		if (const NodePtr node = node_of(entry.node))
			(after ? entry.after : entry.before).put(node);
	}
}

void SetPiecesAction::undo()
{
	put(false);
}

void SetPiecesAction::redo()
{
	put(true);
}

void releaseUndoActions()
{
	if (live_actions > 0)
		Undo::reset();
}

} // namespace ArtistTool
