#include "WireBuilderPage.h"
#include "AssetField.h"
#include "PlaceableAsset.h"
#include "SurfaceFilterButton.h"
#include "UiHelpers.h"

#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>

#include <UnigineEngine.h>

#include <QAbstractButton>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QAbstractItemView>
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
#include <QLineEdit>
#include <QListWidget>
#include <QMessageBox>
#include <QPushButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QSpinBox>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>
#include <limits>

using ::UnigineEditor::Selection;

namespace ArtistTool
{

namespace
{

const char SETTINGS_ORGANIZATION[] = "zloy_pingvin";
const char SETTINGS_APPLICATION[] = "artist_tool";
const char SETTINGS_GROUP[] = "wire_builder";

const int BUTTON_HEIGHT = 28;
const QSize BUTTON_ICON_SIZE(16, 16);
const int MAX_VISIBLE_LINES = 4;
// A line of more triangles than this asks before it is generated.
const int TRIANGLES_WARNING_COUNT = 300000;

QIcon editor_icon(const char *path)
{
	return QIcon(QString::fromUtf8(path));
}

QString line_item_text(const WireLineInfo &line)
{
	return uiText("%1  -  %2 poles, %3 pieces", "%1  -  столбов: %2, кусков: %3").arg(line.name).arg(line.poles).arg(line.pieces);
}

QString small_button_style()
{
	return toggleButtonStyle(QString::fromUtf8("#d6d6d6"), QString::fromUtf8("#e4e4e4"), 0)
		+ QString::fromUtf8("QToolButton { padding: 3px 6px; } QToolButton:pressed { background-color: #5c6066; }");
}

QToolButton *make_small_button(const QString &text, const char *icon, const QString &tooltip)
{
	QToolButton *button = new QToolButton;
	button->setText(text);
	button->setIcon(editor_icon(icon));
	button->setIconSize(BUTTON_ICON_SIZE);
	button->setToolButtonStyle(text.isEmpty() ? Qt::ToolButtonIconOnly : Qt::ToolButtonTextBesideIcon);
	button->setFocusPolicy(Qt::NoFocus);
	button->setStyleSheet(small_button_style());
	button->setToolTip(tooltip);
	return button;
}

} // namespace

WireBuilderPage::WireBuilderPage(QWidget *parent)
	: QWidget(parent)
{
	builder_ = new WireBuilder(this);

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
		"Strings poles with wires that hang as real ones do under their weight: power lines, "
		"cables between houses, garlands. The wires go between the attach points of the poles "
		"and follow them when they are moved; Generate saves them to the project.",
		"Натягивает между столбами провода, которые провисают под своим весом, как настоящие: "
		"ЛЭП, кабели между домами, гирлянды. Провода идут между точками крепления столбов и "
		"следуют за ними, когда столбы двигают; «Сгенерировать» сохраняет их в проект."));
	description->setWordWrap(true);
	layout->addWidget(description);

	layout->addWidget(build_line_group());
	layout->addWidget(build_poles_group());
	layout->addWidget(build_sag_group());
	layout->addWidget(build_shape_group());
	layout->addWidget(build_texture_group());
	layout->addWidget(build_output_group());

	// What is drawn over the wires, as in the Road.
	const QString view_tooltip = loc_.tip(
		"What is drawn over the wires in the viewport.\n"
		"Segments - a line along every wire and a short one across it at every joint of its "
		"segments: how smooth the wires are going to be. A yellow plane across the line at a pole - "
		"there the line is cut into pieces (see Piece Size).\n"
		"Nothing - only the wires themselves and the attach points.",
		"Что рисуется поверх проводов во вьюпорте.\n"
		"[[Segments|Сегменты]] - линия вдоль каждого провода и чёрточка поперёк на каждом стыке "
		"сегментов: насколько плавными будут провода. Жёлтая плоскость поперёк линии у столба - "
		"там линия режется на куски (см. [[Piece Size|«Размер куска»]]).\n"
		"[[Nothing|Ничего]] - только сами провода и точки крепления.");
	QHBoxLayout *view_row = new QHBoxLayout;
	QLabel *view_label = new QLabel(uiText("In the viewport", "Во вьюпорте"));
	view_label->setToolTip(view_tooltip);
	view_row->addWidget(view_label);
	view_ = new QComboBox;
	view_->addItem(uiText("Segments", "Сегменты"), int(WIRE_VIEW_SEGMENTS));
	view_->addItem(uiText("Nothing", "Ничего"), int(WIRE_VIEW_NONE));
	view_->setToolTip(view_tooltip);
	view_->setFocusPolicy(Qt::StrongFocus);
	view_->installEventFilter(this);
	view_row->addWidget(view_);
	view_row->addStretch(1);
	connect(view_, &QComboBox::currentIndexChanged, this, [this](int) {
		builder_->setView(WireView(view_->currentData().toInt()));
	});
	layout->addLayout(view_row);

	stats_label_ = new QLabel;
	stats_label_->setWordWrap(true);
	stats_label_->setToolTip(loc_.tip("What the wires are going to be, as shown in the viewport.",
		"Какими получатся провода - как показано во вьюпорте."));
	layout->addWidget(stats_label_);

	generate_button_ = new QPushButton(uiText("Generate", "Сгенерировать"));
	generate_button_->setMinimumHeight(30);
	generate_button_->setIcon(editor_icon(":/images/tools/icon_drop_to_ground.png"));
	generate_button_->setIconSize(BUTTON_ICON_SIZE);
	generate_button_->setStyleSheet(goButtonStyle());
	generate_button_->setToolTip(loc_.tip(
		"Make the wires as they are shown in the viewport and save them to the folder of the "
		"line. The pieces that are in \"mesh\" of the line stay and only get the new geometry - "
		"a material given to them by hand is kept.\n"
		"The line keeps its file names: generating it again overwrites them.\n"
		"Can be undone with Ctrl+Z (the files stay as they were written).",
		"Сделать провода такими, как они показаны во вьюпорте, и сохранить в папку линии. "
		"Куски, что уже лежат в «mesh» линии, остаются и получают только новую геометрию - "
		"материал, назначенный им вручную, сохраняется.\n"
		"Имена файлов у линии постоянные: повторная генерация их перезаписывает.\n"
		"Отменяется через Ctrl+Z (файлы при этом остаются как записаны)."));
	layout->addWidget(generate_button_);
	connect(generate_button_, &QPushButton::clicked, this, [this]() { generate(); });

	status_label_ = new QLabel;
	status_label_->setWordWrap(true);
	layout->addWidget(status_label_);

	QLabel *hotkeys = new QLabel(loc_.hotkeys(
		"LMB On The Ground - Add A Point (and a pole) To The End Of The Line (while Add Points is on)\n"
		"Ctrl + LMB On The Ground - Add A Corner Point (while Add Points is on)\n"
		"LMB On A Point - Select It; Ctrl / Shift + LMB - Add It To The Selection\n"
		"LMB (Hold) On A Point + Mouse Move - Move It, Its Pole Follows\n"
		"Delete - Remove The Selected Point\n"
		"LMB On A Pole - Add It To The End Of The Line (Connect Poles, while Add Poles is on)\n"
		"Esc - Stop Adding (a new line left empty is removed)\n"
		"Ctrl + Z - Undo New Line, Added / Moved Point, Added Pole, Settings, Generate",
		"ЛКМ по земле - добавить точку (и столб) в конец линии (пока включено «[[Add Points|Добавить точки]]»)\n"
		"Ctrl + ЛКМ по земле - добавить угловую точку (пока включено «[[Add Points|Добавить точки]]»)\n"
		"ЛКМ по точке - выделить её; Ctrl / Shift + ЛКМ - добавить к выделению\n"
		"ЛКМ (держать) по точке + движение мыши - двигать её, столб едет следом\n"
		"Delete - удалить выделенную точку\n"
		"ЛКМ по столбу - добавить его в конец линии («[[Connect Poles|Соединить столбы]]», пока включено «[[Add Poles|Добавить столбы]]»)\n"
		"Esc - закончить добавление (новая пустая линия удаляется)\n"
		"Ctrl + Z - отменить создание линии, добавление / сдвиг точки, добавление столба, настройку, генерацию"));
	hotkeys->setWordWrap(true);
	hotkeys->setContentsMargins(6, 2, 6, 6);
	hotkeys_header_ = makeSectionHeader(loc_.hotkeys("Hotkeys", "Горячие клавиши"), hotkeys);
	hotkeys_header_->setToolTip(loc_.tip("Keys and mouse actions of the tool.", "Клавиши и действия мышью для этого инструмента."));
	layout->addWidget(hotkeys_header_);
	layout->addWidget(hotkeys);
	layout->addStretch(1);

	{
		QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
		settings.beginGroup(SETTINGS_GROUP);
		hotkeys_header_->setChecked(settings.value("hotkeys_expanded", true).toBool());
		const int view = view_->findData(settings.value("view", int(WIRE_VIEW_SEGMENTS)).toInt());
		view_->setCurrentIndex(view >= 0 ? view : 0);
		builder_->setView(WireView(view_->currentData().toInt()));
	}

	builder_->on_changed = [this]() { request_refresh(); };
	builder_->on_message = [this](int, const QString &text) {
		message_ = text;
		refresh();
	};
	builder_->on_lines_changed = [this]() { rescan_lines(); };
	connect(Selection::instance(), &Selection::changed, this, [this]() {
		if (isVisible())
			builder_->selectionChanged();
	});

	refresh();
}

WireBuilderPage::~WireBuilderPage()
{
	builder_->on_changed = nullptr;
	builder_->on_message = nullptr;
	builder_->on_lines_changed = nullptr;
	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);
	settings.setValue("hotkeys_expanded", hotkeys_header_->isChecked());
	settings.setValue("view", view_->currentData().toInt());
}

void WireBuilderPage::stop()
{
	builder_->setActive(false);
}

void WireBuilderPage::resetSettings()
{
	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.remove(QString::fromUtf8(SETTINGS_GROUP));
	hotkeys_header_->setChecked(true);
	view_->setCurrentIndex(0);
}

void WireBuilderPage::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	builder_->setActive(true);
	rescan_lines();
}

void WireBuilderPage::hideEvent(QHideEvent *event)
{
	builder_->setActive(false);
	QWidget::hideEvent(event);
}

bool WireBuilderPage::eventFilter(QObject *watched, QEvent *event)
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
	// A click into a number field selects the whole number, so that a new one can be
	// typed right away.
	if (event->type() == QEvent::FocusIn)
	{
		if (QAbstractSpinBox *box = qobject_cast<QAbstractSpinBox *>(watched))
			QTimer::singleShot(0, box, &QAbstractSpinBox::selectAll);
	}
	return QWidget::eventFilter(watched, event);
}

QDoubleSpinBox *WireBuilderPage::make_spin_box(double minimum, double maximum, double step, int decimals,
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
	connect(box, &QDoubleSpinBox::valueChanged, this, [this](double) { apply_settings(); });
	return box;
}

void WireBuilderPage::add_field(QGridLayout *layout, int row, int side, const QString &label_text, QWidget *field,
	const QString &tooltip, const std::function<void()> &reset, QVector<QWidget *> *group)
{
	QLabel *label = new QLabel(label_text);
	label->setToolTip(tooltip);
	if (side == 1)
		label->setContentsMargins(8, 0, 0, 0);
	layout->addWidget(label, row, side * 3);
	layout->addWidget(field, row, side * 3 + 1);
	QToolButton *reset_button = makeResetButton(loc_.tip("Reset to default", "Сбросить на значение по умолчанию"), this, reset);
	layout->addWidget(reset_button, row, side * 3 + 2);
	if (group)
		*group << label << field << reset_button;
}

