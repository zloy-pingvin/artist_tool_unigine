#pragma once

#include "Localization.h"
#include "RoadBuilder.h"

#include <QHash>
#include <QString>
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QComboBox;
class QDial;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QListWidget;
class QPushButton;
class QSpinBox;
class QToolButton;

namespace ArtistTool
{

class AssetField;
class SurfaceFilterButton;

// "Road" tool page: the current road (create, add points, smooth / corner points,
// closed loop), the shape of its grid, the texture coordinates, what it is made into
// and Generate.
class RoadBuilderPage final : public QWidget
{
public:
	explicit RoadBuilderPage(QWidget *parent = nullptr);
	~RoadBuilderPage() override;

	// Stops adding points and the drawing of the road.
	void stop();
	// Forgets the remembered conveniences of the page (the hotkeys list, Snap After
	// Release). The settings of the roads are kept in the roads themselves.
	void resetSettings();

protected:
	// The tool draws the road only while its page is shown.
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;
	// Keeps the mouse wheel from changing values while the page is being scrolled.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void build_ui();
	QWidget *build_road_group();
	QWidget *build_shape_group();
	QWidget *build_ground_block();
	QWidget *build_texture_group();
	QWidget *build_output_group();
	QWidget *build_bend_block();
	// The two axes of the bent meshes can not be the same: the other one takes the
	// axis the picked one has left.
	void apply_bend_axes(bool forward_picked);
	// Puts Side Offset, Roll and Max Piece Length where they go for a bent mesh (next
	// to its checkboxes, with no empty places) or for a grid.
	void arrange_shape(bool bent);
	// The rows of the list of the bent meshes, made anew if the list has changed.
	void refresh_bend_list();
	QDoubleSpinBox *make_spin_box(double minimum, double maximum, double step, int decimals,
		const QString &tooltip);

	void load_settings();
	void save_settings() const;

	RoadSettings read_settings() const;
	void apply_settings();
	void refresh();
	void rescan_roads();
	void refresh_roads(bool rescan_if_missing = true);
	void browse_folder();
	void generate();

	Localization loc_;
	RoadBuilder *builder_{nullptr};
	QString message_;
	// Set while the controls are being filled from the tool, so that they do not
	// write the same values back.
	bool updating_{false};
	// The settings revision of the tool the fields last showed (see
	// RoadBuilder::getSettingsRevision()).
	int shown_revision_{-1};

	QLabel *road_label_{nullptr};
	QListWidget *roads_list_{nullptr};
	QVector<RoadInfo> roads_;
	int missing_road_id_{0};
	int shown_road_id_{0};
	QPushButton *new_road_button_{nullptr};
	QPushButton *add_points_button_{nullptr};
	QToolButton *smooth_button_{nullptr};
	QToolButton *corner_button_{nullptr};
	QToolButton *subdivide_button_{nullptr};
	QToolButton *on_ground_button_{nullptr};
	QToolButton *off_ground_button_{nullptr};
	QDoubleSpinBox *roll_{nullptr};
	// The width of the selected points.
	QDoubleSpinBox *point_width_{nullptr};
	// Number fields that keep a narrow width of their own, see eventFilter().
	QVector<QWidget *> narrow_fields_;
	QCheckBox *closed_{nullptr};
	QPushButton *reset_settings_button_{nullptr};
	// Clone the current road, remove its curve only, remove it all.
	QToolButton *clone_road_button_{nullptr};
	QToolButton *remove_curve_button_{nullptr};
	QToolButton *remove_road_button_{nullptr};
	// The parent of the new roads.
	QLabel *parent_label_{nullptr};
	QToolButton *parent_clear_button_{nullptr};

	QDoubleSpinBox *width_{nullptr};
	QSpinBox *columns_{nullptr};
	QDoubleSpinBox *step_{nullptr};
	QDoubleSpinBox *max_angle_{nullptr};
	QDoubleSpinBox *inner_round_{nullptr};
	QDoubleSpinBox *road_roll_{nullptr};
	QDoubleSpinBox *side_offset_{nullptr};
	// The grid of Shape and the label, field and reset button of every field in it.
	QGridLayout *shape_layout_{nullptr};
	QHash<QWidget *, QVector<QWidget *>> shape_fields_;
	int shape_bent_{-1};
	QCheckBox *snap_to_ground_{nullptr};
	QPushButton *refresh_ground_{nullptr};
	QCheckBox *snap_after_release_{nullptr};
	// The row under Snap To Ground, shown only while it is on and a mesh is made.
	QWidget *ground_offset_row_[3]{};
	QDoubleSpinBox *ground_offset_{nullptr};
	SurfaceFilterButton *ground_filter_{nullptr};

	QDoubleSpinBox *uv_length_{nullptr};
	QDoubleSpinBox *uv_width_{nullptr};
	QDial *uv_rotation_{nullptr};
	QLabel *uv_rotation_value_{nullptr};


	QButtonGroup *output_{nullptr};
	// The rows of the decal settings, shown only for a decal.
	QVector<QWidget *> decal_rows_;
	QDoubleSpinBox *decal_depth_{nullptr};
	QDoubleSpinBox *decal_lift_{nullptr};
	QDoubleSpinBox *decal_visible_distance_{nullptr};
	QDoubleSpinBox *decal_fade_distance_{nullptr};
	QDoubleSpinBox *piece_length_{nullptr};
	AssetField *material_field_{nullptr};
	// The rows of the material, hidden for a bent mesh: it has the materials of its asset.
	QVector<QWidget *> material_rows_;
	// The bent mesh: its rows, shown only for it.
	QVector<QWidget *> bend_rows_;
	QButtonGroup *bend_forward_{nullptr};
	QButtonGroup *bend_up_{nullptr};
	QCheckBox *bend_merge_{nullptr};
	QDoubleSpinBox *bend_mesh_angle_{nullptr};
	// The list of the bent meshes, a row per mesh: its fields are filled from the
	// settings; the rows are made anew only when the list itself changes.
	struct BendPartRow
	{
		QComboBox *role{nullptr};
		QSpinBox *count{nullptr};
		QDoubleSpinBox *scale[3]{};
		QDoubleSpinBox *length{nullptr};
		QToolButton *length_reset{nullptr};
	};
	QVector<BendPartRow> bend_part_rows_;
	QGridLayout *bend_list_{nullptr};
	QLabel *bend_empty_label_{nullptr};
	QPushButton *bend_clear_button_{nullptr};
	QString bend_signature_;
	// What only a road of a grid has (its shape, its texture, its colors, the ground
	// under it): hidden for a bent mesh, which follows the curve itself.
	QVector<QWidget *> grid_only_widgets_;
	AssetField *folder_field_{nullptr};

	// What is drawn over the road in the viewport (RoadView).
	QComboBox *view_{nullptr};
	QCheckBox *hide_outdated_{nullptr};
	QLabel *stats_label_{nullptr};
	QPushButton *generate_button_{nullptr};
	QLabel *status_label_{nullptr};
	QToolButton *hotkeys_header_{nullptr};
};

} // namespace ArtistTool
