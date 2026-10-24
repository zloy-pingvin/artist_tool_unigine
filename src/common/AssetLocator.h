#pragma once

#include <UnigineCallback.h>
#include <UnigineGUID.h>
#include <UnigineNode.h>

#include <QElapsedTimer>
#include <QObject>
#include <QPoint>

class QMenu;

namespace ArtistTool
{

// Takes the user from a node in the world to the asset it is made of in the Asset
// Browser - something the editor has only as a button in the Parameters window. Two
// ways in: a hotkey of the editor (Ctrl+Shift+F by default, listed in the editor's
// hotkey settings under "Artist Tools", where it can be changed) and an item at the
// end of the context menu of nodes in the viewport and the World Hierarchy.
class AssetLocator : public QObject
{
public:
	explicit AssetLocator(QObject *parent = nullptr);
	~AssetLocator() override;

	// The asset a node is made of: the .node of a Node Reference, the mesh of a mesh
	// object; for a node inside a Node Reference - the asset of that reference. Empty
	// if there is none.
	static Unigine::UGUID assetOfNode(const Unigine::NodePtr &node);
	// The asset of the first selected node that has one.
	static Unigine::UGUID assetOfSelection();

	// Selects the asset of the selected node in the Asset Browser. False if none of
	// the selected nodes is made of an asset.
	bool showSelectedAsset();

protected:
	// Adds the item to the context menu of nodes as it opens.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void add_menu_item(QMenu *menu);

	// Where the last right click was: the item goes only into the menu a right click in a
	// viewport opens, not into the other context menus of the editor (the surfaces of the
	// Parameters window, the World Hierarchy...).
	quint64 last_right_press_{0};
	QPoint right_press_position_;
	QElapsedTimer right_press_time_;

	Unigine::EventConnections connections_;
};

} // namespace ArtistTool
