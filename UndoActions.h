#pragma once

#include <editor/UnigineUndo.h>

#include <UnigineGUID.h>
#include <UnigineNode.h>

#include <QByteArray>
#include <QString>
#include <QVector>

#include <memory>

// Steps of the undo history of the editor for what its own actions do not cover: the
// data kept in nodes (the settings of a road, the values of its points) and the
// materials of nodes.
//
// The steps are made by the plugin, so their code goes away with it: when the plugin
// is unloaded while some of them are in the history, the history is cleared (see
// releaseUndoActions()).
namespace ArtistTool
{

// Values in the data of nodes, changed as one step. The nodes are found by their IDs,
// so the step still works after they were removed and brought back by other steps.
class SetNodeDataAction final : public ::UnigineEditor::Action
{
public:
	SetNodeDataAction();
	~SetNodeDataAction() override;

	// The value the data 'name' of the node is to get; what it has now is kept for undo.
	void set(const Unigine::NodePtr &node, const char *name, const QByteArray &value);
	bool isEmpty() const;
	// What is changed, for merging (see push()): changes of the same data join only if
	// they change the same thing. Empty - any change of that data.
	void setMergeKey(const QString &key) { merge_key_ = key; }

	void apply() override;
	void undo() override;
	void redo() override;

	// Applies the action as a step of the history and takes it over; an action that
	// changes nothing is dropped. With 'merge', a change of the same data of the same
	// nodes right after the previous one - a value dragged in a field, typed digit by
	// digit - joins that step instead, so that one Ctrl+Z takes it all back.
	static void push(SetNodeDataAction *action, bool merge = true);

private:
	struct Entry
	{
		int node{0};
		QByteArray name;
		QByteArray before;
		QByteArray after;
	};
	void put(bool after) const;

	// Shared with push(), which tells by it whether the last step is still there.
	std::shared_ptr<QVector<Entry>> entries_;
	QString merge_key_;
};

// The materials of nodes (meshes and decals) changed as one step.
class SetMaterialsAction final : public ::UnigineEditor::Action
{
public:
	SetMaterialsAction();
	~SetMaterialsAction() override;

	// The material all the surfaces of the node are to get; the ones every surface has
	// now are kept for undo.
	void set(const Unigine::NodePtr &node, const Unigine::UGUID &material);
	bool isEmpty() const { return entries_.isEmpty(); }

	void apply() override;
	void undo() override;
	void redo() override;

	// The material of a surface of the node (-1 - of the first one), and giving one to
	// a surface (-1 - to all of them).
	static Unigine::UGUID materialOf(const Unigine::NodePtr &node, int surface = -1);
	static void setMaterial(const Unigine::NodePtr &node, const Unigine::UGUID &material, int surface = -1);

private:
	struct Entry
	{
		int node{0};
		QVector<Unigine::UGUID> before;
		Unigine::UGUID after;
	};
	void put(bool after) const;

	QVector<Entry> entries_;
};

// Files of the project overwritten, as one step: undo writes back what they had and
// the engine reloads them. Files are not created or removed by it: an asset removed
// and made again would get another GUID, and what refers to it would lose it.
class SetFilesAction final : public ::UnigineEditor::Action
{
public:
	SetFilesAction();
	~SetFilesAction() override;

	// Call before the file (a path in the data of the project) is written: it is kept
	// for undo if it is there; then after().
	void before(const QString &file);
	// Call after the file is written: what it has now is kept for redo.
	void after(const QString &file);
	bool isEmpty() const { return entries_.isEmpty(); }

	// The files are written already when the step is applied.
	void apply() override {}
	void undo() override;
	void redo() override;

private:
	struct Entry
	{
		QString file;
		// The asset is found by it when the step is undone or redone: the file may
		// have been moved or renamed since.
		Unigine::UGUID guid;
		QByteArray before;
		QByteArray after;
	};
	void put(bool after) const;

	QVector<Entry> entries_;
};

// What a road piece has of its own: its name, its mesh and the settings of its
// surfaces (a decal: its material, depth and distances).
struct PieceState
{
	struct Surface
	{
		QByteArray name;
		Unigine::UGUID material;
		float min_visible{0.0f};
		float max_visible{0.0f};
		float min_fade{0.0f};
		float max_fade{0.0f};
		int viewport_mask{0};
		int shadow_mask{0};
		bool cast_shadow{false};
		bool cast_world_shadow{false};
	};
	QByteArray name;
	QByteArray mesh;
	QVector<Surface> surfaces;
	float radius{0.0f};

	static PieceState of(const Unigine::NodePtr &node);
	// The surface of that name, or nullptr.
	const Surface *surface(const char *name) const;
	// Gives a surface of the mesh node the settings.
	static void putSurface(const Unigine::NodePtr &node, int surface, const Surface &settings);
	// Gives the node all of it: the mesh first, then its surfaces by their names.
	void put(const Unigine::NodePtr &node) const;
};

// Road pieces changed in place by a Generate, as one step: undo gives them back the
// mesh, the name and the surface settings they had.
class SetPiecesAction final : public ::UnigineEditor::Action
{
public:
	SetPiecesAction();
	~SetPiecesAction() override;

	// 'before' - what the node had before it was changed; what it has now is kept for
	// redo.
	void add(const Unigine::NodePtr &node, const PieceState &before);
	bool isEmpty() const { return entries_.isEmpty(); }

	// The pieces are changed already when the step is applied.
	void apply() override {}
	void undo() override;
	void redo() override;

private:
	struct Entry
	{
		int node{0};
		PieceState before;
		PieceState after;
	};
	void put(bool after) const;

	QVector<Entry> entries_;
};

// Clears the undo history of the editor if it holds steps made by the plugin. Called
// when the plugin is unloaded: the editor could not undo or even delete them later.
void releaseUndoActions();

} // namespace ArtistTool
