#pragma once

#include "Localization.h"
#include "ObjectPlacer.h"

#include <QPointer>
#include <QVector>
#include <QWidget>

#include <functional>

class QButtonGroup;
class QCheckBox;
class QDial;
class QDoubleSpinBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QSettings;
class QToolButton;

namespace ArtistTool
{

// "Object Placer" tool page: the surface filter, the asset picked in the Asset
// Browser, the placement parameters (laid out like the Objects settings of the
// Cluster Paint mode) and the toggle that turns click-to-place on and off.
class ObjectPlacerPage final : public QWidget
{
public:
	explicit ObjectPlacerPage(QWidget *parent = nullptr);
	~ObjectPlacerPage() override;

	// Turns click-to-place off.
	void stop();
	// Puts the "Place on" filter back to its default; done when the tool window is
	// closed.
	void resetFilter();

	// Creates the controls of the tool that are shown on the Debug tab of the window
	// rather than on this page. The caller owns the returned widget.
	QWidget *createDebugControls();

	// Forgets the remembered settings of the page and puts its controls back to their
	// defaults.
	void resetSettings();

protected:
	// Switching to another tab or closing the window stops the tool, so that clicks
	// in the viewport never place objects while its controls are out of sight.
	void hideEvent(QHideEvent *event) override;
	// Keeps the mouse wheel from changing values while the page is being scrolled,
	// and refits the text of the filter button when its width changes.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	// Min / max values of a parameter along X, Y and Z.
	struct Vec3Range
	{
		QDoubleSpinBox *min[3]{};
		QDoubleSpinBox *max[3]{};
	};

	// Min / max angles around X, Y and Z, set with dials.
	struct AngleRange
	{
		QDial *min[3]{};
		QDial *max[3]{};
	};

	struct FilterItem
	{
		QCheckBox *check{nullptr};
		int surface_type{0};
	};

	void build_ui();
	QWidget *build_filter();
	QToolButton *make_reset_button(const std::function<void()> &reset);
	QDoubleSpinBox *make_spin_box(double minimum, double maximum, double step, int decimals,
		const QString &tooltip);
	QWidget *make_axis_cell(QWidget *field, QWidget *extra, int axis) const;
	Vec3Range add_vec3_range(QGridLayout *grid, const QString &label, double minimum, double maximum,
		double step, int decimals, const QString &tooltip, const Unigine::Math::vec3 &default_min,
		const Unigine::Math::vec3 &default_max);
	QWidget *make_dial(QDial *&dial, const QString &caption, const QString &tooltip);
	QWidget *make_angle_cell(QDial *&min_dial, QDial *&max_dial, int axis, const QString &tooltip);
	AngleRange add_angle_range(QGridLayout *grid, const QString &label, const QString &tooltip,
		const Unigine::Math::vec3 &default_min, const Unigine::Math::vec3 &default_max);

	void load_settings();
	void save_settings() const;
	static void load_vec3(const QSettings &settings, const QString &key, QDoubleSpinBox *const boxes[3],
		const Unigine::Math::vec3 &default_value);
	static void save_vec3(QSettings &settings, const QString &key, const Unigine::Math::vec3 &value);
	static Unigine::Math::vec3 read_vec3(QDoubleSpinBox *const boxes[3]);
	static void write_vec3(QDoubleSpinBox *const boxes[3], const Unigine::Math::vec3 &value);
	static void load_angles(const QSettings &settings, const QString &key, QDial *const dials[3],
		const Unigine::Math::vec3 &default_value);
	static Unigine::Math::vec3 read_angles(QDial *const dials[3]);
	static void write_angles(QDial *const dials[3], const Unigine::Math::vec3 &value);

	PlacerOptions read_options() const;
	void set_up_axis(PlacerOptions::UpAxis axis);
	void apply_options();
	void refresh();
	void reset_filter();
	void refresh_filter_summary();
	void fit_filter_text();
	void use_selected_as_parent();

	Localization loc_;
	ObjectPlacer *placer_{nullptr};
	QString message_;
	// Set while several controls are changed at once, to push the options only once.
	bool updating_{false};

	QPushButton *filter_button_{nullptr};
	// What the filter button says when it is wide enough to show all of it.
	QString filter_summary_;
	QVector<FilterItem> filter_types_;
	QCheckBox *only_immovable_{nullptr};
	QCheckBox *only_intersection_{nullptr};

	QButtonGroup *source_{nullptr};
	QLabel *asset_title_{nullptr};
	QLabel *asset_label_{nullptr};
	QLabel *parent_label_{nullptr};
	QToolButton *parent_select_button_{nullptr};
	QToolButton *parent_clear_button_{nullptr};
	QButtonGroup *up_axis_{nullptr};

	QCheckBox *normal_orientation_{nullptr};
	QDoubleSpinBox *surface_offset_{nullptr};
	// Set from the Debug tab, see createDebugControls().
	bool move_while_held_{true};
	QPointer<QCheckBox> move_while_held_check_;
	Vec3Range local_offset_;
	Vec3Range world_offset_;
	AngleRange local_rotation_;
	AngleRange world_rotation_;
	QDoubleSpinBox *scale_min_{nullptr};
	QDoubleSpinBox *scale_max_{nullptr};

	QPushButton *place_button_{nullptr};
	QLabel *status_label_{nullptr};
	QToolButton *hotkeys_header_{nullptr};
};

} // namespace ArtistTool
