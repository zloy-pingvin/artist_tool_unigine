#include "GeneratedFiles.h"
#include "Localization.h"
#include "UndoActions.h"

#include "NodeData.h"

#include <editor/UnigineActions.h>
#include <editor/UnigineAssetManager.h>
#include <editor/UnigineUndo.h>

#include <UnigineDecals.h>
#include <UnigineEngine.h>
#include <UnigineFileSystem.h>
#include <UnigineObjects.h>
#include <UnigineWorld.h>

#include <QDir>
#include <QFile>
#include <QFileInfo>
#include <QHash>

using namespace Unigine;
using ::UnigineEditor::AssetManager;

namespace ArtistTool
{

namespace
{

QString guid_text(const UGUID &guid)
{
	return guid.isValid() ? QString::fromUtf8(guid.makeString().get()) : QString();
}

} // namespace

QString fileNameOf(const QString &name, const QString &fallback)
{
	QString result;
	for (const QChar c : name)
		result += (c.isLetterOrNumber() && c.unicode() < 128) || c == QLatin1Char('-') ? c : QLatin1Char('_');
	return result.isEmpty() ? fallback : result;
}

QString assetFile(const QString &folder, const QString &base, int piece)
{
	const QString name = QString::fromUtf8("%1_%2.mesh").arg(base).arg(piece, 2, 10, QLatin1Char('0'));
	return folder.isEmpty() ? name : folder + QLatin1Char('/') + name;
}

QString dataPath()
{
	QString path = QDir::fromNativeSeparators(QString::fromUtf8(Engine::get()->getDataPath()));
	if (!path.endsWith(QLatin1Char('/')))
		path += QLatin1Char('/');
	return path;
}

UGUID guidOf(const QString &text)
{
	return text.isEmpty() ? UGUID() : UGUID(text.toUtf8().constData());
}

bool writeMeshFiles(const QString &folder, const QString &base, const QVector<MeshPtr> &meshes,
	SetFilesAction *&action, QStringList &files, QVector<bool> &existed, QString &error)
{
	QStringList fresh;
	for (int piece = 0; piece < meshes.size(); ++piece)
	{
		const QString file = assetFile(folder, base, piece);
		existed.append(AssetManager::isAsset(file.toUtf8().constData()));
		if (QFileInfo::exists(dataPath() + file))
			action->before(file);
		else
			fresh.append(file);
		if (!meshes[piece]->save((dataPath() + file).toUtf8().constData()))
		{
			action->undo();
			delete action;
			action = nullptr;
			for (const QString &written : fresh)
				QFile::remove(dataPath() + written);
			error = uiText("Could not save %1. Nothing was changed.", "Не удалось сохранить %1. Ничего не изменено.").arg(file);
			return false;
		}
		action->after(file);
		files.append(file);
	}
	return true;
}

QStringList fileGuids(const QStringList &paths)
{
	QStringList guids;
	for (const QString &path : paths)
		guids.append(guid_text(AssetManager::getAssetGUIDFromPath(path.toUtf8().constData())));
	return guids;
}

QStringList ownFiles(const QStringList &paths, const QStringList &guids)
{
	QStringList own;
	for (int i = 0; i < paths.size(); ++i)
	{
		const QString remembered = i < guids.size() ? guids[i] : QString();
		if (remembered.isEmpty() || guid_text(AssetManager::getAssetGUIDFromPath(paths[i].toUtf8().constData())) == remembered)
			own.append(paths[i]);
	}
	return own;
}

QSet<QString> filesUsedOutside(const QStringList &paths, const NodePtr &root)
{
	QSet<QString> used;
	QHash<QString, QString> path_of_guid;
	for (const QString &path : paths)
	{
		const QString guid = guid_text(AssetManager::getAssetGUIDFromPath(path.toUtf8().constData()));
		if (!guid.isEmpty())
			path_of_guid.insert(guid, path);
	}
	if (path_of_guid.isEmpty())
		return used;

	const int root_id = root ? root->getID() : -1;
	// The content of the node references too: a mesh of the road may be in a .node the
	// world has as a reference.
	Vector<NodePtr> nodes;
	World::getNodes(nodes, true, false);
	for (const NodePtr &node : nodes)
	{
		if (!node || node.isDeleted())
			continue;
		const char *mesh = nullptr;
		switch (node->getType())
		{
			case Node::OBJECT_MESH_STATIC: mesh = checked_ptr_cast<ObjectMeshStatic>(node)->getMeshPath(); break;
			case Node::OBJECT_MESH_CLUSTER: mesh = checked_ptr_cast<ObjectMeshCluster>(node)->getMeshPath(); break;
			case Node::OBJECT_MESH_CLUTTER: mesh = checked_ptr_cast<ObjectMeshClutter>(node)->getMeshPath(); break;
			case Node::OBJECT_MESH_SPLINE_CLUSTER: mesh = checked_ptr_cast<ObjectMeshSplineCluster>(node)->getMeshPath(); break;
			case Node::DECAL_MESH: mesh = checked_ptr_cast<DecalMesh>(node)->getMeshPath(); break;
			default: break;
		}
		if (!mesh || !*mesh)
			continue;
		const auto it = path_of_guid.constFind(guid_text(FileSystem::getGUID(mesh)));
		if (it == path_of_guid.constEnd())
			continue;
		// Inside the root itself: its own piece.
		bool inside = false;
		for (NodePtr n = node; n && !inside; n = n->getParent())
			inside = n->getID() == root_id;
		if (!inside)
			used.insert(it.value());
	}
	return used;
}

QStringList ownGeneratedFiles(const NodePtr &root, const GeneratedData &data, bool *own)
{
	const QString owner = dataString(root, data.owner);
	const bool own_files = owner.isEmpty() || owner == QString::number(root->getID());
	if (own)
		*own = own_files;
	if (!own_files)
		return QStringList();
	QStringList files = ownFiles(dataString(root, data.files).split(QLatin1Char('\n'), Qt::SkipEmptyParts),
		dataString(root, data.guids).split(QLatin1Char('\n')));
	const QSet<QString> shared = filesUsedOutside(files, root);
	files.removeIf([&shared](const QString &file) { return shared.contains(file); });
	return files;
}

QString pickBaseName(const NodePtr &root, const GeneratedData &data, bool own, const QString &folder,
	const QString &name, int num_pieces, const QStringList &own_files)
{
	const auto free_name = [&](const QString &base) {
		for (int piece = 0; piece < num_pieces; ++piece)
		{
			const QString file = assetFile(folder, base, piece);
			if (own_files.contains(file))
				continue;
			if (AssetManager::isExist(file.toUtf8().constData()) || QFileInfo::exists(dataPath() + file))
				return false;
		}
		return true;
	};
	QString base = own && dataString(root, data.folder) == folder ? dataString(root, data.base) : QString();
	if (!base.isEmpty() && free_name(base))
		return base;
	base = name;
	for (int i = 2; !free_name(base); ++i)
		base = QString::fromUtf8("%1_%2").arg(name).arg(i);
	return base;
}

QStringList generatedFileList(const QStringList &written, const QStringList &old_files)
{
	QStringList all = written;
	for (const QString &file : old_files)
	{
		if (!all.contains(file) && AssetManager::isAsset(file.toUtf8().constData()))
			all.append(file);
	}
	return all;
}

void setGeneratedData(SetNodeDataAction *action, const NodePtr &root, const GeneratedData &data,
	const QStringList &files, const QString &base, const QString &folder)
{
	action->set(root, data.files, files.join(QLatin1Char('\n')).toUtf8());
	action->set(root, data.guids, fileGuids(files).join(QLatin1Char('\n')).toUtf8());
	action->set(root, data.base, base.toUtf8());
	action->set(root, data.folder, folder.toUtf8());
	action->set(root, data.owner, QByteArray::number(root->getID()));
}

void commitGenerate(SetFilesAction *files, const QVector<NodePtr> &kept, const QVector<PieceState> &kept_before,
	const QVector<Math::Mat4> &kept_transforms, const Vector<NodePtr> &created, const Vector<NodePtr> &removed,
	SetNodeDataAction *data)
{
	SetPiecesAction *pieces = new SetPiecesAction;
	for (int i = 0; i < kept.size(); ++i)
		pieces->add(kept[i], kept_before[i]);
	pieces->setFiles(files);

	using namespace ::UnigineEditor;
	Undo::begin();
	if (pieces->isEmpty())
		delete pieces;
	else
		Undo::apply(pieces);
	for (int i = 0; i < kept.size(); ++i)
	{
		if (kept[i]->getWorldTransform() != kept_transforms[i])
			Undo::apply(new SetNodeTransformAction(kept[i], kept_transforms[i]));
	}
	if (created.size())
		Undo::apply(new CreateNodesAction(created));
	if (removed.size())
		Undo::apply(new RemoveNodesAction(removed));
	SetNodeDataAction::push(data, false);
	Undo::commit();
}

} // namespace ArtistTool
