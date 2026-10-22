#include "RoadBuilderPage.h"
#include "AssetField.h"
#include "PlaceableAsset.h"
#include "SurfaceFilterButton.h"
#include "UiHelpers.h"

#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>

#include <UnigineEngine.h>

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QDial>
#include <QDir>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileDialog>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QListWidget>
#include <QMessageBox>
#include <QPainter>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

using ::UnigineEditor::Selection;

namespace ArtistTool
{

namespace
{

const char SETTINGS_ORGANIZATION[] = "zloy_pingvin";
const char SETTINGS_APPLICATION[] = "artist_tool";
const char SETTINGS_GROUP[] = "road_builder";

const int BUTTON_HEIGHT = 28;
const QSize BUTTON_ICON_SIZE(16, 16);
const int MAX_VISIBLE_ROADS = 4;
// A road of more triangles than this asks before it is generated.
const int TRIANGLES_WARNING_COUNT = 200000;

// The width a number field is to keep whatever the style of the editor sets, see
// RoadBuilderPage::eventFilter().
const char FIXED_WIDTH_PROPERTY[] = "artist_tool_fixed_width";
const int NARROW_FIELD_WIDTH = 50;

void keep_fixed_width(QWidget *widget)
{
	const int width = widget->property(FIXED_WIDTH_PROPERTY).toInt();
	if (width > 0 && (widget->minimumWidth() != width || widget->maximumWidth() != width))
		widget->setFixedWidth(width);
}

QIcon editor_icon(const char *path)
{
	return QIcon(QString::fromUtf8(path));
}

// A button that changes its look between the ordinary one and 'style' keeps the size
// of the larger of the two, so that the row does not jump when it changes color.
void keep_size_for_style(QPushButton *button, const QString &style)
{
	const QSize plain = button->sizeHint();
	button->setStyleSheet(style);
	const QSize styled = button->sizeHint();
	button->setStyleSheet(QString());
	button->setMinimumSize(plain.expandedTo(styled).expandedTo(QSize(0, BUTTON_HEIGHT)));
}

void set_style(QPushButton *button, const QString &style)
{
	if (button->styleSheet() != style)
		button->setStyleSheet(style);
}

QString road_item_text(const RoadInfo &road)
{
	return uiText("%1  -  %2 points, %3 pieces", "%1  -  точек: %2, кусков: %3")
		.arg(road.name).arg(road.points).arg(road.pieces);
}

} // namespace

RoadBuilderPage::RoadBuilderPage(QWidget *parent)
	: QWidget(parent)
{
	builder_ = new RoadBuilder(this);

	build_ui();
	load_settings();

	builder_->on_changed = [this]() { refresh(); };
	builder_->on_message = [this](int, const QString &text) {
		message_ = text;
		refresh();
	};
	builder_->on_roads_changed = [this]() { rescan_roads(); };

	connect(Selection::instance(), &Selection::changed, this, [this]() {
		if (isVisible())
			builder_->updateFromSelection();
	});

	refresh();
}

RoadBuilderPage::~RoadBuilderPage()
{
	builder_->on_changed = nullptr;
	builder_->on_message = nullptr;
	builder_->on_roads_changed = nullptr;
	save_settings();
}

void RoadBuilderPage::stop()
{
	builder_->setActive(false);
}

void RoadBuilderPage::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	builder_->setActive(true);
	rescan_roads();
}

void RoadBuilderPage::hideEvent(QHideEvent *event)
{
	builder_->setActive(false);
	QWidget::hideEvent(event);
}

bool RoadBuilderPage::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::Wheel)
	{
		const QWidget *widget = qobject_cast<QWidget *>(watched);
		if (widget && !widget->hasFocus())
		{
			event->ignore();
			return true;
		}
	}
	// The style of the editor gives number fields a width of its own every time it is
	// applied to them (also when the window is docked or undocked); a narrow field gets
	// its width back right after that.
	if (event->type() == QEvent::StyleChange || event->type() == QEvent::Polish)
	{
		if (QWidget *widget = qobject_cast<QWidget *>(watched))
			keep_fixed_width(widget);
	}
	// A click into a number field selects the whole number, so that a new one can be
	// typed right away.
	if (event->type() == QEvent::FocusIn)
	{
		if (QAbstractSpinBox *box = qobject_cast<QAbstractSpinBox *>(watched))
			QTimer::singleShot(0, box, &QAbstractSpinBox::selectAll);
	}
	return QWidget::eventFilter(watched, event);
}

void RoadBuilderPage::build_ui()
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
		"Builds a mesh along a curve drawn through points: roads, paths, tracks. The grid "
		"the mesh is going to have is drawn over the curve; Generate makes the mesh and "
		"saves it to the project.",
		"Строит меш вдоль кривой, проведённой через точки: дороги, тропинки, колеи. Сетка "
		"будущего меша рисуется поверх кривой; «Сгенерировать» создаёт меш и сохраняет "
		"его в проект."));
	description->setWordWrap(true);
	layout->addWidget(description);

	layout->addWidget(build_road_group());
	layout->addWidget(build_shape_group());
	QWidget *texture_group = build_texture_group();
	grid_only_widgets_ << texture_group;
	layout->addWidget(texture_group);
	layout->addWidget(build_output_group());

	// What is drawn over the road: the grid, the curve alone, or the vertex colors.
	const QString view_tooltip = loc_.tip(
		"What is drawn over the road in the viewport.\n"
		"Grid - the grid the mesh is going to have: the edges of the road, its rows and "
		"strips, the joints of the pieces.\n"
		"Curve Only - the curve and its points; the grid is still built and Generate makes "
		"the same mesh.",
		"Что рисуется поверх дороги во вьюпорте.\n"
		"[[Grid|Сетка]] - сетка будущего меша: края дороги, ряды и полосы, стыки кусков.\n"
		"[[Curve Only|Только кривая]] - кривая и точки; сетка всё равно строится, и "
		"«Сгенерировать» делает тот же меш.");
	QHBoxLayout *view_row = new QHBoxLayout;
	QLabel *view_label = new QLabel(uiText("In the viewport", "Во вьюпорте"));
	view_label->setToolTip(view_tooltip);
	view_row->addWidget(view_label);
	view_ = new QComboBox;
	view_->addItem(uiText("Grid", "Сетка"), int(ROAD_VIEW_GRID));
	view_->addItem(uiText("Curve Only", "Только кривая"), int(ROAD_VIEW_CURVE));
	view_->setToolTip(view_tooltip);
	view_->setFocusPolicy(Qt::StrongFocus);
	view_->installEventFilter(this);
	view_row->addWidget(view_);
	view_row->addStretch(1);
	connect(view_, &QComboBox::currentIndexChanged, this, [this](int) {
		builder_->setView(RoadView(view_->currentData().toInt()));
	});
	layout->addLayout(view_row);


	stats_label_ = new QLabel;
	stats_label_->setWordWrap(true);
	stats_label_->setToolTip(loc_.tip(
		"What the mesh is going to be, as drawn in the viewport: rows along the road, strips "
		"across it, triangles and pieces.",
		"Каким получится меш - как нарисовано во вьюпорте: рядов вдоль дороги, полос поперёк, "
		"треугольников и кусков."));
	layout->addWidget(stats_label_);

	generate_button_ = new QPushButton;
	generate_button_->setMinimumHeight(30);
	generate_button_->setIcon(editor_icon(":/images/tools/icon_drop_to_ground.png"));
	generate_button_->setIconSize(BUTTON_ICON_SIZE);
	generate_button_->setStyleSheet(goButtonStyle());
	generate_button_->setToolTip(loc_.tip(
		"Build the mesh of the road as it is drawn in the viewport and save it to the "
		"folder of the road. The pieces that are in \"mesh\" of the road stay and only get "
		"the new geometry - their material and whatever else was set on them by hand is "
		"kept; a longer road gets copies of its first piece, a shorter one loses the extra "
		"ones.\n"
		"The road keeps its file names: generating it again overwrites them.\n"
		"Can be undone with Ctrl+Z (the files stay as they were written).",
		"Построить меш дороги так, как он нарисован во вьюпорте, и сохранить его в папку "
		"дороги. Куски, что уже лежат в «mesh» дороги, остаются и получают только новую "
		"геометрию - их материал и всё, что настроено на них вручную, сохраняется; длинной "
		"дороге добавляются копии первого куска, у короткой лишние убираются.\n"
		"Имена файлов у дороги постоянные: повторная генерация их перезаписывает.\n"
		"Отменяется через Ctrl+Z (файлы при этом остаются как записаны)."));
	layout->addWidget(generate_button_);
	connect(generate_button_, &QPushButton::clicked, this, [this]() { generate(); });

	status_label_ = new QLabel;
	status_label_->setWordWrap(true);
	layout->addWidget(status_label_);

	QLabel *hotkeys = new QLabel(loc_.hotkeys(
		"LMB On A Point - Select It\n"
		"Ctrl / Shift + LMB On A Point - Add It To The Selection\n"
		"LMB (Hold) On A Point + Mouse Move - Move It Over Surfaces\n"
		"LMB On A Surface - Add Point To The End (while Add Points is on)\n"
		"Ctrl + LMB On A Surface - Add Corner Point To The End (while Add Points is on)\n"
		"Alt + LMB - Editor Camera (not taken by the tool)\n"
		"Esc - Stop Adding Points (a new road left without points is removed)\n"
		"Delete - Remove The Selected Point\n"
		"Ctrl + Z - Undo New Road, Added / Moved Point, Subdivide, Generate",
		"ЛКМ по точке - выделить её\n"
		"Ctrl / Shift + ЛКМ по точке - добавить её к выделению\n"
		"ЛКМ (держать) по точке + движение мыши - двигать её по поверхностям\n"
		"ЛКМ по поверхности - добавить точку в конец дороги (пока включено «[[Add Points|Добавить точки]]»)\n"
		"Ctrl + ЛКМ по поверхности - добавить в конец угловую точку (пока включено «[[Add Points|Добавить точки]]»)\n"
		"Alt + ЛКМ - камера редактора (инструмент её не перехватывает)\n"
		"Esc - закончить добавление точек (новая дорога без точек удаляется)\n"
		"Delete - удалить выделенную точку\n"
		"Ctrl + Z - отменить создание дороги, добавление / сдвиг точки, разделение, генерацию"));
	hotkeys->setWordWrap(true);
	hotkeys->setContentsMargins(6, 2, 6, 6);
	hotkeys_header_ = makeSectionHeader(loc_.hotkeys("Hotkeys", "Горячие клавиши"), hotkeys);
	hotkeys_header_->setToolTip(loc_.tip("Keys and mouse actions of the tool.", "Клавиши и действия мышью для этого инструмента."));
	layout->addWidget(hotkeys_header_);
	layout->addWidget(hotkeys);

	layout->addStretch(1);

	// The narrow fields get their width when they are in the page and the style has
	// done its part.
	for (QWidget *field : narrow_fields_)
	{
		field->setProperty(FIXED_WIDTH_PROPERTY, NARROW_FIELD_WIDTH);
		field->ensurePolished();
		keep_fixed_width(field);
	}
}

