#include "ArtistToolWindow.h"
#include "BakerWindow.h"
#include "Localization.h"
#include "ClutterUnpackPage.h"
#include "CurveEditor.h"
#include "MeasurePage.h"
#include "ObjectPlacerPage.h"
#include "PathPlacerPage.h"
#include "RoadBuilderPage.h"
#include "UiHelpers.h"
#include "WireBuilderPage.h"

#ifdef ARTIST_TOOL_DEV_PAGES
	#include "IconBrowserPage.h"
	#include "UiGalleryPage.h"
#endif

#include <editor/UnigineWindowManager.h>
#include <editor/UniginePluginInfo.h>
#include <editor/UniginePluginManager.h>

#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QCheckBox>
#include <QColor>
#include <QButtonGroup>
#include <QAbstractButton>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QStackedWidget>
#include <QTabWidget>
#include <QToolButton>
#include <QVector>
#include <QTimer>
#include <QVBoxLayout>

#include <cmath>
#include <cstring>
#include <functional>
#include <iterator>

namespace ArtistTool
{

// The tabs of the window in two rows, so that all of them fit a narrow window: a button
// per page, the page of the pressed one under them.
class TabRows final : public QWidget
{
public:
	TabRows()
	{
		QVBoxLayout *layout = new QVBoxLayout(this);
		layout->setContentsMargins(0, 0, 0, 0);
		layout->setSpacing(0);
		buttons_ = new QGridLayout;
		buttons_->setContentsMargins(4, 4, 4, 2);
		buttons_->setHorizontalSpacing(2);
		buttons_->setVerticalSpacing(3);
		layout->addLayout(buttons_);
		pages_ = new QStackedWidget;
		layout->addWidget(pages_, 1);
		group_ = new QButtonGroup(this);
		group_->setExclusive(true);
		connect(group_, &QButtonGroup::idClicked, this, [this](int id) {
			pages_->setCurrentIndex(id);
			place_tail();
			// The tab that is open is the one the window opens with next time, after a
			// reload of the plugin too.
			QSettings settings(QString::fromUtf8("zloy_pingvin"), QString::fromUtf8("artist_tool"));
			settings.setValue("window/tab", id);
		});

		// The tails of the rows: a line under every row, from edge to edge, over the bottom
		// of its tabs. The one of the open tab is in its color: the tab runs into it, the
		// others go under it - tabs of a folder, not buttons. The other row has a gray one.
		// Above the tabs, clicks go through them.
		for (QWidget *&tail : {std::ref(tail_), std::ref(other_tail_)})
		{
			tail = new QWidget(this);
			tail->setAttribute(Qt::WA_TransparentForMouseEvents);
			tail->setAttribute(Qt::WA_StyledBackground);
		}

		other_tail_->setStyleSheet(QString::fromUtf8("background-color: #50535a;"));
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		// The tabs are laid out anew when the window changes its size.
		if (event->type() == QEvent::Move || event->type() == QEvent::Resize)
			place_tail();
		return QWidget::eventFilter(watched, event);
	}

public:
	// 'colored' - a tab in a color of the rainbow, the next one after the tabs before it;
	// else gray.
	void addTab(QWidget *page, const QIcon &icon, const QString &text, bool colored = true)
	{
		// The frames of every tab a little darker than the page.
		tintPageFrames(page);
		QToolButton *button = new QToolButton;
		button->setText(text);
		button->setIcon(icon);
		button->setIconSize(QSize(16, 16));
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		// Every tab a button of its own in a dull color of its own, the colors of the
		// rainbow in order; the open one in a bright color of the same hue, a gray tab in
		// a lighter gray.
		const double rainbow[] = {0.0, 0.08, 0.125, 0.33, 0.535, 0.62, 0.78};
		const double hue = rainbow[colored_tabs_ % int(std::size(rainbow))];
		const double tint = colored ? 1.0 : 0.0;
		if (colored)
			++colored_tabs_;
		const auto color = [hue, tint](double saturation, double value) {
			return QColor::fromHsvF(float(hue), float(saturation * tint), float(value)).name();
		};
		// Dull, the amber of the open yellow tab reads as orange: the dull one is yellower.
		const double dull_hue = hue > 0.1 && hue < 0.2 ? 0.16 : hue;
		const auto dull = [dull_hue, tint](double saturation, double value) {
			return QColor::fromHsvF(float(dull_hue), float(saturation * tint), float(value)).name();
		};
		// Yellow, green and cyan look lighter than the other hues at the same value: they are a
		// little darker, so that white text stays readable on them; the green less vivid.
		double open_saturation = 0.70;
		double open_value = 0.72;
		if (hue > 0.1 && hue < 0.2)
			open_value = 0.62;
		else if (hue > 0.25 && hue < 0.42)
		{
			open_saturation = 0.55;
			open_value = 0.60;
		}
		else if (hue > 0.45 && hue < 0.58)
			open_value = 0.64;
		// Red, blue and violet are the most vivid: a little less saturated.
		else if (hue < 0.04 || hue > 0.58)
		{
			open_saturation = 0.58;
			// the red a little darker too
			if (hue < 0.04)
				open_value = 0.64;
		}
		const QString open_color = colored ? color(open_saturation, open_value) : color(0.0, 0.46);
		open_colors_.append(open_color);
		button->setStyleSheet(QString::fromUtf8(
			"QToolButton { color: #e8e8e8; background-color: %1; border: 1px solid %2;"
			" border-radius: 3px; padding: 2px 8px 1px 8px; }"
			"QToolButton:hover { background-color: %3; border-color: %4; }"
			"QToolButton:checked { color: white; font-weight: bold; background-color: %5; border-color: %5;"
			" border-bottom-left-radius: 0; border-bottom-right-radius: 0; }")
			.arg(dull(0.15, 0.29), dull(0.17, 0.38), dull(0.17, 0.36), dull(0.20, 0.47), open_color));
		group_->addButton(button, pages_->count());
		pages_->addWidget(page);
		buttons_list_.append(button);
		button->installEventFilter(this);
		if (buttons_list_.size() == 1)
			button->setChecked(true);

		// Laid out anew: half of the tabs in the first row, the rest in the second.
		for (QToolButton *b : buttons_list_)
			buttons_->removeWidget(b);
		columns_ = (int(buttons_list_.size()) + 1) / 2;
		for (int i = 0; i < buttons_list_.size(); ++i)
			buttons_->addWidget(buttons_list_[i], i / columns_, i % columns_);
		place_tail();
	}

