#include "ArtistToolWindow.h"
#include "Localization.h"
#include "ClutterUnpackPage.h"
#include "ObjectPlacerPage.h"
#include "PathPlacerPage.h"

#ifdef ARTIST_TOOL_DEV_PAGES
	#include "IconBrowserPage.h"
	#include "UiGalleryPage.h"
#endif

#include <editor/UniginePluginInfo.h>
#include <editor/UniginePluginManager.h>

#include <QGroupBox>
#include <QIcon>
#include <QLabel>
#include <QTabWidget>
#include <QTimer>
#include <QVBoxLayout>

#include <cstring>

namespace ArtistTool
{

ArtistToolWindow::ArtistToolWindow(QWidget *parent)
	: QWidget(parent)
{
	// No QStringLiteral anywhere in the plugin: its strings point into the plugin
	// DLL, and the editor keeps some of them (e.g. the window title) after the plugin
	// is reloaded - reading them then crashes the editor.
	setWindowTitle(QString::fromUtf8("Artist Tools"));
	setObjectName(QString::fromUtf8("ArtistToolWindow"));

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	tabs_ = new QTabWidget;
	tabs_->setDocumentMode(true);
	layout->addWidget(tabs_);

	// New tools are added here as tabs, the ones used most often first. The icons are
	// the editor's own ones, taken from its resources. The Russian names are short so
	// that all the tabs fit the window. The Debug tab is not translated.
	object_placer_page_ = new ObjectPlacerPage;
	tabs_->addTab(object_placer_page_, QIcon(QString::fromUtf8(":/images/tools/icon_snap_to_surface.png")),
		uiText("Object Placer", "Расстановка"));

	path_placer_page_ = new PathPlacerPage;
	tabs_->addTab(path_placer_page_, QIcon(QString::fromUtf8(":/sandworm/images/icon_fence.png")),
		uiText("Path Placer", "По кривой"));

	clutter_unpack_page_ = new ClutterUnpackPage;
	tabs_->addTab(clutter_unpack_page_, QIcon(QString::fromUtf8(":/sandworm/images/icon_vegetation.png")),
		uiText("Unpack Clutter", "Распаковка"));

	tabs_->addTab(build_debug_page(), QString::fromUtf8("Debug"));

	layout->addWidget(build_about_label());
}

// The credit line at the bottom of the window, the same as in the Texture Baker
// plugin: the name of the plugin, its version, the author and the links. The version
// is read from the manifest the plugin was built with (artist_tool.json), so the
// window can not show a version the manifest disagrees with.
QWidget *ArtistToolWindow::build_about_label()
{
	QString full_version;
	for (const ::UnigineEditor::PluginInfo *info : ::UnigineEditor::PluginManager::plugins())
	{
		if (info && info->name() && strcmp(info->name(), "artist_tool") == 0 && info->version())
			full_version = QString::fromUtf8(info->version());
	}
	// The manifest carries the 4-part form ("1.0.0.0"); major.minor.patch is shown.
	const QString short_version = full_version.count(QLatin1Char('.')) >= 2
		? full_version.section(QLatin1Char('.'), 0, 2) : full_version;
	const QString title = short_version.isEmpty() ? QString::fromUtf8("Artist Tools")
		: QString::fromUtf8("Artist Tools v%1").arg(short_version);

	QLabel *label = new QLabel(
		QString::fromUtf8("<span style=\"color:#9a9a9a;\">%1 by zloy_pingvin</span>&nbsp;&nbsp;").arg(title)
		+ QString::fromUtf8(
			"<a href=\"https://github.com/zloy-pingvin/artist_tool_unigine\" "
			"style=\"color:#7aa7cc; text-decoration:none;\">GitHub</a>&nbsp;&middot;&nbsp;"
			"<a href=\"https://t.me/zloytux\" "
			"style=\"color:#7aa7cc; text-decoration:none;\">Telegram</a>"));
	label->setOpenExternalLinks(true);
	label->setAlignment(Qt::AlignRight);
	label->setContentsMargins(6, 0, 8, 4);
	if (!full_version.isEmpty())
		label->setToolTip(Localization().tip("Plugin version: %1", "Версия плагина: %1").arg(full_version));
	return label;
}

// The Debug tab: switches of the tools that are rarely touched, and under them, in
// the working build, the reference pages as tabs of their own.
QWidget *ArtistToolWindow::build_debug_page()
{
	QWidget *page = new QWidget;
	QVBoxLayout *layout = new QVBoxLayout(page);

	QGroupBox *placer_group = new QGroupBox(QString::fromUtf8("Object Placer"));
	QVBoxLayout *placer_layout = new QVBoxLayout(placer_group);
	placer_layout->addWidget(object_placer_page_->createDebugControls());
	layout->addWidget(placer_group);

#ifdef ARTIST_TOOL_DEV_PAGES
	QTabWidget *reference_tabs = new QTabWidget;
	reference_tabs->setDocumentMode(true);

	ui_gallery_page_ = new UiGalleryPage;
	reference_tabs->addTab(ui_gallery_page_, QString::fromUtf8("UI Gallery"));

	icon_browser_page_ = new IconBrowserPage;
	reference_tabs->addTab(icon_browser_page_, QString::fromUtf8("Icons"));

	layout->addWidget(reference_tabs, 1);
#else
	layout->addStretch(1);
#endif
	return page;
}

void ArtistToolWindow::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);

	// Docking and undocking hide the window for a moment too: it is closed only if it
	// is still hidden when that is over.
	QTimer::singleShot(0, this, [this]() {
		if (!isVisible())
			object_placer_page_->resetFilter();
	});
}

void ArtistToolWindow::shutdown()
{
	clutter_unpack_page_->cancel();
	object_placer_page_->stop();
	path_placer_page_->stop();
#ifdef ARTIST_TOOL_DEV_PAGES
	ui_gallery_page_->stop();
#endif
}

} // namespace ArtistTool
