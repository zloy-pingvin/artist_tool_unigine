#pragma once

#include "Localization.h"
#include "WireBuilder.h"

#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDial;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QLineEdit;
class QListWidget;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace ArtistTool
{

class AssetField;
class SurfaceFilterButton;

// "Wires" tool page: the current line and its poles, how the wires hang, what they are
// made of, and Generate.
class WireBuilderPage final : public QWidget
{
public:
	explicit WireBuilderPage(QWidget *parent = nullptr);
	~WireBuilderPage() override;

	// Stops adding poles and the drawing of the wires.
	void stop();
	// Forgets the remembered conveniences of the page (the hotkeys list). The settings
	// of the lines are kept in the lines themselves.
	void resetSettings();

protected:
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	// Keeps the mouse wheel from changing values while the page is being scrolled.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	QWidget *build_line_group();
	QWidget *build_poles_group();
	// The rows of the list of the pole assets, made anew if the list has changed.
	void refresh_pole_list();
	QWidget *build_sag_group();
	QWidget *build_shape_group();
	QWidget *build_texture_group();
	QWidget *build_output_group();
	QDoubleSpinBox *make_spin_box(double minimum, double maximum, double step, int decimals, const QString &tooltip);
	// A label, a field and its reset button in a grid of two such fields in a row:
	// 'side' 0 - the left half, 1 - the right one.
	void add_field(QGridLayout *layout, int row, int side, const QString &label_text, QWidget *field,
		const QString &tooltip, const std::function<void()> &reset, QVector<QWidget *> *group = nullptr);
	// The two axes of the mesh can not be the same: the other one takes the axis the
	// picked one has left.
	void apply_mesh_axes(bool forward_picked);
	// The same for the axes of the poles.
	void apply_pole_axes(bool forward_picked);

	WireSettings read_settings() const;
	void apply_settings();
	void refresh();
	// refresh() once the current events are done.
	void request_refresh();
	void rescan_lines();
	void refresh_lines();
	void browse_folder();
	void generate();

	Localization loc_;
	WireBuilder *builder_{nullptr};
	QString message_;
	bool updating_{false};
	int shown_revision_{-1};

	QLabel *line_label_{nullptr};
	QPushButton *new_line_button_{nullptr};
	QPushButton *new_link_button_{nullptr};
	QPushButton *add_points_button_{nullptr};
	// The selected points of a line drawn as a curve.
	QWidget *point_row_{nullptr};
	QToolButton *smooth_button_{nullptr};
	QToolButton *corner_button_{nullptr};
	QToolButton *subdivide_button_{nullptr};
	// The sag and the random of the span from the selected points.
	QDoubleSpinBox *point_sag_{nullptr};
	QDoubleSpinBox *point_random_{nullptr};
	QDoubleSpinBox *point_thickness_{nullptr};
	QDoubleSpinBox *point_twist_{nullptr};
	// The buttons of a line of poles of the world.
	QWidget *link_row_{nullptr};
	QPushButton *add_poles_button_{nullptr};
	QToolButton *add_selected_button_{nullptr};
	QToolButton *remove_last_button_{nullptr};
	QToolButton *clear_poles_button_{nullptr};
	QLineEdit *attach_name_{nullptr};
	QCheckBox *closed_{nullptr};
	QPushButton *reset_settings_button_{nullptr};
	QListWidget *lines_list_{nullptr};
	QVector<WireLineInfo> lines_;
	QToolButton *clone_line_button_{nullptr};
	QToolButton *remove_curve_button_{nullptr};
	QToolButton *remove_line_button_{nullptr};
	// The parent of the new lines.
	QLabel *parent_label_{nullptr};
	QToolButton *parent_clear_button_{nullptr};