	// Opens the tab the window had open last time.
	void restoreTab()
	{
		QSettings settings(QString::fromUtf8("zloy_pingvin"), QString::fromUtf8("artist_tool"));
		const int id = settings.value("window/tab", 0).toInt();
		if (QAbstractButton *button = group_->button(id))
		{
			button->setChecked(true);
			pages_->setCurrentIndex(id);
			place_tail();
		}
	}

private:
	// Under the rows, over the bottom 2 pixels of their tabs: the color one under the row
	// of the open tab, the gray one under the other row.
	void place_tail()
	{
		const int open = group_->checkedId();
		if (open < 0 || columns_ < 1)
			return;
		const QMargins margins = buttons_->contentsMargins();
		const int open_row = open / columns_;
		const int other_row = 1 - open_row;
		const auto put = [&](QWidget *tail, int row) {
			const int first = row * columns_;
			tail->setVisible(first < buttons_list_.size());
			if (first >= buttons_list_.size())
				return;
			tail->setGeometry(margins.left(), buttons_list_[first]->geometry().bottom() - 1,
				width() - margins.left() - margins.right(), 3);
			tail->raise();
		};
		put(other_tail_, other_row);
		put(tail_, open_row);
		// The tail in the color of the open tab.
		if (open < open_colors_.size())
			tail_->setStyleSheet(QString::fromUtf8("background-color: %1;").arg(open_colors_[open]));
	}

