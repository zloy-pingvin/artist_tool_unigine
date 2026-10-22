#pragma once

#include <UnigineMathLib.h>
#include <UnigineNode.h>

#include <QByteArray>
#include <QString>

// The data the tools keep in their nodes, and the bytes they compare to tell a change.
namespace ArtistTool
{

// The data of the node by its name; empty if there is none.
QString dataString(const Unigine::NodePtr &node, const char *name);

// Appends the value to the bytes as it is: a state made of them is compared, not read.
void putBytes(QByteArray &bytes, double value);
void putBytes(QByteArray &bytes, const Unigine::Math::Vec3 &value);
void putBytes(QByteArray &bytes, const Unigine::Math::Mat4 &value);

} // namespace ArtistTool
