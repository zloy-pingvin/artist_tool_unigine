#pragma once

#include <UnigineMathLib.h>
#include <UnigineNode.h>

#include <QByteArray>
#include <QSet>
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

// The name of a copy of a line (a road, a line of wires) named 'source': the first one of
// "<source>_2", "<source>_3"... not in 'taken'. A copy of a copy goes on with the numbers
// of the line it was copied from - the copies of "road_6" and of "road_6_2" are both
// "road_6_N" - instead of adding one more number every time.
QString copyName(const QString &source, const QSet<QString> &taken);

} // namespace ArtistTool
