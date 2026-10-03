#include "Localization.h"

#include <QAction>
#include <QApplication>
#include <QFile>
#include <QJsonDocument>
#include <QJsonObject>
#include <QStandardPaths>
#include <QWidget>

namespace ArtistTool
{

namespace
{

struct Language
{
	bool ru_labels{false};
	bool ru_tooltips{false};
};

// The editor has no API to query its UI language: read its own config
// (unigine/Editor/editor1.1.cfg in the per-user data folder, JSON). "editor/language" is written
// when the language differs from the default English; "editor/keep_original_ui" = "1"
// keeps the captions English and translates only the tooltips. Fallback: Cyrillic in
// the main menu "File" action.
Language detect_language()
{
	Language language;

	// The config is in the per-user data folder: %LOCALAPPDATA% on Windows,
	// ~/.local/share on Linux.
	QString data_dir = qEnvironmentVariable("LOCALAPPDATA");
	if (data_dir.isEmpty())
		data_dir = QStandardPaths::writableLocation(QStandardPaths::GenericDataLocation);
	const QString config_path = data_dir + "/unigine/Editor/editor1.1.cfg";
	QFile file(config_path);
	if (file.open(QIODevice::ReadOnly))
	{
		const QJsonObject editor = QJsonDocument::fromJson(file.readAll()).object()
			.value("editor").toObject();
		const bool ru = editor.value("language").toString().startsWith("ru", Qt::CaseInsensitive);
		const bool tooltips_only = editor.value("keep_original_ui").toString() == QLatin1String("1");
		language.ru_tooltips = ru;
		language.ru_labels = ru && !tooltips_only;
		return language;
	}

	for (QWidget *top : QApplication::topLevelWidgets())
	{
		for (QAction *action : top->findChildren<QAction *>())
		{
			if (action->objectName() != QLatin1String("menuFile"))
				continue;
			for (QChar c : action->text())
			{
				if (c.unicode() >= 0x0400 && c.unicode() <= 0x04FF)
				{
					language.ru_labels = true;
					language.ru_tooltips = true;
					return language;
				}
			}
			return language;
		}
	}
	return language;
}

// Detected once per load of the plugin: the editor applies a change of its language
// only after a restart.
const Language &editor_language()
{
	static const Language language = detect_language();
	return language;
}

// Replaces every "[[english|russian]]" with one of the two names.
QString resolve_names(const QString &text, bool russian)
{
	QString result;
	int position = 0;
	while (true)
	{
		const int open = text.indexOf(QLatin1String("[["), position);
		const int separator = open < 0 ? -1 : text.indexOf(QLatin1Char('|'), open + 2);
		const int close = separator < 0 ? -1 : text.indexOf(QLatin1String("]]"), separator + 1);
		if (close < 0)
			break;

		result += text.mid(position, open - position);
		result += russian ? text.mid(separator + 1, close - separator - 1)
			: text.mid(open + 2, separator - open - 2);
		position = close + 2;
	}
	result += text.mid(position);
	return result;
}

} // namespace

QString uiText(const char *en, const char *ru)
{
	return QString::fromUtf8(editor_language().ru_labels ? ru : en);
}

Localization::Localization()
	: ru_labels_(editor_language().ru_labels)
	, ru_tooltips_(editor_language().ru_tooltips)
{
}

QString Localization::tip(const char *en, const char *ru) const
{
	if (!ru_tooltips_)
		return QString::fromUtf8(en);
	return resolve_names(QString::fromUtf8(ru), ru_labels_);
}

} // namespace ArtistTool