////////////////////////////////////////////////////////////////////////////////
// The line and its poles.
////////////////////////////////////////////////////////////////////////////////
QWidget *WireBuilderPage::build_line_group()
{
	QGroupBox *group = new QGroupBox(uiText("Line", "Линия"));
	QVBoxLayout *layout = new QVBoxLayout(group);

	line_label_ = new QLabel;
	line_label_->setWordWrap(true);
	line_label_->setToolTip(loc_.tip(
		"The line the tool works with. Select its root or a piece of its mesh in the World "
		"Hierarchy, or click it in the list below, to switch to it.",
		"Линия, с которой работает инструмент. Чтобы переключиться на другую, выделите её "
		"корень или кусок меша в World Hierarchy или кликните по ней в списке ниже."));
	layout->addWidget(line_label_);

	QHBoxLayout *buttons = new QHBoxLayout;
	new_line_button_ = new QPushButton(uiText("New Line", "Новая линия"));
	new_line_button_->setIcon(editor_icon(":/images/icon_add.png"));
	new_line_button_->setIconSize(BUTTON_ICON_SIZE);
	new_line_button_->setMinimumHeight(BUTTON_HEIGHT);
	new_line_button_->setToolTip(loc_.tip(
		"Create a new line and draw it as a curve, the way a road is drawn: every click on the "
		"ground adds a point, a pole of the assets of Poles stands there, the wires go from pole "
		"to pole. Ctrl + click - a corner point. Esc - done.",
		"Создать новую линию и нарисовать её кривой, как дорогу: каждый клик по земле добавляет "
		"точку, в ней встаёт столб из ассетов блока «Столбы», провода идут от столба к столбу. "
		"Ctrl + клик - угловая точка. Esc - готово."));
	buttons->addWidget(new_line_button_);

	new_link_button_ = new QPushButton(uiText("Connect Poles", "Соединить столбы"));
	new_link_button_->setIcon(editor_icon(":/images/icon_stack.png"));
	new_link_button_->setIconSize(BUTTON_ICON_SIZE);
	new_link_button_->setMinimumHeight(BUTTON_HEIGHT);
	new_link_button_->setToolTip(loc_.tip(
		"Create a line between poles already in the world - placed by hand, houses with hooks on "
		"their walls: click them in the viewport one after another. Esc - done.",
		"Создать линию между уже стоящими в мире столбами - расставленными вручную, домами с "
		"крюками на стенах: кликайте по ним во вьюпорте один за другим. Esc - готово."));
	buttons->addWidget(new_link_button_);
	connect(new_link_button_, &QPushButton::clicked, this, [this]() {
		message_.clear();
		WireSettings settings = read_settings();
		settings.closed = false;
		builder_->createLinkLine(settings);
	});

	add_points_button_ = new QPushButton;
	add_points_button_->setCheckable(true);
	add_points_button_->setMinimumHeight(BUTTON_HEIGHT);
	add_points_button_->setIcon(editor_icon(":/images/icons_pencil.png"));
	add_points_button_->setIconSize(BUTTON_ICON_SIZE);
	add_points_button_->setStyleSheet(primaryButtonStyle());
	add_points_button_->setToolTip(loc_.tip(
		"While it is on, every left click on a surface in the viewport adds a point at the end of "
		"the line, a pole stands there; hold the button to drag it. Ctrl + click - a corner point. "
		"Esc turns it off.\n"
		"The points are dragged with the mouse (the manipulator of the editor is hidden meanwhile); "
		"to put a point between two others, use Subdivide.",
		"Пока включено, каждый клик левой кнопкой по поверхности во вьюпорте добавляет точку в "
		"конец линии, в ней встаёт столб; не отпуская кнопку, её можно тащить. Ctrl + клик - "
		"угловая точка. Esc выключает.\n"
		"Точки двигаются мышью (гизмо редактора на это время прячется); чтобы поставить точку "
		"между двумя другими - [[Subdivide|«Разделить»]]."));
	buttons->addWidget(add_points_button_, 1);
	connect(add_points_button_, &QPushButton::toggled, this, [this](bool checked) {
		if (updating_)
			return;
		message_.clear();
		builder_->setAddingPoints(checked);
	});

	add_poles_button_ = new QPushButton;
	add_poles_button_->setCheckable(true);
	add_poles_button_->setMinimumHeight(BUTTON_HEIGHT);
	add_poles_button_->setIcon(editor_icon(":/images/icons_pencil.png"));
	add_poles_button_->setIconSize(BUTTON_ICON_SIZE);
	add_poles_button_->setStyleSheet(primaryButtonStyle());
	add_poles_button_->setToolTip(loc_.tip(
		"While it is on, every node you select in the viewport or in the World Hierarchy is added "
		"to the end of the line as a pole - just click the poles one after another. Esc turns "
		"it off.\n"
		"A pole is a node with the attach points inside it (see Attach Points), a hook on a wall "
		"too; a node with none is not taken. To just hang a wire anywhere, draw a line as a curve.",
		"Пока включено, каждая нода, выделенная во вьюпорте или в World Hierarchy, добавляется "
		"в конец линии как столб - просто кликайте по столбам один за другим. Esc выключает.\n"
		"Столб - нода с точками крепления внутри (см. [[Attach Points|«Точки крепления»]]), крюк на "
		"стене тоже; нода без них не берётся. Чтобы просто повесить провод где угодно - нарисуйте "
		"линию кривой."));
	buttons->addWidget(add_poles_button_, 1);
	layout->addLayout(buttons);

	// The selected points of a curve, as in the Road.
	const QString point_tooltip = loc_.tip(
		"What the selected points of the line are.\n"
		"Smooth - the line bends through the point without a break.\n"
		"Corner - a sharp bend; the pole there gets the Corner asset of Poles, if there is one.\n"
		"Click a point to select it, Ctrl + click - another one too.",
		"Какие выделенные точки линии.\n"
		"[[Smooth|Плавная]] - линия проходит через точку плавно, без излома.\n"
		"[[Corner|Угол]] - резкий излом; столб там получает «Угловой» ассет из «Столбов», если он есть.\n"
		"Клик по точке выделяет её, Ctrl + клик - добавляет ещё одну.");
	// The rows of the selected points, in a frame of their own, as in the Road.
	point_row_ = new QGroupBox(uiText("Point", "Точка"));
	// The buttons on the left; the fields of the span in two rows on the right, one under
	// the other.
	QGridLayout *point_grid = new QGridLayout(point_row_);
	point_grid->setHorizontalSpacing(3);
	point_grid->setVerticalSpacing(3);
	QHBoxLayout *point_row = new QHBoxLayout;
	point_row->setSpacing(3);
	point_grid->addLayout(point_row, 0, 0);
	point_grid->setColumnStretch(1, 1);
	QLabel *point_label = new QLabel(uiText("Selected point", "Выделенная точка"));
	point_label->setToolTip(point_tooltip);
	point_row->addWidget(point_label);
	point_row->addSpacing(6);
	smooth_button_ = make_small_button(uiText("Smooth", "Плавная"), ":/images/icon_stroke_default.png", point_tooltip);
	corner_button_ = make_small_button(uiText("Corner", "Угол"), ":/images/icon_curvature.png", point_tooltip);
	for (QToolButton *button : {smooth_button_, corner_button_})
	{
		button->setIcon(makeToggleIcon(button == smooth_button_ ? QString::fromUtf8(":/images/icon_stroke_default.png")
			: QString::fromUtf8(":/images/icon_curvature.png")));
		button->setCheckable(true);
		point_row->addWidget(button);
	}
	subdivide_button_ = make_small_button(uiText("Subdivide", "Разделить"), ":/sandworm/images/icon_point.png", loc_.tip(
		"Put a new point in the middle of the line between two neighbouring points. Select both of "
		"them first: click one, then Ctrl + click the other.",
		"Поставить новую точку посередине линии между двумя соседними точками. Сначала выделите "
		"обе: клик по одной, затем Ctrl + клик по другой."));
	point_row->addWidget(subdivide_button_);

	// The sag and the random of the span from the selected points to the next ones.
	const auto make_point_field = [this](double minimum, double maximum, double step, int decimals, const QString &tooltip) {
		QDoubleSpinBox *box = new QDoubleSpinBox;
		box->setRange(minimum, maximum);
		box->setSingleStep(step);
		box->setDecimals(decimals);
		box->setToolTip(tooltip);
		box->setFocusPolicy(Qt::StrongFocus);
		box->installEventFilter(this);
		enableSpinBoxDrag(box);
		return box;
	};
	const QString point_sag_tooltip = loc_.tip(
		"The sag of the span from the selected point to the next one, a multiplier of the Sag of the "
		"line: 1 - as the line, 2 - twice as low, 0 - pulled tight.",
		"Провисание пролёта от выделенной точки до следующей, множитель [[Sag|«Провисания»]] линии: "
		"1 - как у линии, 2 - вдвое ниже, 0 - натянут.");
	// A field of the span: its caption, the field and its reset button, in the row and the
	// column (0 - the left pair, 1 - the right one) of the grid.
	const auto add_point_field = [point_grid](int row, int side, const QString &caption, const QString &tooltip, QDoubleSpinBox *box,
		QWidget *reset) {
		QLabel *label = new QLabel(caption);
		label->setToolTip(tooltip);
		label->setContentsMargins(8, 0, 0, 0);
		point_grid->addWidget(label, row, 2 + side * 3);
		point_grid->addWidget(box, row, 3 + side * 3);
		point_grid->addWidget(reset, row, 4 + side * 3);
	};
	point_sag_ = make_point_field(0.0, 1000.0, 0.1, 2, point_sag_tooltip);
	add_point_field(0, 0, uiText("Sag ×", "Провисание ×"), point_sag_tooltip, point_sag_,
		makeResetButton(loc_.tip("As the line (1)", "Как у линии (1)"), this, [this]() {
			if (point_sag_->isEnabled())
				builder_->setSelectedPointsSagScale(1.0);
		}));
	connect(point_sag_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
		if (!updating_)
			builder_->setSelectedPointsSagScale(value);
	});

	const QString point_random_tooltip = loc_.tip(
		"The Random of the span from the selected point to the next one, in percent. Line - that of "
		"the line.",
		"[[Random|«Разброс»]] пролёта от выделенной точки до следующей, в процентах. «Линии» - как у "
		"линии.");
	point_random_ = make_point_field(-1.0, 1000.0, 1.0, 1, point_random_tooltip);
	point_random_->setSpecialValueText(uiText("Line", "Линии"));
	point_random_->setSuffix(QString::fromUtf8(" %"));
	add_point_field(0, 1, uiText("Random", "Разброс"), point_random_tooltip, point_random_,
		makeResetButton(loc_.tip("As the line", "Как у линии"), this, [this]() {
			if (point_random_->isEnabled())
				builder_->setSelectedPointsSagRandom(-1.0);
		}));
	connect(point_random_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
		if (!updating_)
			builder_->setSelectedPointsSagRandom(value);
	});

	// The shape of the span, under its sag.
	const QString point_thickness_tooltip = loc_.tip(
		"The thickness of the span from the selected point to the next one, a multiplier of the "
		"Thickness of the line: 1 - as the line, 2 - twice as thick. A mesh gets thicker across the "
		"wire, its copies keep their length.",
		"Толщина пролёта от выделенной точки до следующей, множитель [[Thickness|«Толщины»]] линии: "
		"1 - как у линии, 2 - вдвое толще. Меш утолщается поперёк провода, длина копий та же.");
	point_thickness_ = make_point_field(0.0, 1000.0, 0.1, 2, point_thickness_tooltip);
	add_point_field(1, 0, uiText("Thickness ×", "Толщина ×"), point_thickness_tooltip, point_thickness_,
		makeResetButton(loc_.tip("As the line (1)", "Как у линии (1)"), this, [this]() {
			if (point_thickness_->isEnabled())
				builder_->setSelectedPointsThickness(1.0);
		}));
	connect(point_thickness_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
		if (!updating_)
			builder_->setSelectedPointsThickness(value);
	});

	const QString point_twist_tooltip = loc_.tip(
		"Turns of the twist the span from the selected point to the next one gets on top of the "
		"Twist of the line, counted by its mode. 0 - as the line; negative - less, or the other way.",
		"Обороты закрутки, которые пролёт от выделенной точки до следующей получает сверх "
		"[[Twist|«Закрутки»]] линии, по её режиму. 0 - как у линии; минус - меньше или в другую сторону.");
	point_twist_ = make_point_field(-1000.0, 1000.0, 0.1, 2, point_twist_tooltip);
	add_point_field(1, 1, uiText("Twist +", "Закрутка +"), point_twist_tooltip, point_twist_,
		makeResetButton(loc_.tip("As the line (0)", "Как у линии (0)"), this, [this]() {
			if (point_twist_->isEnabled())
				builder_->setSelectedPointsTwist(0.0);
		}));
	connect(point_twist_, &QDoubleSpinBox::valueChanged, this, [this](double value) {
		if (!updating_)
			builder_->setSelectedPointsTwist(value);
	});
	layout->addWidget(point_row_);
	connect(smooth_button_, &QToolButton::clicked, this, [this]() { builder_->setSelectedPointsCorner(false); });
	connect(corner_button_, &QToolButton::clicked, this, [this]() { builder_->setSelectedPointsCorner(true); });
	connect(subdivide_button_, &QToolButton::clicked, this, [this]() {
		message_.clear();
		builder_->subdivide();
	});

	link_row_ = new QWidget;
	QHBoxLayout *pole_row = new QHBoxLayout(link_row_);
	pole_row->setContentsMargins(0, 0, 0, 0);
	pole_row->setSpacing(3);
	add_selected_button_ = make_small_button(uiText("Add Selected", "Добавить выделенные"), ":/images/icon_stack.png", loc_.tip(
		"Add the nodes selected now to the end of the line, in the order they were selected.",
		"Добавить выделенные сейчас ноды в конец линии, в том порядке, в каком их выделяли."));
	remove_last_button_ = make_small_button(uiText("Remove Last", "Убрать последний"), ":/images/icon_delete_cross.png", loc_.tip(
		"Remove the last pole from the line. The pole itself stays in the world.",
		"Убрать последний столб из линии. Сам столб остаётся в мире."));
	clear_poles_button_ = make_small_button(uiText("Clear", "Очистить"), ":/images/icon_delete_trash.png", loc_.tip(
		"Remove all the poles from the line. The poles themselves stay in the world.",
		"Убрать из линии все столбы. Сами столбы остаются в мире."));
	pole_row->addWidget(add_selected_button_);
	pole_row->addWidget(remove_last_button_);
	pole_row->addWidget(clear_poles_button_);
	pole_row->addStretch(1);
	layout->addWidget(link_row_);

	// The attach points and Closed.
	const QString attach_tooltip = loc_.tip(
		"The name of the attach points inside the poles, before their number: with \"wire\" the "
		"points are the nodes named wire_1, wire_2... (also wire1, Wire.01). Put dummies of such "
		"names into the asset of the pole where the wires are fixed - on its insulators.\n"
		"The wire of number N goes from the point N of one pole to the point N of the next one. "
		"A pole with no such points is one point itself: all the wires go to it.\n"
		"The points are shown in the viewport as yellow dots.",
		"Имя точек крепления внутри столбов, перед номером: при «wire» точки - это ноды с "
		"именами wire_1, wire_2... (а также wire1, Wire.01). Поставьте пустышки (dummy) с такими "
		"именами в ассет столба там, где крепятся провода, - на изоляторах.\n"
		"Провод номер N идёт от точки N одного столба к точке N следующего. Столб без таких "
		"точек - сам одна точка: к нему приходят все провода.\n"
		"Во вьюпорте точки показаны жёлтыми точками.");
	QHBoxLayout *attach_row = new QHBoxLayout;
	attach_row->setSpacing(3);
	QLabel *attach_label = new QLabel(uiText("Attach Points", "Точки крепления"));
	attach_label->setToolTip(attach_tooltip);
	attach_row->addWidget(attach_label);
	attach_row->addSpacing(6);
	attach_name_ = new QLineEdit;
	attach_name_->setToolTip(attach_tooltip);
	attach_name_->setMaximumWidth(120);
	attach_row->addWidget(attach_name_);
	QLabel *attach_suffix = new QLabel(QString::fromUtf8("_1, _2..."));
	attach_suffix->setEnabled(false);
	attach_row->addWidget(attach_suffix);
	QToolButton *attach_reset = makeResetButton(loc_.tip("Reset to default", "Сбросить на значение по умолчанию"), this, [this]() {
		attach_name_->setText(WireSettings().attach_name);
		apply_settings();
	});
	attach_row->addWidget(attach_reset);
	attach_widgets_ << attach_label << attach_name_ << attach_suffix << attach_reset;
	attach_row->addStretch(1);
	layout->addLayout(attach_row);

	// Closed, and all the settings of the line back to their defaults, as in the Road.
	QHBoxLayout *closed_row = new QHBoxLayout;
	closed_ = new QCheckBox(uiText("Closed", "Замкнутая"));
	closed_->setToolTip(loc_.tip(
		"On: the last pole is joined to the first one, the line is a ring (needs at least three "
		"poles).",
		"Вкл: последний столб соединяется с первым, линия замыкается в кольцо (нужно минимум три "
		"столба)."));
	closed_row->addWidget(closed_, 1);
	reset_settings_button_ = new QPushButton(uiText("Reset Line Settings", "Сбросить настройки линии"));
	reset_settings_button_->setIcon(makeResetIcon());
	reset_settings_button_->setIconSize(BUTTON_ICON_SIZE);
	reset_settings_button_->setToolTip(loc_.tip(
		"Put the settings of the current line back to their defaults. What its wires and poles are "
		"made of, its attach points, its folder and Closed stay.",
		"Вернуть настройки текущей линии к значениям по умолчанию. Из чего сделаны провода и "
		"столбы, точки крепления, папка и [[Closed|«Замкнутая»]] остаются."));
	closed_row->addWidget(reset_settings_button_);
	closed_row->addWidget(makeResetButton(loc_.tip("Reset to default", "Сбросить на значение по умолчанию"), this,
		[this]() { closed_->setChecked(WireSettings().closed); }));
	layout->addLayout(closed_row);
	connect(reset_settings_button_, &QPushButton::clicked, this, [this]() {
		const QMessageBox::StandardButton answer = QMessageBox::question(this,
			uiText("Reset Line Settings", "Сбросить настройки линии"),
			uiText("Put the settings of the line \"%1\" back to their defaults?",
				"Вернуть настройки линии «%1» к значениям по умолчанию?").arg(builder_->getLineName()),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
		message_.clear();
		builder_->resetSettings();
	});
	connect(attach_name_, &QLineEdit::editingFinished, this, [this]() { apply_settings(); });
	connect(closed_, &QCheckBox::toggled, this, [this](bool) { apply_settings(); });

	// The lines of the world.
	const QString lines_tooltip = loc_.tip(
		"All the lines of wires in the world. Click one to work with it: it becomes the current "
		"line and its root node is selected in the editor.",
		"Все линии проводов в мире. Кликните по линии, чтобы работать с ней: она становится "
		"текущей, а её корневая нода выделяется в редакторе.");
	QHBoxLayout *lines_header = new QHBoxLayout;
	lines_header->setSpacing(3);
	QLabel *lines_label = new QLabel(uiText("Lines in the world", "Линии в мире"));
	lines_label->setToolTip(lines_tooltip);
	lines_header->addWidget(lines_label, 1);
	QToolButton *rescan_button = make_small_button(uiText("Find Lines", "Найти линии"), ":/images/icon_go_to_asset.png",
		loc_.tip("Look through the world for lines again.", "Заново найти линии в мире."));
	lines_header->addWidget(rescan_button);
	connect(rescan_button, &QToolButton::clicked, this, [this]() { rescan_lines(); });
	clone_line_button_ = make_small_button(QString(), ":/images/icon_clone.png", loc_.tip(
		"Clone the current line: a new line with the same points and settings, no mesh yet.",
		"Клонировать текущую линию: новая линия с теми же точками и настройками, меша пока нет."));
	lines_header->addWidget(clone_line_button_);
	connect(clone_line_button_, &QToolButton::clicked, this, [this]() {
		message_.clear();
		builder_->cloneLine();
	});
	remove_curve_button_ = make_small_button(QString(), ":/images/icon_delete_cross.png", loc_.tip(
		"Remove the curve of the current line and keep its poles and its generated mesh as ordinary "
		"nodes, in a node of the name of the line. Ctrl+Z brings it back.",
		"Удалить кривую текущей линии, оставив столбы и сгенерированный меш обычными нодами в ноде "
		"с именем линии. Ctrl+Z вернёт её."));
	lines_header->addWidget(remove_curve_button_);
	connect(remove_curve_button_, &QToolButton::clicked, this, [this]() {
		message_.clear();
		builder_->removeLineCurve();
	});
	remove_line_button_ = make_small_button(QString(), ":/images/icon_delete_trash.png", loc_.tip(
		"Remove the current line completely: its poles, its generated mesh and its mesh files.",
		"Удалить текущую линию полностью: столбы, сгенерированный меш и его файлы."));
	lines_header->addWidget(remove_line_button_);
	connect(remove_line_button_, &QToolButton::clicked, this, [this]() {
		const QMessageBox::StandardButton answer = QMessageBox::question(this, uiText("Remove Line", "Удалить линию"),
			uiText("Remove the line \"%1\" with its poles and mesh and delete its mesh files? Ctrl+Z brings back the nodes, not the files.",
				"Удалить линию «%1» вместе со столбами и мешем и стереть файлы её мешей? Ctrl+Z вернёт ноды, но не файлы.")
				.arg(builder_->getLineName()),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
		message_.clear();
		builder_->removeLine(true);
	});
	layout->addLayout(lines_header);

	lines_list_ = new QListWidget;
	lines_list_->setToolTip(lines_tooltip);
	lines_list_->setAlternatingRowColors(true);
	lines_list_->setUniformItemSizes(true);
	lines_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	lines_list_->setStyleSheet(QString::fromUtf8(
		"QListWidget { background-color: #26282b; alternate-background-color: #303338; }"
		"QListWidget::item { padding: 1px 4px; }"
		"QListWidget::item:selected { background-color: #2f6fd6; color: white; }"));
	layout->addWidget(lines_list_);
	connect(lines_list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
		message_.clear();
		if (!builder_->selectLine(item->data(Qt::UserRole).toInt()))
			rescan_lines();
	});

	// Where the new lines go, as in the Road.
	const QString parent_tooltip = loc_.tip(
		"New lines (New Line, Connect Poles, Clone) become children of this node. World root - the "
		"end of the World Hierarchy; a clone then goes next to its line.",
		"Новые линии («Новая линия», «Соединить столбы», клон) становятся детьми этой ноды. "
		"[[World root|Корень мира]] - конец World Hierarchy; клон тогда кладётся рядом со своей линией.");
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
	QToolButton *parent_select = new QToolButton;
	parent_select->setText(uiText("Use selected", "Взять выделенную"));
	parent_select->setIcon(editor_icon(":/images/icon_stack.png"));
	parent_select->setIconSize(BUTTON_ICON_SIZE);
	parent_select->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	parent_select->setFocusPolicy(Qt::NoFocus);
	parent_select->setStyleSheet(choiceButtonStyle());
	parent_select->setToolTip(loc_.tip("Make the node selected in the World Hierarchy the parent of the new lines.",
		"Сделать ноду, выделенную в World Hierarchy, родителем новых линий."));
	parent_row->addWidget(parent_select);
	parent_clear_button_ = new QToolButton;
	parent_clear_button_->setText(uiText("Clear", "Сбросить"));
	parent_clear_button_->setIcon(editor_icon(":/images/icon_delete_trash.png"));
	parent_clear_button_->setIconSize(BUTTON_ICON_SIZE);
	parent_clear_button_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	parent_clear_button_->setFocusPolicy(Qt::NoFocus);
	parent_clear_button_->setStyleSheet(choiceButtonStyle());
	parent_clear_button_->setToolTip(loc_.tip("Put the new lines into the world root again.", "Снова класть новые линии в корень мира."));
	parent_row->addWidget(parent_clear_button_);
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

	connect(new_line_button_, &QPushButton::clicked, this, [this]() {
		message_.clear();
		// A new line starts open, with the settings shown on the page.
		WireSettings settings = read_settings();
		settings.closed = false;
		builder_->createLine(settings);
	});
	connect(add_poles_button_, &QPushButton::toggled, this, [this](bool checked) {
		if (updating_)
			return;
		message_.clear();
		builder_->setAddingPoles(checked);
	});
	connect(add_selected_button_, &QToolButton::clicked, this, [this]() {
		message_.clear();
		builder_->addSelectedPoles();
	});
	connect(remove_last_button_, &QToolButton::clicked, this, [this]() { builder_->removeLastPole(); });
	connect(clear_poles_button_, &QToolButton::clicked, this, [this]() { builder_->clearPoles(); });
	return group;
}

