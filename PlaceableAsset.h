#pragma once

#include <UnigineGUID.h>
#include <UnigineNode.h>
#include <UnigineString.h>
#include <UnigineVector.h>

#include <QString>
#include <QStringList>
#include <QVector>

namespace ArtistTool
{

// An asset of the project a tool can put into the world, and what node it becomes.
struct PlaceableAsset
{
	enum Kind
	{
		KIND_NONE = 0,
		KIND_NODE_REFERENCE,	// a .node asset - placed as a NodeReference
		KIND_MODEL,				// an imported model (.fbx ...) - its objects, with materials
		KIND_MESH,				// a .mesh - placed as an ObjectMeshStatic
	};

	Kind kind{KIND_NONE};
	// File the node is created from, as a GUID path ("guid://..."): the .node / .mesh
	// itself or a runtime of an imported asset (e.g. of an .fbx).
	Unigine::String path;
	// The asset of the project it comes from; empty if only a part of an imported
	// asset was selected. Needed to find the other assets of the same folder.
	Unigine::String asset_path;
	QString name;

	bool isValid() const { return kind != KIND_NONE; }
};

// Picks what to place for an asset of the project:
//  - a .node asset is placed as a NodeReference;
//  - a .mesh is placed as an ObjectMeshStatic;
//  - an imported model (.fbx ...) is placed as its objects with their materials: the
//    content of the node the import generated, not a reference to it. A model that
//    has no such node is placed as its first mesh.
bool resolveAsset(const Unigine::String &asset_path, PlaceableAsset &asset);

// Picks what to place for the given Asset Browser items / for the current selection
// of the editor. Returns false if nothing placeable is selected.
bool resolveSelection(const Unigine::Vector<Unigine::UGUID> &guids, PlaceableAsset &asset);
bool resolveSelectedAsset(PlaceableAsset &asset);
// Everything placeable among the items selected in the Asset Browser, each asset
// once, sorted by name.
QVector<PlaceableAsset> resolveSelectedAssets();

// Assets of the same type (file extension) in the folder of the given one, sorted by
// name - the list the editor's Quick asset change goes through.
QStringList siblingAssets(const Unigine::String &asset_path);

// Creates the node of the asset, named after it. The node is not given to the editor
// yet (see UnigineEditor::CreateNodesAction). Null if the asset is not valid.
Unigine::NodePtr createNode(const PlaceableAsset &asset);

QString kindLabel(PlaceableAsset::Kind kind);

// Selects the asset in the editor, which makes the Asset Browser show it. False if
// the file of the asset is not found.
bool showInAssetBrowser(const PlaceableAsset &asset);

} // namespace ArtistTool
