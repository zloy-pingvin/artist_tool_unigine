#include "AssetLocator.h"
#include "Localization.h"
#include "PlaceableAsset.h"
#include "ViewportPicking.h"

#include <editor/UnigineAssetManager.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>
#include <editor/UnigineShortcutManager.h>

#include <UnigineFileSystem.h>
#include <UnigineInput.h>
#include <UnigineLog.h>
#include <UnigineNodes.h>
#include <UnigineObjects.h>

#include <QApplication>
#include <QEvent>
#include <QMenu>
#include <QMouseEvent>

#include <cstring>

using namespace Unigine;
using ::UnigineEditor::AssetManager;
using ::UnigineEditor::Selection;
using ::UnigineEditor::SelectorNodes;
using ::UnigineEditor::Shortcut;
using ::UnigineEditor::ShortcutContextPtr;
using ::UnigineEditor::ShortcutManager;
using ::UnigineEditor::ShortcutPtr;

namespace ArtistTool
{

namespace
{

// The hotkey, as the editor's hotkey settings show it.
const char SHORTCUT_CONTEXT[] = "artist_tool";
const char SHORTCUT_ID[] = "show_asset_of_node";

// The context menu of nodes is the editor's own menu class, created without a
// parent widget for the time it is open.
const char EDITOR_MENU_CLASS[] = "Menu";
const char MENU_ITEM_PROPERTY[] = "artist_tool_show_asset";

// The file a node is made of, as the node refers to it; null if the node is not made
// of a file.
const char *file_of_node(const NodePtr &node)
{
	switch (node->getType())
	{
		case Node::NODE_REFERENCE:             return checked_ptr_cast<NodeReference>(node)->getNodePath();
		case Node::OBJECT_MESH_STATIC:         return checked_ptr_cast<ObjectMeshStatic>(node)->getMeshPath();
		case Node::OBJECT_MESH_SKINNED:        return checked_ptr_cast<ObjectMeshSkinned>(node)->getMeshPath();
		case Node::OBJECT_MESH_CLUSTER:        return checked_ptr_cast<ObjectMeshCluster>(node)->getMeshPath();
		case Node::OBJECT_MESH_CLUTTER:        return checked_ptr_cast<ObjectMeshClutter>(node)->getMeshPath();
		case Node::OBJECT_MESH_SPLINE_CLUSTER: return checked_ptr_cast<ObjectMeshSplineCluster>(node)->getMeshPath();
		case Node::OBJECT_WATER_MESH:          return checked_ptr_cast<ObjectWaterMesh>(node)->getMeshPath();
		default:                               return nullptr;
	}
}

} // namespace

AssetLocator::AssetLocator(QObject *parent)
	: QObject(parent)
{
	// The hotkey. The editor keeps the key the user gives it in the hotkey settings,
	// so the default is set only while the shortcut has none.
	ShortcutContextPtr context = ShortcutManager::hasContext(SHORTCUT_CONTEXT)
		? ShortcutManager::getContext(SHORTCUT_CONTEXT)
		: ShortcutManager::createContext(SHORTCUT_CONTEXT, "Artist Tools");
	if (context)
	{
		ShortcutPtr shortcut = context->hasShortcut(SHORTCUT_ID)
			? context->getShortcut(SHORTCUT_ID) : context->createShortcut(SHORTCUT_ID);
		if (shortcut)
		{
			shortcut->setTitle(uiText("Show Asset of Node in Asset Browser",
				"Показать ассет ноды в Asset Browser").toUtf8().constData());
			shortcut->setTooltip(uiText(
				"Selects the asset the selected node is made of (its .node or mesh) in the Asset Browser.",
				"Выделяет в Asset Browser ассет, из которого сделана выделенная нода (её .node или меш).")
				.toUtf8().constData());
			if (shortcut->getKey() == Input::KEY_UNKNOWN && shortcut->getModifierMask() == 0)
			{
				shortcut->setKey(Input::KEY_F);
				shortcut->setModifierMask(Shortcut::MODIFIER_MASK_CTRL | Shortcut::MODIFIER_MASK_SHIFT);
			}
			shortcut->setEnabled(true);
			shortcut->getEventPressed().connect(connections_, [this]() { showSelectedAsset(); });
		}
		context->setEnabled(true);
		context->setActive(true);
	}

	// The menu item.
	qApp->installEventFilter(this);
}

AssetLocator::~AssetLocator()
{
	qApp->removeEventFilter(this);
	connections_.disconnectAll();
	if (ShortcutManager::hasContext(SHORTCUT_CONTEXT))
		ShortcutManager::removeContext(SHORTCUT_CONTEXT);
}

UGUID AssetLocator::assetOfNode(const NodePtr &node)
{
	for (NodePtr n = node; n && !n.isDeleted(); n = n->getPossessor())
	{
		const char *file = file_of_node(n);
		if (!file || !*file)
			continue;

		const UGUID guid = FileSystem::getGUID(file);
		if (!guid.isValid())
			continue;

		// The asset of the project the file belongs to (a mesh may be a part of an
		// imported model): that is what the Asset Browser lists.
		const String asset_path = AssetManager::getAssetPathFromGUID(guid);
		const UGUID asset = asset_path.empty() ? UGUID() : AssetManager::getAssetGUIDFromPath(asset_path);
		return asset.isValid() ? asset : guid;
	}
	return UGUID();
}

UGUID AssetLocator::assetOfSelection()
{
	if (const SelectorNodes *selector = Selection::getSelectorNodes())
	{
		for (const NodePtr &node : selector->getNodes())
		{
			const UGUID asset = assetOfNode(node);
			if (asset.isValid())
				return asset;
		}
	}
	return UGUID();
}

bool AssetLocator::showSelectedAsset()
{
	const UGUID asset = assetOfSelection();
	return asset.isValid() && showAssetInBrowser(asset);
}

bool AssetLocator::eventFilter(QObject *watched, QEvent *event)
{
	// A right press, where it was. In a viewport it is delivered to the window of the
	// engine inside it, not to a widget: where it was is looked up when the menu comes.
	if (event->type() == QEvent::MouseButtonPress && static_cast<QMouseEvent *>(event)->button() == Qt::RightButton)
	{
		const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
		if (mouse->timestamp() != last_right_press_)
		{
			last_right_press_ = mouse->timestamp();
			right_press_position_ = mouse->globalPosition().toPoint();
			right_press_time_.start();
		}
	}
	if (event->type() == QEvent::Show && watched->isWidgetType()
		&& strcmp(watched->metaObject()->className(), EDITOR_MENU_CLASS) == 0)
	{
		QMenu *menu = static_cast<QMenu *>(watched);
		// A menu of its own (the context menu of nodes) a right click in a viewport has
		// just opened, with nodes selected, and not one that already has the item. Just: a
		// menu opened from the keyboard later is not one of a click.
		const bool recent = right_press_time_.isValid() && right_press_time_.elapsed() < 1500;
		QWidget *under = recent ? QApplication::widgetAt(right_press_position_) : nullptr;
		const bool from_viewport = under && findViewportWidget(under);
		if (from_viewport && !menu->parentWidget() && !menu->property(MENU_ITEM_PROPERTY).toBool()
			&& assetOfSelection().isValid())
			add_menu_item(menu);
		right_press_time_.invalidate();
	}
	return false;
}

void AssetLocator::add_menu_item(QMenu *menu)
{
	menu->setProperty(MENU_ITEM_PROPERTY, true);
	menu->addSeparator();
	// No icon: the other items of the editor's menu have none.
	QAction *action = menu->addAction(uiText("Show Asset in Asset Browser", "Показать ассет в Asset Browser"));
	action->setToolTip(uiText(
		"<p>Select the asset the node is made of (its .node or mesh) in the Asset Browser.</p>",
		"<p>Выделить в Asset Browser ассет, из которого сделана нода (её .node или меш).</p>"));
	connect(action, &QAction::triggered, this, [this]() { showSelectedAsset(); });
}

} // namespace ArtistTool