////////////////////////////////////////////////////////////////////////////////
// The poles of a line drawn as a curve.
////////////////////////////////////////////////////////////////////////////////
QWidget *WireBuilderPage::build_poles_group()
{
	const WireSettings defaults;
	QGroupBox *group = new QGroupBox(uiText("Poles", "Столбы"));
	poles_group_ = group;
	QVBoxLayout *layout = new QVBoxLayout(group);

	pole_empty_label_ = new QLabel(uiText(
		"No assets: the wires hang from the points of the curve themselves - a garland. Select the "
		"assets of the poles in the Asset Browser and press Add Selected Assets.",
		"Ассетов нет: провода висят прямо от точек кривой - как гирлянда. Выделите ассеты столбов в "
		"Asset Browser и нажмите «Добавить выбранные ассеты»."));
	pole_empty_label_->setWordWrap(true);
	pole_empty_label_->setEnabled(false);
	layout->addWidget(pole_empty_label_);
	QWidget *list_widget = new QWidget;
	pole_list_ = new QGridLayout(list_widget);
	pole_list_->setContentsMargins(0, 0, 0, 0);
	pole_list_->setHorizontalSpacing(3);
	pole_list_->setVerticalSpacing(2);
	pole_list_->setColumnStretch(0, 1);
	layout->addWidget(list_widget);

	QHBoxLayout *buttons = new QHBoxLayout;
	QPushButton *add_button = new QPushButton(uiText("Add Selected Assets", "Добавить выбранные ассеты"));
	add_button->setIcon(editor_icon(":/images/mods/icon_object_mode.png"));
	add_button->setIconSize(BUTTON_ICON_SIZE);
	add_button->setMinimumHeight(BUTTON_HEIGHT);
	add_button->setToolTip(loc_.tip(
		"Add the assets selected in the Asset Browser to the list: .node, .mesh or imported models. "
		"Put dummies named wire_1, wire_2... into the asset where the wires are fixed (see Attach "
		"Points). Pick the role of every asset in its row: Pattern - the poles along the line, "
		"several such assets take turns; Start / End - the first / the last pole; Corner - the "
		"poles at the corner points.",
		"Добавить ассеты, выделенные в Asset Browser, в список: .node, .mesh или импортированные "
		"модели. В ассет столба поставьте пустышки wire_1, wire_2... там, где крепятся провода "
		"(см. [[Attach Points|«Точки крепления»]]). Роль каждого ассета выберите в его строке: "
		"«Паттерн» - столбы вдоль линии, несколько таких чередуются; «Начало» / «Конец» - первый / "
		"последний столб; «Угловой» - столбы на угловых точках."));
	buttons->addWidget(add_button, 1);
	pole_clear_button_ = new QPushButton(uiText("Clear", "Очистить"));
	pole_clear_button_->setIcon(editor_icon(":/images/icon_delete_trash.png"));
	pole_clear_button_->setIconSize(BUTTON_ICON_SIZE);
	pole_clear_button_->setMinimumHeight(BUTTON_HEIGHT);
	pole_clear_button_->setToolTip(loc_.tip("Remove all the assets from the list; the poles are removed too.",
		"Убрать все ассеты из списка; столбы тоже убираются."));
	buttons->addWidget(pole_clear_button_);
	layout->addLayout(buttons);
	connect(add_button, &QPushButton::clicked, this, [this]() {
		message_.clear();
		builder_->addSelectedPoleParts();
	});
	connect(pole_clear_button_, &QPushButton::clicked, this, [this]() {
		message_.clear();
		builder_->clearPoleParts();
	});

	QGridLayout *grid = new QGridLayout;
	grid->setColumnStretch(1, 1);
	grid->setColumnStretch(4, 1);
	// Where the poles stand goes first, above the list of their assets; its columns are
	// those of the grid under the list.
	QGridLayout *top = new QGridLayout;
	top->setColumnStretch(1, 1);
	top->setColumnStretch(4, 1);
	QVector<QLabel *> first_column;
	// The axes of the pole assets, as in the Road and the Path Placer.
	const QString turn_note = loc_.tip(
		"\nA pole can be turned and tilted by hand, as a point of a road: select its point of the "
		"curve and rotate it with the manipulator about any axis - a leaning or a broken pole. It "
		"keeps that rotation, relative to the line, when the curve changes.",
		"\nСтолб можно довернуть и наклонить вручную, как точку дороги: выделите его точку кривой и "
		"поверните гизмо вокруг любой оси - покосившийся, сломанный столб. Он сохранит этот "
		"поворот относительно линии, когда кривая меняется.");
	const QString forward_tooltip = loc_.tip(
		"Which axis of the pole assets goes along the line; at a corner - halfway between its two "
		"directions.",
		"Какая ось ассетов столбов идёт вдоль линии; на угловой точке - посередине между двумя "
		"направлениями.") + turn_note;
	const QString up_tooltip = loc_.tip(
		"Which axis of the pole assets looks up. It can not be the same axis as the forward one: "
		"picking it switches the other.",
		"Какая ось ассетов столбов смотрит вверх. Не может совпадать с [[Forward axis|«Осью вперёд»]]: "
		"при таком выборе вторая ось переключится сама.") + turn_note;
	const auto add_axis_row = [&](int row, const QString &text, const QString &tooltip, QButtonGroup *&group,
		const std::function<void()> &reset) {
		QLabel *label = new QLabel(text);
		label->setToolTip(tooltip);
		grid->addWidget(label, row, 0);
		first_column.append(label);
		QHBoxLayout *buttons_row = new QHBoxLayout;
		buttons_row->setSpacing(3);
		group = makeAxisButtons(buttons_row, this, tooltip);
		buttons_row->addStretch(1);
		grid->addLayout(buttons_row, row, 1, 1, 4);
		QToolButton *reset_button = makeResetButton(loc_.tip("Reset to default", "Сбросить на значение по умолчанию"), this, reset);
		grid->addWidget(reset_button, row, 5);
		pole_asset_widgets_ << label << reset_button;
		for (QAbstractButton *button : group->buttons())
			pole_asset_widgets_ << button;
	};
	add_axis_row(0, uiText("Forward axis", "Ось вперёд"), forward_tooltip, pole_forward_, [this, defaults]() {
		WireSettings settings = builder_->getSettings();
		settings.pole_forward = defaults.pole_forward;
		if (int(settings.pole_up) / 2 == int(settings.pole_forward) / 2)
			settings.pole_up = defaults.pole_up;
		builder_->setSettings(settings);
	});
	add_axis_row(1, uiText("Up axis", "Ось вверх"), up_tooltip, pole_up_, [this, defaults]() {
		WireSettings settings = builder_->getSettings();
		settings.pole_up = defaults.pole_up;
		if (int(settings.pole_up) / 2 == int(settings.pole_forward) / 2)
			settings.pole_forward = defaults.pole_forward;
		builder_->setSettings(settings);
	});
	connect(pole_forward_, &QButtonGroup::idClicked, this, [this](int) { apply_pole_axes(true); });
	connect(pole_up_, &QButtonGroup::idClicked, this, [this](int) { apply_pole_axes(false); });

	reset_turns_button_ = new QPushButton(uiText("Reset Turns", "Сбросить повороты"));
	reset_turns_button_->setIcon(makeResetIcon());
	reset_turns_button_->setIconSize(BUTTON_ICON_SIZE);
	reset_turns_button_->setToolTip(loc_.tip(
		"The points turned or tilted by hand with the manipulator, and their poles, stand straight "
		"along the line again.",
		"Точки, повёрнутые или наклонённые вручную гизмо, и их столбы снова встают ровно вдоль линии."));
	connect(reset_turns_button_, &QPushButton::clicked, this, [this]() { builder_->resetPoleTurns(); });
	pole_asset_widgets_ << reset_turns_button_;

	// Where the poles stand: at the points only, or also between them every so many units.
	const QString spacing_tooltip = loc_.tip(
		"At Points - a pole at every point of the curve.\n"
		"Every - also between the points, about every this many units, evenly along each span: "
		"put points only where the line turns.",
		"[[At Points|В точках]] - столб в каждой точке кривой.\n"
		"[[Every|Каждые]] - ещё и между точками, примерно через столько юнитов, равномерно по "
		"пролёту: точки ставьте только там, где линия поворачивает.");
	QLabel *spacing_label = new QLabel(uiText("Poles", "Столбы"));
	spacing_label->setToolTip(spacing_tooltip);
	top->addWidget(spacing_label, 0, 0);
	first_column.append(spacing_label);
	QHBoxLayout *spacing_row = new QHBoxLayout;
	spacing_row->setSpacing(3);
	pole_every_ = new QButtonGroup(this);
	const QString spacing_names[] = {uiText("At Points", "В точках"), uiText("Every", "Каждые")};
	for (int id = 0; id < 2; ++id)
	{
		QToolButton *button = new QToolButton;
		button->setText(spacing_names[id]);
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(spacing_tooltip);
		button->setStyleSheet(choiceButtonStyle());
		pole_every_->addButton(button, id);
		spacing_row->addWidget(button);
	}
	pole_spacing_ = make_spin_box(0.1, 100000.0, 1.0, 1, spacing_tooltip);
	spacing_row->addWidget(pole_spacing_, 1);
	top->addLayout(spacing_row, 0, 1, 1, 4);
	top->addWidget(makeResetButton(loc_.tip("Reset to default", "Сбросить на значение по умолчанию"), this, [this, defaults]() {
		if (QAbstractButton *button = pole_every_->button(defaults.pole_every ? 1 : 0))
			button->setChecked(true);
		pole_spacing_->setValue(defaults.pole_spacing);
		apply_settings();
	}), 0, 5);
	layout->insertLayout(0, top);
	connect(pole_every_, &QButtonGroup::idClicked, this, [this](int) { apply_settings(); });
	pole_on_ground_ = new QCheckBox(uiText("On Ground", "На землю"));
	pole_on_ground_->setToolTip(loc_.tip(
		"On: every pole stands on the surface under its place on the curve (the poles between the "
		"points too). Off: at the height of the curve.",
		"Вкл: каждый столб встаёт на поверхность под своим местом на кривой (и столбы между "
		"точками тоже). Выкл: на высоте кривой."));
	connect(pole_on_ground_, &QCheckBox::toggled, this, [this](bool) { apply_settings(); });
	grid->addWidget(pole_on_ground_, 2, 0, 1, 3);
	grid->addWidget(reset_turns_button_, 2, 3, 1, 3, Qt::AlignRight);

	// As in the Road: what is left out is looked through.
	SurfaceFilterButton::Texts filter_texts;
	filter_texts.tooltip = loc_.tip(
		"What the points of the line and its poles are put on. The unchecked kinds are looked "
		"through: with only Terrain checked the poles stand on the terrain under the rocks, and a "
		"click on a rock puts a point onto the terrain under it.",
		"На что ставятся точки линии и столбы. Неотмеченные типы пропускаются насквозь: если "
		"отмечен только Terrain, столбы встают на террейн под камнями, а клик по камню ставит "
		"точку на террейн под ним.");
	filter_texts.only_immovable = uiText("Only On Immovable Nodes", "Только на ноды с флагом Immovable");
	filter_texts.only_immovable_tooltip = loc_.tip(
		"Put the points and the poles only on nodes with the Immovable flag.",
		"Ставить точки и столбы только на ноды с флагом Immovable.");
	filter_texts.only_intersection = uiText("Only On Surfaces With Intersection", "Только на поверхности с Intersection");
	filter_texts.only_intersection_tooltip = loc_.tip(
		"Put the points and the poles only on surfaces with the Intersection flag and a bit of the "
		"Intersection Mask.",
		"Ставить точки и столбы только на поверхности с флагом Intersection и хотя бы одним битом "
		"Intersection Mask.");
	filter_texts.reset_tooltip = loc_.tip("Reset to default", "Сбросить на значение по умолчанию");
	ground_filter_ = new SurfaceFilterButton(filter_texts);
	QLabel *filter_label = new QLabel(uiText("Lay On", "Класть на"));
	filter_label->setToolTip(filter_texts.tooltip);
	grid->addWidget(filter_label, 3, 0);
	first_column.append(filter_label);
	grid->addWidget(ground_filter_, 3, 1, 1, 5);
	ground_filter_->on_changed = [this]() { apply_settings(); };
	layout->addLayout(grid);
	// The fields of the two grids start at the same place.
	int label_width = 0;
	for (QLabel *label : first_column)
		label_width = qMax(label_width, label->sizeHint().width());
	top->setColumnMinimumWidth(0, label_width);
	grid->setColumnMinimumWidth(0, label_width);
	return group;
}