QWidget *RoadBuilderPage::build_road_group()
{
	QGroupBox *group = new QGroupBox(uiText("Road", "Дорога"));
	QVBoxLayout *layout = new QVBoxLayout(group);

	road_label_ = new QLabel;
	road_label_->setWordWrap(true);
	road_label_->setToolTip(loc_.tip(
		"The road the tool works with. Select any node of a road in the World Hierarchy "
		"(its root, a point or a piece of its mesh) to switch to it.",
		"Дорога, с которой работает инструмент. Чтобы переключиться на другую, выделите "
		"любую её ноду в World Hierarchy: корень, точку или кусок меша."));
	layout->addWidget(road_label_);

	QHBoxLayout *buttons = new QHBoxLayout;
	new_road_button_ = new QPushButton(uiText("New Road", "Новая дорога"));
	new_road_button_->setIcon(editor_icon(":/images/icon_add.png"));
	new_road_button_->setIconSize(BUTTON_ICON_SIZE);
	keep_size_for_style(new_road_button_, goButtonStyle());
	new_road_button_->setToolTip(loc_.tip(
		"Create an empty road in the world: a node with \"path\" (the points) and \"mesh\" "
		"(the generated pieces) inside. Then add points with Add Points.",
		"Создать в мире пустую дорогу: ноду, внутри которой «path» (точки) и «mesh» "
		"(сгенерированные куски). Затем добавьте точки кнопкой [[Add Points|«Добавить точки»]]."));
	buttons->addWidget(new_road_button_);

	add_points_button_ = new QPushButton;
	add_points_button_->setCheckable(true);
	add_points_button_->setMinimumHeight(BUTTON_HEIGHT);
	add_points_button_->setIcon(editor_icon(":/images/icons_pencil.png"));
	add_points_button_->setIconSize(BUTTON_ICON_SIZE);
	add_points_button_->setStyleSheet(primaryButtonStyle());
	add_points_button_->setToolTip(loc_.tip(
		"While it is on, every left click on a surface in the viewport adds a point at the "
		"end of the road; hold the button to drag it. Esc turns it off.\n"
		"Ctrl + click adds a corner point right away. To put a point between two others, "
		"use Subdivide.\n"
		"The manipulator of the editor is hidden meanwhile, so that a click does not miss "
		"it: drag the points with the mouse.",
		"Пока включено, каждый клик левой кнопкой по поверхности во вьюпорте добавляет "
		"точку в конец дороги; не отпуская кнопку, её можно тащить. Esc выключает.\n"
		"Ctrl + клик сразу ставит угловую точку. Чтобы поставить точку между двумя "
		"другими - кнопка [[Subdivide|«Разделить»]].\n"
		"Гизмо редактора на это время прячется, чтобы клик мимо него не ставил точку: "
		"точки двигайте мышью."));
	buttons->addWidget(add_points_button_, 1);
	layout->addLayout(buttons);

	const QString point_tooltip = loc_.tip(
		"What the selected points of the road are.\n"
		"Smooth - the road bends through the point without a break.\n"
		"Corner - a sharp bend; the edges of the road meet at an angle there.\n"
		"In the viewport the selected points are orange, corners white, smooth points "
		"blue, the last point green.",
		"Какие точки дороги выделены.\n"
		"[[Smooth|Плавная]] - дорога проходит через точку плавно, без излома.\n"
		"[[Corner|Угол]] - резкий излом; края дороги сходятся в нём под углом.\n"
		"Во вьюпорте выделенные точки оранжевые, угловые белые, обычные синие, последняя "
		"точка зелёная.");
	QHBoxLayout *point_row = new QHBoxLayout;
	point_row->setSpacing(3);
	QLabel *point_label = new QLabel(uiText("Selected point", "Выделенная точка"));
	point_label->setToolTip(point_tooltip);
	point_row->addWidget(point_label);
	point_row->addSpacing(6);

	const QString point_button_style = toggleButtonStyle(QString::fromUtf8("#d6d6d6"), QString::fromUtf8("#e4e4e4"), 64)
		+ QString::fromUtf8("QToolButton { padding: 3px 6px; }");
	smooth_button_ = new QToolButton;
	smooth_button_->setText(uiText("Smooth", "Плавная"));
	smooth_button_->setIcon(makeToggleIcon(QString::fromUtf8(":/images/icon_stroke_default.png")));
	corner_button_ = new QToolButton;
	corner_button_->setText(uiText("Corner", "Угол"));
	corner_button_->setIcon(makeToggleIcon(QString::fromUtf8(":/images/icon_curvature.png")));
	for (QToolButton *button : {smooth_button_, corner_button_})
	{
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(point_tooltip);
		button->setStyleSheet(point_button_style);
		point_row->addWidget(button);
	}
	subdivide_button_ = new QToolButton;
	subdivide_button_->setText(uiText("Subdivide", "Разделить"));
	subdivide_button_->setIcon(editor_icon(":/sandworm/images/icon_point.png"));
	subdivide_button_->setFocusPolicy(Qt::NoFocus);
	subdivide_button_->setStyleSheet(point_button_style
		+ QString::fromUtf8("QToolButton:pressed { background-color: #5c6066; }"));
	subdivide_button_->setToolTip(loc_.tip(
		"Put a new point in the middle of the road between two neighbouring points. Select "
		"both of them first: click one, then Ctrl + click the other.",
		"Поставить новую точку посередине дороги между двумя соседними точками. Сначала "
		"выделите обе: клик по одной, затем Ctrl + клик по другой."));
	point_row->addWidget(subdivide_button_);
	point_row->addStretch(1);

	QSize point_button_size;
	for (QToolButton *button : {smooth_button_, corner_button_, subdivide_button_})
	{
		button->setIconSize(BUTTON_ICON_SIZE);
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		point_button_size = point_button_size.expandedTo(button->sizeHint());
	}
	for (QToolButton *button : {smooth_button_, corner_button_, subdivide_button_})
		button->setFixedSize(point_button_size);
	// The rows of the selected points, in a frame of their own.
	QGroupBox *point_box = new QGroupBox(uiText("Point", "Точка"));
	QVBoxLayout *point_layout = new QVBoxLayout(point_box);
	point_layout->addLayout(point_row);
	layout->addWidget(point_box);

	// On the ground / at the height of the curve, for the selected points.
	const QString ground_tooltip = loc_.tip(
		"Where the road goes at the selected points while Snap To Ground is on.\n"
		"On Ground - it lies on the ground (the default).\n"
		"On Curve - it keeps to the height of the curve: lift or lower the point with the "
		"manipulator, e.g. the points of a bridge over a ravine. A line down to the ground "
		"shows how high such a point is.\n"
		"Between points of the two kinds the road goes smoothly from the ground to the "
		"height of the curve, a ramp with no step.",
		"Где идёт дорога у выделенных точек, пока включено [[Snap To Ground|«Прижать к земле»]].\n"
		"[[On Ground|На земле]] - лежит на земле (по умолчанию).\n"
		"[[On Curve|По кривой]] - идёт на высоте кривой: поднимите или опустите точку гизмо, "
		"например точки моста над оврагом. Линия вниз до земли показывает, насколько "
		"высоко такая точка.\n"
		"Между точками разного вида дорога плавно переходит от земли к высоте кривой - "
		"пандус без ступеньки.");
	QHBoxLayout *ground_row = new QHBoxLayout;
	ground_row->setSpacing(3);
	QLabel *ground_label = new QLabel(uiText("Point height", "Высота точки"));
	ground_label->setToolTip(ground_tooltip);
	ground_label->setMinimumWidth(point_label->sizeHint().width());
	ground_row->addWidget(ground_label);
	ground_row->addSpacing(6);
	on_ground_button_ = new QToolButton;
	on_ground_button_->setText(uiText("On Ground", "На земле"));
	on_ground_button_->setIcon(makeToggleIcon(QString::fromUtf8(":/images/tools/icon_drop_to_ground.png")));
	off_ground_button_ = new QToolButton;
	off_ground_button_->setText(uiText("On Curve", "По кривой"));
	off_ground_button_->setIcon(makeToggleIcon(QString::fromUtf8(":/images/tools/icon_snap_to_surface.png")));
	for (QToolButton *button : {on_ground_button_, off_ground_button_})
	{
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(ground_tooltip);
		button->setStyleSheet(point_button_style);
		button->setIconSize(BUTTON_ICON_SIZE);
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		button->setFixedSize(point_button_size.expandedTo(button->sizeHint()));
		ground_row->addWidget(button);
	}
	// The roll of the selected points, also set with the rotation manipulator.
	const QString roll_tooltip = loc_.tip(
		"How much the road leans to the side about the curve at the selected points, in "
		"degrees: a banked turn, a tilted bridge. Positive - the right edge down. Between "
		"the points it changes smoothly.\n"
		"It is the turn of the point itself. The points look along the curve by themselves: "
		"switch the manipulator of the editor to the Local basis and turn the Y ring (green) "
		"of a selected point - or type the angle here. For the whole road at once - Roll in Shape.\n"
		"It shows where the road keeps to the curve (On Curve points, or Snap To Ground "
		"off); where it lies on the ground, the ground gives its shape.",
		"Насколько дорога наклонена вбок вокруг кривой у выделенных точек, в градусах: "
		"вираж на повороте, скошенный мост. Плюс - правый край ниже. Между точками "
		"наклон меняется плавно.\n"
		"Это поворот самой точки. Точки сами смотрят вдоль кривой: переключите гизмо "
		"редактора в локальные оси (Local) и вращайте у выделенной точки кольцо Y "
		"(зелёное) - или введите угол здесь. Для всей дороги сразу - [[Roll|«Крен»]] в «Форме».\n"
		"Виден там, где дорога идёт по кривой (точки [[On Curve|«По кривой»]] или "
		"выключенное [[Snap To Ground|«Прижать к земле»]]); где дорога лежит на земле, "
		"её форму задаёт земля.");
	const QString point_width_tooltip = loc_.tip(
		"The width of the road at the selected points, a multiplier of Width in Shape: 1 - "
		"Width itself, 0.5 - half of it, 2 - twice as wide. Between the points the width "
		"changes smoothly.",
		"Ширина дороги у выделенных точек, множитель [[Width|«Ширины»]] из «Формы»: 1 - сама "
		"«Ширина», 0.5 - половина, 2 - вдвое шире. Между точками ширина меняется плавно.");
	// The width and the roll at the right end of the row.
	ground_row->addStretch(1);
	QLabel *point_width_label = new QLabel(uiText("Width", "Ширина"));
	point_width_label->setToolTip(point_width_tooltip);
	point_width_label->setContentsMargins(8, 0, 0, 0);
	ground_row->addWidget(point_width_label);
	point_width_ = make_spin_box(0.01, 100.0, 0.05, 2, point_width_tooltip);
	narrow_fields_.append(point_width_);
	ground_row->addWidget(point_width_);
	ground_row->addWidget(makeResetButton(loc_.tip("The width of the road (1)", "Ширина дороги (1)"), this, [this]() {
		if (point_width_->isEnabled())
			builder_->setSelectedPointsWidth(1.0);
	}));
	connect(point_width_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
		if (!updating_)
			builder_->setSelectedPointsWidth(value);
	});

	QLabel *roll_label = new QLabel(uiText("Roll", "Крен"));
	roll_label->setToolTip(roll_tooltip);
	roll_label->setContentsMargins(8, 0, 0, 0);
	ground_row->addWidget(roll_label);
	roll_ = make_spin_box(-90.0, 90.0, 1.0, 1, roll_tooltip);
	roll_->setSuffix(QString::fromUtf8("°"));
	ground_row->addWidget(roll_);
	ground_row->addWidget(makeResetButton(loc_.tip("No roll", "Без крена"), this, [this]() {
		if (roll_->isEnabled())
			builder_->setSelectedPointsRoll(0.0);
	}));
	point_layout->addLayout(ground_row);
	connect(roll_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
		if (!updating_)
			builder_->setSelectedPointsRoll(value);
	});


	QHBoxLayout *closed_row = new QHBoxLayout;
	closed_ = new QCheckBox(uiText("Closed Loop", "Замкнутая"));
	closed_->setToolTip(loc_.tip(
		"On: the last point is joined to the first one, the road is a ring (needs at least "
		"three points).",
		"Вкл: последняя точка соединяется с первой, дорога замыкается в кольцо (нужно "
		"минимум три точки)."));
	closed_row->addWidget(closed_, 1);
	closed_row->addWidget(makeResetButton(loc_.tip("Reset to default", "Сбросить на значение по умолчанию"),
		this, [this]() { closed_->setChecked(RoadSettings().closed); }));
	layout->addLayout(closed_row);

	// How the road lies on the ground, in a frame of its own under Closed Loop.
	layout->addWidget(build_ground_block());

	// All the settings of the road back to their defaults at once.
	reset_settings_button_ = new QPushButton(uiText("Reset Road Settings", "Сбросить настройки дороги"));
	reset_settings_button_->setIcon(makeResetIcon());
	reset_settings_button_->setIconSize(BUTTON_ICON_SIZE);
	// As tall as the point buttons it stands next to.
	reset_settings_button_->setFixedHeight(point_button_size.height());
	reset_settings_button_->setToolTip(loc_.tip(
		"Put all the settings of the current road back to their defaults: shape, ground, "
		"texture, decal. Its material, its folder and Closed Loop stay.\n"
		"Every road keeps its settings in itself, in the world; a new road starts with "
		"those of the road that was current.",
		"Вернуть все настройки текущей дороги к значениям по умолчанию: форму, землю, "
		"текстуру, декаль. Материал, папка и [[Closed Loop|«Замкнутая»]] остаются.\n"
		"Каждая дорога хранит свои настройки в себе, в мире; новая дорога начинает с "
		"настроек дороги, которая была текущей."));
	// In the row of Closed Loop, before its reset button: it is about the road, not
	// about the points.
	closed_row->insertWidget(1, reset_settings_button_);
	connect(reset_settings_button_, &QPushButton::clicked, this, [this]() {
		const QMessageBox::StandardButton answer = QMessageBox::question(this,
			uiText("Reset Road Settings", "Сбросить настройки дороги"),
			uiText("Put all the settings of the road \"%1\" back to their defaults? The material, the folder and Closed Loop stay.",
				"Вернуть все настройки дороги «%1» к значениям по умолчанию? Материал, папка и «Замкнутая» останутся.")
				.arg(builder_->getRoadName()),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
		message_.clear();
		builder_->resetSettings();
	});

	const QString roads_tooltip = loc_.tip(
		"All the roads of the world. Click one to work with it: it becomes the current road "
		"and its root node is selected in the editor.",
		"Все дороги мира. Кликните по дороге, чтобы работать с ней: она становится текущей, "
		"а её корневая нода выделяется в редакторе.");
	QHBoxLayout *roads_header = new QHBoxLayout;
	QLabel *roads_label = new QLabel(uiText("Roads in the world", "Дороги в мире"));
	roads_label->setToolTip(roads_tooltip);
	roads_header->addWidget(roads_label, 1);
	QToolButton *rescan_button = new QToolButton;
	rescan_button->setText(uiText("Find Roads", "Найти дороги"));
	rescan_button->setIcon(editor_icon(":/images/icon_go_to_asset.png"));
	rescan_button->setIconSize(BUTTON_ICON_SIZE);
	rescan_button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	rescan_button->setFocusPolicy(Qt::NoFocus);
	rescan_button->setStyleSheet(point_button_style
		+ QString::fromUtf8("QToolButton:pressed { background-color: #5c6066; }"));
	rescan_button->setToolTip(loc_.tip("Look through the world for roads again.", "Заново найти дороги в мире."));
	connect(rescan_button, &QToolButton::clicked, this, [this]() { rescan_roads(); });
	roads_header->addWidget(rescan_button);

	// What can be done with the current road, as small buttons after Find Roads.
	const auto make_road_button = [&](const char *icon, const QString &tooltip) {
		QToolButton *button = new QToolButton;
		button->setIcon(editor_icon(icon));
		button->setIconSize(BUTTON_ICON_SIZE);
		button->setFocusPolicy(Qt::NoFocus);
		button->setStyleSheet(point_button_style + QString::fromUtf8("QToolButton:pressed { background-color: #5c6066; }"));
		button->setToolTip(tooltip);
		roads_header->addWidget(button);
		return button;
	};
	clone_road_button_ = make_road_button(":/images/icon_clone.png", loc_.tip(
		"Clone the current road: a new road with the same points and settings, no mesh yet. "
		"E.g. a kerb along a road: clone it, make the copy a Bent Mesh and move it with Side "
		"Offset.",
		"Клонировать текущую дорогу: новая дорога с теми же точками и настройками, меша пока "
		"нет. Например, бордюр вдоль дороги: клонируйте её, сделайте копию «Изогнутым мешем» и "
		"сдвиньте [[Side Offset|«Смещением вбок»]]."));
	remove_curve_button_ = make_road_button(":/images/icon_delete_cross.png", loc_.tip(
		"Remove the curve of the current road and keep its generated mesh: the pieces stay "
		"as ordinary nodes, in a node of the name of the road. The road can not be edited "
		"any more. Ctrl+Z brings it back.",
		"Удалить кривую текущей дороги, оставив сгенерированный меш: куски остаются обычными "
		"нодами в ноде с именем дороги. Править дорогу больше нельзя. Ctrl+Z вернёт её."));
	remove_road_button_ = make_road_button(":/images/icon_delete_trash.png", loc_.tip(
		"Remove the current road completely: the curve, the generated mesh and its mesh files.",
		"Удалить текущую дорогу полностью: кривую, сгенерированный меш и его файлы."));
	connect(clone_road_button_, &QToolButton::clicked, this, [this]() {
		message_.clear();
		builder_->cloneRoad();
	});
	connect(remove_curve_button_, &QToolButton::clicked, this, [this]() {
		message_.clear();
		builder_->removeRoadCurve();
	});
	connect(remove_road_button_, &QToolButton::clicked, this, [this]() {
		const QMessageBox::StandardButton answer = QMessageBox::question(this,
			uiText("Remove Road", "Удалить дорогу"),
			uiText("Remove the road \"%1\" with its mesh and delete its mesh files? Ctrl+Z brings back the nodes, not the files.",
				"Удалить дорогу «%1» вместе с мешем и стереть файлы её мешей? Ctrl+Z вернёт ноды, но не файлы.")
				.arg(builder_->getRoadName()),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
		message_.clear();
		builder_->removeRoad(true);
	});
	layout->addLayout(roads_header);

	roads_list_ = new QListWidget;
	roads_list_->setToolTip(roads_tooltip);
	roads_list_->setAlternatingRowColors(true);
	roads_list_->setUniformItemSizes(true);
	roads_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	roads_list_->setStyleSheet(QString::fromUtf8(
		"QListWidget { background-color: #26282b; alternate-background-color: #303338; }"
		"QListWidget::item { padding: 1px 4px; }"
		"QListWidget::item:selected { background-color: #2f6fd6; color: white; }"));
	layout->addWidget(roads_list_);

	// Where the new roads go, as the Parent of the Object Placer.
	const QString parent_tooltip = loc_.tip(
		"New roads (New Road, Clone) become children of this node. World root - they are added "
		"to the end of the World Hierarchy; a clone then goes next to its road.",
		"Новые дороги («Новая дорога», клон) становятся детьми этой ноды. [[World root|Корень мира]] - "
		"добавляются в конец World Hierarchy; клон тогда кладётся рядом со своей дорогой.");
	QHBoxLayout *parent_row = new QHBoxLayout;
	parent_row->setSpacing(3);
	QLabel *parent_title = new QLabel(uiText("Parent", "Родитель"));
	parent_title->setToolTip(parent_tooltip);
	parent_row->addWidget(parent_title);
	parent_row->addSpacing(6);
	parent_label_ = new QLabel;
	parent_label_->setToolTip(parent_tooltip);
	parent_label_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
	parent_row->addWidget(parent_label_, 1);
	const auto make_parent_button = [&](const QString &text, const char *icon, const QString &tooltip) {
		QToolButton *button = new QToolButton;
		button->setText(text);
		button->setIcon(editor_icon(icon));
		button->setIconSize(BUTTON_ICON_SIZE);
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(tooltip);
		button->setStyleSheet(choiceButtonStyle());
		parent_row->addWidget(button);
		return button;
	};
	QToolButton *parent_select = make_parent_button(uiText("Use selected", "Взять выделенную"), ":/images/icon_stack.png",
		loc_.tip("Make the node selected in the World Hierarchy the parent of the new roads.",
			"Сделать ноду, выделенную в World Hierarchy, родителем новых дорог."));
	parent_clear_button_ = make_parent_button(uiText("Clear", "Сбросить"), ":/images/icon_delete_trash.png",
		loc_.tip("Put the new roads into the world root again.", "Снова класть новые дороги в корень мира."));
	layout->addLayout(parent_row);
	connect(parent_select, &QToolButton::clicked, this, [this]() {
		const ::UnigineEditor::SelectorNodes *selector = Selection::getSelectorNodes();
		if (!selector || selector->empty())
		{
			message_ = uiText("Select a node in the World Hierarchy to use it as the parent.",
				"Выделите ноду в World Hierarchy, чтобы сделать её родителем.");
			refresh();
			return;
		}
		message_.clear();
		builder_->setParentNode(selector->getNodes()[0]);
	});
	connect(parent_clear_button_, &QToolButton::clicked, this, [this]() {
		message_.clear();
		builder_->setParentNode(Unigine::NodePtr());
	});

	connect(roads_list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
		message_.clear();
		if (!builder_->selectRoad(item->data(Qt::UserRole).toInt()))
			rescan_roads();
	});
	connect(new_road_button_, &QPushButton::clicked, this, [this]() {
		message_.clear();
		// A new road starts open, whatever the current one is.
		RoadSettings settings = read_settings();
		settings.closed = false;
		builder_->createRoad(settings);
		builder_->setAddingPoints(true);
	});
	connect(add_points_button_, &QPushButton::toggled, this, [this](bool checked) {
		message_.clear();
		builder_->setAddingPoints(checked);
	});
	connect(smooth_button_, &QToolButton::clicked, this, [this]() { builder_->setSelectedPointsCorner(false); });
	connect(on_ground_button_, &QToolButton::clicked, this, [this]() { builder_->setSelectedPointsOffGround(false); });
	connect(off_ground_button_, &QToolButton::clicked, this, [this]() { builder_->setSelectedPointsOffGround(true); });
	connect(corner_button_, &QToolButton::clicked, this, [this]() { builder_->setSelectedPointsCorner(true); });
	connect(subdivide_button_, &QToolButton::clicked, this, [this]() {
		message_.clear();
		builder_->subdivide();
	});
	connect(closed_, &QCheckBox::toggled, this, [this](bool) { apply_settings(); });

	return group;
}

