#include "MeasurePage.h"
#include "SurfaceFilterButton.h"
#include "UiHelpers.h"

#include <QTimer>
#include <QButtonGroup>
#include <QCheckBox>
#include <QClipboard>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QGuiApplication>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalBlocker>
#include <QSettings>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

using namespace Unigine;

namespace ArtistTool
{

namespace
{

const char SETTINGS_ORGANIZATION[] = "zloy_pingvin";
const char SETTINGS_APPLICATION[] = "artist_tool";
const char SETTINGS_GROUP[] = "measure";

const int BUTTON_HEIGHT = 28;
const QSize BUTTON_ICON_SIZE(16, 16);
// The most rows the results have.
const int MAX_RESULT_ROWS = 5;

QString format_length(double length)
{
	return uiText("%1 m", "%1 м").arg(length, 0, 'f', 3);
}

QString format_angle(double degrees)
{
	return QString::fromUtf8("%1\xC2\xB0").arg(degrees, 0, 'f', 2);
}

// A height difference with its sign: up is plus.
QString format_rise(double rise)
{
	return (rise > 0.0 ? QString::fromUtf8("+") : QString()) + format_length(rise);
}

double horizontal_length(const Math::Vec3 &a, const Math::Vec3 &b)
{
	const double x = double(b.x - a.x);
	const double y = double(b.y - a.y);
	return std::sqrt(x * x + y * y);
}

// The slope of a span: its angle to the horizon and the grade (rise per 100 of run).
QString format_slope(const Math::Vec3 &a, const Math::Vec3 &b)
{
	const double run = horizontal_length(a, b);
	const double rise = double(b.z - a.z);
	const double degrees = std::atan2(rise, run) * 180.0 / 3.14159265358979323846;
	if (run < 1e-9)
		return format_angle(degrees);
	return QString::fromUtf8("%1  (%2 %)").arg(format_angle(degrees)).arg(rise / run * 100.0, 0, 'f', 1);
}

// What a span goes along each world axis.
QString format_axes(const Math::Vec3 &a, const Math::Vec3 &b)
{
	return QString::fromUtf8("X %1   Y %2   Z %3").arg(double(b.x - a.x), 0, 'f', 3)
		.arg(double(b.y - a.y), 0, 'f', 3).arg(double(b.z - a.z), 0, 'f', 3);
}

} // namespace

MeasurePage::MeasurePage(QWidget *parent)
	: QWidget(parent)
{
	tool_ = new MeasureTool(this);
	// The measurements left on the screen while another tab is open are hidden with
	// the window of the plugin (the page is inside its tabs).
	tool_->is_window_shown = [this]() {
		for (const QWidget *widget = this; widget; widget = widget->parentWidget())
		{
			if (widget->objectName() == QLatin1String("ArtistToolWindow"))
				return widget->isVisible();
		}
		return false;
	};

	build_ui();
	load_settings();

	tool_->on_changed = [this]() { refresh(); };
	tool_->on_message = [this](const QString &text) {
		message_ = text;
		refresh();
	};

	refresh();
}

MeasurePage::~MeasurePage()
{
	tool_->on_changed = nullptr;
	tool_->on_message = nullptr;
	tool_->is_window_shown = nullptr;
	save_settings();
}

void MeasurePage::stop()
{
	tool_->setShownInactive(false);
	tool_->setActive(false);
}

void MeasurePage::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	tool_->setActive(true);
	// Measuring is on the first time the page is opened, so that clicks in the viewport
	// put points right away; leaving the page stops it for good. The next figure is of
	// the usual height again, whatever the wheel made it last time.
	tool_->setMeasuring(measuring_on_show_);
	tool_->setFigureHeight(MeasureTool::FIGURE_HEIGHT);
}

void MeasurePage::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);
	// Docking or undocking the window hides it for a moment: only a page that stays
	// hidden stops measuring (the measurement being put is not lost to a dock).
	QTimer::singleShot(0, this, [this]() {
		if (isVisible())
			return;
		measuring_on_show_ = false;
		tool_->setMeasuring(false);
		tool_->setActive(false);
	});
}