void WireBuilderPage::refresh_pole_list()
{
	const QVector<WirePolePart> &parts = builder_->getSettings().pole_parts;
	pole_empty_label_->setVisible(parts.isEmpty());
	pole_clear_button_->setEnabled(!parts.isEmpty());

	// The rows are made anew only when the list itself changes.
	QString signature;
	for (const WirePolePart &part : parts)
		signature += part.source + QLatin1Char('|');
	if (signature != pole_signature_)
	{
		pole_signature_ = signature;
		pole_role_boxes_.clear();
		while (QLayoutItem *item = pole_list_->takeAt(0))
		{
			if (QWidget *widget = item->widget())
				widget->deleteLater();
			delete item;
		}
		const QString role_tooltip = loc_.tip(
			"Which poles of the line this asset is for: Pattern - along the line, in turn with the "
			"other pattern assets; Start / End - the first / the last pole; Corner - the poles at the "
			"corner points. A role no asset has is taken by the pattern.\n"
			"Insulator - not a pole: it is put on every attach point of every pole, turned as the "
			"point is. The wire is tied to the node inside it named as the attach points (wire, "
			"wire_1); none - to the insulator itself. Several insulators - one of them at random "
			"by Seed. Only insulators in the list - they stand at the points of the curve, as poles.",
			"Для каких столбов линии этот ассет: «Паттерн» - вдоль линии, по очереди с другими "
			"ассетами паттерна; «Начало» / «Конец» - первый / последний столб; «Угловой» - столбы на "
			"угловых точках. Роль, для которой ассета нет, берёт паттерн.\n"
			"«Изолятор» - не столб: ставится на каждую точку крепления каждого столба, повёрнутый "
			"как эта точка. Провод крепится к ноде внутри него с именем точек крепления (wire, "
			"wire_1); нет такой - к самому изолятору. Несколько изоляторов - на каждую точку "
			"случайный по [[Seed|«Сиду»]]. В списке только изоляторы - они сами стоят на точках "
			"кривой, как столбы.");
		for (int k = 0; k < parts.size(); ++k)
		{
			// The asset field of the editor: another pole is picked in the dialog or dropped
			// onto it.
			AssetField::Options name_options;
			name_options.tooltip = parts[k].source_name;
			name_options.dialog_title = uiText("Asset of the pole", "Ассет столба");
			name_options.icon = QString::fromUtf8(":/images/mods/icon_object_mode.png");
			name_options.show_tooltip = loc_.tip("Show the asset in the Asset Browser.", "Показать ассет в Asset Browser.");
			AssetField *name = new AssetField(name_options);
			name->setValue(parts[k].source_name, true);
			name->setIcon(kindIcon(PlaceableAsset::Kind(parts[k].source_kind)));
			name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
			pole_list_->addWidget(name, k, 0);
			// Later: the row goes away with the list.
			name->on_picked = [this, k](const Unigine::Vector<Unigine::UGUID> &guids) {
				QTimer::singleShot(0, this, [this, k, guids]() { builder_->replacePolePart(k, guids); });
			};
			name->on_show = [this, k]() { builder_->showPolePart(k); };
			QComboBox *role = new QComboBox;
			role->addItem(uiText("Pattern", "Паттерн"), int(WIRE_POLE_PATTERN));
			role->addItem(uiText("Start", "Начало"), int(WIRE_POLE_START));
			role->addItem(uiText("End", "Конец"), int(WIRE_POLE_END));
			role->addItem(uiText("Corner", "Угловой"), int(WIRE_POLE_CORNER));
			role->addItem(uiText("Insulator", "Изолятор"), int(WIRE_POLE_INSULATOR));
			// The list opens wider than the box: "Insulator" is not cut.
			role->view()->setMinimumWidth(role->view()->sizeHintForColumn(0) + 32);
			role->setToolTip(role_tooltip);
			role->setFocusPolicy(Qt::StrongFocus);
			role->installEventFilter(this);
			pole_list_->addWidget(role, k, 1);
			connect(role, &QComboBox::currentIndexChanged, this, [this, role, k](int) {
				if (!updating_)
					builder_->setPolePartRole(k, WirePoleRole(role->currentData().toInt()));
			});
			pole_role_boxes_.append(role);
			QToolButton *remove = new QToolButton;
			remove->setText(QString::fromUtf8("✕"));
			remove->setFixedSize(20, 22);
			remove->setAutoRaise(true);
			remove->setFocusPolicy(Qt::NoFocus);
			remove->setToolTip(loc_.tip("Remove the asset from the list.", "Убрать ассет из списка."));
			pole_list_->addWidget(remove, k, 2);
			// Later: the row it is in goes away with the list.
			connect(remove, &QToolButton::clicked, this, [this, k]() {
				QTimer::singleShot(0, this, [this, k]() { builder_->removePolePart(k); });
			});
		}
	}
	for (int k = 0; k < parts.size() && k < pole_role_boxes_.size(); ++k)
	{
		const int index = pole_role_boxes_[k]->findData(int(parts[k].role));
		if (pole_role_boxes_[k]->currentIndex() != index)
			pole_role_boxes_[k]->setCurrentIndex(index);
	}
}

