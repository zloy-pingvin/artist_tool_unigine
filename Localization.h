#pragma once

#include <QString>

namespace ArtistTool
{

// The language of the plugin follows the translation settings of the editor:
//  - English editor: everything is in English;
//  - Russian editor in the "translate tooltips only" mode: captions stay English,
//    tooltips are Russian;
//  - Russian editor, full translation: captions, messages and tooltips are Russian.
// The Debug tab is for developing the plugin and is not translated.

// A caption, a button, a message: Russian only in the full translation mode.
QString uiText(const char *en, const char *ru);

class Localization
{
public:
	Localization();

	bool isRussianTooltips() const { return ru_tooltips_; }
	bool isRussianLabels() const { return ru_labels_; }

	// A tooltip: Russian in both Russian modes. A Russian tooltip names the controls of
	// the plugin the way they are written on the screen, so such a name is given in
	// both languages - "[[Add Points|Добавить точки]]" - and the one in the language
	// of the captions is shown.
	QString tip(const char *en, const char *ru) const;

private:
	bool ru_labels_{false};
	bool ru_tooltips_{false};
};

} // namespace ArtistTool