void MeasurePage::build_ui()
{
	QVBoxLayout *page_layout = new QVBoxLayout(this);
	page_layout->setContentsMargins(0, 0, 0, 0);

	QScrollArea *scroll = new QScrollArea;
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	page_layout->addWidget(scroll);

	QWidget *content = new QWidget;
	scroll->setWidget(content);
	QVBoxLayout *layout = new QVBoxLayout(content);

	QLabel *description = new QLabel(uiText(
		"Measures distances and angles between points clicked in the viewport. The "
		"measurements are temporary: nothing is added to the world, and they are gone "
		"when cleared.",
		"Измеряет расстояния и углы между точками, поставленными кликом во вьюпорте. "
		"Замеры временные: в мир ничего не добавляется, и они исчезают после очистки."));
	description->setWordWrap(true);
	layout->addWidget(description);

	layout->addWidget(build_tool_group());
	layout->addWidget(build_result_group());

	status_label_ = new QLabel;
	status_label_->setWordWrap(true);
	layout->addWidget(status_label_);

	QLabel *hotkeys = new QLabel(loc_.hotkeys(
		"LMB - Put A Point (while Measure is on)\n"
		"Ctrl (Press And Let Go) - Switch Surface / Vertex Snap (while Measure is on)\n"
		"Shift (Hold) - Cursor 3 Times Slower, To Aim Exactly\n"
		"Alt (Press And Let Go) - Switch Ruler / Angle / Person (while Measure is on)\n"
		"LMB On A Point - Put The Point On It (with Snap To Points)\n"
		"LMB On A Measurement - Show Its Numbers (while Measure is off)\n"
		"LMB (Hold) On A Point + Mouse Move - Move It\n"
		"X / Y / Z - Next Point Only Along The World Axis\n"
		"Shift + X / Y / Z - Next Point Only In The Plane Of The Two Other Axes\n"
		"The Same Key Once More - Free The Point\n"
		"Esc - Free The Point; Drop The Measurement Being Put; Stop Measuring\n"
		"Backspace - Remove The Last Point\n"
		"Mouse Wheel - Height Of The Person +/- 5 cm (in the Person mode; over a figure - its own)\n"
		"Alt + LMB - Editor Camera (not taken by the tool)",
		"ЛКМ - поставить точку (пока включено «[[Measure|Измерять]]»)\n"
		"Ctrl (нажать и отпустить) - переключить привязку поверхность / вершина (пока включено «[[Measure|Измерять]]»)\n"
		"Shift (держать) - курсор в 3 раза медленнее, чтобы точно прицелиться\n"
		"Alt (нажать и отпустить) - переключить линейку / угол / человека (пока включено «[[Measure|Измерять]]»)\n"
		"ЛКМ по точке - поставить точку прямо на неё (при включённой «[[Snap To Points|Привязка к точкам]]»)\n"
		"ЛКМ по замеру - показать его цифры (пока измерение выключено)\n"
		"ЛКМ (держать) по точке + движение мыши - двигать её\n"
		"X / Y / Z - следующая точка только вдоль мировой оси\n"
		"Shift + X / Y / Z - следующая точка только в плоскости двух других осей\n"
		"Та же клавиша ещё раз - снять фиксацию\n"
		"Esc - снять фиксацию; убрать недоставленный замер; выключить измерение\n"
		"Backspace - убрать последнюю точку\n"
		"Колесо мыши - рост человека +/- 5 см (в режиме «[[Person|Человек]]»; над фигурой - её собственный)\n"
		"Alt + ЛКМ - камера редактора (инструмент её не перехватывает)"));
	hotkeys->setWordWrap(true);
	hotkeys->setContentsMargins(6, 2, 6, 6);
	hotkeys->setToolTip(loc_.tip(
		"LMB - the left mouse button. The keys work while the mouse is over the viewport.\n"
		"A lock holds one span, from the point before: after the point is put the next "
		"one is free again. A dragged point is locked from where it was picked up.\n"
		"With a lock, a vertex or a point aimed at is brought to the axis or the plane - "
		"e.g. Z and a vertex give its height above the point before.",
		"[[LMB|ЛКМ]] - левая кнопка мыши. Клавиши работают, пока курсор над вьюпортом.\n"
		"Фиксация действует на один отрезок, от предыдущей точки: после того как точка "
		"поставлена, следующая снова свободна. Перетаскиваемая точка фиксируется от "
		"места, где её взяли.\n"
		"При фиксации вершина или точка под курсором переносится на ось или плоскость - "
		"например, Z и вершина дают её высоту над предыдущей точкой."));

	hotkeys_header_ = makeSectionHeader(loc_.hotkeys("Hotkeys", "Горячие клавиши"), hotkeys);
	hotkeys_header_->setToolTip(loc_.tip(
		"Keys and mouse actions of the tool.",
		"Клавиши и действия мышью для этого инструмента."));
	layout->addWidget(hotkeys_header_);
	layout->addWidget(hotkeys);

	layout->addStretch(1);
}