////////////////////////////////////////////////////////////////////////////////
// The sag.
////////////////////////////////////////////////////////////////////////////////
QWidget *WireBuilderPage::build_sag_group()
{
	const WireSettings defaults;
	QGroupBox *group = new QGroupBox(uiText("Sag", "Провисание"));
	QGridLayout *layout = new QGridLayout(group);
	layout->setColumnStretch(1, 1);
	layout->setColumnStretch(4, 1);

	const QString mode_tooltip = loc_.tip(
		"How Sag is set. It is how far the wire hangs below the straight line between its ends, "
		"at the deepest place.\n"
		"Like Real Wires - Sag is that of a wire 10 units long; a twice longer wire hangs four "
		"times as low, as real wires pulled with the same force do.\n"
		"Percent Of Length - Sag is a part of the length of the wire: a twice longer one hangs "
		"twice as low.\n"
		"Units - every wire hangs Sag units low, whatever its length.",
		"Как задаётся [[Sag|«Провисание»]]. Это насколько провод опускается ниже прямой между "
		"его концами в самом глубоком месте.\n"
		"[[Like Real Wires|Как у настоящих]] - провисание провода длиной 10 юнитов; вдвое длинный "
		"провод провисает вчетверо ниже, как настоящие провода, натянутые с одной силой.\n"
		"[[Percent Of Length|Процент длины]] - часть длины провода: вдвое длинный провисает вдвое ниже.\n"
		"[[Units|Юниты]] - каждый провод провисает на столько юнитов, какой бы длины он ни был.");
	sag_mode_ = new QComboBox;
	sag_mode_->addItem(uiText("Units", "Юниты"), int(WIRE_SAG_UNITS));
	sag_mode_->addItem(uiText("Percent Of Length", "Процент длины"), int(WIRE_SAG_PERCENT));
	sag_mode_->addItem(uiText("Like Real Wires", "Как у настоящих"), int(WIRE_SAG_TENSION));
	sag_mode_->setToolTip(mode_tooltip);
	sag_mode_->setFocusPolicy(Qt::StrongFocus);
	sag_mode_->installEventFilter(this);
	connect(sag_mode_, &QComboBox::currentIndexChanged, this, [this](int) { apply_settings(); });
	add_field(layout, 0, 0, uiText("Mode", "Режим"), sag_mode_, mode_tooltip, [this, defaults]() {
		sag_mode_->setCurrentIndex(sag_mode_->findData(int(defaults.sag_mode)));
	});

	const QString sag_tooltip = loc_.tip(
		"How low the wires hang, by Mode: in units, in percent of the length of a wire, or the "
		"sag of a wire 10 units long. 0 - pulled tight, straight.",
		"Насколько низко провисают провода, по [[Mode|«Режиму»]]: в юнитах, в процентах длины "
		"провода или провисание провода длиной 10 юнитов. 0 - натянут, прямой.");
	sag_ = make_spin_box(0.0, 10000.0, 0.05, 3, sag_tooltip);
	add_field(layout, 1, 0, uiText("Sag", "Провисание"), sag_, sag_tooltip, [this, defaults]() { sag_->setValue(defaults.sag); });

	const QString random_tooltip = loc_.tip(
		"How much more than Sag a wire may hang, in percent of Sag: every wire gets its own sag "
		"from Sag itself to Sag plus this part - real wires never hang all the same. It only adds: "
		"none of the wires hangs higher than Sag. Every wire keeps its own while the poles are "
		"moved; Seed gives others.",
		"Насколько сильнее [[Sag|«Провисания»]] может провиснуть провод, в процентах от него: каждый "
		"провод получает своё провисание - от самого «Провисания» до «Провисания» плюс эта часть, - "
		"настоящие провода никогда не висят одинаково. Только добавляет: ни один провод не висит "
		"выше «Провисания». Каждый провод сохраняет своё, пока столбы двигают; [[Seed|«Сид»]] даёт другие.");
	sag_random_ = make_spin_box(0.0, 1000.0, 1.0, 1, random_tooltip);
	sag_random_->setSuffix(QString::fromUtf8(" %"));
	add_field(layout, 3, 0, uiText("Random", "Разброс"), sag_random_, random_tooltip,
		[this, defaults]() { sag_random_->setValue(defaults.sag_random); });

	const QString angle_tooltip = loc_.tip(
		"How the wires are cut into segments: a wire turns by at most this many degrees from one "
		"segment to the next. A wire pulled tight gets few segments, a hanging one more; along a "
		"wire they are long at the poles, where it is nearly straight, and short at its bottom, "
		"where it bends the most. Smaller - smoother and more triangles.",
		"Как провода режутся на сегменты: от сегмента к сегменту провод поворачивает не больше "
		"чем на столько градусов. Натянутый провод получает мало сегментов, провисший - больше; "
		"вдоль провода они длинные у столбов, где он почти прямой, и короткие внизу, где он "
		"изгибается сильнее всего. Меньше - плавнее и больше треугольников.");
	segment_angle_ = make_spin_box(0.1, 90.0, 0.5, 1, angle_tooltip);
	segment_angle_->setSuffix(QString::fromUtf8("°"));
	add_field(layout, 0, 1, uiText("Bend Angle", "Угол изгиба"), segment_angle_, angle_tooltip,
		[this, defaults]() { segment_angle_->setValue(defaults.segment_angle); });

	const QString min_tooltip = loc_.tip(
		"The fewest segments a wire has, however straight it is: short or tight wires do not get "
		"too angular.",
		"Наименьшее число сегментов у провода, каким бы прямым он ни был: короткие и натянутые "
		"провода не становятся слишком угловатыми.");
	min_segments_ = new QSpinBox;
	min_segments_->setRange(1, 256);
	min_segments_->setToolTip(min_tooltip);
	min_segments_->setFocusPolicy(Qt::StrongFocus);
	min_segments_->installEventFilter(this);
	enableSpinBoxDrag(min_segments_);
	connect(min_segments_, &QSpinBox::valueChanged, this, [this](int) { apply_settings(); });
	add_field(layout, 1, 1, uiText("Min Segments", "Мин. сегментов"), min_segments_, min_tooltip,
		[this, defaults]() { min_segments_->setValue(defaults.min_segments); });

	const QString max_tooltip = loc_.tip(
		"The longest a segment may be, in units: a longer one is cut into equal ones. Needed only "
		"for a twist or for the vertex animation of the material - they need vertices on the "
		"straight parts of a wire too. Off - no limit, the segments follow the bend alone.",
		"Самая большая длина сегмента, в юнитах: более длинный режется на равные. Нужна только "
		"для [[Twist|«Закрутки»]] и вертексной анимации в материале - им нужны вершины и на прямых "
		"участках провода. Выкл - без ограничения, сегменты задаются только изгибом.");
	max_segment_length_ = make_spin_box(0.0, 10000.0, 0.5, 2, max_tooltip);
	max_segment_length_->setSpecialValueText(uiText("Off", "Выкл"));
	add_field(layout, 2, 1, uiText("Max Length", "Макс. длина"), max_segment_length_, max_tooltip,
		[this, defaults]() { max_segment_length_->setValue(defaults.max_segment_length); });

	above_ground_ = new QCheckBox(uiText("Above Ground", "Не ниже земли"));
	above_ground_->setToolTip(loc_.tip(
		"On: where a wire would hang below the ground, it lies on it instead, as a cable does. The "
		"ground is what Lay On of Poles lets through.",
		"Вкл: где провод провис бы ниже земли, он ложится на неё, как кабель. Земля - то, что "
		"пропускает [[Lay On|«Класть на»]] из «Столбов»."));
	layout->addWidget(above_ground_, 3, 3, 1, 3);
	connect(above_ground_, &QCheckBox::toggled, this, [this](bool) { apply_settings(); });

	const QString seed_tooltip = loc_.tip(
		"Another number - other random sags and turns of the wires.",
		"Другое число - другие случайные провисания и повороты проводов.");
	seed_ = new QSpinBox;
	seed_->setRange(0, std::numeric_limits<int>::max());
	seed_->setToolTip(seed_tooltip);
	seed_->setFocusPolicy(Qt::StrongFocus);
	seed_->installEventFilter(this);
	enableSpinBoxDrag(seed_);
	connect(seed_, &QSpinBox::valueChanged, this, [this](int) { apply_settings(); });
	// The field and a dice that puts a random number into it, as the seeds of the editor
	// have (Cluster Paint).
	QWidget *seed_field = new QWidget;
	QHBoxLayout *seed_layout = new QHBoxLayout(seed_field);
	seed_layout->setContentsMargins(0, 0, 0, 0);
	seed_layout->setSpacing(3);
	seed_layout->addWidget(seed_, 1);
	QToolButton *seed_random = new QToolButton;
	seed_random->setFixedSize(20, 20);
	seed_random->setIcon(editor_icon(":/images/icon_dice.png"));
	seed_random->setAutoRaise(true);
	seed_random->setFocusPolicy(Qt::NoFocus);
	seed_random->setToolTip(loc_.tip("Generate a random seed.", "Сгенерировать случайный сид."));
	seed_layout->addWidget(seed_random);
	connect(seed_random, &QToolButton::clicked, this, [this]() {
		seed_->setValue(int(QRandomGenerator::global()->bounded(std::numeric_limits<int>::max())));
	});
	add_field(layout, 2, 0, uiText("Seed", "Сид"), seed_field, seed_tooltip, [this, defaults]() { seed_->setValue(defaults.seed); });
	return group;
}