QWidget *RoadBuilderPage::build_shape_group()
{
	const RoadSettings defaults;
	const QString reset_tooltip = loc_.tip("Reset to default", "Сбросить на значение по умолчанию");

	// Two short fields in a row: label, field, reset button on the left half and the
	// same on the right one. A long field takes the whole row.
	QGroupBox *group = new QGroupBox(uiText("Shape", "Форма"));
	QGridLayout *layout = new QGridLayout(group);
	layout->setColumnStretch(1, 1);
	layout->setColumnStretch(4, 1);
	shape_layout_ = layout;

	const auto add_field = [&](int row, int side, const QString &label_text, QWidget *field, const QString &tooltip,
		const std::function<void()> &reset, int field_span = 1) {
		QLabel *label = new QLabel(label_text);
		label->setToolTip(tooltip);
		if (side == 1)
			label->setContentsMargins(8, 0, 0, 0);
		layout->addWidget(label, row, side * 3);
		layout->addWidget(field, row, side * 3 + 1, 1, field_span);
		QToolButton *reset_button = makeResetButton(reset_tooltip, this, reset);
		layout->addWidget(reset_button, row, side * 3 + field_span + 1);
		shape_fields_.insert(field, {label, field, reset_button});
		// A bent mesh keeps only the roll and the length of the pieces: the rest is the
		// shape of the grid.
		if (field != road_roll_ && field != piece_length_ && field != side_offset_)
			grid_only_widgets_ << label << field << reset_button;
	};

	const QString width_tooltip = loc_.tip(
		"The width of the road, in units. The orange lines in the viewport are its edges.",
		"Ширина дороги, в юнитах. Оранжевые линии во вьюпорте - её края.");
	width_ = make_spin_box(0.01, 10000.0, 0.1, 2, width_tooltip);
	add_field(0, 0, uiText("Width", "Ширина"), width_, width_tooltip, [this, defaults]() { width_->setValue(defaults.width); });

	const QString columns_tooltip = loc_.tip(
		"How many strips the grid has across the road. One strip is enough for a flat "
		"road; more let it follow the bumps of the ground under it.",
		"Сколько полос у сетки поперёк дороги. Для ровной дороги хватит одной; больше полос - "
		"дорога лучше ложится на неровности земли.");
	columns_ = new QSpinBox;
	columns_->setRange(1, 64);
	columns_->setToolTip(columns_tooltip);
	columns_->setFocusPolicy(Qt::StrongFocus);
	columns_->installEventFilter(this);
	enableSpinBoxDrag(columns_);
	add_field(1, 0, uiText("Strips Across", "Полос поперёк"), columns_, columns_tooltip,
		[this, defaults]() { columns_->setValue(defaults.columns); });

	const QString step_tooltip = loc_.tip(
		"The longest a row of the grid may be along the road, in units. On straight parts "
		"the rows are this long; on bends they get shorter, see Bend Angle.",
		"Самая большая длина ряда сетки вдоль дороги, в юнитах. На прямых участках ряды "
		"такой длины; на поворотах короче, см. [[Bend Angle|«Угол на поворотах»]].");
	step_ = make_spin_box(0.05, 10000.0, 0.1, 2, step_tooltip);
	add_field(2, 0, uiText("Step Along", "Шаг вдоль"), step_, step_tooltip, [this, defaults]() { step_->setValue(defaults.step); });

	const QString offset_tooltip = loc_.tip(
		"Moves the road to the side of the curve, in units: positive - to the right, negative - "
		"to the left (looking along the curve, from its first point). The curve itself is "
		"moved and the road is built along it, nothing is stretched: on the outer side of a "
		"bend the moved curve is longer and takes more. E.g. a kerb along the edge of a road "
		"drawn by its middle - half the width of the road.",
		"Смещает дорогу вбок от кривой, в юнитах: плюс - вправо, минус - влево (если смотреть "
		"вдоль кривой от первой точки). Смещается сама кривая, и дорога строится вдоль неё, "
		"ничего не растягивается: на внешней стороне поворота смещённая кривая длиннее и "
		"вмещает больше. Например, бордюр вдоль края дороги, проведённой по середине, - "
		"половина ширины дороги.");
	side_offset_ = make_spin_box(-10000.0, 10000.0, 0.1, 2, offset_tooltip);
	add_field(2, 1, uiText("Side Offset", "Смещение вбок"), side_offset_, offset_tooltip,
		[this, defaults]() { side_offset_->setValue(defaults.side_offset); });

	const QString road_roll_tooltip = loc_.tip(
		"How much the whole road leans to the side about the curve, in degrees, on top of "
		"the Roll of its points. Positive - the right edge down.\n"
		"It shows where the road keeps to the curve (On Curve points, or Snap To Ground off).",
		"Насколько вся дорога наклонена вбок вокруг кривой, в градусах, - вдобавок к "
		"[[Roll|«Крену»]] её точек. Плюс - правый край ниже.\n"
		"Виден там, где дорога идёт по кривой (точки [[On Curve|«По кривой»]] или выключенное "
		"[[Snap To Ground|«Прижать к земле»]]).");
	road_roll_ = make_spin_box(-90.0, 90.0, 1.0, 1, road_roll_tooltip);
	road_roll_->setSuffix(QString::fromUtf8("°"));
	add_field(3, 1, uiText("Roll", "Крен"), road_roll_, road_roll_tooltip, [this, defaults]() { road_roll_->setValue(defaults.roll); });

	hide_outdated_ = new QCheckBox(uiText("Hide Outdated Mesh", "Прятать устаревший меш"));
	hide_outdated_->setChecked(true);
	hide_outdated_->setToolTip(loc_.tip(
		"On: while the curve or the settings differ from those the mesh of the road was "
		"generated for, its pieces are hidden, so the old mesh does not overlap the grid or "
		"the preview of the new one. They come back after Generate, when the road is put "
		"back as it was (Ctrl+Z), when the tool is off and while the world is saved.\n"
		"Off: the old mesh stays seen.",
		"Вкл: пока кривая или настройки отличаются от тех, для которых меш дороги был "
		"сгенерирован, его куски спрятаны - старый меш не наслаивается на сетку и превью "
		"нового. Они возвращаются после «Сгенерировать», когда дорогу вернули как было "
		"(Ctrl+Z), когда инструмент выключен и пока мир сохраняется.\n"
		"Выкл: старый меш остаётся видимым."));
	connect(hide_outdated_, &QCheckBox::toggled, this, [this](bool checked) { builder_->setHideOutdated(checked); });
	// At the top of the left column, in the place of Width: a bent mesh has no Width.
	layout->addWidget(hide_outdated_, 0, 0, 1, 3);
	// Only a bent mesh has a preview the old pieces would overlap.
	bend_rows_ << hide_outdated_;

	bend_merge_ = new QCheckBox(uiText("Merge Surfaces By Material", "Объединять поверхности по материалу"));
	bend_merge_->setToolTip(loc_.tip(
		"On: the surfaces of all the meshes of the list that have the same material become one "
		"surface of each piece - fewer draw calls. A LOD level and the shadow LOD stay apart "
		"(lod0 with lod0, lod1 with lod1), else all the LODs would show at once. Such a "
		"surface takes the LOD distances and the shadows of the first mesh of the pattern "
		"that has the material, and is named after the material.\n"
		"Off: every mesh keeps its own surfaces (named m0_..., m1_... - the number of the "
		"asset in the list).",
		"Вкл: поверхности всех мешей списка с одинаковым материалом становятся одной "
		"поверхностью в каждом куске - меньше draw call-ов. Уровни LOD и shadow LOD "
		"сливаются только сами с собой (lod0 с lod0, lod1 с lod1), иначе все LOD-ы были бы "
		"видны сразу. Такая поверхность берёт дистанции LOD и тени у первого меша паттерна "
		"с этим материалом и называется по материалу.\n"
		"Выкл: у каждого меша свои поверхности (m0_..., m1_... - номер ассета в списке)."));
	// Under Hide Outdated Mesh, in the place of Strips Across.
	layout->addWidget(bend_merge_, 1, 0, 1, 3);
	connect(bend_merge_, &QCheckBox::toggled, this, [this](bool checked) {
		if (updating_)
			return;
		RoadSettings settings = builder_->getSettings();
		settings.bend_merge_materials = checked;
		builder_->setSettings(settings);
	});
	bend_rows_ << bend_merge_;

	// A bent mesh only: in the place left free under Roll.
	const QString mesh_angle_tooltip = loc_.tip(
		"The meshes get more segments only where the curve bends or rolls them by more than this "
		"angle across one segment: they bend smoothly, and the straight parts get no more triangles. "
		"Off - as they are.",
		"Меши получают дополнительные сегменты только там, где кривая гнёт или наклоняет их "
		"больше чем на этот угол в пределах одного сегмента: они гнутся плавно, а прямые участки "
		"лишних треугольников не получают. «Выкл» - как есть.");
	QLabel *mesh_angle_label = new QLabel(uiText("Mesh Angle", "Угол меша"));
	mesh_angle_label->setToolTip(mesh_angle_tooltip);
	mesh_angle_label->setContentsMargins(8, 0, 0, 0);
	bend_mesh_angle_ = make_spin_box(0.0, 90.0, 1.0, 1, mesh_angle_tooltip);
	bend_mesh_angle_->setSuffix(QString::fromUtf8("°"));
	bend_mesh_angle_->setSpecialValueText(uiText("Off", "Выкл"));
	QToolButton *mesh_angle_reset = makeResetButton(reset_tooltip, this,
		[this, defaults]() { bend_mesh_angle_->setValue(defaults.bend_mesh_angle); });
	layout->addWidget(mesh_angle_label, 2, 3);
	layout->addWidget(bend_mesh_angle_, 2, 4);
	layout->addWidget(mesh_angle_reset, 2, 5);
	bend_rows_ << mesh_angle_label << bend_mesh_angle_ << mesh_angle_reset;
	connect(bend_mesh_angle_, &QDoubleSpinBox::valueChanged, this, [this](double) {
		if (updating_)
			return;
		RoadSettings settings = builder_->getSettings();
		settings.bend_mesh_angle = float(bend_mesh_angle_->value());
		builder_->setSettings(settings);
	});

	const QString piece_tooltip = loc_.tip(
		"The longest one piece of the road may be, in units. A longer road is cut into "
		"pieces of about the same length, each a node and a mesh of its own: the engine "
		"then draws only the pieces that are seen. The joints are yellow in the "
		"viewport. 0 - the road is one piece.\n"
		"A piece of a decal road also gets at most %1 triangles: a mesh decal of more is "
		"slow to draw, so such a piece is cut further by itself.",
		"Самая большая длина одного куска дороги, в юнитах. Более длинная дорога режется "
		"на куски примерно равной длины, у каждого своя нода и свой меш: движок рисует "
		"только видимые куски. Стыки во вьюпорте жёлтые. 0 - дорога одним куском.\n"
		"Кусок дороги-декали к тому же не больше %1 треугольников: тяжёлая декаль-меш "
		"рисуется медленно, поэтому такой кусок дополнительно режется сам.").arg(RoadBuilder::maxDecalTriangles());
	piece_length_ = make_spin_box(0.0, 100000.0, 1.0, 1, piece_tooltip);
	// Under Step Along, on the left of Roll.
	add_field(3, 0, uiText("Max Piece Length", "Макс. длина куска"), piece_length_, piece_tooltip,
		[this, defaults]() { piece_length_->setValue(defaults.piece_length); });


	const QString angle_tooltip = loc_.tip(
		"How smooth the bends are, in degrees: the road turns by at most this angle from "
		"one row of the grid to the next. Smaller - more rows on bends, smoother curves; "
		"the straight parts do not get more rows.",
		"Насколько плавные повороты, в градусах: от ряда к ряду сетки дорога поворачивает "
		"не больше чем на этот угол. Меньше - больше рядов на поворотах и плавнее изгиб; "
		"на прямых рядов не прибавляется.");
	max_angle_ = make_spin_box(0.5, 90.0, 0.5, 1, angle_tooltip);
	max_angle_->setSuffix(QString::fromUtf8("°"));
	add_field(1, 1, uiText("Bend Angle", "Угол на поворотах"), max_angle_, angle_tooltip,
		[this, defaults]() { max_angle_->setValue(defaults.max_angle); });

	const QString round_tooltip = loc_.tip(
		"On a tight bend the inner edge of the road gets a sharp point, and on a bend "
		"tighter than half the road it would fold over itself. Such a place is laid along "
		"a smooth arc instead, and the grid fans out evenly around it.\n"
		"The value, in units, is both how sharp the edge may get (the smallest radius it "
		"turns around) and how much of the road before and after the place the arc takes "
		"in: more - a softer, wider inner corner; 0 - only the folds are rounded.\n"
		"The outer edge is not touched: a corner point keeps its sharp outer corner.",
		"На крутом повороте внутренний край дороги заостряется, а на повороте круче "
		"половины ширины дороги сложился бы сам на себя. Такое место укладывается по "
		"плавной дуге, а сетка равномерно расходится вокруг неё веером.\n"
		"Значение, в юнитах, - это и насколько острым может быть край (самый малый радиус "
		"его поворота), и сколько дороги до и после этого места захватывает дуга: больше - "
		"мягче и шире внутренний угол; 0 - скругляются только складки.\n"
		"Внешний край не трогается: угловая точка сохраняет острый внешний угол.");
	inner_round_ = make_spin_box(0.0, 10000.0, 0.1, 2, round_tooltip);
	add_field(0, 1, uiText("Inner Edge Rounding", "Скругление внутреннего края"), inner_round_, round_tooltip,
		[this, defaults]() { inner_round_->setValue(defaults.inner_round); });

	for (QDoubleSpinBox *box : {width_, step_, max_angle_, inner_round_, road_roll_, side_offset_})
		connect(box, &QDoubleSpinBox::valueChanged, this, [this](double) { apply_settings(); });
	connect(columns_, &QSpinBox::valueChanged, this, [this](int) { apply_settings(); });

	return group;
}