	// The poles of a line drawn as a curve: the list of their assets, a role per row.
	QWidget *poles_group_{nullptr};
	QGridLayout *pole_list_{nullptr};
	QVector<QComboBox *> pole_role_boxes_;
	QString pole_signature_;
	QLabel *pole_empty_label_{nullptr};
	QPushButton *pole_clear_button_{nullptr};
	QPushButton *reset_turns_button_{nullptr};
	QButtonGroup *pole_forward_{nullptr};
	QButtonGroup *pole_up_{nullptr};
	// At the points only (0) or also every pole_spacing_ units between them (1).
	QButtonGroup *pole_every_{nullptr};
	QDoubleSpinBox *pole_spacing_{nullptr};
	QCheckBox *pole_on_ground_{nullptr};
	SurfaceFilterButton *ground_filter_{nullptr};

	QComboBox *sag_mode_{nullptr};
	QDoubleSpinBox *sag_{nullptr};
	QDoubleSpinBox *sag_random_{nullptr};
	QDoubleSpinBox *segment_angle_{nullptr};
	QSpinBox *min_segments_{nullptr};
	QDoubleSpinBox *max_segment_length_{nullptr};
	QCheckBox *above_ground_{nullptr};
	QSpinBox *seed_{nullptr};

	QButtonGroup *shape_{nullptr};
	QDoubleSpinBox *thickness_{nullptr};
	QSpinBox *sides_{nullptr};
	QDoubleSpinBox *rotation_{nullptr};
	QCheckBox *random_rotation_{nullptr};
	QComboBox *twist_mode_{nullptr};
	QDoubleSpinBox *twist_{nullptr};
	QDoubleSpinBox *uv_length_{nullptr};
	QDoubleSpinBox *uv_width_{nullptr};
	QDial *uv_rotation_{nullptr};
	QLabel *uv_rotation_value_{nullptr};
	AssetField *material_field_{nullptr};
	AssetField *mesh_field_{nullptr};
	QButtonGroup *mesh_forward_{nullptr};
	QButtonGroup *mesh_up_{nullptr};
	QDoubleSpinBox *mesh_scale_{nullptr};
	QDoubleSpinBox *mesh_angle_{nullptr};
	QDoubleSpinBox *twist_step_{nullptr};
	QDoubleSpinBox *end_offset_{nullptr};
	// What only strips and tubes have, what only a tube has, what only a mesh has.
	QVector<QWidget *> strip_widgets_;
	QVector<QWidget *> tube_widgets_;
	QVector<QWidget *> mesh_widgets_;
	// Twist Step: only for a strip or a tube that is twisted.
	QVector<QWidget *> twist_step_widgets_;
	// The axes of the pole assets and Reset Turns: only while the list has assets.
	QVector<QWidget *> pole_asset_widgets_;
	// The name of the attach points: not for a garland, it has no poles.
	QVector<QWidget *> attach_widgets_;
	// What of Poles is only for a line drawn as a curve: a line of the poles of the world
	// keeps only the attach points there.
	QVector<QWidget *> curve_pole_widgets_;
	bool refresh_pending_{false};

	QDoubleSpinBox *piece_size_{nullptr};
	// The LOD of the strips and the tubes; its fields are shown while it is on.
	QCheckBox *lod_{nullptr};
	QSpinBox *lod_factor_{nullptr};
	QDoubleSpinBox *lod_distance_{nullptr};
	QDoubleSpinBox *lod_fade_{nullptr};
	QVector<QWidget *> lod_widgets_;
	QDoubleSpinBox *visible_distance_{nullptr};
	QDoubleSpinBox *fade_distance_{nullptr};
	AssetField *folder_field_{nullptr};

	// What is drawn over the wires in the viewport (WireView).
	QComboBox *view_{nullptr};
	QLabel *stats_label_{nullptr};
	QPushButton *generate_button_{nullptr};
	QLabel *status_label_{nullptr};
	QToolButton *hotkeys_header_{nullptr};
	// "Advanced Settings" of Sag, folded by default.
	QToolButton *sag_advanced_header_{nullptr};
	// "Advanced Settings" of Shape (the rotation and the twist), folded by default.
	QToolButton *shape_advanced_header_{nullptr};
};

} // namespace ArtistTool