////////////////////////////////////////////////////////////////////////////////
// What the wires are made of.
////////////////////////////////////////////////////////////////////////////////
QWidget *WireBuilderPage::build_shape_group()
{
	const WireSettings defaults;
	QGroupBox *group = new QGroupBox(uiText("Shape", "Форма"));
	QGridLayout *layout = new QGridLayout(group);
	layout->setColumnStretch(1, 1);
	layout->setColumnStretch(4, 1);
	int row = 0;

	const QString shape_tooltip = loc_.tip(
		"What the wires are made of.\n"
		"Cross - two strips crossed at a right angle along the wire, for a material with alpha "
		"(a wire drawn in the texture): light and looks like a round wire from any side.\n"
		"Ribbon - one strip along the wire: a band, a ribbon of a garland; twist it with Twist.\n"
		"Tube - a round tube of real thickness, for thick cables seen up close.\n"
		"Mesh - a mesh of the project laid along every wire again and again, every copy bent to "
		"follow it: a twisted cable, a chain; it keeps its materials.",
		"Из чего сделаны провода.\n"
		"[[Cross|Крестовина]] - две полоски крест-накрест вдоль провода, для материала с альфой "
		"(провод нарисован в текстуре): лёгкая и с любой стороны выглядит как круглый провод.\n"
		"[[Ribbon|Лента]] - одна полоска вдоль провода: лента, ленточка гирлянды; закрутите её "
		"[[Twist|«Закруткой»]].\n"
		"[[Tube|Трубка]] - круглая трубка настоящей толщины, для толстых кабелей вблизи.\n"
		"[[Mesh|Меш]] - меш проекта раскладывается вдоль каждого провода раз за разом, каждая "
		"копия изгибается по нему: витой кабель, цепь; материалы у него свои.");
	QLabel *shape_label = new QLabel(uiText("Make Of", "Сделать из"));
	shape_label->setToolTip(shape_tooltip);
	layout->addWidget(shape_label, row, 0);
	QHBoxLayout *shape_row = new QHBoxLayout;
	shape_row->setSpacing(3);
	shape_ = new QButtonGroup(this);
	const QString names[] = {uiText("Cross", "Крестовина"), uiText("Ribbon", "Лента"), uiText("Tube", "Трубка"),
		uiText("Mesh", "Меш")};
	const char *const icons[] = {":/images/icon_delete_cross.png", ":/images/icon_stroke_default.png",
		":/images/icon_replace_color.png", ":/sandworm/images/icon_extent.png"};
	for (int id = WIRE_SHAPE_CROSS; id <= WIRE_SHAPE_MESH; ++id)
	{
		QToolButton *button = new QToolButton;
		button->setText(names[id]);
		button->setIcon(editor_icon(icons[id]));
		button->setIconSize(BUTTON_ICON_SIZE);
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(shape_tooltip);
		button->setStyleSheet(choiceButtonStyle());
		shape_->addButton(button, id);
		shape_row->addWidget(button);
	}
	shape_row->addStretch(1);
	layout->addLayout(shape_row, row, 1, 1, 5);
	connect(shape_, &QButtonGroup::idClicked, this, [this](int) { apply_settings(); });
	++row;

	const QString thickness_tooltip = loc_.tip(
		"The width of a strip of a cross or a ribbon, the diameter of a tube, in units. For a "
		"cross with alpha make it a little wider than the wire drawn in the texture.",
		"Ширина полоски крестовины или ленты, диаметр трубки, в юнитах. Для крестовины с альфой "
		"делайте её чуть шире провода, нарисованного в текстуре.");
	thickness_ = make_spin_box(0.0001, 100.0, 0.005, 4, thickness_tooltip);
	add_field(layout, row, 0, uiText("Thickness", "Толщина"), thickness_, thickness_tooltip,
		[this, defaults]() { thickness_->setValue(defaults.thickness); }, &strip_widgets_);

	const QString sides_tooltip = loc_.tip("How many sides the tube has around.", "Сколько граней у трубки по кругу.");
	sides_ = new QSpinBox;
	sides_->setRange(3, 64);
	sides_->setToolTip(sides_tooltip);
	sides_->setFocusPolicy(Qt::StrongFocus);
	sides_->installEventFilter(this);
	enableSpinBoxDrag(sides_);
	connect(sides_, &QSpinBox::valueChanged, this, [this](int) { apply_settings(); });
	add_field(layout, row, 1, uiText("Sides", "Граней"), sides_, sides_tooltip, [this, defaults]() { sides_->setValue(defaults.sides); },
		&tube_widgets_);
	++row;

	const QString rotation_tooltip = loc_.tip(
		"How the shape is turned about the wire at its start, in degrees. For a cross 45° often "
		"looks better: neither of its strips lies flat. Random - every wire gets a turn of its own.",
		"Насколько форма повёрнута вокруг провода в его начале, в градусах. Для крестовины часто "
		"лучше 45°: ни одна полоска не лежит плашмя. [[Random|«Случайно»]] - каждому проводу свой поворот.");
	rotation_ = make_spin_box(-360.0, 360.0, 5.0, 1, rotation_tooltip);
	rotation_->setSuffix(QString::fromUtf8("°"));
	add_field(layout, row, 0, uiText("Rotation", "Поворот"), rotation_, rotation_tooltip,
		[this, defaults]() { rotation_->setValue(defaults.rotation); });
	random_rotation_ = new QCheckBox(uiText("Random", "Случайно"));
	random_rotation_->setToolTip(rotation_tooltip);
	connect(random_rotation_, &QCheckBox::toggled, this, [this](bool) { apply_settings(); });
	layout->addWidget(random_rotation_, row, 3, 1, 3);
	++row;

	const QString twist_tooltip = loc_.tip(
		"How many turns the shape winds about the wire: a twisted ribbon, a twisted cable. The "
		"turns are counted by the mode next to it. Negative - the other way. 0 - no twist.\n"
		"A mesh is cut more where it twists, by Mesh Angle.",
		"Сколько оборотов форма делает вокруг провода: перекрученная лента, витой кабель. Как "
		"считаются обороты - по режиму рядом. Минус - в другую сторону. 0 - без закрутки.\n"
		"Меш режется чаще там, где закручивается, по [[Mesh Angle|«Углу меша»]].");
	const QString twist_mode_tooltip = loc_.tip(
		"What Twist counts.\n"
		"Turns Per Span - turns from one pole to the next, whatever the length: every wire "
		"twists the same number of times.\n"
		"Turns Per Unit - turns per unit of length: a longer wire twists more times.",
		"Что считает [[Twist|«Закрутка»]].\n"
		"[[Turns Per Span|Оборотов на пролёт]] - оборотов от столба до столба, какой бы длины "
		"пролёт ни был: каждый провод закручен одинаковое число раз.\n"
		"[[Turns Per Unit|Оборотов на юнит]] - оборотов на юнит длины: длинный провод "
		"закручен больше раз.");
	twist_ = make_spin_box(-1000.0, 1000.0, 0.1, 2, twist_tooltip);
	add_field(layout, row, 0, uiText("Twist", "Закрутка"), twist_, twist_tooltip, [this, defaults]() { twist_->setValue(defaults.twist); });
	twist_mode_ = new QComboBox;
	twist_mode_->addItem(uiText("Turns Per Span", "Оборотов на пролёт"), int(WIRE_TWIST_PER_SPAN));
	twist_mode_->addItem(uiText("Turns Per Unit", "Оборотов на юнит"), int(WIRE_TWIST_PER_UNIT));
	twist_mode_->setToolTip(twist_mode_tooltip);
	twist_mode_->setFocusPolicy(Qt::StrongFocus);
	twist_mode_->installEventFilter(this);
	connect(twist_mode_, &QComboBox::currentIndexChanged, this, [this](int) { apply_settings(); });
	layout->addWidget(twist_mode_, row, 3, 1, 3);
	++row;

	const QString twist_step_tooltip = loc_.tip(
		"A strip or a tube turns by the twist at its joints only. A segment longer than this many "
		"degrees of the twist gets joints in between, so the twist is smooth. Larger - fewer "
		"triangles, the twist goes in steps. Off - the joints of the bend only.",
		"Лента или трубка поворачивается закруткой только в узлах. Сегмент, на котором закрутка "
		"больше этого угла, получает промежуточные узлы, и закрутка идёт плавно. Больше - меньше "
		"треугольников, закрутка ступеньками. «Выкл» - только узлы изгиба.");
	twist_step_ = make_spin_box(0.0, 180.0, 5.0, 1, twist_step_tooltip);
	twist_step_->setSuffix(QString::fromUtf8("°"));
	twist_step_->setSpecialValueText(uiText("Off", "Выкл"));
	add_field(layout, row, 0, uiText("Twist Step", "Шаг закрутки"), twist_step_, twist_step_tooltip,
		[this, defaults]() { twist_step_->setValue(defaults.twist_step); }, &twist_step_widgets_);
	++row;

	// The material of the strips and the tubes, a field as the editor's own.
	const QString material_tooltip = loc_.tip(
		"The material of the wires: pick it with the folder button or drag it here from the Asset "
		"Browser. For a cross or a ribbon take one with alpha test, two sided.\n"
		"Vertex colors: R and B - sway (0 at the attach points, 1 in the middle), G - a random "
		"number of the wire.",
		"Материал проводов: выберите кнопкой с папкой или перетащите сюда из Asset Browser. Для "
		"крестовины и ленты - с alpha test, двусторонний.\n"
		"Цвета вершин: R и B - качание (0 у точек крепления, 1 посередине), G - случайное число "
		"провода.");
	QLabel *material_title = new QLabel(uiText("Material", "Материал"));
	material_title->setToolTip(material_tooltip);
	layout->addWidget(material_title, row, 0);
	AssetField::Options material_options;
	material_options.tooltip = material_tooltip;
	material_options.dialog_title = uiText("Material of the wires", "Материал проводов");
	material_options.filter = QString::fromUtf8(".mat");
	material_options.icon = QString::fromUtf8(":/images/assets/file_material.png");
	material_options.reset_tooltip = loc_.tip("Back to the base material.", "Вернуть базовый материал.");
	material_options.show_tooltip = loc_.tip("Show the material in the Asset Browser.", "Показать материал в Asset Browser.");
	material_options.take_text = uiText("Use Selected", "Взять выделенный");
	material_options.take_tooltip = loc_.tip("Take the material selected in the Asset Browser.",
		"Взять материал, выделенный в Asset Browser.");
	material_field_ = new AssetField(material_options);
	layout->addWidget(material_field_, row, 1, 1, 5);
	strip_widgets_ << material_title << material_field_;
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

	// The mesh laid along the wires.
	const QString mesh_tooltip = loc_.tip(
		"The mesh laid along every wire: pick a .mesh, a .node or a model with the folder button or "
		"drag it here from the Asset Browser. It is repeated along the wire and bent to follow it.",
		"Меш, который раскладывается вдоль каждого провода: выберите .mesh, .node или модель кнопкой "
		"с папкой или перетащите сюда из Asset Browser. Он повторяется вдоль провода и изгибается по нему.");
	QLabel *mesh_title = new QLabel(uiText("Mesh", "Меш"));
	mesh_title->setToolTip(mesh_tooltip);
	layout->addWidget(mesh_title, row, 0);
	AssetField::Options mesh_options;
	mesh_options.tooltip = mesh_tooltip;
	mesh_options.dialog_title = uiText("Mesh laid along the wires", "Меш вдоль проводов");
	mesh_options.icon = QString::fromUtf8(":/images/mods/icon_object_mode.png");
	mesh_options.show_tooltip = loc_.tip("Show the mesh in the Asset Browser.", "Показать меш в Asset Browser.");
	mesh_options.take_text = uiText("Use Selected", "Взять выделенный");
	mesh_options.take_tooltip = loc_.tip("Take the mesh, node or model selected in the Asset Browser.",
		"Взять меш, ноду или модель, выделенную в Asset Browser.");
	mesh_field_ = new AssetField(mesh_options);
	layout->addWidget(mesh_field_, row, 1, 1, 5);
	mesh_widgets_ << mesh_title << mesh_field_;
	mesh_field_->on_picked = [this](const Unigine::Vector<Unigine::UGUID> &guids) {
		message_.clear();
		builder_->takeMesh(guids);
	};
	mesh_field_->on_show = [this]() {
		message_ = builder_->showMesh() ? QString() : uiText("The mesh is not found in the project.", "Меш не найден в проекте.");
		refresh();
	};
	++row;

	// The axes of the mesh, as in the Road.
	const auto add_axis_row = [&](const QString &text, const QString &tooltip, QButtonGroup *&axis_group,
		const std::function<void()> &reset) {
		QLabel *label = new QLabel(text);
		label->setToolTip(tooltip);
		layout->addWidget(label, row, 0);
		QWidget *buttons_widget = new QWidget;
		QHBoxLayout *buttons = new QHBoxLayout(buttons_widget);
		buttons->setContentsMargins(0, 0, 0, 0);
		buttons->setSpacing(3);
		axis_group = makeAxisButtons(buttons, this, tooltip);
		buttons->addStretch(1);
		layout->addWidget(buttons_widget, row, 1, 1, 4);
		QToolButton *reset_button = makeResetButton(loc_.tip("Reset to default", "Сбросить на значение по умолчанию"), this, reset);
		layout->addWidget(reset_button, row, 5);
		mesh_widgets_ << label << buttons_widget << reset_button;
		++row;
	};
	add_axis_row(uiText("Forward axis", "Ось вперёд"), loc_.tip("Which axis of the mesh goes along the wire.",
		"Какая ось меша идёт вдоль провода."), mesh_forward_, [this, defaults]() {
		WireSettings settings = builder_->getSettings();
		settings.mesh_forward = defaults.mesh_forward;
		if (int(settings.mesh_up) / 2 == int(settings.mesh_forward) / 2)
			settings.mesh_up = defaults.mesh_up;
		builder_->setSettings(settings);
	});
	add_axis_row(uiText("Up axis", "Ось вверх"), loc_.tip(
		"Which axis of the mesh looks up (before the twist). It can not be the same axis as the "
		"forward one: picking it switches the other.",
		"Какая ось меша смотрит вверх (до закрутки). Не может совпадать с [[Forward axis|«Осью вперёд»]]: "
		"при таком выборе вторая ось переключится сама."), mesh_up_, [this, defaults]() {
		WireSettings settings = builder_->getSettings();
		settings.mesh_up = defaults.mesh_up;
		if (int(settings.mesh_up) / 2 == int(settings.mesh_forward) / 2)
			settings.mesh_forward = defaults.mesh_forward;
		builder_->setSettings(settings);
	});
	connect(mesh_forward_, &QButtonGroup::idClicked, this, [this](int) { apply_mesh_axes(true); });
	connect(mesh_up_, &QButtonGroup::idClicked, this, [this](int) { apply_mesh_axes(false); });

	const QString scale_tooltip = loc_.tip(
		"The scale of the mesh, all its axes: its thickness and the length of one copy along the wire.",
		"Масштаб меша по всем осям: его толщина и длина одной копии вдоль провода.");
	mesh_scale_ = make_spin_box(0.001, 1000.0, 0.05, 3, scale_tooltip);
	add_field(layout, row, 0, uiText("Scale", "Масштаб"), mesh_scale_, scale_tooltip,
		[this, defaults]() { mesh_scale_->setValue(defaults.mesh_scale); }, &mesh_widgets_);
	const QString angle_tooltip = loc_.tip(
		"The mesh gets more segments only where the wire bends or twists it by more than this "
		"angle across one segment: it bends and twists smoothly, and the straight parts get no more "
		"triangles. Off - as it is.",
		"Меш получает дополнительные сегменты только там, где провод гнёт или закручивает его "
		"больше чем на этот угол в пределах одного сегмента: он гнётся и закручивается плавно, а "
		"прямые участки лишних треугольников не получают. «Выкл» - как есть.");
	mesh_angle_ = make_spin_box(0.0, 90.0, 1.0, 1, angle_tooltip);
	mesh_angle_->setSuffix(QString::fromUtf8("°"));
	mesh_angle_->setSpecialValueText(uiText("Off", "Выкл"));
	add_field(layout, row, 1, uiText("Mesh Angle", "Угол меша"), mesh_angle_, angle_tooltip,
		[this, defaults]() { mesh_angle_->setValue(defaults.mesh_angle); }, &mesh_widgets_);
	++row;
	return group;
}

void WireBuilderPage::apply_pole_axes(bool forward_picked)
{
	if (updating_)
		return;
	WireSettings settings = builder_->getSettings();
	const AxisDirection old_forward = settings.pole_forward;
	const AxisDirection old_up = settings.pole_up;
	settings.pole_forward = AxisDirection(qMax(0, pole_forward_->checkedId()));
	settings.pole_up = AxisDirection(qMax(0, pole_up_->checkedId()));
	if (int(settings.pole_forward) / 2 == int(settings.pole_up) / 2)
	{
		if (forward_picked)
			settings.pole_up = old_forward;
		else
			settings.pole_forward = old_up;
	}
	builder_->setSettings(settings);
}

void WireBuilderPage::apply_mesh_axes(bool forward_picked)
{
	if (updating_)
		return;
	WireSettings settings = builder_->getSettings();
	const AxisDirection old_forward = settings.mesh_forward;
	const AxisDirection old_up = settings.mesh_up;
	settings.mesh_forward = AxisDirection(qMax(0, mesh_forward_->checkedId()));
	settings.mesh_up = AxisDirection(qMax(0, mesh_up_->checkedId()));
	if (int(settings.mesh_forward) / 2 == int(settings.mesh_up) / 2)
	{
		if (forward_picked)
			settings.mesh_up = old_forward;
		else
			settings.mesh_forward = old_up;
	}
	builder_->setSettings(settings);
}