QWidget *MeasurePage::build_tool_group()
{
	QGroupBox *group = new QGroupBox(uiText("Measure", "Замер"));
	QGridLayout *layout = new QGridLayout(group);
	layout->setColumnStretch(1, 1);

	// One look and one size for all the buttons of the rows: an icon and a caption.
	QVector<QToolButton *> buttons;
	const auto make_button = [&buttons](const QString &text, const QIcon &icon, const QString &tooltip) {
		QToolButton *button = new QToolButton;
		button->setText(text);
		button->setIcon(icon);
		button->setIconSize(BUTTON_ICON_SIZE);
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(tooltip);
		button->setStyleSheet(choiceButtonStyle());
		buttons.append(button);
		return button;
	};
	const auto make_row = [layout](int row, const QString &caption, const QString &tooltip,
		const QVector<QToolButton *> &row_buttons) {
		QLabel *label = new QLabel(caption);
		label->setToolTip(tooltip);
		layout->addWidget(label, row, 0);
		QHBoxLayout *row_layout = new QHBoxLayout;
		row_layout->setSpacing(3);
		for (QToolButton *button : row_buttons)
			row_layout->addWidget(button);
		row_layout->addStretch(1);
		layout->addLayout(row_layout, row, 1);
	};

	const QString mode_tooltip = loc_.tip(
		"What is measured.\n"
		"Ruler - the distance between two points.\n"
		"Angle - the angle between two sides: put the end of one side, the corner, then "
		"the end of the other side.\n"
		"Person - a pale figure of a person (1.80 m tall to start with), to judge the "
		"scale by: click where it is to stand. It is upright and always turned to the "
		"camera. The mouse wheel changes the height by 5 cm.\n"
		"A measurement is done as soon as it has its points. To go on from where it "
		"ends, start the next one on its last point.\n"
		"Alt pressed and let go over the viewport switches them; a measurement being "
		"put goes on from its last point.",
		"Что измерять.\n"
		"[[Ruler|Линейка]] - расстояние между двумя точками.\n"
		"[[Angle|Угол]] - угол между двумя сторонами: поставьте конец одной стороны, "
		"вершину угла, затем конец другой стороны.\n"
		"[[Person|Человек]] - блёклая фигура человека (сначала ростом 1,80 м), чтобы оценить "
		"масштаб: кликните туда, где он должен стоять. Стоит вертикально и всегда повёрнут "
		"к камере. Колесо мыши меняет рост на 5 см.\n"
		"Замер готов, как только поставлены его точки. Чтобы продолжить от его конца, "
		"начните следующий замер на его последней точке.\n"
		"Alt (нажать и отпустить над вьюпортом) переключает их; начатый замер "
		"продолжается от своей последней точки.");
	mode_ = new QButtonGroup(this);
	mode_->setExclusive(true);
	QToolButton *ruler_button = make_button(uiText("Ruler", "Линейка"), makeRulerIcon(), mode_tooltip);
	QToolButton *angle_button = make_button(uiText("Angle", "Угол"), makeAngleIcon(), mode_tooltip);
	QToolButton *figure_button = make_button(uiText("Person", "Человек"), makeFigureIcon(), mode_tooltip);
	mode_->addButton(ruler_button, MeasureTool::MODE_DISTANCE);
	mode_->addButton(angle_button, MeasureTool::MODE_ANGLE);
	mode_->addButton(figure_button, MeasureTool::MODE_FIGURE);
	make_row(0, uiText("Tool", "Инструмент"), mode_tooltip, {ruler_button, angle_button, figure_button});

	const QString snap_tooltip = loc_.tip(
		"Where a click puts the point.\n"
		"Surface - right where the mouse points, on any surface.\n"
		"Vertex - on the vertex of a mesh closest to the mouse; where there is none "
		"near, on the surface. The mesh is outlined and the vertex ringed before the "
		"click. Static, dynamic and cluster meshes have vertices to snap to.\n"
		"Ctrl pressed and let go over the viewport switches them.",
		"Куда клик ставит точку.\n"
		"[[Surface|Поверхность]] - прямо туда, куда указывает курсор, на любую поверхность.\n"
		"[[Vertex|Вершина]] - на ближайшую к курсору вершину меша; если рядом её нет - на "
		"поверхность. Меш обводится, а вершина отмечается кольцом ещё до клика. Вершины "
		"есть у статичных, динамических мешей и кластеров.\n"
		"Ctrl (нажать и отпустить над вьюпортом) переключает их.");
	snap_ = new QButtonGroup(this);
	snap_->setExclusive(true);
	QToolButton *surface_button = make_button(uiText("Surface", "Поверхность"),
		QIcon(QString::fromUtf8(":/images/tools/icon_snap_to_surface.png")), snap_tooltip);
	QToolButton *vertex_button = make_button(uiText("Vertex", "Вершина"),
		QIcon(QString::fromUtf8(":/sandworm/images/icon_point.png")), snap_tooltip);
	snap_->addButton(surface_button, MeasureTool::SNAP_SURFACE);
	snap_->addButton(vertex_button, MeasureTool::SNAP_VERTEX);
	make_row(1, uiText("Snap", "Привязка"), snap_tooltip, {surface_button, vertex_button});

	// The same filter as on the Road page: what it leaves out is looked through.
	SurfaceFilterButton::Texts filter_texts;
	filter_texts.tooltip = loc_.tip(
		"What the points are put on. The unchecked kinds of objects are looked through: e.g. "
		"with only Terrain checked a click through dense grass or bushes puts the point on "
		"the terrain under them. The snap to vertices is not filtered.",
		"На что ставятся точки. Неотмеченные типы объектов пропускаются насквозь: например, "
		"если отмечен только Terrain, клик сквозь густую траву или кусты ставит точку на "
		"террейн под ними. Привязка к вершинам не фильтруется.");
	filter_texts.only_immovable = uiText("Only On Immovable Nodes", "Только на ноды с флагом Immovable");
	filter_texts.only_immovable_tooltip = loc_.tip(
		"Put the points only on nodes that have the Immovable flag enabled.",
		"Ставить точки только на ноды с включённым флагом Immovable.");
	filter_texts.only_intersection = uiText("Only On Surfaces With Intersection", "Только на поверхности с Intersection");
	filter_texts.only_intersection_tooltip = loc_.tip(
		"Put the points only on surfaces that have the Intersection flag and any bit of the "
		"Intersection Mask enabled.",
		"Ставить точки только на поверхности с включённым флагом Intersection и хотя бы "
		"одним битом Intersection Mask.");
	filter_texts.reset_tooltip = loc_.tip("Reset to default", "Сбросить на значение по умолчанию");
	filter_ = new SurfaceFilterButton(filter_texts);
	QLabel *filter_label = new QLabel(uiText("Place On", "Ставить на"));
	filter_label->setToolTip(filter_texts.tooltip);
	layout->addWidget(filter_label, 2, 0);
	layout->addWidget(filter_, 2, 1);
	filter_->on_changed = [this]() { tool_->setSurfaceFilter(filter_->getFilter()); };

	point_snap_ = new QCheckBox(uiText("Snap To Points", "Привязка к точкам"));
	point_snap_->setToolTip(loc_.tip(
		"On: a click near a point of a measurement puts the new point right on it - "
		"to start a measurement where another one ends, or to measure between their "
		"points. It comes before the snap to a vertex or a surface.\n"
		"Off: the points of the measurements are not snapped to, a click goes to what "
		"is under them. Holding the button on a point still drags it.",
		"Вкл: клик рядом с точкой замера ставит новую точку прямо на неё - чтобы начать "
		"замер там, где кончается другой, или измерить между их точками. Действует раньше "
		"привязки к вершине или поверхности.\n"
		"Выкл: к точкам замеров привязки нет, клик попадает в то, что под ними. Если "
		"зажать кнопку на точке, она всё равно перетаскивается."));
	layout->addWidget(point_snap_, 3, 0, 1, 2);

	loupe_ = new QCheckBox(uiText("Magnifier", "Лупа"));
	loupe_->setToolTip(loc_.tip(
		"Next to the mouse, the view around the point being put or dragged, four times "
		"larger, with a crosshair right where the point goes. Its frame is green on a "
		"vertex, orange on a point of a measurement, in the color of the axis with a lock.",
		"Рядом с курсором - вид вокруг ставящейся или перетаскиваемой точки, увеличенный "
		"в четыре раза, с перекрестием ровно там, куда встанет точка. Рамка зелёная на "
		"вершине, оранжевая на точке замера, цвета оси при фиксации."));
	layout->addWidget(loupe_, 4, 0, 1, 2);

	only_on_tab_ = new QCheckBox(uiText("Show Only On This Tab", "Показывать только на этой вкладке"));
	only_on_tab_->setToolTip(loc_.tip(
		"On: the measurements are seen only while this tab is open.\n"
		"Off: they stay in the viewport while other tabs of the plugin are open too - "
		"only to be seen there: they can not be clicked or moved until this tab is open "
		"again. When the window of the plugin is closed, they are not shown.",
		"Вкл: замеры видны, только пока открыта эта вкладка.\n"
		"Выкл: они остаются во вьюпорте и на других вкладках плагина - только для "
		"просмотра: кликнуть или сдвинуть их нельзя, пока снова не открыта эта вкладка. "
		"Когда окно плагина закрыто, они не показываются."));
	layout->addWidget(only_on_tab_, 5, 0, 1, 2);

	QSize button_size;
	for (QToolButton *button : buttons)
		button_size = button_size.expandedTo(button->sizeHint());
	for (QToolButton *button : buttons)
		button->setFixedSize(button_size);

	measure_button_ = new QPushButton;
	measure_button_->setCheckable(true);
	measure_button_->setMinimumHeight(BUTTON_HEIGHT);
	measure_button_->setIcon(makeRulerIcon());
	measure_button_->setIconSize(BUTTON_ICON_SIZE);
	measure_button_->setStyleSheet(primaryButtonStyle());
	measure_button_->setToolTip(loc_.tip(
		"While it is on, every left click in the viewport puts a point of a measurement. "
		"It is on when the tab is first opened; leaving the tab or Esc turns it "
		"off.\n"
		"While it is off, a click on a measurement in the viewport shows its numbers.\n"
		"The points of the measurements can be dragged whether it is on or not.",
		"Пока включено, каждый клик левой кнопкой во вьюпорте ставит точку замера. "
		"Включено при первом открытии вкладки; уход с вкладки или Esc выключает.\n"
		"Пока выключено, клик по замеру во вьюпорте показывает его цифры.\n"
		"Точки замеров можно перетаскивать, включено это или нет."));
	layout->addWidget(measure_button_, 6, 0, 1, 2);

	connect(mode_, &QButtonGroup::idClicked, this, [this](int id) {
		message_.clear();
		tool_->setMode(MeasureTool::Mode(id));
	});
	connect(snap_, &QButtonGroup::idClicked, this, [this](int id) { tool_->setSnap(MeasureTool::Snap(id)); });
	connect(point_snap_, &QCheckBox::toggled, this, [this](bool checked) { tool_->setPointSnap(checked); });
	connect(loupe_, &QCheckBox::toggled, this, [this](bool checked) { tool_->setLoupe(checked); });
	connect(only_on_tab_, &QCheckBox::toggled, this, [this](bool checked) { tool_->setShownInactive(!checked); });
	connect(measure_button_, &QPushButton::toggled, this, [this](bool checked) { tool_->setMeasuring(checked); });

	return group;
}

