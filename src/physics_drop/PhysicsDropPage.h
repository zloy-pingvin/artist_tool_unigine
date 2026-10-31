#pragma once

#include "Localization.h"
#include "PhysicsBrush.h"
#include "PhysicsDrop.h"

#include <QStringList>
#include <QVector>
#include <QWidget>

class QAbstractSpinBox;
class QButtonGroup;
class QCheckBox;
class QDial;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QHBoxLayout;
class QLabel;
class QPlainTextEdit;
class QPushButton;
class QSlider;
class QSpinBox;
class QToolButton;
class QVBoxLayout;

namespace ArtistTool
{

class SurfaceFilterButton;

// "Physics Drop" tool page, three ways of using the physics for putting things in their places:
//  - Drop: the selected objects fall, and it ends by itself when they have settled;
//  - Brush: new objects are poured from the assets of a list;
//  - Drag: objects of the world are taken with the mouse or pushed around.
// The last two are one session, until it is applied: one can pour, then drag, then pour more.
class PhysicsDropPage final : public QWidget
{
public:
	explicit PhysicsDropPage(QWidget *parent = nullptr);
	~PhysicsDropPage() override;

	// Puts the objects back if there is something going on; called before the plugin unloads.
	void stop();
	// Puts the controls of the page back to their defaults and forgets the remembered ones.
	void resetSettings();
	// What is going on is over, and all stays as it is: the window is closed, nobody could end it.
	void finishRunning();

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	enum PageMode
	{
		PAGE_DROP = 0,
		PAGE_BRUSH,
		PAGE_DRAG,
	};

	void build_ui();
	QWidget *build_modes();
	QGroupBox *build_assets_group();
	QGroupBox *build_brush_group();
	QGroupBox *build_drag_group();
	QGroupBox *build_options_group();
	QGroupBox *build_result_group();
	QWidget *build_buttons();
	QDoubleSpinBox *make_spin_box(double minimum, double maximum, double step, int decimals, const QString &tooltip);
	// A number field and the slider that shows the same value.
	QSlider *make_slider(QSpinBox *&box, int minimum, int maximum, const QString &tooltip);
	// A dial for an angle from -180 to 180 with its caption and value written under it, as in the
	// Object Placer.
	QWidget *make_dial(QDial *&dial, const QString &caption, const QString &tooltip);
	// The Min and Max dials of an axis side by side.
	QWidget *make_angle_cell(QDial *&min_dial, QDial *&max_dial, const QString &tooltip);
	// The choice of how much of what is hidden in a pile goes.
	QHBoxLayout *make_hidden_row(QButtonGroup *&group, const QString &tooltip);
	void set_up_number_field(QAbstractSpinBox *box);

	void load_settings();
	void save_settings() const;
	PhysicsDrop::Options read_options() const;
	PhysicsBrush::Settings read_brush_settings() const;
	void apply_brush_settings();
	PageMode page_mode() const;

	void add_selected_assets();
	void refresh_assets();
	// Shows what means something in the mode and with the chosen shape.
	void refresh_shape_options();
	void refresh();
	void press_main_button();
	void append_log(int level, const QString &text);

	Localization loc_;
	PhysicsDrop *drop_{nullptr};
	PhysicsBrush *brush_{nullptr};
	QVector<PlaceableAsset> assets_;
	int can_fall_count_{0};
	bool updating_{false};

	QLabel *description_{nullptr};
	QButtonGroup *mode_{nullptr};
	QVector<QToolButton *> mode_buttons_;

	QGroupBox *assets_group_{nullptr};
	QLabel *assets_empty_{nullptr};
	QVBoxLayout *assets_rows_{nullptr};
	QPushButton *assets_clear_{nullptr};

	QGroupBox *brush_group_{nullptr};
	QSpinBox *count_{nullptr};
	QSpinBox *rate_{nullptr};
	QDoubleSpinBox *radius_{nullptr};
	QDoubleSpinBox *height_{nullptr};
	QDoubleSpinBox *scale_min_{nullptr};
	QDoubleSpinBox *scale_max_{nullptr};
	QDial *rotation_min_[3]{};
	QDial *rotation_max_[3]{};
	QPushButton *clear_new_{nullptr};

	QGroupBox *drag_group_{nullptr};
	QDoubleSpinBox *zone_{nullptr};
	QDoubleSpinBox *max_size_{nullptr};
	QButtonGroup *take_with_{nullptr};
	QCheckBox *only_selected_{nullptr};
	QDoubleSpinBox *force_{nullptr};

	QGroupBox *options_group_{nullptr};
	QFormLayout *options_layout_{nullptr};
	QButtonGroup *shape_{nullptr};
	QCheckBox *ignore_own_shapes_{nullptr};
	SurfaceFilterButton *filter_{nullptr};
	QDoubleSpinBox *friction_{nullptr};
	QCheckBox *lock_rotation_{nullptr};
	QLabel *max_time_label_{nullptr};
	QDoubleSpinBox *max_time_{nullptr};
	QDoubleSpinBox *dome_radius_{nullptr};
	QCheckBox *show_dome_{nullptr};
	QCheckBox *show_shapes_{nullptr};

	// What is settled only on applying: it stands right above the buttons, and can be changed while it goes.
	QGroupBox *result_group_{nullptr};
	QFormLayout *result_layout_{nullptr};
	QHBoxLayout *result_row_{nullptr};
	QHBoxLayout *hidden_row_{nullptr};
	QHBoxLayout *drag_hidden_row_{nullptr};
	QButtonGroup *result_{nullptr};
	QButtonGroup *hidden_{nullptr};
	QButtonGroup *drag_hidden_{nullptr};

	QPushButton *main_button_{nullptr};
	QPushButton *cancel_button_{nullptr};
	QPushButton *jitter_button_{nullptr};
	QLabel *status_label_{nullptr};
	// The last messages of importance: how it ended, what went wrong.
	QLabel *notice_label_{nullptr};
	QStringList notices_;
	QToolButton *hotkeys_header_{nullptr};
	QLabel *hotkeys_{nullptr};
	QToolButton *log_header_{nullptr};
	QPlainTextEdit *log_{nullptr};
};

} // namespace ArtistTool