////////////////////////////////////////////////////////////////////////////////
// The texture of the strips and the tubes.
////////////////////////////////////////////////////////////////////////////////
QWidget *WireBuilderPage::build_texture_group()
{
	const WireSettings defaults;
	const QString reset_tooltip = loc_.tip("Reset to default", "Сбросить на значение по умолчанию");

	// As in the Road: the fields on the left, the rotation dial right after them.
	QGroupBox *group = new QGroupBox(uiText("Texture", "Текстура"));
	QHBoxLayout *group_layout = new QHBoxLayout(group);
	QGridLayout *layout = new QGridLayout;
	group_layout->addLayout(layout);

	const QString length_tooltip = loc_.tip(
		"The length of the wire one repeat of the texture takes, in units.",
		"Какую длину провода занимает один повтор текстуры, в юнитах.");
	QLabel *length_label = new QLabel(uiText("Texture Length", "Длина текстуры"));
	length_label->setToolTip(length_tooltip);
	layout->addWidget(length_label, 0, 0);
	uv_length_ = make_spin_box(0.001, 10000.0, 0.1, 3, length_tooltip);
	layout->addWidget(uv_length_, 0, 1);
	layout->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() { uv_length_->setValue(defaults.uv_length); }), 0, 2);

	const QString width_tooltip = loc_.tip(
		"How many times the texture repeats across a strip, around a tube.",
		"Сколько раз текстура повторяется поперёк полоски, вокруг трубки.");
	QLabel *width_label = new QLabel(uiText("Repeats Across", "Повторов поперёк"));
	width_label->setToolTip(width_tooltip);
	layout->addWidget(width_label, 1, 0);
	uv_width_ = make_spin_box(-1000.0, 1000.0, 0.1, 3, width_tooltip);
	layout->addWidget(uv_width_, 1, 1);
	layout->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() { uv_width_->setValue(defaults.uv_width); }), 1, 2);

	const QString rotation_tooltip = loc_.tip(
		"How the texture is turned on the wire, in degrees. 0 - the wire goes along V (up the "
		"texture); 90 - along U (a wire drawn from left to right). The dial sticks to 0, 45, 90, 135 "
		"and 180 degrees.",
		"Поворот текстуры на проводе, в градусах. 0 - провод идёт вдоль V (вверх по текстуре); 90 - "
		"вдоль U (провод нарисован слева направо). Диск залипает на 0, 45, 90, 135 и 180 градусах.");
	uv_rotation_ = makeAngleDial(-180, 180);
	uv_rotation_->setWrapping(true);
	uv_rotation_->setFixedSize(56, 56);
	uv_rotation_->setToolTip(rotation_tooltip);
	uv_rotation_->setFocusPolicy(Qt::StrongFocus);
	uv_rotation_->installEventFilter(this);
	uv_rotation_value_ = new QLabel;
	uv_rotation_value_->setToolTip(rotation_tooltip);
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
	connect(uv_rotation_, &QDial::valueChanged, this, [this](int angle) {
		uv_rotation_value_->setText(QString::fromUtf8("%1°").arg(angle));
		apply_settings();
	});

	// A mesh has the textures of its asset.
	strip_widgets_ << group;
	return group;
}

////////////////////////////////////////////////////////////////////////////////
// Where the result goes.
////////////////////////////////////////////////////////////////////////////////
QWidget *WireBuilderPage::build_output_group()
{
	const WireSettings defaults;
	QGroupBox *group = new QGroupBox(uiText("Result", "Результат"));
	QGridLayout *layout = new QGridLayout(group);
	layout->setColumnStretch(1, 1);
	layout->setColumnStretch(4, 1);

	const QString piece_tooltip = loc_.tip(
		"The wires are cut into pieces by squares of the world grid of this size, in units: every "
		"piece is a node and a mesh of its own, with the wires whose middle is in its square - "
		"what is far away is not drawn. A line is cut at the poles, shown by yellow planes in the "
		"viewport. 0 - the whole line is one piece.",
		"Провода режутся на куски по квадратам сетки мира такого размера, в юнитах: каждый кусок - "
		"своя нода и свой меш с проводами, середина которых в его квадрате, - то, что далеко, не "
		"рисуется. Линия режется у столбов - во вьюпорте там жёлтые плоскости. 0 - вся линия одним куском.");
	piece_size_ = make_spin_box(0.0, 100000.0, 10.0, 1, piece_tooltip);
	add_field(layout, 0, 0, uiText("Piece Size", "Размер куска"), piece_size_, piece_tooltip,
		[this, defaults]() { piece_size_->setValue(defaults.piece_size); });

	// The LOD of the strips and the tubes, under the distances of the whole wires.
	lod_ = new QCheckBox(uiText("LOD", "LOD"));
	lod_->setToolTip(loc_.tip(
		"On: every piece also gets the same wires with fewer segments (a surface wire_lod1), seen from "
		"LOD Distance on instead of the full ones. For a cross, a ribbon and a tube; a mesh has the "
		"LODs of its asset.",
		"Вкл: каждый кусок получает ещё и те же провода с меньшим числом сегментов (поверхность "
		"wire_lod1), они видны вместо полных начиная с [[LOD Distance|«Дистанции LOD»]]. Для "
		"крестовины, ленты и трубки; у меша LOD-ы свои, из ассета."));
	layout->addWidget(lod_, 2, 0, 1, 6);
	strip_widgets_ << lod_;
	connect(lod_, &QCheckBox::toggled, this, [this](bool) { apply_settings(); });

	const QString factor_tooltip = loc_.tip(
		"How many times fewer segments the LOD has.",
		"Во сколько раз меньше сегментов у LOD.");
	lod_factor_ = new QSpinBox;
	lod_factor_->setRange(2, 64);
	lod_factor_->setPrefix(QString::fromUtf8("× "));
	lod_factor_->setToolTip(factor_tooltip);
	lod_factor_->setFocusPolicy(Qt::StrongFocus);
	lod_factor_->installEventFilter(this);
	enableSpinBoxDrag(lod_factor_);
	connect(lod_factor_, &QSpinBox::valueChanged, this, [this](int) { apply_settings(); });
	add_field(layout, 3, 0, uiText("Simplify", "Упростить в"), lod_factor_, factor_tooltip,
		[this, defaults]() { lod_factor_->setValue(defaults.lod_factor); }, &lod_widgets_);
	const QString lod_distance_tooltip = loc_.tip(
		"From what distance to the camera the LOD is seen instead of the full wires, in units.",
		"С какого расстояния до камеры вместо полных проводов виден LOD, в юнитах.");
	lod_distance_ = make_spin_box(0.0, 1000000.0, 5.0, 1, lod_distance_tooltip);
	add_field(layout, 4, 0, uiText("LOD Distance", "Дистанция LOD"), lod_distance_, lod_distance_tooltip,
		[this, defaults]() { lod_distance_->setValue(defaults.lod_distance); }, &lod_widgets_);
	const QString lod_fade_tooltip = loc_.tip(
		"Over what distance the full wires and the LOD fade into each other, in units. 0 - they switch "
		"at once.",
		"На каком расстоянии полные провода и LOD плавно сменяют друг друга, в юнитах. 0 - "
		"переключаются сразу.");
	lod_fade_ = make_spin_box(0.0, 1000000.0, 1.0, 1, lod_fade_tooltip);
	add_field(layout, 4, 1, uiText("LOD Fade", "Затухание LOD"), lod_fade_, lod_fade_tooltip,
		[this, defaults]() { lod_fade_->setValue(defaults.lod_fade); }, &lod_widgets_);

	// As the distances of a decal of the Road.
	const QString visible_tooltip = loc_.tip(
		"From what distance to the camera the wires start to fade out, in units (Max Visible "
		"Distance). inf - always seen. The pieces get it at once.",
		"С какого расстояния до камеры провода начинают исчезать, в юнитах (Max Visible Distance). "
		"inf - видны всегда. Куски получают значение сразу.");
	visible_distance_ = make_spin_box(0.0, 1000000.0, 10.0, 1, visible_tooltip);
	visible_distance_->setSpecialValueText(QString::fromUtf8("inf"));
	add_field(layout, 1, 0, uiText("Visible Distance", "Видны до"), visible_distance_, visible_tooltip,
		[this, defaults]() { visible_distance_->setValue(defaults.visible_distance); });
	const QString fade_tooltip = loc_.tip(
		"Over what distance after Visible Distance the wires fade out smoothly, in units (Max Fade "
		"Distance). 0 - they disappear at once.",
		"На каком расстоянии после [[Visible Distance|«Видны до»]] провода плавно исчезают, в "
		"юнитах (Max Fade Distance). 0 - пропадают сразу.");
	fade_distance_ = make_spin_box(0.0, 1000000.0, 5.0, 1, fade_tooltip);
	add_field(layout, 1, 1, uiText("Fade Distance", "Затухание"), fade_distance_, fade_tooltip,
		[this, defaults]() { fade_distance_->setValue(defaults.fade_distance); });

	const QString folder_tooltip = loc_.tip(
		"The folder of the project the meshes of the line are saved to, \"wires\" next to the world "
		"by default. The files are named after the line: <line>_00.mesh...",
		"Папка проекта, куда сохраняются меши линии, по умолчанию «wires» рядом с миром. Файлы "
		"называются по имени линии: <линия>_00.mesh...");
	QLabel *folder_title = new QLabel(uiText("Folder", "Папка"));
	folder_title->setToolTip(folder_tooltip);
	layout->addWidget(folder_title, 5, 0);
	AssetField::Options folder_options;
	folder_options.tooltip = folder_tooltip;
	folder_options.dialog_title = uiText("Folder for the meshes of the line", "Папка для мешей линии");
	folder_options.icon = QString::fromUtf8(":/images/assets/folder.png");
	folder_options.reset_tooltip = loc_.tip("Back to the default folder.", "Вернуть папку по умолчанию.");
	folder_options.show_tooltip = loc_.tip("Show the generated meshes of the line in the Asset Browser.",
		"Показать сгенерированные меши линии в Asset Browser.");
	folder_field_ = new AssetField(folder_options);
	layout->addWidget(folder_field_, 5, 1, 1, 5);
	folder_field_->on_browse = [this]() { browse_folder(); };
	folder_field_->on_show = [this]() {
		message_ = builder_->showGeneratedFiles() ? QString()
			: uiText("The generated meshes are not found in the project.", "Сгенерированные меши не найдены в проекте.");
		refresh();
	};
	folder_field_->on_reset = [this]() {
		WireSettings settings = builder_->getSettings();
		settings.folder.clear();
		builder_->setSettings(settings);
	};
	return group;
}

////////////////////////////////////////////////////////////////////////////////
// Settings.
////////////////////////////////////////////////////////////////////////////////
WireSettings WireBuilderPage::read_settings() const
{
	const WireSettings &current = builder_->getSettings();
	const auto field_value = [](const QDoubleSpinBox *field, float kept) {
		// The field shows the kept value rounded: it is the kept one that is meant.
		const double precision = 0.6 * std::pow(10.0, -field->decimals());
		return qAbs(field->value() - double(kept)) < precision ? kept : float(field->value());
	};

	WireSettings settings = current;
	settings.attach_name = attach_name_->text().trimmed();
	settings.closed = closed_->isChecked();
	settings.pole_every = pole_every_->checkedId() == 1;
	settings.pole_spacing = field_value(pole_spacing_, current.pole_spacing);
	settings.pole_on_ground = pole_on_ground_->isChecked();
	settings.ground = ground_filter_->getFilter();
	settings.sag_mode = WireSagMode(sag_mode_->currentData().toInt());
	settings.sag = field_value(sag_, current.sag);
	settings.sag_random = field_value(sag_random_, current.sag_random);
	settings.segment_angle = field_value(segment_angle_, current.segment_angle);
	settings.above_ground = above_ground_->isChecked();
	settings.min_segments = min_segments_->value();
	settings.max_segment_length = field_value(max_segment_length_, current.max_segment_length);
	settings.seed = seed_->value();
	settings.shape = WireShapeKind(qMax(0, shape_->checkedId()));
	settings.thickness = field_value(thickness_, current.thickness);
	settings.sides = sides_->value();
	settings.rotation = field_value(rotation_, current.rotation);
	settings.random_rotation = random_rotation_->isChecked();
	settings.twist_mode = WireTwistMode(twist_mode_->currentData().toInt());
	settings.twist = field_value(twist_, current.twist);
	settings.uv_length = field_value(uv_length_, current.uv_length);
	settings.uv_width = field_value(uv_width_, current.uv_width);
	settings.uv_rotation = float(uv_rotation_->value());
	settings.mesh_scale = field_value(mesh_scale_, current.mesh_scale);
	settings.mesh_angle = field_value(mesh_angle_, current.mesh_angle);
	settings.twist_step = field_value(twist_step_, current.twist_step);
	settings.piece_size = field_value(piece_size_, current.piece_size);
	settings.visible_distance = field_value(visible_distance_, current.visible_distance);
	settings.lod = lod_->isChecked();
	settings.lod_factor = lod_factor_->value();
	settings.lod_distance = field_value(lod_distance_, current.lod_distance);
	settings.lod_fade = field_value(lod_fade_, current.lod_fade);
	settings.fade_distance = field_value(fade_distance_, current.fade_distance);
	return settings;
}

void WireBuilderPage::apply_settings()
{
	if (!updating_)
		builder_->setSettings(read_settings());
}

void WireBuilderPage::request_refresh()
{
	// The builder tells of changes several times a frame while a point is dragged: the
	// page is refreshed once, when the current events are done.
	if (refresh_pending_)
		return;
	refresh_pending_ = true;
	QTimer::singleShot(0, this, [this]() {
		refresh_pending_ = false;
		refresh();
	});
}