// How the road lies on the ground: Snap To Ground with its Recompute, Snap After
// Release, the offset and what the road is laid on. A frame of its own in the Road
// group, under Closed Loop.
QWidget *RoadBuilderPage::build_ground_block()
{
	const RoadSettings defaults;
	const QString reset_tooltip = loc_.tip("Reset to default", "Сбросить на значение по умолчанию");

	QGroupBox *block = new QGroupBox(uiText("Ground", "Земля"));
	QGridLayout *layout = new QGridLayout(block);
	layout->setColumnStretch(1, 1);
	int row = 0;

	const auto add_row = [&](const QString &label_text, QWidget *field, const QString &tooltip,
		const std::function<void()> &reset, QWidget **row_widgets = nullptr) {
		QLabel *label = new QLabel(label_text);
		label->setToolTip(tooltip);
		layout->addWidget(label, row, 0);
		layout->addWidget(field, row, 1);
		QToolButton *reset_button = makeResetButton(reset_tooltip, this, reset);
		layout->addWidget(reset_button, row, 2);
		if (row_widgets)
		{
			row_widgets[0] = label;
			row_widgets[1] = field;
			row_widgets[2] = reset_button;
		}
		++row;
	};

	snap_to_ground_ = new QCheckBox(uiText("Snap To Ground", "Прижать к земле"));
	snap_to_ground_->setToolTip(loc_.tip(
		"On: every vertex of the grid is put on the surface under it (searched 5 units "
		"above the curve and 100 below), so the road lies on the ground.\n"
		"Off: the road is level across and follows the height of the curve.",
		"Вкл: каждая вершина сетки ставится на поверхность под ней (ищется на 5 юнитов "
		"выше кривой и на 100 ниже) - дорога ложится на землю.\n"
		"Выкл: дорога ровная поперёк и идёт по высоте кривой."));
	QHBoxLayout *snap_row = new QHBoxLayout;
	snap_row->setSpacing(6);
	snap_row->addWidget(snap_to_ground_);
	// The heights of the ground are measured once and kept while the road is edited:
	// this measures them all anew.
	refresh_ground_ = new QPushButton(uiText("Recompute", "Пересчитать"));
	refresh_ground_->setIcon(editor_icon(":/images/tools/icon_drop_to_ground.png"));
	refresh_ground_->setIconSize(BUTTON_ICON_SIZE);
	refresh_ground_->setFocusPolicy(Qt::NoFocus);
	refresh_ground_->setToolTip(loc_.tip(
		"Measure the ground under the whole road anew.\n"
		"The heights of the ground are measured once and remembered: when a point is "
		"moved, only the part of the road around it is measured, the rest is taken from "
		"what is remembered - that is what keeps the editing fast. If the ground itself "
		"has changed (the terrain was sculpted, objects were moved), press this.\n"
		"Generate always measures the whole road anew.",
		"Заново измерить землю под всей дорогой.\n"
		"Высоты земли измеряются один раз и запоминаются: при сдвиге точки измеряется "
		"только участок дороги вокруг неё, остальное берётся из памяти - поэтому "
		"редактирование быстрое. Если изменилась сама земля (подправили террейн, сдвинули "
		"объекты), нажмите эту кнопку.\n"
		"[[Generate|«Сгенерировать»]] всегда измеряет всю дорогу заново."));
	// At the right end of the row, next to the reset button.
	snap_row->addStretch(1);
	snap_row->addWidget(refresh_ground_);
	layout->addLayout(snap_row, row, 0, 1, 2);
	QToolButton *snap_reset = makeResetButton(reset_tooltip, this, [this, defaults]() {
		snap_to_ground_->setChecked(defaults.snap_to_ground);
	});
	layout->addWidget(snap_reset, row, 2);
	grid_only_widgets_ << snap_to_ground_ << refresh_ground_ << snap_reset;
	++row;

	// A setting of the page, not of a road: how the editing feels.
	snap_after_release_ = new QCheckBox(uiText("Snap After Release", "Прижимать после отпускания"));
	snap_after_release_->setToolTip(loc_.tip(
		"On: while a point is dragged (or anything is changed with the mouse button held), "
		"the road is drawn at the height of the curve and is put on the ground when the "
		"button is let go. The fastest way to edit a long road over a heavy scene.\n"
		"Off: the road lies on the ground all the time.",
		"Вкл: пока точку тащат (или что-то меняют с зажатой кнопкой мыши), дорога рисуется "
		"на высоте кривой и ложится на землю, когда кнопку отпускают. Самый быстрый способ "
		"править длинную дорогу в тяжёлой сцене.\n"
		"Выкл: дорога всё время лежит на земле."));
	layout->addWidget(snap_after_release_, row, 0, 1, 2);
	QToolButton *after_release_reset = makeResetButton(reset_tooltip, this, [this]() { snap_after_release_->setChecked(false); });
	layout->addWidget(after_release_reset, row, 2);
	grid_only_widgets_ << snap_after_release_ << after_release_reset;
	++row;
	connect(refresh_ground_, &QPushButton::clicked, this, [this]() {
		message_.clear();
		builder_->refreshGround();
	});
	connect(snap_after_release_, &QCheckBox::toggled, this, [this](bool checked) { builder_->setSnapAfterRelease(checked); });

	const QString offset_tooltip = loc_.tip(
		"How far above the surface the road is put, in units: a little lift keeps the "
		"ground from showing through it.",
		"На сколько дорога приподнята над поверхностью, в юнитах: небольшой подъём не даёт "
		"земле просвечивать сквозь неё.");
	ground_offset_ = make_spin_box(-1000.0, 1000.0, 0.01, 3, offset_tooltip);
	// In the row of Snap To Ground, between it and Recompute.
	QLabel *offset_label = new QLabel(uiText("Offset From Surface", "Отступ от поверхности"));
	offset_label->setToolTip(offset_tooltip);
	offset_label->setContentsMargins(8, 0, 0, 0);
	QToolButton *offset_reset = makeResetButton(reset_tooltip, this, [this, defaults]() {
		ground_offset_->setValue(defaults.ground_offset);
	});
	snap_row->insertWidget(1, offset_label);
	snap_row->insertWidget(2, ground_offset_, 1);
	snap_row->insertWidget(3, offset_reset);
	ground_offset_row_[0] = offset_label;
	ground_offset_row_[1] = ground_offset_;
	ground_offset_row_[2] = offset_reset;

	// The same filter as "Place on" of the Object Placer, but what it leaves out is
	// looked through: the road goes onto the terrain under the rocks, not over them.
	SurfaceFilterButton::Texts filter_texts;
	filter_texts.tooltip = loc_.tip(
		"What the road and its points are laid on. The unchecked kinds of objects are "
		"looked through: e.g. with only Terrain checked the road lies on the terrain and "
		"goes under the rocks and the buildings standing on it, and a click on a rock "
		"puts a point onto the terrain under it.",
		"На что ложится дорога и ставятся её точки. Неотмеченные типы объектов "
		"пропускаются насквозь: например, если отмечен только Terrain, дорога ложится на "
		"террейн и проходит под камнями и домами, стоящими на нём, а клик по камню ставит "
		"точку на террейн под ним.");
	filter_texts.only_immovable = uiText("Only On Immovable Nodes", "Только на ноды с флагом Immovable");
	filter_texts.only_immovable_tooltip = loc_.tip(
		"Lay the road only on nodes that have the Immovable flag enabled.",
		"Класть дорогу только на ноды с включённым флагом Immovable.");
	filter_texts.only_intersection = uiText("Only On Surfaces With Intersection", "Только на поверхности с Intersection");
	filter_texts.only_intersection_tooltip = loc_.tip(
		"Lay the road only on surfaces that have the Intersection flag and any bit of the "
		"Intersection Mask enabled.",
		"Класть дорогу только на поверхности с включённым флагом Intersection и хотя бы "
		"одним битом Intersection Mask.");
	filter_texts.reset_tooltip = reset_tooltip;
	ground_filter_ = new SurfaceFilterButton(filter_texts);
	QLabel *filter_label = new QLabel(uiText("Lay On", "Класть на"));
	filter_label->setToolTip(filter_texts.tooltip);
	layout->addWidget(filter_label, row, 0);
	layout->addWidget(ground_filter_, row, 1, 1, 2);
	++row;
	ground_filter_->on_changed = [this]() { apply_settings(); };

	connect(ground_offset_, &QDoubleSpinBox::valueChanged, this, [this](double) { apply_settings(); });
	connect(snap_to_ground_, &QCheckBox::toggled, this, [this](bool) { apply_settings(); });

	return block;
}

