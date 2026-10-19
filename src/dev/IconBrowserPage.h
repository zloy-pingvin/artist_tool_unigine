#pragma once

#include "Localization.h"

#include <QWidget>

class QComboBox;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSlider;

namespace ArtistTool
{

// "Icons" page: every icon a tool can use - the ones built into the editor, the Qt
// standard ones and the user's own PNG files from the plugin "icons" folder. A click
// on an icon shows the path a tool uses to load it.
class IconBrowserPage final : public QWidget
{
public:
	explicit IconBrowserPage(QWidget *parent = nullptr);

	// Folder with the user's own icons, next to the plugin DLL.
	static QString pluginIconsDir();

protected:
	// The icons are collected on the first show, not when the editor starts.
	void showEvent(QShowEvent *event) override;

private:
	enum Source
	{
		SOURCE_ALL = 0,
		SOURCE_EDITOR,
		SOURCE_QT,
		SOURCE_PLUGIN,
	};

	void reload();
	void add_item(const QIcon &icon, const QString &name, const QString &path, Source source,
		const QString &details);
	void apply_filter();
	void apply_icon_size();
	void update_info();

	Localization loc_;
	bool loaded_{false};

	QComboBox *source_{nullptr};
	QLineEdit *filter_{nullptr};
	QSlider *size_{nullptr};
	QListWidget *list_{nullptr};
	QLabel *count_label_{nullptr};
	QLabel *info_label_{nullptr};
	QPushButton *copy_button_{nullptr};
};

} // namespace ArtistTool
