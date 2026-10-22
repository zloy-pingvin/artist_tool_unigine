#pragma once

#include <UnigineGUID.h>
#include <UnigineVector.h>

#include <QString>
#include <QWidget>

#include <functional>

class QAction;
class QIcon;
class QLineEdit;
class QToolButton;

// A field of one asset (or of a folder), the way the editor's own asset fields look: the
// name in a box, and small buttons after it - pick it in the asset dialog of the editor,
// show it in the Asset Browser, back to the default. An asset dragged from the Asset
// Browser or the Materials window is dropped onto it.
namespace ArtistTool
{

class AssetField final : public QWidget
{
public:
	struct Options
	{
		// The tooltip of the field.
		QString tooltip;
		// The title of the asset dialog and the extensions it shows (".mat", ".mesh.node"),
		// empty - all.
		QString dialog_title;
		QString filter;
		// The image of the field (a path in the editor's resources), shown before the name.
		QString icon;
		// The tooltip of the button that puts the default back; empty - no such button.
		QString reset_tooltip;
		// The tooltip of the button that shows the asset in the Asset Browser.
		QString show_tooltip;
		// The caption and the tooltip of the button that takes the asset selected in the
		// Asset Browser (and in the Materials window); no caption - no such button.
		QString take_text;
		QString take_tooltip;
	};

	explicit AssetField(const Options &options, QWidget *parent = nullptr);

	// What the field shows: the name, and whether it is set (else it is the default,
	// shown dimmed).
	void setValue(const QString &text, bool is_set);
	// Whether there is something to show in the Asset Browser.
	void setShowEnabled(bool enabled);
	// The image before the name, e.g. by the kind of the asset.
	void setIcon(const QIcon &icon);

	// An asset picked in the dialog or dropped: its GUIDs (assets and runtimes).
	std::function<void(const Unigine::Vector<Unigine::UGUID> &guids)> on_picked;
	// Instead of the asset dialog (e.g. a folder picked on the disk).
	std::function<void()> on_browse;
	std::function<void()> on_show;
	std::function<void()> on_reset;

protected:
	void dragEnterEvent(QDragEnterEvent *event) override;
	void dropEvent(QDropEvent *event) override;

private:
	Options options_;
	QLineEdit *name_{nullptr};
	QAction *icon_{nullptr};
	QToolButton *show_{nullptr};
	QToolButton *reset_{nullptr};
};

} // namespace ArtistTool