QWidget *MeasurePage::build_result_group()
{
	QGroupBox *group = new QGroupBox(uiText("Result", "Результат"));
	QVBoxLayout *layout = new QVBoxLayout(group);

	result_title_ = new QLabel;
	result_title_->setWordWrap(true);
	layout->addWidget(result_title_);

	result_grid_ = new QGridLayout;
	result_grid_->setColumnStretch(1, 1);
	result_grid_->setHorizontalSpacing(12);
	for (int i = 0; i < MAX_RESULT_ROWS; ++i)
	{
		ResultRow row;
		row.name = new QLabel;
		row.value = new QLabel;
		// The numbers can be selected and copied by hand too.
		row.value->setTextInteractionFlags(Qt::TextSelectableByMouse);
		QFont font = row.value->font();
		font.setBold(true);
		row.value->setFont(font);
		result_grid_->addWidget(row.name, i, 0);
		result_grid_->addWidget(row.value, i, 1);
		result_rows_.append(row);
	}
	layout->addLayout(result_grid_);

	QHBoxLayout *buttons = new QHBoxLayout;
	copy_button_ = new QPushButton(uiText("Copy", "Копировать"));
	copy_button_->setIcon(QIcon(QString::fromUtf8(":/images/icon_copy.png")));
	copy_button_->setIconSize(BUTTON_ICON_SIZE);
	copy_button_->setToolTip(loc_.tip(
		"Put the main number to the clipboard: the length of the ruler or the angle, "
		"without the units.",
		"Скопировать главное число в буфер обмена: длину линейки или угол, без единиц "
		"измерения."));
	buttons->addWidget(copy_button_);

	delete_button_ = new QPushButton(uiText("Delete", "Удалить"));
	delete_button_->setToolTip(loc_.tip(
		"Remove the measurement shown here (orange in the viewport).",
		"Убрать замер, показанный здесь (во вьюпорте он оранжевый)."));
	buttons->addWidget(delete_button_);

	clear_button_ = new QPushButton(uiText("Clear All", "Очистить"));
	clear_button_->setIcon(makeResetIcon());
	clear_button_->setIconSize(BUTTON_ICON_SIZE);
	clear_button_->setToolTip(loc_.tip("Remove all the measurements.", "Убрать все замеры."));
	buttons->addWidget(clear_button_);
	buttons->addStretch(1);
	layout->addLayout(buttons);

	connect(copy_button_, &QPushButton::clicked, this, [this]() { copy_value(); });
	connect(delete_button_, &QPushButton::clicked, this, [this]() { tool_->deleteShown(); });
	connect(clear_button_, &QPushButton::clicked, this, [this]() { tool_->clear(); });

	return group;
}

