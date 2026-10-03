#include "PlaceableAsset.h"
#include "Localization.h"

#include <editor/UnigineActions.h>
#include <editor/UnigineAssetManager.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>

#include <UnigineFileSystem.h>
#include <UnigineNodes.h>
#include <UnigineObjects.h>
#include <UnigineWorld.h>

#include <QFileInfo>

#include <algorithm>
#include <cstring>

using namespace Unigine;
using ::UnigineEditor::AssetManager;

namespace ArtistTool
{

namespace
{

QString extension_of(const char *path)
{
	return QFileInfo(QString::fromUtf8(path)).suffix().toLower();
}

// Extension of the file behind a GUID: "node", "mesh", ...
QString extension_of(const UGUID &guid)
{
	QString extension = QString::fromUtf8(FileSystem::getExtension(guid).get()).toLower();
	if (extension.startsWith('.'))
		extension.remove(0, 1);
	return extension;
}

QString runtime_name(const UGUID &guid)
{
	const String alias = AssetManager::getRuntimeAlias(guid);
	if (!alias.empty())
		return QString::fromUtf8(alias.get());
	return QFileInfo(QString::fromUtf8(FileSystem::getVirtualPath(guid).get())).fileName();
}

} // namespace

bool resolveAsset(const String &asset_path, PlaceableAsset &asset)
{
	if (asset_path.empty() || !AssetManager::isAsset(asset_path))
		return false;

	asset.asset_path = asset_path;
	asset.name = QFileInfo(QString::fromUtf8(asset_path.get())).fileName();

	// Files are referred to by GUID, the way the editor does it: such a reference
	// survives renaming and moving the asset.
	const UGUID guid = AssetManager::getAssetGUIDFromPath(asset_path);
	const String reference = guid.isValid() ? FileSystem::guidToPath(guid) : asset_path;

	const QString extension = extension_of(asset_path.get());
	if (extension == QLatin1String("node"))
	{
		asset.kind = PlaceableAsset::KIND_NODE_REFERENCE;
		asset.path = reference;
		return true;
	}
	if (extension == QLatin1String("mesh"))
	{
		asset.kind = PlaceableAsset::KIND_MESH;
		asset.path = reference;
		return true;
	}

	// A container: look at what the import produced.
	const Vector<UGUID> runtimes = AssetManager::getRuntimeGUIDs(asset_path);
	for (const UGUID &runtime : runtimes)
	{
		if (extension_of(runtime) != QLatin1String("node"))
			continue;
		asset.kind = PlaceableAsset::KIND_MODEL;
		asset.path = FileSystem::guidToPath(runtime);
		return true;
	}
	for (const UGUID &runtime : runtimes)
	{
		if (extension_of(runtime) != QLatin1String("mesh"))
			continue;
		asset.kind = PlaceableAsset::KIND_MESH;
		asset.path = FileSystem::guidToPath(runtime);
		return true;
	}

	return false;
}

bool resolveSelection(const Vector<UGUID> &guids, PlaceableAsset &asset)
{
	// Assets: the selected item is a file of the project.
	for (const UGUID &guid : guids)
	{
		if (resolveAsset(AssetManager::getAssetPathFromGUID(guid), asset))
			return true;
	}

	// Runtimes: the selected item is a part of an imported asset.
	for (const char *wanted : {"node", "mesh"})
	{
		for (const UGUID &guid : guids)
		{
			if (extension_of(guid) != QLatin1String(wanted))
				continue;
			asset.kind = strcmp(wanted, "node") == 0 ? PlaceableAsset::KIND_MODEL : PlaceableAsset::KIND_MESH;
			asset.path = FileSystem::guidToPath(guid);
			asset.asset_path = String();
			asset.name = runtime_name(guid);
			return true;
		}
	}

	return false;
}

bool resolveSelectedAsset(PlaceableAsset &asset)
{
	const ::UnigineEditor::SelectorGUIDs *selector = ::UnigineEditor::Selection::getSelectorRuntimes();
	if (!selector || selector->empty())
		return false;
	return resolveSelection(selector->getGUIDs(), asset);
}

QVector<PlaceableAsset> resolveSelectedAssets()
{
	QVector<PlaceableAsset> assets;

	const ::UnigineEditor::SelectorGUIDs *selector = ::UnigineEditor::Selection::getSelectorRuntimes();
	if (!selector || selector->empty())
		return assets;

	// Several selected items may lead to one and the same file (e.g. the parts of one
	// imported model): it is taken once.
	const auto add = [&assets](const PlaceableAsset &asset) {
		for (const PlaceableAsset &added : assets)
		{
			if (added.path == asset.path)
				return;
		}
		assets.append(asset);
	};

	const Vector<UGUID> guids = selector->getGUIDs();
	for (const UGUID &guid : guids)
	{
		// One item at a time, by the same rules as for a single selected asset.
		Vector<UGUID> single;
		single.append(guid);

		PlaceableAsset asset;
		if (resolveSelection(single, asset))
			add(asset);
	}

	std::sort(assets.begin(), assets.end(), [](const PlaceableAsset &a, const PlaceableAsset &b) {
		return a.name.compare(b.name, Qt::CaseInsensitive) < 0;
	});
	return assets;
}

QStringList siblingAssets(const String &asset_path)
{
	const QFileInfo info(QString::fromUtf8(asset_path.get()));
	const QString extension = info.suffix().toLower();
	const QString directory = info.path();

	Vector<String> paths = AssetManager::getAssetPathsForDirectory(directory.toUtf8().constData());
	if (paths.empty())
		paths = AssetManager::getAssetPathsForDirectory((directory + QLatin1Char('/')).toUtf8().constData());

	QStringList siblings;
	for (const String &sibling : paths)
	{
		const QString sibling_path = QString::fromUtf8(sibling.get());
		if (QFileInfo(sibling_path).suffix().toLower() == extension)
			siblings.append(sibling_path);
	}
	siblings.sort(Qt::CaseInsensitive);
	return siblings;
}

NodePtr createNode(const PlaceableAsset &asset)
{
	NodePtr node;
	switch (asset.kind)
	{
		case PlaceableAsset::KIND_NODE_REFERENCE: node = NodeReference::create(asset.path.get()); break;
		// The content of the node itself, not a reference to it.
		case PlaceableAsset::KIND_MODEL: node = World::loadNode(asset.path.get()); break;
		case PlaceableAsset::KIND_MESH: node = ObjectMeshStatic::create(asset.path.get()); break;
		default: break;
	}

	if (node)
		node->setName(QFileInfo(asset.name).completeBaseName().toUtf8().constData());
	return node;
}

QString kindLabel(PlaceableAsset::Kind kind)
{
	switch (kind)
	{
		case PlaceableAsset::KIND_NODE_REFERENCE: return QString::fromUtf8("Node Reference");
		case PlaceableAsset::KIND_MODEL:          return uiText("Model", "Модель");
		case PlaceableAsset::KIND_MESH:           return QString::fromUtf8("Mesh Static");
		default:                                  return QString();
	}
}

bool showInAssetBrowser(const PlaceableAsset &asset)
{
	// The asset of the project if it is known, otherwise the file the node is made of.
	UGUID guid;
	if (!asset.asset_path.empty())
		guid = AssetManager::getAssetGUIDFromPath(asset.asset_path);
	if (!guid.isValid() && !asset.path.empty())
		guid = FileSystem::getGUID(asset.path.get());
	if (!guid.isValid())
		return false;

	// The Asset Browser shows what is selected in the editor.
	Vector<UGUID> guids;
	guids.append(guid);
	::UnigineEditor::SelectionAction::applySelection(
		::UnigineEditor::SelectorGUIDs::createRuntimesSelector(guids));
	::UnigineEditor::SelectionAction::refreshSelection(true);
	return true;
}

} // namespace ArtistTool
