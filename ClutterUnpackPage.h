#pragma once

#include "ClutterUnpacker.h"
#include "Localization.h"

#include <QIcon>
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QProgressBar;
class QPushButton;
class QTreeWidget;

namespace ArtistTool
{

// "Unpack Clutter" tool page: shows which of the selected nodes can be unpacked,
// the unpack options, and the progress / log of the running operation.
class ClutterUnpackPage final : public QWidget
{
public:
	explicit ClutterUnpackPage(QWidget *parent = nullptr);
	~ClutterUnpackPage() override;

	// Stops a running bake and restores the clutter being baked.
	void cancel();

private:
	void build_ui();
	void load_settings();
	void save_settings() const;

	UnpackOptions read_options() const;
	void refresh_selection();
	void start_unpack();
	void set_busy(bool busy);
	void append_log(int level, const QString &text);

	Localization loc_;
	ClutterUnpacker *unpacker_{nullptr};
	// How many clutters the running (or the last) unpacking was started for.
	int queued_count_{0};
	int supported_count_{0};

	QLabel *selection_label_{nullptr};
	QTreeWidget *selection_tree_{nullptr};
	QIcon mesh_clutter_icon_;
	QIcon node_clutter_icon_;

	QWidget *options_widget_{nullptr};
	QButtonGroup *source_mode_{nullptr};
	QLineEdit *suffix_{nullptr};
	QCheckBox *keep_parent_{nullptr};
	QCheckBox *select_result_{nullptr};
	QCheckBox *group_by_asset_{nullptr};

	QPushButton *unpack_button_{nullptr};
	QPushButton *cancel_button_{nullptr};
	QProgressBar *progress_{nullptr};
	QLabel *status_label_{nullptr};
	QPlainTextEdit *log_{nullptr};
};

} // namespace ArtistTool