void MeasurePage::load_settings()
{
	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);
	tool_->setMode(MeasureTool::Mode(qBound(0, settings.value("mode", 0).toInt(), int(MeasureTool::MODE_FIGURE))));
	tool_->setSnap(MeasureTool::Snap(qBound(0, settings.value("snap", 0).toInt(), 1)));
	tool_->setPointSnap(settings.value("point_snap", true).toBool());
	SurfaceFilter filter;
	filter.surface_types = settings.value("filter_types", filter.surface_types).toInt();
	filter.only_immovable = settings.value("filter_immovable", false).toBool();
	filter.only_intersection = settings.value("filter_intersection", false).toBool();
	filter_->setFilter(filter);
	tool_->setSurfaceFilter(filter);
	tool_->setLoupe(settings.value("loupe", true).toBool());
	only_on_tab_->setChecked(settings.value("only_on_tab", false).toBool());
	tool_->setShownInactive(!only_on_tab_->isChecked());
	hotkeys_header_->setChecked(settings.value("hotkeys_expanded", true).toBool());
}

void MeasurePage::save_settings() const
{
	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);
	settings.setValue("mode", int(tool_->getMode()));
	settings.setValue("snap", int(tool_->getSnap()));
	settings.setValue("point_snap", tool_->isPointSnap());
	settings.setValue("filter_types", tool_->getSurfaceFilter().surface_types);
	settings.setValue("filter_immovable", tool_->getSurfaceFilter().only_immovable);
	settings.setValue("filter_intersection", tool_->getSurfaceFilter().only_intersection);
	settings.setValue("loupe", tool_->isLoupe());
	settings.setValue("only_on_tab", only_on_tab_->isChecked());
	settings.setValue("hotkeys_expanded", hotkeys_header_->isChecked());
}