QWidget *RoadBuilderPage::build_texture_group()
{
	const RoadSettings defaults;
	const QString reset_tooltip = loc_.tip("Reset to default", "Сбросить на значение по умолчанию");

	// The fields on the left, as wide as they need, and the rotation dial right after
	// them, in a column of its own; the room that is left stays on the right.
	QGroupBox *group = new QGroupBox(uiText("Texture", "Текстура"));
	QHBoxLayout *group_layout = new QHBoxLayout(group);
	QGridLayout *layout = new QGridLayout;
	group_layout->addLayout(layout);

	const QString length_tooltip = loc_.tip(
		"The length of the road one repeat of the texture takes, in units: the texture is "
		"not stretched however long the road is. Around a closed road it is adjusted a "
		"little, so that the texture meets itself where the road closes.",
		"Какую длину дороги занимает один повтор текстуры, в юнитах: текстура не "
		"растягивается, какой бы длинной ни была дорога. У замкнутой дороги длина чуть "
		"подгоняется, чтобы текстура сошлась в месте замыкания.");
	QLabel *length_label = new QLabel(uiText("Texture Length", "Длина текстуры"));
	length_label->setToolTip(length_tooltip);
	layout->addWidget(length_label, 0, 0);
	uv_length_ = make_spin_box(0.001, 100000.0, 0.1, 3, length_tooltip);
	layout->addWidget(uv_length_, 0, 1);
	layout->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() { uv_length_->setValue(defaults.uv_length); }), 0, 2);

	const QString width_tooltip = loc_.tip(
		"How many times the texture repeats across the road, from its left edge to the "
		"right one.",
		"Сколько раз текстура повторяется поперёк дороги, от левого края до правого.");
	QLabel *width_label = new QLabel(uiText("Repeats Across", "Повторов поперёк"));
	width_label->setToolTip(width_tooltip);
	layout->addWidget(width_label, 1, 0);
	uv_width_ = make_spin_box(-1000.0, 1000.0, 0.1, 3, width_tooltip);
	layout->addWidget(uv_width_, 1, 1);
	layout->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() { uv_width_->setValue(defaults.uv_width); }), 1, 2);

	// A dial like the rotation ones of the Object Placer, with the angle under it.
	const QString rotation_tooltip = loc_.tip(
		"How the texture is turned on the road, in degrees. 0 - the road goes along V "
		"(up the texture); 90 - for a texture drawn with its road going across it, "
		"along U. Turn the dial - it sticks to 0, 45, 90, 135 and 180 degrees (the long "
		"marks) - or click it and use the arrow keys (1 degree) and Page Up / Page Down "
		"(15 degrees).",
		"Поворот текстуры на дороге, в градусах. 0 - дорога идёт вдоль V (вверх по "
		"текстуре); 90 - для текстуры, где дорога нарисована поперёк, вдоль U. Крутите "
		"диск - он залипает на 0, 45, 90, 135 и 180 градусах (длинные риски), - или "
		"кликните по нему и жмите стрелки (1 градус) и Page Up / Page Down (15 градусов).");
	uv_rotation_ = makeAngleDial(-180, 180);
	// A whole circle, like a compass: 0 at the top, 90 on the right, -90 on the left,
	// 180 at the bottom, and no stop there.
	uv_rotation_->setWrapping(true);
	uv_rotation_->setFixedSize(56, 56);
	uv_rotation_->setToolTip(rotation_tooltip);
	uv_rotation_->setFocusPolicy(Qt::StrongFocus);
	uv_rotation_->installEventFilter(this);
	uv_rotation_value_ = new QLabel;
	uv_rotation_value_->setToolTip(rotation_tooltip);

	// The dial, its caption with the angle under it, and the reset button on the right.
	QHBoxLayout *rotation_layout = new QHBoxLayout;
	rotation_layout->setSpacing(6);
	rotation_layout->addWidget(uv_rotation_, 0, Qt::AlignVCenter);
	QVBoxLayout *rotation_info = new QVBoxLayout;
	rotation_info->setSpacing(1);
	rotation_info->addStretch(1);
	QLabel *rotation_label = new QLabel(uiText("UV Rotation", "Поворот UV"));
	rotation_label->setToolTip(rotation_tooltip);
	rotation_info->addWidget(rotation_label);
	rotation_info->addWidget(uv_rotation_value_);
	rotation_info->addStretch(1);
	rotation_layout->addLayout(rotation_info);
	rotation_layout->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() {
		uv_rotation_->setValue(int(defaults.uv_rotation));
	}), 0, Qt::AlignVCenter);
	group_layout->addSpacing(24);
	group_layout->addLayout(rotation_layout);
	group_layout->addStretch(1);

	for (QDoubleSpinBox *box : {uv_length_, uv_width_})
		connect(box, &QDoubleSpinBox::valueChanged, this, [this](double) { apply_settings(); });
	connect(uv_rotation_, &QDial::valueChanged, this, [this](int angle) {
		uv_rotation_value_->setText(QString::fromUtf8("%1°").arg(angle));
		apply_settings();
	});

	return group;
}

QWidget *RoadBuilderPage::build_bend_block()
{
	const RoadSettings defaults;
	const QString reset_tooltip = loc_.tip("Reset to default", "Сбросить на значение по умолчанию");

	QWidget *block = new QWidget;
	QVBoxLayout *block_layout = new QVBoxLayout(block);
	block_layout->setContentsMargins(0, 0, 0, 0);

	// The axes, as in the Path Placer.
	QGridLayout *axes = new QGridLayout;
	axes->setColumnStretch(1, 1);
	const QString forward_tooltip = loc_.tip(
		"Which axis of the meshes goes along the curve.",
		"Какая ось мешей идёт вдоль кривой.");
	const QString up_tooltip = loc_.tip(
		"Which axis of the meshes looks up. It can not be the same axis as the forward one: "
		"picking it switches the other.",
		"Какая ось мешей смотрит вверх. Не может совпадать с [[Forward axis|«Осью вперёд»]]: при "
		"таком выборе вторая ось переключится сама.");
	const auto add_axis_row = [&](int row, const QString &text, const QString &tooltip, QButtonGroup *&group,
		const std::function<void()> &reset) {
		QLabel *label = new QLabel(text);
		label->setToolTip(tooltip);
		axes->addWidget(label, row, 0);
		QHBoxLayout *buttons = new QHBoxLayout;
		buttons->setSpacing(3);
		group = makeAxisButtons(buttons, this, tooltip);
		buttons->addStretch(1);
		axes->addLayout(buttons, row, 1);
		axes->addWidget(makeResetButton(reset_tooltip, this, reset), row, 2);
	};
	add_axis_row(0, uiText("Forward axis", "Ось вперёд"), forward_tooltip, bend_forward_, [this, defaults]() {
		RoadSettings settings = builder_->getSettings();
		settings.bend_forward = defaults.bend_forward;
		if (int(settings.bend_up) / 2 == int(settings.bend_forward) / 2)
			settings.bend_up = defaults.bend_up;
		builder_->setSettings(settings);
	});
	add_axis_row(1, uiText("Up axis", "Ось вверх"), up_tooltip, bend_up_, [this, defaults]() {
		RoadSettings settings = builder_->getSettings();
		settings.bend_up = defaults.bend_up;
		if (int(settings.bend_up) / 2 == int(settings.bend_forward) / 2)
			settings.bend_forward = defaults.bend_forward;
		builder_->setSettings(settings);
	});
	block_layout->addLayout(axes);

	connect(bend_forward_, &QButtonGroup::idClicked, this, [this](int) { apply_bend_axes(true); });
	connect(bend_up_, &QButtonGroup::idClicked, this, [this](int) { apply_bend_axes(false); });

	// The list of the meshes, as the fill pattern of the Path Placer.
	QLabel *title = new QLabel(uiText("Meshes", "Меши"));
	block_layout->addWidget(title);
	bend_empty_label_ = new QLabel(uiText(
		"No meshes. Select meshes, nodes or models in the Asset Browser and press Add Selected Assets.",
		"Мешей нет. Выделите меши, ноды или модели в Asset Browser и нажмите «Добавить выбранные ассеты»."));
	bend_empty_label_->setWordWrap(true);
	bend_empty_label_->setEnabled(false);
	block_layout->addWidget(bend_empty_label_);
	QWidget *list_widget = new QWidget;
	bend_list_ = new QGridLayout(list_widget);
	bend_list_->setContentsMargins(0, 0, 0, 0);
	bend_list_->setHorizontalSpacing(3);
	bend_list_->setVerticalSpacing(2);
	bend_list_->setColumnStretch(1, 1);
	block_layout->addWidget(list_widget);

	QHBoxLayout *buttons = new QHBoxLayout;
	QPushButton *add_button = new QPushButton(uiText("Add Selected Assets", "Добавить выбранные ассеты"));
	add_button->setIcon(editor_icon(":/images/mods/icon_object_mode.png"));
	add_button->setIconSize(BUTTON_ICON_SIZE);
	add_button->setMinimumHeight(BUTTON_HEIGHT);
	add_button->setToolTip(loc_.tip(
		"Add the assets selected in the Asset Browser to the end of the list: .mesh, .node or "
		"imported models (.fbx ...). All the objects of an asset are bent together, with their "
		"materials. The LODs stay LODs: every one is bent and keeps the visible and fade "
		"distances of the asset; LODs told only by their names (_lod1, _lod_1...) with no "
		"distances set up would all show at once, so only the first of them is taken. A new "
		"mesh is a part of the pattern; pick "
		"Start or End in its row to put it once at an end of the curve.",
		"Добавить ассеты, выделенные в Asset Browser, в конец списка: .mesh, .node или "
		"импортированные модели (.fbx ...). Все объекты ассета изгибаются вместе, со своими "
		"материалами. LOD-ы остаются LOD-ами: каждый изгибается и сохраняет дистанции "
		"видимости и затухания ассета; LOD-ы, заданные только именами (_lod1, _lod_1...) без "
		"настроенных дистанций, были бы видны все сразу, поэтому берётся только первый. Новый "
		"меш - часть паттерна; выберите в его "
		"строке «Начало» или «Конец», чтобы он ставился один раз на конце кривой."));
	buttons->addWidget(add_button, 1);
	bend_clear_button_ = new QPushButton(uiText("Clear", "Очистить"));
	bend_clear_button_->setIcon(editor_icon(":/images/icon_delete_trash.png"));
	bend_clear_button_->setIconSize(BUTTON_ICON_SIZE);
	bend_clear_button_->setMinimumHeight(BUTTON_HEIGHT);
	bend_clear_button_->setToolTip(loc_.tip("Remove all the meshes from the list.", "Убрать все меши из списка."));
	buttons->addWidget(bend_clear_button_);
	block_layout->addLayout(buttons);
	connect(add_button, &QPushButton::clicked, this, [this]() {
		message_.clear();
		builder_->addSelectedBendParts();
	});
	connect(bend_clear_button_, &QPushButton::clicked, this, [this]() {
		message_.clear();
		builder_->clearBendParts();
	});
	return block;
}

