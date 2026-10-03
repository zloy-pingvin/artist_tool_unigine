#pragma once

#include <QWidget>

class QTabWidget;

namespace ArtistTool
{

class ClutterUnpackPage;
class IconBrowserPage;
class ObjectPlacerPage;
class PathPlacerPage;
class UiGalleryPage;

// Main tool window of the plugin: one tab per artist tool, plus the Debug tab with
// rarely needed switches of the tools and the reference pages (UI Gallery, Icons).
//
// The reference pages are for developing the plugin and are only in the working
// build: the build for the store (ArtistToolStore=true, see the project file) does
// not define ARTIST_TOOL_DEV_PAGES and does not compile their sources at all.
class ArtistToolWindow final : public QWidget
{
public:
	explicit ArtistToolWindow(QWidget *parent = nullptr);

	// Stops everything that is still running; called before the plugin unloads.
	void shutdown();

protected:
	// Closing the window puts the "Place on" filter of the Object Placer back to its
	// default, whichever tab is open.
	void hideEvent(QHideEvent *event) override;

private:
	QWidget *build_debug_page();
	QWidget *build_about_label();

	QTabWidget *tabs_{nullptr};
	ClutterUnpackPage *clutter_unpack_page_{nullptr};
	ObjectPlacerPage *object_placer_page_{nullptr};
	PathPlacerPage *path_placer_page_{nullptr};
	UiGalleryPage *ui_gallery_page_{nullptr};
	IconBrowserPage *icon_browser_page_{nullptr};
};

} // namespace ArtistTool