void MeasurePage::resetSettings()
{
	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.remove(QString::fromUtf8(SETTINGS_GROUP));

	tool_->setMode(MeasureTool::MODE_DISTANCE);
	tool_->setSnap(MeasureTool::SNAP_SURFACE);
	tool_->setPointSnap(true);
	tool_->setLoupe(true);
	filter_->setFilter(SurfaceFilter());
	tool_->setSurfaceFilter(SurfaceFilter());
	only_on_tab_->setChecked(false);
	tool_->setFigureHeight(MeasureTool::FIGURE_HEIGHT);
	hotkeys_header_->setChecked(true);
}

void MeasurePage::set_results(const QVector<QPair<QString, QString>> &rows)
{
	for (int i = 0; i < result_rows_.size(); ++i)
	{
		const bool shown = i < rows.size();
		result_rows_[i].name->setVisible(shown);
		result_rows_[i].value->setVisible(shown);
		if (shown)
		{
			result_rows_[i].name->setText(rows[i].first);
			result_rows_[i].value->setText(rows[i].second);
		}
	}
}

void MeasurePage::copy_value()
{
	if (!copy_value_.isEmpty())
		QGuiApplication::clipboard()->setText(copy_value_);
}

void MeasurePage::refresh()
{
	if (QAbstractButton *button = mode_->button(tool_->getMode()))
		button->setChecked(true);
	if (QAbstractButton *button = snap_->button(tool_->getSnap()))
		button->setChecked(true);
	for (QCheckBox *box : {point_snap_, loupe_})
	{
		const QSignalBlocker blocker(box);
		box->setChecked(box == point_snap_ ? tool_->isPointSnap() : tool_->isLoupe());
	}
	{
		const QSignalBlocker blocker(measure_button_);
		measure_button_->setChecked(tool_->isMeasuring());
	}
	measure_button_->setText(tool_->isMeasuring() ? uiText("Stop Measuring (Esc)", "Закончить измерение (Esc)")
		: uiText("Measure", "Измерять"));

	// The numbers of the measurement being made (with the point under the mouse), or
	// of the one last clicked or made.
	MeasureTool::Measurement shown;
	bool preview = false;
	QVector<QPair<QString, QString>> rows;
	copy_value_.clear();
	if (!tool_->getShown(shown, preview))
	{
		result_title_->setText(uiText("No measurements yet.", "Замеров пока нет."));
	} else
	{
		const QVector<Math::Vec3> &points = shown.points;
		QString kind = uiText("Ruler", "Линейка");
		if (shown.mode == MeasureTool::MODE_ANGLE)
			kind = uiText("Angle", "Угол");
		else if (shown.mode == MeasureTool::MODE_FIGURE)
			kind = uiText("Person", "Человек");
		result_title_->setText(shown.finished ? kind : uiText("%1 - being put", "%1 - ставится").arg(kind));

		if (shown.mode == MeasureTool::MODE_FIGURE && !points.isEmpty())
		{
			rows.append({uiText("Height", "Рост"), QString::fromUtf8("%1").arg(shown.height, 0, 'f', 2)
				+ uiText(" m", " м")});
			rows.append({uiText("Stands at", "Стоит в точке"), QString::fromUtf8("X %1   Y %2   Z %3")
				.arg(double(points[0].x), 0, 'f', 3).arg(double(points[0].y), 0, 'f', 3).arg(double(points[0].z), 0, 'f', 3)});
			copy_value_ = QString::number(shown.height, 'f', 2);
		} else if (shown.mode == MeasureTool::MODE_DISTANCE && points.size() >= 2)
		{
			const Math::Vec3 &first = points[0];
			const Math::Vec3 &second = points[1];
			const double length = double(Math::length(second - first));
			rows.append({uiText("Length", "Длина"), format_length(length)});
			rows.append({uiText("Horizontal", "По горизонтали"), format_length(horizontal_length(first, second))});
			rows.append({uiText("Height difference", "Перепад высоты"), format_rise(double(second.z - first.z))});
			rows.append({uiText("Slope", "Уклон"), format_slope(first, second)});
			rows.append({uiText("Along axes", "По осям"), format_axes(first, second)});
			copy_value_ = QString::number(length, 'f', 3);
		} else if (shown.mode == MeasureTool::MODE_ANGLE && points.size() >= 3)
		{
			const double angle = MeasureTool::angleAt(points[0], points[1], points[2]);
			rows.append({uiText("Angle", "Угол"), format_angle(angle)});
			rows.append({uiText("To 180\xC2\xB0", "Дополнение до 180\xC2\xB0"), format_angle(180.0 - angle)});
			copy_value_ = QString::number(angle, 'f', 2);
		}
	}
	set_results(rows);
	copy_button_->setEnabled(!copy_value_.isEmpty());
	delete_button_->setEnabled(tool_->getNumMeasurements() > 0);
	clear_button_->setEnabled(tool_->getNumMeasurements() > 0);

	// What to do next.
	QString status;
	if (!tool_->isMeasuring())
	{
		status = uiText("Measuring is off: click a measurement in the viewport to see its numbers, "
			"press Measure to put points.",
			"Измерение выключено: кликните по замеру во вьюпорте, чтобы увидеть его цифры, "
			"нажмите «Измерять», чтобы ставить точки.");
	} else if (tool_->getMode() == MeasureTool::MODE_FIGURE)
	{
		status = uiText("Click in the viewport to stand a person %1 m tall there. The mouse wheel changes "
			"the height by 5 cm (over a figure - its own). Hold the button on a figure to move it.",
			"Кликните во вьюпорте, чтобы поставить туда человека ростом %1 м. Колесо мыши меняет "
			"рост на 5 см (над фигурой - её собственный). Зажмите кнопку на фигуре, чтобы "
			"передвинуть её.").arg(tool_->getFigureHeight(), 0, 'f', 2);
	} else if (!tool_->isMeasurementInProgress())
		status = uiText("Click in the viewport to put the first point.", "Кликните во вьюпорте, чтобы поставить первую точку.");
	else
	{
		const int placed = shown.points.size() - (preview ? 1 : 0);
		if (shown.mode == MeasureTool::MODE_ANGLE && placed == 1)
			status = uiText("Put the corner of the angle.", "Поставьте вершину угла.");
		else if (shown.mode == MeasureTool::MODE_ANGLE)
			status = uiText("Put the end of the second side.", "Поставьте конец второй стороны.");
		else
			status = uiText("Put the second point.", "Поставьте вторую точку.");

		const char *const axes[3] = {"X", "Y", "Z"};
		const QString axis = QString::fromUtf8(axes[qBound(0, tool_->getLockAxis(), 2)]);
		if (tool_->getLock() == MeasureTool::LOCK_AXIS)
		{
			status += uiText("\nLocked along %1 (the same key or Esc frees it).",
				"\nЗафиксировано вдоль оси %1 (та же клавиша или Esc снимает).").arg(axis);
		} else if (tool_->getLock() == MeasureTool::LOCK_PLANE)
		{
			status += uiText("\nLocked in the plane without %1 (the same key or Esc frees it).",
				"\nЗафиксировано в плоскости без оси %1 (та же клавиша или Esc снимает).").arg(axis);
		} else
		{
			status += uiText("\nX / Y / Z - along an axis, Shift + X / Y / Z - in a plane.",
				"\nX / Y / Z - вдоль оси, Shift + X / Y / Z - в плоскости.");
		}
	}
	if (!message_.isEmpty())
		status += QString::fromUtf8("\n") + message_;
	status_label_->setText(status);
}

} // namespace ArtistTool
