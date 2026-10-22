#include "NodeData.h"

using namespace Unigine;

namespace ArtistTool
{

QString dataString(const NodePtr &node, const char *name)
{
	const char *data = node ? node->getData(name) : nullptr;
	return QString::fromUtf8(data ? data : "");
}

void putBytes(QByteArray &bytes, double value)
{
	bytes.append(reinterpret_cast<const char *>(&value), sizeof(value));
}

void putBytes(QByteArray &bytes, const Math::Vec3 &value)
{
	putBytes(bytes, double(value.x));
	putBytes(bytes, double(value.y));
	putBytes(bytes, double(value.z));
}

void putBytes(QByteArray &bytes, const Math::Mat4 &value)
{
	for (int column = 0; column < 4; ++column)
		putBytes(bytes, Math::Vec3(value.getColumn3(column)));
}

} // namespace ArtistTool