void RoadBuilderPage::refresh_bend_list()
{
	const QVector<RoadBendPart> &parts = builder_->getSettings().bend_parts;
	bend_empty_label_->setVisible(parts.isEmpty());
	bend_clear_button_->setEnabled(!parts.isEmpty());

	// The rows are made anew only when the list of assets changes or another road becomes
	// the current one, so that typing a value does not lose the focus.
	QString signature = QString::number(builder_->getRoadId()) + QLatin1Char(':');
	for (const RoadBendPart &part : parts)
		signature += part.source + QLatin1Char('|') + QString::number(int(part.role)) + QLatin1Char('|');
	if (signature != bend_signature_)
	{
		bend_signature_ = signature;
		bend_part_rows_.clear();
		while (QLayoutItem *item = bend_list_->takeAt(0))
		{
			if (QWidget *widget = item->widget())
				widget->deleteLater();
			delete item;
		}

		const QString role_tooltip = loc_.tip(
			"What the mesh is.\n"
			"Pattern - repeated along the curve; several pattern meshes go one after another, "
			"in the order of the list.\n"
			"Start / End - put once at the start / at the end of the curve (a closed curve has "
			"no ends).",
			"Что это за меш.\n"
			"[[Pattern|Паттерн]] - повторяется вдоль кривой; несколько мешей паттерна идут друг "
			"за другом, в порядке списка.\n"
			"[[Start|Начало]] / [[End|Конец]] - ставится один раз в начале / в конце кривой (у "
			"замкнутой кривой концов нет).");
		const QString count_tooltip = loc_.tip(
			"How many times in a row this mesh is put before the next one of the pattern. The "
			"start and the end are put once.",
			"Сколько раз подряд ставится этот меш, прежде чем пойдёт следующий из паттерна. "
			"Начало и конец ставятся один раз.");
		const QString scale_tooltip = loc_.tip(
			"The scale of the mesh along its own X, Y and Z: the size of the bent mesh. The "
			"length follows it while it is measured.",
			"Масштаб меша по его собственным осям X, Y и Z - размер изогнутого меша. Длина "
			"следует за ним, пока измеряется сама.");
		const QString length_tooltip = loc_.tip(
			"The length of the curve from the start of this mesh to the start of the next one, "
			"in units, as in the Path Placer: it does not stretch the mesh. More than the mesh - "
			"a gap after it, less - the next one overlaps it; 0 - the next one starts at the "
			"same place.\n"
			"It is measured from the mesh along the forward axis (with its scale); the button "
			"next to it measures it again. To fill the curve exactly, all the meshes are then "
			"stretched or squeezed a little.",
			"Длина кривой от начала этого меша до начала следующего, в юнитах, как в «По "
			"кривой»: меш она не растягивает. Больше меша - после него промежуток, меньше - "
			"следующий заходит на него; 0 - следующий начинается там же.\n"
			"Измеряется по мешу вдоль оси вперёд (с масштабом); кнопка рядом измеряет заново. "
			"Чтобы ровно заполнить кривую, все меши потом чуть растягиваются или сжимаются.");

		// The captions of the columns.
		if (!parts.isEmpty())
		{
			const char *const axis_names[3] = {"X", "Y", "Z"};
			for (int axis = 0; axis < 3; ++axis)
			{
				QLabel *caption = new QLabel(QString::fromUtf8(axis_names[axis]));
				caption->setAlignment(Qt::AlignCenter);
				caption->setStyleSheet(QString::fromUtf8("QLabel { color: %1; font-weight: bold; }").arg(axisColor(axis)));
				caption->setToolTip(scale_tooltip);
				bend_list_->addWidget(caption, 0, 6 + axis);
			}
			QLabel *length_caption = new QLabel(uiText("Length", "Длина"));
			length_caption->setAlignment(Qt::AlignCenter);
			length_caption->setToolTip(length_tooltip);
			// Over the field only, not over its reset button.
			bend_list_->addWidget(length_caption, 0, 9);
		}

		QVector<QWidget *> narrow_fields;
		for (int i = 0; i < parts.size(); ++i)
		{
			const int r = i + 1;
			BendPartRow row;

			QLabel *number = new QLabel(QString::fromUtf8("%1.").arg(i + 1));
			number->setEnabled(false);
			bend_list_->addWidget(number, r, 0);

			// The asset field of the editor: another mesh is picked in the dialog or dropped
			// onto it.
			AssetField::Options name_options;
			name_options.tooltip = parts[i].source_name;
			name_options.dialog_title = uiText("Mesh to bend", "Меш для изгиба");
			name_options.icon = QString::fromUtf8(":/images/mods/icon_object_mode.png");
			name_options.show_tooltip = loc_.tip("Show this asset in the Asset Browser.", "Показать этот ассет в Asset Browser.");
			AssetField *name = new AssetField(name_options);
			name->setValue(parts[i].source_name, true);
			name->setIcon(kindIcon(PlaceableAsset::Kind(parts[i].source_kind)));
			name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
			bend_list_->addWidget(name, r, 1, 1, 2);
			// Later: the row goes away with the list.
			name->on_picked = [this, i](const Unigine::Vector<Unigine::UGUID> &guids) {
				QTimer::singleShot(0, this, [this, i, guids]() {
					builder_->replaceBendPart(i, guids);
					refresh();
				});
			};
			name->on_show = [this, i]() {
				message_ = builder_->showBendPart(i) ? QString()
					: uiText("The asset is not found in the project.", "Ассет не найден в проекте.");
				refresh();
			};

			row.role = new QComboBox;
			row.role->addItem(uiText("Pattern", "Паттерн"), int(ROAD_BEND_PATTERN));
			row.role->addItem(uiText("Start", "Начало"), int(ROAD_BEND_START));
			row.role->addItem(uiText("End", "Конец"), int(ROAD_BEND_END));
			row.role->setToolTip(role_tooltip);
			row.role->setFocusPolicy(Qt::StrongFocus);
			row.role->installEventFilter(this);
			bend_list_->addWidget(row.role, r, 3);

			// The sign stands next to the field, as in the Path Placer.
			QLabel *times = new QLabel(QString::fromUtf8("×"));
			times->setEnabled(false);
			times->setToolTip(count_tooltip);
			bend_list_->addWidget(times, r, 4);
			row.count = new QSpinBox;
			row.count->setRange(1, 999);
			row.count->setToolTip(count_tooltip);
			row.count->setFocusPolicy(Qt::StrongFocus);
			row.count->installEventFilter(this);
			enableSpinBoxDrag(row.count);
			row.count->setProperty(FIXED_WIDTH_PROPERTY, 44);
			narrow_fields.append(row.count);
			// The start and the end are put once.
			row.count->setEnabled(parts[i].role == ROAD_BEND_PATTERN);
			bend_list_->addWidget(row.count, r, 5);
			connect(row.count, &QSpinBox::valueChanged, this, [this, i](int value) {
				if (updating_)
					return;
				RoadSettings settings = builder_->getSettings();
				if (i >= settings.bend_parts.size())
					return;
				settings.bend_parts[i].count = value;
				builder_->setSettings(settings);
			});
			connect(row.role, &QComboBox::currentIndexChanged, this, [this, i](int index) {
				if (updating_)
					return;
				RoadSettings settings = builder_->getSettings();
				if (i >= settings.bend_parts.size())
					return;
				const RoadBendRole role = RoadBendRole(bend_part_rows_[i].role->itemData(index).toInt());
				// One start and one end: the mesh that had the role becomes a part of the
				// pattern. The start goes to the top of the list, the end to the bottom.
				if (role != ROAD_BEND_PATTERN)
				{
					for (RoadBendPart &part : settings.bend_parts)
					{
						if (part.role == role)
							part.role = ROAD_BEND_PATTERN;
					}
				}
				settings.bend_parts[i].role = role;
				builder_->setSettings(settings);
			});

			for (int axis = 0; axis < 3; ++axis)
			{
				QDoubleSpinBox *box = make_spin_box(0.001, 1000.0, 0.05, 3, scale_tooltip);
				box->setProperty(FIXED_WIDTH_PROPERTY, 46);
				narrow_fields.append(box);
				row.scale[axis] = box;
				bend_list_->addWidget(box, r, 6 + axis);
				connect(box, &QDoubleSpinBox::valueChanged, this, [this, i, axis](double value) {
					if (updating_)
						return;
					RoadSettings settings = builder_->getSettings();
					if (i >= settings.bend_parts.size())
						return;
					settings.bend_parts[i].scale[axis] = float(value);
					builder_->setSettings(settings);
				});
			}

			row.length = make_spin_box(0.0, 100000.0, 0.1, 3, length_tooltip);
			row.length->setProperty(FIXED_WIDTH_PROPERTY, 58);
			narrow_fields.append(row.length);
			bend_list_->addWidget(row.length, r, 9);
			connect(row.length, &QDoubleSpinBox::valueChanged, this, [this, i](double value) {
				if (updating_)
					return;
				RoadSettings settings = builder_->getSettings();
				if (i >= settings.bend_parts.size())
					return;
				settings.bend_parts[i].length = float(value);
				settings.bend_parts[i].auto_length = false;
				builder_->setSettings(settings);
			});
			row.length_reset = makeResetButton(loc_.tip("Measure the length from the mesh again.", "Заново измерить длину по мешу."),
				this, [this, i]() {
					RoadSettings settings = builder_->getSettings();
					if (i >= settings.bend_parts.size())
						return;
					settings.bend_parts[i].auto_length = true;
					builder_->setSettings(settings);
				});
			bend_list_->addWidget(row.length_reset, r, 10);

			QToolButton *up = new QToolButton;
			up->setArrowType(Qt::UpArrow);
			up->setFixedSize(16, 20);
			up->setAutoRaise(true);
			up->setFocusPolicy(Qt::NoFocus);
			// Only within the pattern: the start stays first, the end last.
			up->setEnabled(i > 0 && parts[i].role == ROAD_BEND_PATTERN && parts[i - 1].role == ROAD_BEND_PATTERN);
			up->setToolTip(loc_.tip("Move this mesh up the list.", "Поднять этот меш выше в списке."));
			bend_list_->addWidget(up, r, 11);
			connect(up, &QToolButton::clicked, this, [this, i]() {
				bend_signature_.clear();
				builder_->moveBendPart(i, -1);
			});

			QToolButton *down = new QToolButton;
			down->setArrowType(Qt::DownArrow);
			down->setFixedSize(16, 20);
			down->setAutoRaise(true);
			down->setFocusPolicy(Qt::NoFocus);
			down->setEnabled(i + 1 < parts.size() && parts[i].role == ROAD_BEND_PATTERN && parts[i + 1].role == ROAD_BEND_PATTERN);
			down->setToolTip(loc_.tip("Move this mesh down the list.", "Опустить этот меш ниже в списке."));
			bend_list_->addWidget(down, r, 12);
			connect(down, &QToolButton::clicked, this, [this, i]() {
				bend_signature_.clear();
				builder_->moveBendPart(i, 1);
			});

			QToolButton *remove = new QToolButton;
			remove->setText(QString::fromUtf8("✕"));
			remove->setFixedSize(18, 20);
			remove->setAutoRaise(true);
			remove->setFocusPolicy(Qt::NoFocus);
			remove->setToolTip(loc_.tip("Remove this mesh from the list.", "Убрать этот меш из списка."));
			bend_list_->addWidget(remove, r, 13);
			connect(remove, &QToolButton::clicked, this, [this, i]() { builder_->removeBendPart(i); });

			bend_part_rows_.append(row);
		}
		for (QWidget *field : narrow_fields)
		{
			field->ensurePolished();
			keep_fixed_width(field);
		}
	}

	// The values of the rows.
	const bool was_updating = updating_;
	updating_ = true;
	for (int i = 0; i < parts.size() && i < bend_part_rows_.size(); ++i)
	{
		BendPartRow &row = bend_part_rows_[i];
		row.role->setCurrentIndex(row.role->findData(int(parts[i].role)));
		if (!row.count->hasFocus())
			row.count->setValue(parts[i].count);
		for (int axis = 0; axis < 3; ++axis)
		{
			if (!row.scale[axis]->hasFocus())
				row.scale[axis]->setValue(double(parts[i].scale[axis]));
		}
		if (!row.length->hasFocus())
			row.length->setValue(builder_->getBendPartLength(i));
		row.length_reset->setEnabled(!parts[i].auto_length);
	}
	updating_ = was_updating;
}

void RoadBuilderPage::arrange_shape(bool bent)
{
	if (shape_bent_ == int(bent))
		return;
	shape_bent_ = int(bent);

	const auto place = [this](QWidget *field, int row, int side) {
		const QVector<QWidget *> widgets = shape_fields_.value(field);
		if (widgets.size() != 3)
			return;
		for (QWidget *widget : widgets)
			shape_layout_->removeWidget(widget);
		widgets[0]->setContentsMargins(side == 1 ? 8 : 0, 0, 0, 0);
		shape_layout_->addWidget(widgets[0], row, side * 3);
		shape_layout_->addWidget(widgets[1], row, side * 3 + 1);
		shape_layout_->addWidget(widgets[2], row, side * 3 + 2);
	};
	if (bent)
	{
		// Next to the two checkboxes, then the length of the pieces under them.
		place(side_offset_, 0, 1);
		place(road_roll_, 1, 1);
		place(piece_length_, 2, 0);
	} else
	{
		place(side_offset_, 2, 1);
		place(road_roll_, 3, 1);
		place(piece_length_, 3, 0);
	}
}

void RoadBuilderPage::apply_bend_axes(bool forward_picked)
{
	if (updating_)
		return;
	RoadSettings settings = builder_->getSettings();
	const AxisDirection old_forward = settings.bend_forward;
	const AxisDirection old_up = settings.bend_up;
	settings.bend_forward = AxisDirection(qMax(0, bend_forward_->checkedId()));
	settings.bend_up = AxisDirection(qMax(0, bend_up_->checkedId()));
	if (int(settings.bend_forward) / 2 == int(settings.bend_up) / 2)
	{
		if (forward_picked)
			settings.bend_up = old_forward;
		else
			settings.bend_forward = old_up;
	}
	builder_->setSettings(settings);
}

