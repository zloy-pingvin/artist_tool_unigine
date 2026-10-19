#pragma once

#include "Localization.h"

#include <editor/UnigineAssetDialogs.h>

#include <QWidget>

class QLabel;
class QProgressBar;
class QTimer;
class QVBoxLayout;

namespace ArtistTool
{

// "UI Gallery" page: a showcase of the interface elements a tool window can be built
// from (Qt 6.5 Widgets drawn with the editor theme). Everything is live, but nothing
// here touches the world.
class UiGalleryPage final : public QWidget
{
public:
	explicit UiGalleryPage(QWidget *parent = nullptr);

	// Stops the demo animations.
	void stop();

private:
	// Adds a collapsible section and returns the widget to fill.
	QWidget *add_section(const QString &title, const QString &hint);

	void build_buttons();
	void build_numbers();
	void build_text();
	void build_colors();
	void build_lists();
	void build_progress();
	void build_drawing();
	void build_layouts();
	void build_editor();

	void refresh_selection();
	void on_asset_picked(const ::UnigineEditor::AssetDialogs::SelectedAsset &asset);

	Localization loc_;
	QVBoxLayout *sections_layout_{nullptr};

	QTimer *progress_timer_{nullptr};
	QProgressBar *progress_{nullptr};

	QLabel *asset_label_{nullptr};
	QLabel *selection_label_{nullptr};
	QLabel *dialog_result_{nullptr};
};

} // namespace ArtistTool