	int columns_{1};
	int colored_tabs_{0};
	QWidget *tail_{nullptr};
	QWidget *other_tail_{nullptr};
	QGridLayout *buttons_{nullptr};
	QStackedWidget *pages_{nullptr};
	QButtonGroup *group_{nullptr};
	QVector<QToolButton *> buttons_list_;
	// The color of every tab while it is open.
	QVector<QString> open_colors_;
};

ArtistToolWindow::ArtistToolWindow(QWidget *parent)
	: QWidget(parent)
{
	// No QStringLiteral anywhere in the plugin: its strings point into the plugin
	// DLL, and the editor keeps some of them (e.g. the window title) after the plugin
	// is reloaded - reading them then crashes the editor.
	setWindowTitle(QString::fromUtf8("Artist Tools"));
	setObjectName(QString::fromUtf8("ArtistToolWindow"));

	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	tabs_ = new TabRows;
	layout->addWidget(tabs_, 1);

	// New tools are added here as tabs, the ones used most often first. The icons are
	// the editor's own ones, taken from its resources. The Russian names are short so
	// that all the tabs fit the window. The Debug tab is not translated.
	object_placer_page_ = new ObjectPlacerPage;
	tabs_->addTab(object_placer_page_, QIcon(QString::fromUtf8(":/images/tools/icon_snap_to_surface.png")),
		uiText("Object Placer", "Расстановка"));

	path_placer_page_ = new PathPlacerPage;
	tabs_->addTab(path_placer_page_, QIcon(QString::fromUtf8(":/sandworm/images/icon_fence.png")),
		uiText("Path Placer", "По кривой"));

	road_builder_page_ = new RoadBuilderPage;
	tabs_->addTab(road_builder_page_, QIcon(QString::fromUtf8(":/sandworm/images/icon_road.png")),
		uiText("Road", "Дорога"));

	wire_builder_page_ = new WireBuilderPage;
	tabs_->addTab(wire_builder_page_, QIcon(QString::fromUtf8(":/images/icon_line.png")),
		uiText("Wires", "Провода"));

	measure_page_ = new MeasurePage;
	tabs_->addTab(measure_page_, makeRulerIcon(), uiText("Measure", "Измерения"));

	clutter_unpack_page_ = new ClutterUnpackPage;
	tabs_->addTab(clutter_unpack_page_, QIcon(QString::fromUtf8(":/sandworm/images/icon_vegetation.png")),
		uiText("Unpack Clutter", "Распаковка"));

	// The Texture Baker, once a plugin of its own: its window as it was, in a tab.
	baker_page_ = new BakerWindow;
	tabs_->addTab(baker_page_, QIcon(QString::fromUtf8(":/images/icon_ambient_occlusion.png")),
		uiText("Baker", "Бейкер"));

	tabs_->addTab(build_debug_page(), QIcon(QString::fromUtf8(":/images/icon_gear_small.png")), QString::fromUtf8("Debug"), false);

	layout->addWidget(build_about_label());
	tabs_->restoreTab();
}

// The credit line at the bottom of the window, the same as in the Texture Baker
// plugin: the name of the plugin, its version, the author and the links. The version
// is read from the manifest the plugin was built with (artist_tool.json), so the
// window can not show a version the manifest disagrees with.
QWidget *ArtistToolWindow::build_about_label()
{
	QString full_version;
	for (const ::UnigineEditor::PluginInfo *info : ::UnigineEditor::PluginManager::plugins())
	{
		if (info && info->name() && strcmp(info->name(), "artist_tool") == 0 && info->version())
			full_version = QString::fromUtf8(info->version());
	}
	// The manifest carries the 4-part form ("1.0.0.0"); major.minor.patch is shown.
	const QString short_version = full_version.count(QLatin1Char('.')) >= 2
		? full_version.section(QLatin1Char('.'), 0, 2) : full_version;
	const QString title = short_version.isEmpty() ? QString::fromUtf8("Artist Tools")
		: QString::fromUtf8("Artist Tools v%1").arg(short_version);

	QLabel *label = new QLabel(
		QString::fromUtf8("<span style=\"color:#9a9a9a;\">%1 by zloy_pingvin</span>&nbsp;&nbsp;").arg(title)
		+ QString::fromUtf8(
			"<a href=\"https://github.com/zloy-pingvin/artist_tool_unigine\" "
			"style=\"color:#7aa7cc; text-decoration:none;\">GitHub</a>&nbsp;&middot;&nbsp;"
			"<a href=\"https://t.me/zloytux\" "
			"style=\"color:#7aa7cc; text-decoration:none;\">Telegram</a>"));
	label->setOpenExternalLinks(true);
	label->setAlignment(Qt::AlignRight);
	label->setContentsMargins(6, 0, 8, 4);
	if (!full_version.isEmpty())
		label->setToolTip(Localization().tip("Plugin version: %1", "Версия плагина: %1").arg(full_version));
	return label;
}

// The Debug tab: switches of the tools that are rarely touched, and under them, in
// the working build, the reference pages as tabs of their own.
QWidget *ArtistToolWindow::build_debug_page()
{
	QWidget *page = new QWidget;
	QVBoxLayout *layout = new QVBoxLayout(page);

	QGroupBox *placer_group = new QGroupBox(QString::fromUtf8("Object Placer"));
	QVBoxLayout *placer_layout = new QVBoxLayout(placer_group);
	placer_layout->addWidget(object_placer_page_->createDebugControls());
	layout->addWidget(placer_group);

	// The curve of the Path Placer and the Road: the smooth spline, or the old one to
	// compare with.
	QGroupBox *curve_group = new QGroupBox(QString::fromUtf8("Curves"));
	QVBoxLayout *curve_layout = new QVBoxLayout(curve_group);
	old_curve_ = new QCheckBox(QString::fromUtf8("Old Curve (Before 1.3)"));
	old_curve_->setToolTip(QString::fromUtf8(
		"On: the curves of Path Placer and Road are built the old way - at every point the "
		"direction is halfway between those to its two neighbours. It makes waves where a "
		"point is added between two others (Subdivide).\n"
		"Off: a smooth spline through all the points (the default).\n"
		"The roads and paths follow at once; the generated meshes change on Generate."));
	{
		QSettings settings(QString::fromUtf8("zloy_pingvin"), QString::fromUtf8("artist_tool"));
		old_curve_->setChecked(settings.value("debug/old_curve", false).toBool());
	}
	setOldCurve(old_curve_->isChecked());
	connect(old_curve_, &QCheckBox::toggled, this, [](bool checked) {
		setOldCurve(checked);
		QSettings settings(QString::fromUtf8("zloy_pingvin"), QString::fromUtf8("artist_tool"));
		settings.setValue("debug/old_curve", checked);
	});
	curve_layout->addWidget(old_curve_);
	layout->addWidget(curve_group);

	// The switches of the Texture Baker that are rarely touched.
	QGroupBox *baker_group = new QGroupBox(QString::fromUtf8("Baker"));
	QVBoxLayout *baker_layout = new QVBoxLayout(baker_group);
	baker_layout->addWidget(baker_page_->takeDebugControls());
	layout->addWidget(baker_group);

	QHBoxLayout *reset_row = new QHBoxLayout;
	QPushButton *reset_button = new QPushButton(QString::fromUtf8("Reset All Plugin Settings"));
	reset_button->setIcon(makeResetIcon());
	reset_button->setToolTip(Localization().tip(
		"Puts every setting of the plugin, on all its tabs, back to its default - as "
		"after a fresh install. The paths in the world keep their own settings.",
		"Вернуть все настройки плагина на всех вкладках к значениям по умолчанию - как "
		"после установки. Кривые в мире сохраняют свои собственные настройки."));
	connect(reset_button, &QPushButton::clicked, this, [this]() { reset_all_settings(); });
	reset_row->addWidget(reset_button);
	reset_row->addStretch(1);
	layout->addLayout(reset_row);

#ifdef ARTIST_TOOL_DEV_PAGES
	QTabWidget *reference_tabs = new QTabWidget;
	reference_tabs->setDocumentMode(true);

	ui_gallery_page_ = new UiGalleryPage;
	reference_tabs->addTab(ui_gallery_page_, QString::fromUtf8("UI Gallery"));

	icon_browser_page_ = new IconBrowserPage;
	reference_tabs->addTab(icon_browser_page_, QString::fromUtf8("Icons"));

	layout->addWidget(reference_tabs, 1);
#else
	layout->addStretch(1);
#endif
	return page;
}

void ArtistToolWindow::reset_all_settings()
{
	const QMessageBox::StandardButton answer = QMessageBox::question(this,
		QString::fromUtf8("Artist Tools"),
		QString::fromUtf8("Reset all the settings of the plugin to their defaults?"),
		QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
	if (answer != QMessageBox::Yes)
		return;

	object_placer_page_->resetSettings();
	path_placer_page_->resetSettings();
	road_builder_page_->resetSettings();
	wire_builder_page_->resetSettings();
	measure_page_->resetSettings();
	old_curve_->setChecked(false);
	clutter_unpack_page_->resetSettings();
	baker_page_->resetSettings();
}

void ArtistToolWindow::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);

	// Docking and undocking hide the window for a moment too, and so does another tab
	// of the same dock area: it is closed only if the editor counts it hidden (not just
	// out of sight) when that is over.
	QTimer::singleShot(0, this, [this]() {
		if (!isVisible() && !::UnigineEditor::WindowManager::isVisible(this))
			object_placer_page_->resetFilter();
	});
}

void ArtistToolWindow::shutdown()
{
	clutter_unpack_page_->cancel();
	object_placer_page_->stop();
	path_placer_page_->stop();
	road_builder_page_->stop();
	wire_builder_page_->stop();
	measure_page_->stop();
	baker_page_->stop();
#ifdef ARTIST_TOOL_DEV_PAGES
	ui_gallery_page_->stop();
#endif
}

} // namespace ArtistTool