QWidget *RoadBuilderPage::build_output_group()
{
	const RoadSettings defaults;
	const QString reset_tooltip = loc_.tip("Reset to default", "Сбросить на значение по умолчанию");

	QGroupBox *group = new QGroupBox(uiText("Result", "Результат"));
	// Six columns, as in Shape: the decal settings go two in a row.
	QGridLayout *layout = new QGridLayout(group);
	layout->setColumnStretch(1, 1);
	layout->setColumnStretch(4, 1);
	int row = 0;

	const QString output_tooltip = loc_.tip(
		"What the road is made into.\n"
		"Mesh - an ordinary static mesh: real geometry, for roads with their own shape, "
		"bridges, anything that is not just drawn on the ground. Takes a mesh material "
		"(mesh_base and its children).\n"
		"Decal - a mesh decal: the road is projected onto the ground under it, so there "
		"is no gap and no flicker between them. For paths, tracks, asphalt on terrain. "
		"Takes a decal material (decal_base and its children).\n"
		"Bent Mesh - a ready mesh of the project (a kerb, a fence, a guard rail, a pipe) "
		"laid along the curve again and again, every copy bent to follow it; it keeps "
		"its materials.",
		"Во что превращается дорога.\n"
		"[[Mesh|Меш]] - обычный статичный меш: настоящая геометрия, для дорог со своей формой, "
		"мостов, всего, что не просто нарисовано на земле. Нужен материал меша (mesh_base "
		"и его наследники).\n"
		"[[Decal|Декаль]] - декаль-меш: дорога проецируется на землю под собой, поэтому между "
		"ними нет щели и мерцания. Для тропинок, колей, асфальта на террейне. Нужен материал "
		"декали (decal_base и его наследники).\n"
		"[[Bent Mesh|Изогнутый меш]] - готовый меш проекта (бордюр, забор, отбойник, труба) "
		"раскладывается вдоль кривой раз за разом, каждая копия изгибается по ней; "
		"материалы у него свои.");
	QLabel *output_label = new QLabel(uiText("Make Into", "Создать как"));
	output_label->setToolTip(output_tooltip);
	layout->addWidget(output_label, row, 0);
	QHBoxLayout *output_layout = new QHBoxLayout;
	output_layout->setSpacing(3);
	output_ = new QButtonGroup(this);
	const QString output_names[] = {uiText("Mesh", "Меш"), uiText("Decal", "Декаль"), uiText("Bent Mesh", "Изогнутый меш")};
	const char *const output_icons[] = {":/sandworm/images/icon_extent.png", ":/images/icon_leaks.png",
		":/images/icon_stroke_default.png"};
	for (int id = ROAD_OUTPUT_MESH; id <= ROAD_OUTPUT_BENT; ++id)
	{
		QToolButton *button = new QToolButton;
		button->setText(output_names[id]);
		button->setIcon(editor_icon(output_icons[id]));
		button->setIconSize(BUTTON_ICON_SIZE);
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(output_tooltip);
		button->setStyleSheet(choiceButtonStyle());
		output_->addButton(button, id);
		output_layout->addWidget(button);
	}
	output_layout->addStretch(1);
	layout->addLayout(output_layout, row, 1, 1, 4);
	layout->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() {
		if (QAbstractButton *button = output_->button(defaults.output))
			button->setChecked(true);
		apply_settings();
	}), row, 5);
	++row;

	// The rows of the settings: a label, a field and a reset button each, two in a row:
	// 'side' 0 - the left half, 1 - the right one, which ends the row. 'group' - the
	// widgets shown only for one kind of the road.
	const auto add_pair_row = [&](int side, const QString &label_text, QDoubleSpinBox *field, const QString &tooltip,
		const std::function<void()> &reset, QVector<QWidget *> *group) {
		QLabel *label = new QLabel(label_text);
		label->setToolTip(tooltip);
		if (side == 1)
			label->setContentsMargins(8, 0, 0, 0);
		layout->addWidget(label, row, side * 3);
		layout->addWidget(field, row, side * 3 + 1);
		QToolButton *reset_button = makeResetButton(reset_tooltip, this, reset);
		layout->addWidget(reset_button, row, side * 3 + 2);
		if (group)
			*group << label << field << reset_button;
		if (side == 1)
			++row;
	};

	// The distances, for every kind of the road.
	const QString visible_tooltip = loc_.tip(
		"From what distance to the camera the road starts to fade out, in units (Max Visible "
		"Distance). inf - always seen. A bent mesh keeps the LOD distances of its asset within it. "
		"The pieces get it at once.",
		"С какого расстояния до камеры дорога начинает исчезать, в юнитах (Max Visible Distance). "
		"inf - видна всегда. Изогнутый меш сохраняет внутри неё дистанции LOD своего ассета. Куски "
		"получают значение сразу.");
	decal_visible_distance_ = make_spin_box(0.0, 1000000.0, 10.0, 1, visible_tooltip);
	// No limit is written "inf", the way the editor shows it; "inf" can be typed too.
	decal_visible_distance_->setSpecialValueText(QString::fromUtf8("inf"));
	add_pair_row(0, uiText("Visible Distance", "Видна до"), decal_visible_distance_, visible_tooltip,
		[this, defaults]() { decal_visible_distance_->setValue(defaults.decal_visible_distance); }, nullptr);

	const QString fade_tooltip = loc_.tip(
		"Over what distance after Visible Distance the road fades out smoothly, in units (Max Fade "
		"Distance). 0 - it disappears at once. The pieces get it at once.",
		"На каком расстоянии после [[Visible Distance|«Видна до»]] дорога плавно исчезает, в юнитах "
		"(Max Fade Distance). 0 - пропадает сразу. Куски получают значение сразу.");
	decal_fade_distance_ = make_spin_box(0.0, 1000000.0, 5.0, 1, fade_tooltip);
	add_pair_row(1, uiText("Fade Distance", "Затухание"), decal_fade_distance_, fade_tooltip,
		[this, defaults]() { decal_fade_distance_->setValue(defaults.decal_fade_distance); }, nullptr);

	// What only a decal has.
	const QString lift_tooltip = loc_.tip(
		"How high above the ground the decal mesh is put, in units: the ground that bulges above "
		"the mesh between its vertices still gets the road. Too high - it falls on kerbs and stones "
		"too.",
		"На сколько меш декали приподнят над землёй, в юнитах: земля, выпирающая над мешем между "
		"вершинами, всё равно получает дорогу. Слишком высоко - дорога ляжет и на бордюры и камни.");
	decal_lift_ = make_spin_box(0.0, 100.0, 0.05, 2, lift_tooltip);
	add_pair_row(0, uiText("Lift Above Ground", "Подъём над землёй"), decal_lift_, lift_tooltip,
		[this, defaults]() { decal_lift_->setValue(defaults.decal_lift); }, &decal_rows_);

	const QString depth_tooltip = loc_.tip(
		"How deep the decal is projected down from its mesh, in units. Too deep - the road also "
		"falls on what is under the ground. The decals get it at once.",
		"На какую глубину декаль проецируется вниз от своего меша, в юнитах. Слишком глубоко - "
		"дорога ляжет и на то, что под землёй. Декали получают значение сразу.");
	decal_depth_ = make_spin_box(0.01, 1000.0, 0.1, 2, depth_tooltip);
	add_pair_row(1, uiText("Projection Depth", "Глубина проекции"), decal_depth_, depth_tooltip,
		[this, defaults]() { decal_depth_->setValue(defaults.decal_depth); }, &decal_rows_);

	// The meshes of a bent road, in a block of their own.
	QWidget *bend_block = build_bend_block();
	layout->addWidget(bend_block, row, 0, 1, 6);
	bend_rows_ << bend_block;
	++row;

	const QString material_tooltip = loc_.tip(
		"The material of the road: pick it with the folder button or drag it here from the Asset "
		"Browser. The pieces get it at once. A decal needs a decal material, a mesh - a mesh one.",
		"Материал дороги: выберите кнопкой с папкой или перетащите сюда из Asset Browser. Куски "
		"получают его сразу. Декали нужен материал декали, мешу - материал меша.");
	QLabel *material_title = new QLabel(uiText("Material", "Материал"));
	material_rows_ << material_title;
	material_title->setToolTip(material_tooltip);
	layout->addWidget(material_title, row, 0);
	AssetField::Options material_options;
	material_options.tooltip = material_tooltip;
	material_options.dialog_title = uiText("Material of the road", "Материал дороги");
	material_options.filter = QString::fromUtf8(".mat");
	material_options.icon = QString::fromUtf8(":/images/assets/file_material.png");
	material_options.reset_tooltip = loc_.tip("Back to the base material.", "Вернуть базовый материал.");
	material_options.show_tooltip = loc_.tip("Show the material in the Asset Browser.", "Показать материал в Asset Browser.");
	material_options.take_text = uiText("Use Selected", "Взять выделенный");
	material_options.take_tooltip = loc_.tip("Take the material selected in the Asset Browser.",
		"Взять материал, выделенный в Asset Browser.");
	material_field_ = new AssetField(material_options);
	material_rows_ << material_field_;
	layout->addWidget(material_field_, row, 1, 1, 5);
	material_field_->on_picked = [this](const Unigine::Vector<Unigine::UGUID> &guids) {
		message_.clear();
		builder_->takeMaterial(guids);
	};
	material_field_->on_show = [this]() { builder_->showMaterial(); };
	material_field_->on_reset = [this]() {
		message_.clear();
		builder_->clearMaterial();
	};
	++row;

	const QString folder_tooltip = loc_.tip(
		"The folder of the project the meshes of the road are saved to, \"roads\" next to the world "
		"by default. The files are named after the road: <road>_00.mesh...",
		"Папка проекта, куда сохраняются меши дороги, по умолчанию «roads» рядом с миром. Файлы "
		"называются по имени дороги: <дорога>_00.mesh...");
	QLabel *folder_title = new QLabel(uiText("Folder", "Папка"));
	folder_title->setToolTip(folder_tooltip);
	layout->addWidget(folder_title, row, 0);
	AssetField::Options folder_options;
	folder_options.tooltip = folder_tooltip;
	folder_options.dialog_title = uiText("Folder for the meshes of the road", "Папка для мешей дороги");
	folder_options.icon = QString::fromUtf8(":/images/assets/folder.png");
	folder_options.reset_tooltip = loc_.tip("Back to the default folder.", "Вернуть папку по умолчанию.");
	folder_options.show_tooltip = loc_.tip("Show the generated meshes of the road in the Asset Browser.",
		"Показать сгенерированные меши дороги в Asset Browser.");
	folder_field_ = new AssetField(folder_options);
	layout->addWidget(folder_field_, row, 1, 1, 5);
	folder_field_->on_browse = [this]() { browse_folder(); };
	folder_field_->on_show = [this]() {
		message_ = builder_->showGeneratedFiles() ? QString()
			: uiText("The generated meshes are not found in the project.", "Сгенерированные меши не найдены в проекте.");
		refresh();
	};
	folder_field_->on_reset = [this]() {
		RoadSettings settings = builder_->getSettings();
		settings.folder.clear();
		builder_->setSettings(settings);
	};
	++row;

	connect(output_, &QButtonGroup::idClicked, this, [this](int) { apply_settings(); });
	for (QDoubleSpinBox *box : {decal_depth_, decal_lift_, decal_visible_distance_, decal_fade_distance_, piece_length_})
		connect(box, &QDoubleSpinBox::valueChanged, this, [this](double) { apply_settings(); });

	return group;
}

QDoubleSpinBox *RoadBuilderPage::make_spin_box(double minimum, double maximum, double step, int decimals,
	const QString &tooltip)
{
	QDoubleSpinBox *box = new QDoubleSpinBox;
	box->setRange(minimum, maximum);
	box->setSingleStep(step);
	box->setDecimals(decimals);
	box->setToolTip(tooltip);
	box->setFocusPolicy(Qt::StrongFocus);
	box->installEventFilter(this);
	enableSpinBoxDrag(box);
	return box;
}

void RoadBuilderPage::load_settings()
{
	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);

	// The settings of a road are kept in the road itself, in its world: nothing of them
	// is carried over from one project to another. Earlier builds kept the ones used
	// last here; they are dropped.
	const QStringList conveniences = {QString::fromUtf8("hotkeys_expanded"), QString::fromUtf8("snap_after_release"),
		QString::fromUtf8("view"), QString::fromUtf8("hide_outdated")};
	// "Show Grid" of the builds before 1.3 is "In the viewport" now.
	if (settings.contains("show_grid") && !settings.contains("view"))
		settings.setValue("view", settings.value("show_grid").toBool() ? int(ROAD_VIEW_GRID) : int(ROAD_VIEW_CURVE));
	for (const QString &key : settings.childKeys())
	{
		if (!conveniences.contains(key))
			settings.remove(key);
	}

	hotkeys_header_->setChecked(settings.value("hotkeys_expanded", true).toBool());
	snap_after_release_->setChecked(settings.value("snap_after_release", false).toBool());
	const int view = view_->findData(settings.value("view", int(ROAD_VIEW_GRID)).toInt());
	view_->setCurrentIndex(view >= 0 ? view : 0);
	hide_outdated_->setChecked(settings.value("hide_outdated", true).toBool());
	builder_->setHideOutdated(hide_outdated_->isChecked());
}

void RoadBuilderPage::resetSettings()
{
	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.remove(QString::fromUtf8(SETTINGS_GROUP));

	hotkeys_header_->setChecked(true);
	snap_after_release_->setChecked(false);
	view_->setCurrentIndex(0);
	hide_outdated_->setChecked(true);
}

void RoadBuilderPage::save_settings() const
{
	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);
	settings.setValue("hotkeys_expanded", hotkeys_header_->isChecked());
	settings.setValue("snap_after_release", snap_after_release_->isChecked());
	settings.setValue("view", view_->currentData().toInt());
	settings.setValue("hide_outdated", hide_outdated_->isChecked());
}

RoadSettings RoadBuilderPage::read_settings() const
{
	const RoadSettings &current = builder_->getSettings();
	const auto field_value = [](const QDoubleSpinBox *field, float kept) {
		// The field shows the kept value rounded: it is the kept one that is meant.
		const double precision = 0.6 * std::pow(10.0, -field->decimals());
		return qAbs(field->value() - double(kept)) < precision ? kept : float(field->value());
	};

	RoadSettings settings = current;
	settings.width = field_value(width_, current.width);
	settings.columns = columns_->value();
	settings.step = field_value(step_, current.step);
	settings.max_angle = field_value(max_angle_, current.max_angle);
	settings.inner_round = field_value(inner_round_, current.inner_round);
	settings.roll = field_value(road_roll_, current.roll);
	settings.side_offset = field_value(side_offset_, current.side_offset);
	settings.ground = ground_filter_->getFilter();
	settings.closed = closed_->isChecked();
	settings.snap_to_ground = snap_to_ground_->isChecked();
	settings.ground_offset = field_value(ground_offset_, current.ground_offset);
	settings.uv_length = field_value(uv_length_, current.uv_length);
	settings.uv_width = field_value(uv_width_, current.uv_width);
	settings.uv_rotation = float(uv_rotation_->value());
	settings.output = RoadOutput(qBound(int(ROAD_OUTPUT_MESH), output_->checkedId(), int(ROAD_OUTPUT_BENT)));
	settings.decal_depth = field_value(decal_depth_, current.decal_depth);
	settings.decal_lift = field_value(decal_lift_, current.decal_lift);
	settings.decal_visible_distance = field_value(decal_visible_distance_, current.decal_visible_distance);
	settings.decal_fade_distance = field_value(decal_fade_distance_, current.decal_fade_distance);
	settings.piece_length = field_value(piece_length_, current.piece_length);
	return settings;
}

