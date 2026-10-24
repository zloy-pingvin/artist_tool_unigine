#include "NodeData.h"

#include <QRegularExpression>

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

QString copyName(const QString &source, const QSet<QString> &taken)
{
	// "road_6_2" is a copy of "road_6": a name with two numbers at the end takes the copy
	// number off. "road_6" has one only - it is the sixth road, not a copy.
	static const QRegularExpression copy_suffix(QString::fromUtf8("^(.*_\\d+)_\\d+$"));
	const QRegularExpressionMatch match = copy_suffix.match(source);
	const QString base = match.hasMatch() ? match.captured(1) : source;
	for (int i = 2;; ++i)
	{
		const QString name = QString::fromUtf8("%1_%2").arg(base).arg(i);
		if (!taken.contains(name))
			return name;
	}
}

} // namespace ArtistTool