void WireBuilderPage::refresh()
{
	const bool has_line = builder_->hasLine();
	const WireSettings &settings = builder_->getSettings();

	updating_ = true;
	// A field being typed in is left alone, unless the settings come from elsewhere: an
	// undo, another line.
	const bool from_elsewhere = builder_->getSettingsRevision() != shown_revision_;
	shown_revision_ = builder_->getSettingsRevision();
	const auto show_value = [from_elsewhere](QDoubleSpinBox *box, float value) {
		if (!box->hasFocus() || from_elsewhere)
			box->setValue(value);
	};
	if (!attach_name_->hasFocus() || from_elsewhere)
		attach_name_->setText(settings.attach_name);
	closed_->setChecked(settings.closed);
	sag_mode_->setCurrentIndex(sag_mode_->findData(int(settings.sag_mode)));
	show_value(sag_, settings.sag);
	show_value(sag_random_, settings.sag_random);
	show_value(segment_angle_, settings.segment_angle);
	above_ground_->setChecked(settings.above_ground);
	if (!min_segments_->hasFocus() || from_elsewhere)
		min_segments_->setValue(settings.min_segments);
	show_value(max_segment_length_, settings.max_segment_length);
	if (!seed_->hasFocus() || from_elsewhere)
		seed_->setValue(settings.seed);
	if (QAbstractButton *button = shape_->button(settings.shape))
		button->setChecked(true);
	show_value(thickness_, settings.thickness);
	if (!sides_->hasFocus() || from_elsewhere)
		sides_->setValue(settings.sides);
	show_value(rotation_, settings.rotation);
	random_rotation_->setChecked(settings.random_rotation);
	twist_mode_->setCurrentIndex(twist_mode_->findData(int(settings.twist_mode)));
	show_value(twist_, settings.twist);
	show_value(uv_length_, settings.uv_length);
	show_value(uv_width_, settings.uv_width);
	if (!uv_rotation_->isSliderDown())
		uv_rotation_->setValue(qRound(settings.uv_rotation));
	uv_rotation_value_->setText(QString::fromUtf8("%1°").arg(uv_rotation_->value()));
	show_value(mesh_scale_, settings.mesh_scale);
	show_value(mesh_angle_, settings.mesh_angle);
	show_value(twist_step_, settings.twist_step);
	if (QAbstractButton *button = mesh_forward_->button(settings.mesh_forward))
		button->setChecked(true);
	if (QAbstractButton *button = mesh_up_->button(settings.mesh_up))
		button->setChecked(true);
	show_value(piece_size_, settings.piece_size);
	show_value(visible_distance_, settings.visible_distance);
	lod_->setChecked(settings.lod);
	if (!lod_factor_->hasFocus() || from_elsewhere)
		lod_factor_->setValue(settings.lod_factor);
	show_value(lod_distance_, settings.lod_distance);
	show_value(lod_fade_, settings.lod_fade);
	show_value(fade_distance_, settings.fade_distance);
	show_value(pole_spacing_, settings.pole_spacing);
	if (QAbstractButton *button = pole_every_->button(settings.pole_every ? 1 : 0))
		button->setChecked(true);
	// Only for poles between the points.
	pole_spacing_->setVisible(settings.pole_every);
	if (QAbstractButton *button = pole_forward_->button(settings.pole_forward))
		button->setChecked(true);
	if (QAbstractButton *button = pole_up_->button(settings.pole_up))
		button->setChecked(true);
	pole_on_ground_->setChecked(settings.pole_on_ground);
	ground_filter_->setFilter(settings.ground);
	refresh_pole_list();
	add_poles_button_->setChecked(builder_->isAddingPoles());
	add_points_button_->setChecked(builder_->isAddingPoints());
	updating_ = false;

	// The rows of the shape that is picked.
	const bool mesh = settings.shape == WIRE_SHAPE_MESH;
	for (QWidget *widget : strip_widgets_)
		widget->setVisible(!mesh);
	for (QWidget *widget : tube_widgets_)
		widget->setVisible(settings.shape == WIRE_SHAPE_TUBE);
	for (QWidget *widget : mesh_widgets_)
		widget->setVisible(mesh);
	for (QWidget *widget : twist_step_widgets_)
		widget->setVisible(!mesh && builder_->hasTwist());
	for (QWidget *widget : lod_widgets_)
		widget->setVisible(!mesh && settings.lod);
	// How the twist is counted, only while something is twisted.
	twist_mode_->setVisible(builder_->hasTwist());

	// What is only about the poles: not for a garland (a curve with no assets of poles);
	// On Ground not for insulators that stand at the points themselves either.
	{
		const bool curve_line = builder_->isCurveLine();
		bool poles = false;
		for (const WirePolePart &part : settings.pole_parts)
			poles = poles || part.role != WIRE_POLE_INSULATOR;
		const bool any_asset = !settings.pole_parts.isEmpty();
		for (QWidget *widget : pole_asset_widgets_)
			widget->setVisible(any_asset);
		pole_on_ground_->setVisible(poles);
		for (QWidget *widget : attach_widgets_)
			widget->setVisible(!curve_line || any_asset);
	}

	const QString material = builder_->getMaterialName();
	material_field_->setValue(material.isEmpty() ? QString::fromUtf8("mesh_base") : material, !material.isEmpty());
	material_field_->setShowEnabled(!material.isEmpty());
	mesh_field_->setValue(settings.mesh_name.isEmpty() ? uiText("(none)", "(не выбран)") : settings.mesh_name,
		!settings.mesh_name.isEmpty());
	mesh_field_->setShowEnabled(!settings.mesh_source.isEmpty());
	if (!settings.mesh_source.isEmpty())
		mesh_field_->setIcon(kindIcon(PlaceableAsset::Kind(settings.mesh_kind)));

	folder_field_->setValue(builder_->getFolder(), !settings.folder.isEmpty());
	folder_field_->setShowEnabled(builder_->hasGeneratedFiles());

	if (has_line)
	{
		line_label_->setText(uiText("%1  -  %2 poles, %3 wires, %4 units of wire", "%1  -  столбов: %2, проводов: %3, длина проводов: %4")
			.arg(builder_->getLineName()).arg(builder_->getNumPoles()).arg(builder_->getWires().size())
			.arg(builder_->getLength(), 0, 'f', 1));
	} else
	{
		line_label_->setText(uiText("No line. Press New Line, or select an existing line in the list.",
			"Линии нет. Нажмите «Новая линия» или выберите существующую в списке."));
	}
	new_line_button_->setStyleSheet(has_line ? QString() : goButtonStyle());
	// A line drawn as a curve has its points; a line of poles of the world - its poles.
	const bool curve = builder_->isCurveLine();
	const bool link = has_line && !curve;
	add_points_button_->setText(builder_->isAddingPoints() ? uiText("Stop Adding Points (Esc)", "Закончить добавление (Esc)")
		: uiText("Add Points", "Добавить точки"));
	add_points_button_->setVisible(!link);
	add_points_button_->setEnabled(curve);
	add_poles_button_->setText(builder_->isAddingPoles() ? uiText("Stop Adding Poles (Esc)", "Закончить добавление (Esc)")
		: uiText("Add Poles", "Добавить столбы"));
	add_poles_button_->setVisible(link);
	point_row_->setVisible(!link);
	link_row_->setVisible(link);
	poles_group_->setVisible(!link);
	const CurvePointsState points = builder_->getSelectedPointsState();
	smooth_button_->setEnabled(curve && points != POINTS_NONE);
	corner_button_->setEnabled(curve && points != POINTS_NONE);
	smooth_button_->setChecked(points == POINTS_SMOOTH);
	corner_button_->setChecked(points == POINTS_CORNER);
	subdivide_button_->setEnabled(curve && builder_->canSubdivide());
	double point_sag = 1.0;
	double point_random = -1.0;
	double point_thickness = 1.0;
	double point_twist = 0.0;
	const bool has_point = curve && builder_->getSelectedPointsSag(point_sag, point_random)
		&& builder_->getSelectedPointsShape(point_thickness, point_twist);
	{
		const bool was_updating = updating_;
		updating_ = true;
		point_sag_->setEnabled(has_point);
		point_random_->setEnabled(has_point);
		point_thickness_->setEnabled(has_point);
		point_twist_->setEnabled(has_point);
		if (!point_sag_->hasFocus())
			point_sag_->setValue(point_sag);
		if (!point_random_->hasFocus())
			point_random_->setValue(point_random);
		if (!point_thickness_->hasFocus())
			point_thickness_->setValue(point_thickness);
		if (!point_twist_->hasFocus())
			point_twist_->setValue(point_twist);
		updating_ = was_updating;
	}
	add_selected_button_->setEnabled(link);
	remove_last_button_->setEnabled(link && builder_->getNumPoles() > 0);
	clear_poles_button_->setEnabled(link && builder_->getNumPoles() > 0);
	reset_settings_button_->setEnabled(has_line);
	clone_line_button_->setEnabled(has_line);
	remove_curve_button_->setEnabled(has_line);
	remove_line_button_->setEnabled(has_line);
	const Unigine::NodePtr parent = builder_->getParentNode();
	parent_label_->setText(parent ? QString::fromUtf8(parent->getName()) : uiText("World root", "Корень мира"));
	parent_clear_button_->setEnabled(bool(parent));

	const QVector<WireSpan> &wires = builder_->getWires();
	stats_label_->setText(uiText("%1 wires, %2 triangles.", "Проводов: %1, треугольников: %2.")
		.arg(wires.size()).arg(builder_->getTriangles()));
	stats_label_->setVisible(has_line && !wires.isEmpty());
	generate_button_->setEnabled(has_line && !wires.isEmpty());

	refresh_lines();

	QString status;
	if (has_line && builder_->isAddingPoles())
	{
		status = uiText("Click the poles in the viewport one after another; Esc - done.",
			"Кликайте по столбам во вьюпорте один за другим; Esc - готово.");
	} else if (has_line && builder_->isAddingPoints())
	{
		status = uiText("Click the ground where the poles stand, one after another; Ctrl + click - a corner; Esc - done.",
			"Кликайте по земле там, где стоят столбы, одну точку за другой; Ctrl + клик - угол; Esc - готово.");
	} else if (has_line && builder_->getNumPoles() < 2)
		status = uiText("The line needs at least two poles.", "Линии нужно минимум два столба.");
	else if (has_line && builder_->hasPolesWithoutPoints())
	{
		status = uiText("Some poles have no attach points (wire_1, wire_2...): their wires go from their bottom.",
			"У некоторых столбов нет точек крепления (wire_1, wire_2...): провода от них идут от основания.");
	}
	else if (has_line && !builder_->getRebuildError().isEmpty())
		status = builder_->getRebuildError();
	else if (has_line && !builder_->getMeshError().isEmpty())
		status = builder_->getMeshError();
	else if (has_line && wires.isEmpty())
	{
		status = uiText("No wires: the neighbouring poles have no attach points of the same numbers.",
			"Проводов нет: у соседних столбов нет точек крепления с одинаковыми номерами.");
	} else if (builder_->isOutdated())
	{
		status = uiText("The mesh of the line was made for other poles or settings: press Generate to update it.",
			"Меш линии сделан для других столбов или настроек: нажмите «Сгенерировать», чтобы обновить его.");
	}
	if (!message_.isEmpty())
		status += (status.isEmpty() ? QString() : QString::fromUtf8("\n")) + message_;
	status_label_->setText(status);
}

void WireBuilderPage::rescan_lines()
{
	lines_ = builder_->findLines();
	const QSignalBlocker blocker(lines_list_);
	lines_list_->clear();
	for (const WireLineInfo &line : lines_)
	{
		QListWidgetItem *item = new QListWidgetItem(line_item_text(line));
		item->setData(Qt::UserRole, line.id);
		lines_list_->addItem(item);
	}
	if (lines_.isEmpty())
	{
		QListWidgetItem *item = new QListWidgetItem(uiText("No lines in the world.", "В мире нет линий."));
		item->setFlags(Qt::NoItemFlags);
		lines_list_->addItem(item);
	}
	const int rows = qBound(1, int(lines_.size()), MAX_VISIBLE_LINES);
	const int row_height = qMax(lines_list_->sizeHintForRow(0), lines_list_->fontMetrics().height() + 2);
	lines_list_->setFixedHeight(rows * row_height + 2 * lines_list_->frameWidth());
	refresh_lines();
}

void WireBuilderPage::refresh_lines()
{
	const int current_id = builder_->getLineId();
	int row = -1;
	for (int i = 0; i < lines_.size(); ++i)
	{
		if (lines_[i].id == current_id)
			row = i;
	}
	const QSignalBlocker blocker(lines_list_);
	lines_list_->setCurrentRow(row);
	if (row >= 0)
	{
		lines_[row].name = builder_->getLineName();
		lines_[row].poles = builder_->getNumPoles();
		lines_[row].pieces = builder_->getNumPieces();
		lines_list_->item(row)->setText(line_item_text(lines_[row]));
	}
}

void WireBuilderPage::browse_folder()
{
	message_.clear();
	QString start = QDir::fromNativeSeparators(QString::fromUtf8(Unigine::Engine::get()->getDataPath()));
	const QString current = start + QLatin1Char('/') + builder_->getFolder();
	if (QDir(current).exists())
		start = current;
	const QString folder = QFileDialog::getExistingDirectory(this, uiText("Folder for the meshes of the line", "Папка для мешей линии"), start);
	if (!folder.isEmpty())
		builder_->setFolderFromDisk(folder);
}

void WireBuilderPage::generate()
{
	message_.clear();
	if (builder_->getTriangles() > TRIANGLES_WARNING_COUNT)
	{
		const QMessageBox::StandardButton answer = QMessageBox::question(this, uiText("Generate", "Сгенерировать"),
			uiText("The wires will have %1 triangles. Make Bend Angle larger to have fewer. Continue?",
				"У проводов будет треугольников: %1. Чтобы их было меньше, увеличьте «Угол изгиба». Продолжить?")
				.arg(builder_->getTriangles()),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
	}
	QApplication::setOverrideCursor(Qt::WaitCursor);
	builder_->generate();
	QApplication::restoreOverrideCursor();
}

} // namespace ArtistTool