void RoadBuilderPage::apply_settings()
{
	if (!updating_)
		builder_->setSettings(read_settings());
}

void RoadBuilderPage::refresh()
{
	const bool has_road = builder_->hasRoad();
	const RoadSettings &settings = builder_->getSettings();
	const bool decal = settings.output == ROAD_OUTPUT_DECAL;

	updating_ = true;
	// A field being typed in is left alone, unless the settings come from elsewhere: an
	// undo, another road. Then it would write the old value back with the next change.
	const bool from_elsewhere = builder_->getSettingsRevision() != shown_revision_;
	shown_revision_ = builder_->getSettingsRevision();
	const auto show_value = [from_elsewhere](QDoubleSpinBox *box, float value) {
		if (!box->hasFocus() || from_elsewhere)
			box->setValue(value);
	};
	show_value(width_, settings.width);
	if (!columns_->hasFocus() || from_elsewhere)
		columns_->setValue(settings.columns);
	show_value(step_, settings.step);
	show_value(max_angle_, settings.max_angle);
	show_value(inner_round_, settings.inner_round);
	show_value(road_roll_, settings.roll);
	show_value(side_offset_, settings.side_offset);
	ground_filter_->setFilter(settings.ground);
	closed_->setChecked(settings.closed);
	snap_to_ground_->setChecked(settings.snap_to_ground);
	refresh_ground_->setEnabled(settings.snap_to_ground);
	show_value(ground_offset_, settings.ground_offset);
	show_value(uv_length_, settings.uv_length);
	show_value(uv_width_, settings.uv_width);
	if (!uv_rotation_->isSliderDown())
		uv_rotation_->setValue(qRound(settings.uv_rotation));
	uv_rotation_value_->setText(QString::fromUtf8("%1°").arg(uv_rotation_->value()));
	if (QAbstractButton *button = output_->button(settings.output))
		button->setChecked(true);
	show_value(decal_depth_, settings.decal_depth);
	show_value(decal_lift_, settings.decal_lift);
	show_value(decal_visible_distance_, settings.decal_visible_distance);
	show_value(decal_fade_distance_, settings.decal_fade_distance);
	show_value(piece_length_, settings.piece_length);
	if (QAbstractButton *button = bend_forward_->button(settings.bend_forward))
		button->setChecked(true);
	if (QAbstractButton *button = bend_up_->button(settings.bend_up))
		button->setChecked(true);
	bend_merge_->setChecked(settings.bend_merge_materials);
	show_value(bend_mesh_angle_, settings.bend_mesh_angle);
	refresh_bend_list();
	add_points_button_->setChecked(builder_->isAddingPoints());
	updating_ = false;

	// A decal has a lift of its own instead of the offset. A bent mesh follows the
	// curve: no grid, no ground, the materials of its asset.
	const bool bent = settings.output == ROAD_OUTPUT_BENT;
	arrange_shape(bent);
	for (QWidget *widget : grid_only_widgets_)
		widget->setVisible(!bent);
	for (QWidget *widget : ground_offset_row_)
		widget->setVisible(settings.snap_to_ground && !decal && !bent);
	for (QWidget *widget : decal_rows_)
		widget->setVisible(decal);
	for (QWidget *widget : material_rows_)
		widget->setVisible(!bent);
	for (QWidget *widget : bend_rows_)
		widget->setVisible(bent);
	bool has_pattern = false;
	for (const RoadBendPart &part : settings.bend_parts)
		has_pattern = has_pattern || part.role == ROAD_BEND_PATTERN;

	const QString material = builder_->getMaterialName();
	material_field_->setValue(material.isEmpty()
		? (decal ? QString::fromUtf8("decal_base") : QString::fromUtf8("mesh_base")) : material, !material.isEmpty());
	material_field_->setShowEnabled(!settings.material.isEmpty());

	folder_field_->setValue(builder_->getFolder(), !settings.folder.isEmpty());
	folder_field_->setShowEnabled(builder_->hasGeneratedFiles());

	if (has_road)
	{
		road_label_->setText(uiText("%1  -  %2 points, %3 units long", "%1  -  точек: %2, длина: %3")
			.arg(builder_->getRoadName()).arg(builder_->getNumPoints()).arg(builder_->getLength(), 0, 'f', 2));
	} else
	{
		road_label_->setText(uiText(
			"No road. Press New Road, or select any node of an existing road.",
			"Дороги нет. Нажмите «Новая дорога» или выделите любую ноду существующей дороги."));
	}
	set_style(new_road_button_, has_road ? QString() : goButtonStyle());
	add_points_button_->setText(builder_->isAddingPoints() ? uiText("Stop Adding Points (Esc)", "Закончить добавление (Esc)")
		: uiText("Add Points", "Добавить точки"));
	add_points_button_->setEnabled(has_road);
	reset_settings_button_->setEnabled(has_road);
	const Unigine::NodePtr parent = builder_->getParentNode();
	parent_label_->setText(parent ? QString::fromUtf8(parent->getName()) : uiText("World root", "Корень мира"));
	parent_clear_button_->setEnabled(bool(parent));
	clone_road_button_->setEnabled(has_road);
	remove_curve_button_->setEnabled(has_road);
	remove_road_button_->setEnabled(has_road);

	const CurvePointsState points = builder_->getSelectedPointsState();
	smooth_button_->setEnabled(points != POINTS_NONE);
	corner_button_->setEnabled(points != POINTS_NONE);
	smooth_button_->setChecked(points == POINTS_SMOOTH);
	corner_button_->setChecked(points == POINTS_CORNER);
	subdivide_button_->setEnabled(builder_->canSubdivide());

	const RoadBuilder::GroundState ground = builder_->getSelectedPointsGround();
	on_ground_button_->setEnabled(ground != RoadBuilder::GROUND_NONE && settings.snap_to_ground && !bent);
	off_ground_button_->setEnabled(ground != RoadBuilder::GROUND_NONE && settings.snap_to_ground && !bent);
	on_ground_button_->setChecked(ground == RoadBuilder::GROUND_ON);

	double roll = 0.0;
	const bool has_roll = builder_->getSelectedPointsRoll(roll);
	roll_->setEnabled(has_roll);
	if (!roll_->hasFocus())
	{
		const bool was_updating = updating_;
		updating_ = true;
		roll_->setValue(has_roll ? roll : 0.0);
		updating_ = was_updating;
	}
	off_ground_button_->setChecked(ground == RoadBuilder::GROUND_OFF);

	double width = 1.0;
	const bool has_width = builder_->getSelectedPointsWidth(width);
	{
		const bool was_updating = updating_;
		updating_ = true;
		point_width_->setEnabled(has_width);
		if (!point_width_->hasFocus())
			point_width_->setValue(width);
		updating_ = was_updating;
	}

	const RoadGrid &grid = builder_->getGrid();
	const RoadBend &bend = builder_->getBend();
	if (!bend.isEmpty())
	{
		stats_label_->setText(uiText("Pattern: %1 meshes, stretched to %2%; %3 triangles, %4 pieces.",
			"Паттерн: мешей - %1, растянуты до %2%; треугольников - %3, кусков - %4.")
			.arg(bend.pattern_copies).arg(bend.pattern_fit * 100.0, 0, 'f', 1).arg(bend.triangles).arg(bend.piece_starts.size()));
	} else if (!grid.isEmpty())
	{
		stats_label_->setText(uiText("Grid: %1 rows along, %2 strips across, %3 triangles, %4 pieces.",
			"Сетка: рядов вдоль - %1, полос поперёк - %2, треугольников - %3, кусков - %4.")
			.arg(grid.stations - 1).arg(grid.columns).arg(grid.numTriangles()).arg(grid.piece_starts.size()));
		if (settings.snap_to_ground)
		{
			stats_label_->setText(stats_label_->text() + QString::fromUtf8("\n") + (builder_->isGroundFromTerrain()
				? uiText("Ground: read from the Landscape Terrain (fast).", "Земля: берётся прямо из Landscape Terrain (быстро).")
				: uiText("Ground: rays onto the surfaces (slower; check only Terrain in Lay On for speed).",
					"Земля: лучи по поверхностям (медленнее; для скорости оставьте в «Класть на» только Terrain).")));
		}
	} else
		stats_label_->setText(QString());
	stats_label_->setVisible(!grid.isEmpty() || !bend.isEmpty());

	generate_button_->setText(uiText("Generate", "Сгенерировать"));
	generate_button_->setEnabled(has_road && (builder_->canGenerate() || bent));

	refresh_roads();

	QString status;
	if (has_road && builder_->getNumPoints() < 2)
		status = uiText("The road needs at least two points.", "Дороге нужно минимум две точки.");
	else if (has_road && bent && !builder_->getBendError().isEmpty())
		status = builder_->getBendError();
	else if (has_road && bent && !has_pattern)
	{
		status = uiText("Add the meshes to bend: select them in the Asset Browser and press Add Selected Assets; at least one of them must be a Pattern.",
			"Добавьте меши для изгиба: выделите их в Asset Browser и нажмите «Добавить выбранные ассеты»; хотя бы один должен быть «Паттерном».");
	}
	else if (builder_->isOutdated())
	{
		status = uiText("The mesh of the road was made for other points or settings: press Generate to update it.",
			"Меш дороги сделан для других точек или настроек: нажмите «Сгенерировать», чтобы обновить его.");
	}
	if (!message_.isEmpty())
		status += (status.isEmpty() ? QString() : QString::fromUtf8("\n")) + message_;
	status_label_->setText(status);
}

void RoadBuilderPage::rescan_roads()
{
	roads_ = builder_->findRoads();

	const QSignalBlocker blocker(roads_list_);
	roads_list_->clear();
	for (const RoadInfo &road : roads_)
	{
		QListWidgetItem *item = new QListWidgetItem(road_item_text(road));
		item->setData(Qt::UserRole, road.id);
		roads_list_->addItem(item);
	}
	if (roads_.isEmpty())
	{
		QListWidgetItem *item = new QListWidgetItem(uiText("No roads in the world.", "В мире нет дорог."));
		item->setFlags(Qt::NoItemFlags);
		roads_list_->addItem(item);
	}

	missing_road_id_ = builder_->getRoadId();
	for (const RoadInfo &road : roads_)
	{
		if (road.id == missing_road_id_)
			missing_road_id_ = 0;
	}

	const int rows = qBound(1, int(roads_.size()), MAX_VISIBLE_ROADS);
	const int row_height = qMax(roads_list_->sizeHintForRow(0), roads_list_->fontMetrics().height() + 2);
	roads_list_->setFixedHeight(rows * row_height + 2 * roads_list_->frameWidth());

	refresh_roads(false);
}

void RoadBuilderPage::refresh_roads(bool rescan_if_missing)
{
	const int current_id = builder_->getRoadId();

	const bool road_gone = shown_road_id_ != 0 && current_id == 0;
	shown_road_id_ = current_id;
	if (road_gone && rescan_if_missing)
	{
		rescan_roads();
		return;
	}

	int row = -1;
	for (int i = 0; i < roads_.size(); ++i)
	{
		if (roads_[i].id == current_id)
			row = i;
	}
	if (current_id != 0 && row < 0 && rescan_if_missing && current_id != missing_road_id_)
	{
		rescan_roads();
		return;
	}

	const QSignalBlocker blocker(roads_list_);
	roads_list_->setCurrentRow(row);
	if (row >= 0)
	{
		roads_[row].name = builder_->getRoadName();
		roads_[row].points = builder_->getNumPoints();
		roads_[row].pieces = builder_->getNumPieces();
		roads_list_->item(row)->setText(road_item_text(roads_[row]));
	}
}

void RoadBuilderPage::browse_folder()
{
	message_.clear();
	QString start = QDir::fromNativeSeparators(QString::fromUtf8(Unigine::Engine::get()->getDataPath()));
	const QString current = start + QLatin1Char('/') + builder_->getFolder();
	if (QDir(current).exists())
		start = current;

	const QString folder = QFileDialog::getExistingDirectory(this,
		uiText("Folder for the meshes of the road", "Папка для мешей дороги"), start);
	if (!folder.isEmpty())
		builder_->setFolderFromDisk(folder);
}

void RoadBuilderPage::generate()
{
	message_.clear();

	const int triangles = qMax(builder_->getGrid().numTriangles(), builder_->getBend().triangles);
	if (triangles > TRIANGLES_WARNING_COUNT)
	{
		const QMessageBox::StandardButton answer = QMessageBox::question(this,
			uiText("Generate", "Сгенерировать"),
			uiText("The road will have %1 triangles. Make the Step or the number of strips larger to have fewer. Continue?",
				"У дороги будет треугольников: %1. Чтобы их было меньше, увеличьте «Шаг вдоль» или уменьшите число полос. Продолжить?")
				.arg(triangles),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
	}

	QApplication::setOverrideCursor(Qt::WaitCursor);
	builder_->generate();
	QApplication::restoreOverrideCursor();
}

} // namespace ArtistTool
