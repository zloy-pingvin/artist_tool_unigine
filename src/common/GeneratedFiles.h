#pragma once

#include <UnigineGUID.h>
#include <UnigineMesh.h>
#include <UnigineNode.h>

#include <QSet>
#include <QString>
#include <QStringList>
#include <QVector>

// The mesh files a Generate has written for a road or a line of wires: which of them are
// still its own to write over or to remove.
namespace ArtistTool
{

class SetFilesAction;
class SetNodeDataAction;
struct PieceState;

// The names of the data a tool keeps in the root of a road or a line about its last
// Generate: the files (one per line), their GUIDs, the base name and the folder of the
// files, and the root that made them (an editor copy has the data of its original).
struct GeneratedData
{
	const char *files;
	const char *guids;
	const char *base;
	const char *folder;
	const char *owner;
};

// The files of the last Generate of the root that are its own to write over or remove:
// none for an editor copy of another root ('own' tells which), and of the others only
// those still at their paths and that nothing outside the root has as its mesh.
QStringList ownGeneratedFiles(const Unigine::NodePtr &root, const GeneratedData &data, bool *own = nullptr);
// The base name of the files of the next Generate: the one of the last Generate if its
// files are free in the folder, else one made of 'name' ("name", "name_2", ...). A name
// is free when none of the files of the pieces is there, or only the root's own ones.
QString pickBaseName(const Unigine::NodePtr &root, const GeneratedData &data, bool own, const QString &folder,
	const QString &name, int num_pieces, const QStringList &own_files);
// The list of the files the root keeps after a Generate: those written, and those of
// the last Generate not written again (the pieces Ctrl+Z brings back use them; removing
// the root with its files takes them away).
QStringList generatedFileList(const QStringList &written, const QStringList &old_files);
// What the root remembers of the Generate, into the step.
void setGeneratedData(SetNodeDataAction *action, const Unigine::NodePtr &root, const GeneratedData &data,
	const QStringList &files, const QString &base, const QString &folder);
// The Generate as one step of the undo history: the files written over together with the
// pieces kept (their settings before), the kept pieces moved, the new ones in, the extra
// ones out, and what the root remembers ('data'). Takes all the actions.
void commitGenerate(SetFilesAction *files, const QVector<Unigine::NodePtr> &kept, const QVector<PieceState> &kept_before,
	const QVector<Unigine::Math::Mat4> &kept_transforms, const Unigine::Vector<Unigine::NodePtr> &created,
	const Unigine::Vector<Unigine::NodePtr> &removed, SetNodeDataAction *data);

// A name a file can have, made of the name of a node: letters, digits, '_' and '-';
// 'fallback' if nothing of it can be kept.
QString fileNameOf(const QString &name, const QString &fallback);
// The path of the mesh of a piece in the data of the project, "<folder>/<base>_NN.mesh";
// 'folder' is empty for the data folder itself.
QString assetFile(const QString &folder, const QString &base, int piece);
// The data folder of the project on the disk, with a slash at the end.
QString dataPath();
// A GUID from its text; an invalid one from an empty text.
Unigine::UGUID guidOf(const QString &text);

// Writes the meshes of the pieces to assetFile(folder, base, piece), each. The files
// written over go into 'action' (before and after), for undo. If one can not be written,
// the ones written so far get back what they had, the new ones are removed, 'action' is
// deleted and set to null, and false comes back with the reason in 'error'. 'files' gets
// the paths, 'existed' whether an asset was already there (then the engine reloads it,
// else the asset is imported). The editor is to be kept from picking the files up on its
// own while they are written (AssetManager::blockAutoRefresh()).
bool writeMeshFiles(const QString &folder, const QString &base, const QVector<Unigine::MeshPtr> &meshes,
	SetFilesAction *&action, QStringList &files, QVector<bool> &existed, QString &error);

// The GUIDs of the files, in the same order, as text: the root keeps them next to the
// paths, so that a file moved away and another one put in its place is told apart.
QStringList fileGuids(const QStringList &paths);

// The files of the list that are still those the Generate wrote: the asset at the path has
// the GUID it had then. A list of an older build has no GUIDs: its paths are trusted.
QStringList ownFiles(const QStringList &paths, const QStringList &guids);

// The files of the list a node of the world outside 'root' has as its mesh: a copy of the
// road made by the editor, a piece taken out of it. They are not written over or removed.
QSet<QString> filesUsedOutside(const QStringList &paths, const Unigine::NodePtr &root);

} // namespace ArtistTool
