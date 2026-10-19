#pragma once

#include "Localization.h"
#include "MeasureTool.h"

#include <QString>
#include <QVector>
#include <QWidget>

class QButtonGroup;
class QCheckBox;
class QGridLayout;
class QLabel;
class QPushButton;
class QToolButton;

namespace ArtistTool
{

class SurfaceFilterButton;

// "Measure" tool page: the ruler and the angle, how their points snap, and the
// numbers of the measurement being made or clicked in the viewport.
class MeasurePage final : public QWidget
{
public:
	explicit MeasurePage(QWidget *parent = nullptr);
	~MeasurePage() override;

	void stop();
	// Puts the controls of the page back to their defaults and forgets the remembered
	// ones.
	void resetSettings();

protected:
	// The tool measures, draws the measurements and takes the clicks only while its
	// page is shown.
	void showEvent(QShowEvent *event) override;
	void hideEvent(QHideEvent *event) override;

private:
	// One row of the results: a name and a value.
	struct ResultRow
	{
		QLabel *name{nullptr};
		QLabel *value{nullptr};
	};

	void build_ui();
	QWidget *build_tool_group();
	QWidget *build_result_group();

	void load_settings();
	void save_settings() const;

	void refresh();
	// Fills the rows of the results; the rest of them are hidden.
	void set_results(const QVector<QPair<QString, QString>> &rows);
	void copy_value();

	Localization loc_;
	MeasureTool *tool_{nullptr};
	QString message_;
	// The main number of the measurement shown, the one Copy puts to the clipboard.
	QString copy_value_;

	QButtonGroup *mode_{nullptr};
	QButtonGroup *snap_{nullptr};
	QCheckBox *point_snap_{nullptr};
	SurfaceFilterButton *filter_{nullptr};
	QCheckBox *loupe_{nullptr};
	QCheckBox *only_on_tab_{nullptr};
	// Whether measuring is on when the page is shown: only the first time.
	bool measuring_on_show_{true};
	QPushButton *measure_button_{nullptr};

	QLabel *result_title_{nullptr};
	QGridLayout *result_grid_{nullptr};
	QVector<ResultRow> result_rows_;
	QPushButton *copy_button_{nullptr};
	QPushButton *delete_button_{nullptr};
	QPushButton *clear_button_{nullptr};

	QLabel *status_label_{nullptr};
	QToolButton *hotkeys_header_{nullptr};
};

} // namespace ArtistTool
