#pragma once

#include "Localization.h"
#include "PathPlacer.h"

#include <QString>
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QDoubleSpinBox;
class QLabel;
class QListWidget;
class QPushButton;
class QToolButton;
class QVBoxLayout;

namespace ArtistTool
{

// "Path Placer" tool page: the current path (create, add points, smooth / corner
// points, closed loop), how the objects stand along it, the fill pattern and the
// one-off fill of the path with it.
class PathPlacerPage final : public QWidget
{
public:
	explicit PathPlacerPage(QWidget *parent = nullptr);
	~PathPlacerPage() override;

	// Stops adding points and the live update.
	void stop();
	// Forgets the remembered settings of the page and puts its controls back to their
	// defaults. The settings of the current path are kept in the path itself, in the
	// world, and are left as they are.
	void resetSettings();

protected:
	// The tool draws the path and keeps the objects on it only while its page is
	// shown.
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	// Keeps the mouse wheel from changing values while the page is being scrolled.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void build_ui();
	QWidget *build_path_group();
	QWidget *build_objects_group();
	QWidget *build_pattern_group();
	QDoubleSpinBox *make_spin_box(double minimum, double maximum, double step, int decimals,
		const QString &tooltip);

	void load_settings();
	void save_settings() const;

	PathSettings read_settings() const;
	void apply_settings();
	// The up axis has been picked: if it is the axis the objects look forward along,
	// the forward axis gives way.
	void apply_up_axis();
	void refresh();
	void refresh_pattern();
	// Looks through the world for paths and fills the list with them.
	void rescan_paths();
	// Marks the current path in the list; if it is not there, the list is built anew
	// first (once: a path the search does not find stays unmarked).
	void refresh_paths(bool rescan_if_missing = true);
	void fill();

	Localization loc_;
	PathPlacer *placer_{nullptr};
	QString message_;
	// Set while the controls are being filled from the tool, so that they do not
	// write the same values back.
	bool updating_{false};
	// What the rows of the pattern list were last built for.
	QString pattern_signature_;

	QLabel *path_label_{nullptr};
	// The paths of the world, to switch between them; in the order of the list.
	QListWidget *paths_list_{nullptr};
	QVector<PathInfo> paths_;
	// The current path the last search did not find (e.g. it is inside a node
	// reference): the world is not searched for it again and again.
	int missing_path_id_{0};
	// The current path the list was last marked for: when it is gone (deleted, or a
	// new path left without points), the list is built anew.
	int shown_path_id_{0};
	QPushButton *new_path_button_{nullptr};
	QPushButton *add_points_button_{nullptr};
	QToolButton *smooth_button_{nullptr};
	QToolButton *corner_button_{nullptr};
	QToolButton *subdivide_button_{nullptr};
	QCheckBox *closed_{nullptr};
	QWidget *old_script_row_{nullptr};

	QDoubleSpinBox *step_{nullptr};
	QButtonGroup *forward_axis_{nullptr};
	QButtonGroup *up_axis_{nullptr};
	QCheckBox *yaw_only_{nullptr};
	QCheckBox *skew_{nullptr};
	QCheckBox *snap_to_ground_{nullptr};
	// The row under Snap To Ground, shown only while it is on.
	QLabel *ground_offset_label_{nullptr};
	QDoubleSpinBox *ground_offset_{nullptr};
	QToolButton *ground_offset_reset_{nullptr};
	QCheckBox *live_update_{nullptr};
	QPushButton *rearrange_button_{nullptr};

	QVBoxLayout *pattern_rows_{nullptr};
	QLabel *pattern_empty_label_{nullptr};
	QPushButton *pattern_add_button_{nullptr};
	QPushButton *pattern_clear_button_{nullptr};
	QPushButton *fill_button_{nullptr};

	QLabel *status_label_{nullptr};
	QToolButton *hotkeys_header_{nullptr};
};

} // namespace ArtistTool
