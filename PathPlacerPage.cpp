#include "PathPlacerPage.h"
#include "UiHelpers.h"

#include <editor/UnigineSelection.h>

#include <QButtonGroup>
#include <QCheckBox>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileInfo>
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

using ::UnigineEditor::Selection;

namespace ArtistTool
{

namespace
{

const char SETTINGS_ORGANIZATION[] = "zloy_pingvin";
const char SETTINGS_APPLICATION[] = "artist_tool";
const char SETTINGS_GROUP[] = "path_placer";

// The width a number field is to keep whatever the style of the editor sets, see
// PathPlacerPage::eventFilter().
const char FIXED_WIDTH_PROPERTY[] = "artist_tool_fixed_width";

void keep_fixed_width(QWidget *widget)
{
	const int width = widget->property(FIXED_WIDTH_PROPERTY).toInt();
	if (width > 0 && (widget->minimumWidth() != width || widget->maximumWidth() != width))
		widget->setFixedWidth(width);
}

// Checks the button with the id; none is left checked if there is no such button.
void check_button(QButtonGroup *group, int id)
{
	if (QAbstractButton *button = group->button(id))
		button->setChecked(true);
}

} // namespace

PathPlacerPage::PathPlacerPage(QWidget *parent)
	: QWidget(parent)
{
	placer_ = new PathPlacer(this);

	build_ui();
	load_settings();

	placer_->on_changed = [this]() { refresh(); };
	placer_->on_message = [this](int, const QString &text) {
		message_ = text;
		refresh();
	};

	connect(Selection::instance(), &Selection::changed, this, [this]() {
		if (isVisible())
			placer_->updateFromSelection();
	});

	refresh();
}

PathPlacerPage::~PathPlacerPage()
{
	placer_->on_changed = nullptr;
	placer_->on_message = nullptr;
	save_settings();
}

void PathPlacerPage::stop()
{
	placer_->setActive(false);
}

void PathPlacerPage::showEvent(QShowEvent *event)
{
	QWidget::showEvent(event);
	placer_->setActive(true);
	// Paths may have been added, removed or loaded with another world meanwhile.
	rescan_paths();
}

void PathPlacerPage::hideEvent(QHideEvent *event)
{
	placer_->setActive(false);
	QWidget::hideEvent(event);
}

bool PathPlacerPage::eventFilter(QObject *watched, QEvent *event)
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
	// applied to them - also when the tool window is docked or undocked, which gives
	// its widgets a new parent. A field that is to be narrow gets its width back right
	// after that.
	if (event->type() == QEvent::StyleChange || event->type() == QEvent::Polish)
	{
		if (QWidget *widget = qobject_cast<QWidget *>(watched))
			keep_fixed_width(widget);
	}
	// A click into a number field selects the whole number, so that a new one can be
	// typed right away instead of erasing the old one first.
	if (event->type() == QEvent::FocusIn)
	{
		if (QAbstractSpinBox *box = qobject_cast<QAbstractSpinBox *>(watched))
			QTimer::singleShot(0, box, &QAbstractSpinBox::selectAll);
	}
	return QWidget::eventFilter(watched, event);
}

namespace
{

const int BUTTON_HEIGHT = 28;
const QSize BUTTON_ICON_SIZE(16, 16);

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

// The same look for a button whether it has the style or not.
void set_style(QPushButton *button, const QString &style)
{
	if (button->styleSheet() != style)
		button->setStyleSheet(style);
}

} // namespace

void PathPlacerPage::build_ui()
{
	QVBoxLayout *page_layout = new QVBoxLayout(this);
	page_layout->setContentsMargins(0, 0, 0, 0);

	// The page is taller than a docked window usually is.
	QScrollArea *scroll = new QScrollArea;
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	page_layout->addWidget(scroll);

	QWidget *content = new QWidget;
	scroll->setWidget(content);
	QVBoxLayout *layout = new QVBoxLayout(content);

	QLabel *description = new QLabel(uiText(
		"Places objects along a curve drawn through points. The curve is kept in the "
		"world: select any node of a path to come back to it, move its points and the "
		"objects follow.",
		"Расставляет объекты вдоль кривой, проведённой через точки. Кривая хранится в "
		"мире: выделите любую ноду кривой, чтобы вернуться к ней, двигайте точки - "
		"объекты последуют за ними."));
	description->setWordWrap(true);
	layout->addWidget(description);

	layout->addWidget(build_path_group());
	layout->addWidget(build_objects_group());
	layout->addWidget(build_pattern_group());

	status_label_ = new QLabel;
	status_label_->setWordWrap(true);
	layout->addWidget(status_label_);

	// Hotkeys: a collapsible list at the bottom, like in the paint tools of the editor.
	QLabel *hotkeys = new QLabel(uiText(
		"LMB On A Point - Select It\n"
		"Ctrl / Shift + LMB On A Point - Add It To The Selection\n"
		"LMB (Hold) On A Point + Mouse Move - Move It Over Surfaces\n"
		"LMB On A Surface - Add Point (while Add Points is on)\n"
		"LMB (Hold) + Mouse Move - Move The Added Point Over Surfaces\n"
		"Alt + LMB - Editor Camera (not taken by the tool)\n"
		"Esc - Stop Adding Points\n"
		"Delete - Remove The Selected Point Or Object\n"
		"Ctrl + Z - Undo Added / Moved Point, Subdivide, Fill",
		"ЛКМ по точке - выделить её\n"
		"Ctrl / Shift + ЛКМ по точке - добавить её к выделению\n"
		"ЛКМ (держать) по точке + движение мыши - двигать её по поверхностям\n"
		"ЛКМ по поверхности - добавить точку (пока включено «Добавить точки»)\n"
		"ЛКМ (держать) + движение мыши - двигать добавленную точку по поверхностям\n"
		"Alt + ЛКМ - камера редактора (инструмент её не перехватывает)\n"
		"Esc - закончить добавление точек\n"
		"Delete - удалить выделенную точку или объект\n"
		"Ctrl + Z - отменить добавление / сдвиг точки, разделение, заполнение"));
	hotkeys->setWordWrap(true);
	hotkeys->setContentsMargins(6, 2, 6, 6);
	hotkeys->setToolTip(loc_.tip(
		"LMB - the left mouse button. Points and objects are ordinary nodes: they are "
		"moved, deleted and reordered in the World Hierarchy as usual.",
		"[[LMB|ЛКМ]] - левая кнопка мыши. Точки и объекты - обычные ноды: их двигают, удаляют и "
		"меняют местами в World Hierarchy как обычно."));

	hotkeys_header_ = makeSectionHeader(uiText("Hotkeys", "Горячие клавиши"), hotkeys);
	hotkeys_header_->setToolTip(loc_.tip(
		"Keys and mouse actions of the tool.",
		"Клавиши и действия мышью для этого инструмента."));
	layout->addWidget(hotkeys_header_);
	layout->addWidget(hotkeys);

	layout->addStretch(1);
}

QWidget *PathPlacerPage::build_path_group()
{
	QGroupBox *group = new QGroupBox(uiText("Path", "Кривая"));
	QVBoxLayout *layout = new QVBoxLayout(group);

	path_label_ = new QLabel;
	path_label_->setWordWrap(true);
	path_label_->setToolTip(loc_.tip(
		"The path the tool works with. Select any node of a path in the World Hierarchy "
		"(its root, a point or an object) to switch to it.",
		"Кривая, с которой работает инструмент. Чтобы переключиться на другую, выделите "
		"любую её ноду в World Hierarchy: корень, точку или объект."));
	layout->addWidget(path_label_);

	QHBoxLayout *buttons = new QHBoxLayout;
	new_path_button_ = new QPushButton(uiText("New Path", "Новая кривая"));
	new_path_button_->setIcon(editor_icon(":/images/icon_add.png"));
	new_path_button_->setIconSize(BUTTON_ICON_SIZE);
	keep_size_for_style(new_path_button_, goButtonStyle());
	new_path_button_->setToolTip(loc_.tip(
		"Create an empty path in the world: a node with \"path\" (the points) and "
		"\"objects\" (what is placed) inside. Then add points with Add Points.",
		"Создать в мире пустую кривую: ноду, внутри которой «path» (точки) и «objects» "
		"(то, что расставляется). Затем добавьте точки кнопкой [[Add Points|«Добавить точки»]]."));
	buttons->addWidget(new_path_button_);

	add_points_button_ = new QPushButton;
	add_points_button_->setCheckable(true);
	add_points_button_->setMinimumHeight(BUTTON_HEIGHT);
	add_points_button_->setIcon(editor_icon(":/images/icons_pencil.png"));
	add_points_button_->setIconSize(BUTTON_ICON_SIZE);
	add_points_button_->setStyleSheet(primaryButtonStyle());
	add_points_button_->setToolTip(loc_.tip(
		"While it is on, every left click on a surface in the viewport adds a point; hold "
		"the button to drag it over the surfaces. Esc turns it off.\n"
		"The point goes after the selected point. If the first point of the path is "
		"selected, it goes before it - the path grows from its start. With no point "
		"selected it goes to the end.\n"
		"Points are ordinary nodes: delete them with Delete.",
		"Пока включено, каждый клик левой кнопкой по поверхности во вьюпорте добавляет "
		"точку; не отпуская кнопку, её можно тащить по поверхностям. Esc выключает.\n"
		"Точка встаёт после выделенной. Если выделена первая точка кривой - перед ней: "
		"кривая растёт от начала. Если не выделена ни одна - в конец.\n"
		"Точки - обычные ноды: удаляйте их клавишей Delete."));
	buttons->addWidget(add_points_button_, 1);
	layout->addLayout(buttons);

	// Smooth / Corner for the points selected in the editor.
	const QString point_tooltip = loc_.tip(
		"What the selected points of the path are.\n"
		"Smooth - the curve bends through the point without a break.\n"
		"Corner - a sharp bend: the curve comes to the point and leaves it straight. "
		"For corners of fences and walls.\n"
		"Select one or several points first: click a point in the viewport (Ctrl + click "
		"adds to the selection) or pick it in the World Hierarchy.\n"
		"In the viewport the selected points are orange, corners white, smooth points "
		"blue, the first point of the path green.",
		"Какие точки кривой выделены.\n"
		"[[Smooth|Плавная]] - кривая проходит через точку плавно, без излома.\n"
		"[[Corner|Угол]] - резкий излом: кривая приходит в точку и уходит из неё по прямой. "
		"Для углов заборов и стен.\n"
		"Сначала выделите одну или несколько точек: клик по точке во вьюпорте (Ctrl + клик "
		"добавляет к выделению) или выбор в World Hierarchy.\n"
		"Во вьюпорте выделенные точки оранжевые, угловые белые, обычные синие, первая "
		"точка кривой зелёная.");
	QHBoxLayout *point_row = new QHBoxLayout;
	point_row->setSpacing(3);
	QLabel *point_label = new QLabel(uiText("Selected point", "Выделенная точка"));
	point_label->setToolTip(point_tooltip);
	point_row->addWidget(point_label);
	point_row->addSpacing(6);

	const QString neutral = QString::fromUtf8("#d6d6d6");
	const QString light_text = QString::fromUtf8("#e4e4e4");
	// An icon and a caption; the icon gets some room at the sides.
	const QString point_button_style = toggleButtonStyle(neutral, light_text, 64)
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
	// The same look and size as Smooth / Corner, but it is an action, not a switch.
	subdivide_button_ = new QToolButton;
	subdivide_button_->setText(uiText("Subdivide", "Разделить"));
	subdivide_button_->setIcon(editor_icon(":/sandworm/images/icon_point.png"));
	subdivide_button_->setFocusPolicy(Qt::NoFocus);
	subdivide_button_->setStyleSheet(point_button_style
		+ QString::fromUtf8("QToolButton:pressed { background-color: #5c6066; }"));
	subdivide_button_->setToolTip(loc_.tip(
		"Put a new point in the middle of the curve between two neighbouring points. "
		"Select both of them first: click one, then Ctrl + click the other.\n"
		"With several neighbouring points selected, every span between them is split.",
		"Поставить новую точку посередине кривой между двумя соседними точками. Сначала "
		"выделите обе: клик по одной, затем Ctrl + клик по другой.\n"
		"Если выделено несколько соседних точек, делится каждый промежуток между ними."));
	point_row->addWidget(subdivide_button_);
	point_row->addStretch(1);

	// One size for the three of them: that of the widest one.
	QSize point_button_size;
	for (QToolButton *button : {smooth_button_, corner_button_, subdivide_button_})
	{
		button->setIconSize(BUTTON_ICON_SIZE);
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		point_button_size = point_button_size.expandedTo(button->sizeHint());
	}
	for (QToolButton *button : {smooth_button_, corner_button_, subdivide_button_})
		button->setFixedSize(point_button_size);
	layout->addLayout(point_row);

	QHBoxLayout *closed_row = new QHBoxLayout;
	closed_ = new QCheckBox(uiText("Closed Loop", "Замкнутая"));
	closed_->setToolTip(loc_.tip(
		"On: the last point is joined to the first one, the path is a ring (needs at "
		"least three points).\n"
		"Off: the path goes from the first point to the last one.",
		"Вкл: последняя точка соединяется с первой, кривая замыкается в кольцо (нужно "
		"минимум три точки).\n"
		"Выкл: кривая идёт от первой точки к последней."));
	closed_row->addWidget(closed_, 1);
	closed_row->addWidget(makeResetButton(loc_.tip("Reset to default", "Сбросить на значение по умолчанию"),
		this, [this]() { closed_->setChecked(PathSettings().closed); }));
	layout->addLayout(closed_row);

	// The paths of the world: a click switches to the path, so it does not have to be
	// looked for in the World Hierarchy.
	const QString paths_tooltip = loc_.tip(
		"All the paths of the world. Click one to work with it: it becomes the current "
		"path and its root node is selected in the editor.\n"
		"The list is refreshed when the page is opened and by the button on the right.",
		"Все кривые мира. Кликните по кривой, чтобы работать с ней: она становится "
		"текущей, а её корневая нода выделяется в редакторе.\n"
		"Список обновляется при открытии вкладки и кнопкой справа.");
	QHBoxLayout *paths_header = new QHBoxLayout;
	QLabel *paths_label = new QLabel(uiText("Paths in the world", "Кривые в мире"));
	paths_label->setToolTip(paths_tooltip);
	paths_header->addWidget(paths_label, 1);
	paths_header->addWidget(makeResetButton(
		loc_.tip("Look through the world for paths again.", "Заново найти кривые в мире."),
		this, [this]() { rescan_paths(); }));
	layout->addLayout(paths_header);

	paths_list_ = new QListWidget;
	paths_list_->setToolTip(paths_tooltip);
	// Striped rows, like the list of the Unpack Clutter page, but tight ones: the
	// list is as tall as its rows, see rescan_paths().
	paths_list_->setAlternatingRowColors(true);
	paths_list_->setUniformItemSizes(true);
	paths_list_->setHorizontalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
	paths_list_->setStyleSheet(QString::fromUtf8(
		"QListWidget { background-color: #26282b; alternate-background-color: #303338; }"
		"QListWidget::item { padding: 1px 4px; }"
		"QListWidget::item:selected { background-color: #2f6fd6; color: white; }"));
	layout->addWidget(paths_list_);

	connect(paths_list_, &QListWidget::itemClicked, this, [this](QListWidgetItem *item) {
		message_.clear();
		// The path may be gone by now: the list is then built anew.
		if (!placer_->selectPath(item->data(Qt::UserRole).toInt()))
			rescan_paths();
	});

	// Shown only for a setup of the old expressions/put_objects_along_path.usc.
	old_script_row_ = new QWidget;
	QHBoxLayout *old_script_layout = new QHBoxLayout(old_script_row_);
	old_script_layout->setContentsMargins(0, 0, 0, 0);
	QLabel *old_script_label = new QLabel(uiText("This path still has the old script node.", "У этой кривой осталась нода старого скрипта."));
	old_script_label->setWordWrap(true);
	old_script_layout->addWidget(old_script_label, 1);
	QPushButton *old_script_button = new QPushButton(uiText("Remove Script", "Удалить скрипт"));
	old_script_button->setToolTip(loc_.tip(
		"This path was made with the old put_objects_along_path script. Its script node "
		"is not needed any more and moves the objects on its own when Animation is on - "
		"remove it to let this tool arrange them. Can be undone with Ctrl+Z.",
		"Эта кривая сделана старым скриптом put_objects_along_path. Его нода-скрипт "
		"больше не нужна и при включённой Animation сама двигает объекты - удалите её, "
		"чтобы расстановкой занимался этот инструмент. Отменяется через Ctrl+Z."));
	old_script_layout->addWidget(old_script_button);
	layout->addWidget(old_script_row_);

	connect(new_path_button_, &QPushButton::clicked, this, [this]() {
		message_.clear();
		// A new path starts open, whatever the path that was the current one is: a
		// closed one would join its ends as soon as it gets its third point.
		PathSettings settings = read_settings();
		settings.closed = false;
		placer_->setSettings(settings);
		placer_->createPath();
		placer_->setAddingPoints(true);
	});
	connect(add_points_button_, &QPushButton::toggled, this, [this](bool checked) {
		message_.clear();
		placer_->setAddingPoints(checked);
	});
	connect(smooth_button_, &QToolButton::clicked, this, [this]() { placer_->setSelectedPointsCorner(false); });
	connect(corner_button_, &QToolButton::clicked, this, [this]() { placer_->setSelectedPointsCorner(true); });
	connect(subdivide_button_, &QToolButton::clicked, this, [this]() {
		message_.clear();
		placer_->subdivide();
	});
	connect(closed_, &QCheckBox::toggled, this, [this](bool) { apply_settings(); });
	connect(old_script_button, &QPushButton::clicked, this, [this]() { placer_->removeOldScript(); });

	return group;
}

QWidget *PathPlacerPage::build_objects_group()
{
	const PathSettings defaults;
	const QString reset_tooltip = loc_.tip("Reset to default", "Сбросить на значение по умолчанию");

	QGroupBox *group = new QGroupBox(uiText("Objects", "Объекты"));
	QGridLayout *layout = new QGridLayout(group);
	layout->setColumnStretch(1, 1);
	int row = 0;

	const QString step_tooltip = loc_.tip(
		"The length of the path one object takes, in units - the distance from the start "
		"of an object to the start of the next one.\n"
		"Used for the objects that have no length of their own: the ones you put into "
		"\"objects\" yourself. The objects made by Fill Path keep the lengths of the "
		"pattern.",
		"Длина кривой, которую занимает один объект, в юнитах - расстояние от начала "
		"объекта до начала следующего.\n"
		"Действует на объекты без собственной длины: те, что вы сами положили в "
		"«objects». Объекты, созданные кнопкой [[Fill Path|«Заполнить кривую»]], сохраняют длины из паттерна.");
	QLabel *step_label = new QLabel(uiText("Step", "Шаг"));
	step_label->setToolTip(step_tooltip);
	layout->addWidget(step_label, row, 0);
	step_ = make_spin_box(0.01, 100000.0, 0.1, 3, step_tooltip);
	layout->addWidget(step_, row, 1);
	layout->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() { step_->setValue(defaults.step); }),
		row, 2);
	++row;

	const QString forward_tooltip = loc_.tip(
		"Which axis of an object looks along the path, towards the next object.",
		"Какая ось объекта смотрит вдоль кривой, в сторону следующего объекта.");
	QLabel *forward_label = new QLabel(uiText("Forward axis", "Ось вперёд"));
	forward_label->setToolTip(forward_tooltip);
	layout->addWidget(forward_label, row, 0);
	QHBoxLayout *forward_layout = new QHBoxLayout;
	forward_layout->setSpacing(3);
	forward_axis_ = makeAxisButtons(forward_layout, this, forward_tooltip);
	forward_layout->addStretch(1);
	layout->addLayout(forward_layout, row, 1);
	layout->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() {
		check_button(forward_axis_, defaults.forward_axis);
		apply_settings();
	}), row, 2);
	++row;

	const QString up_tooltip = loc_.tip(
		"Which axis of an object looks up. It can not be the same axis as the forward "
		"one: picking it switches the other.",
		"Какая ось объекта смотрит вверх. Не может совпадать с осью [[Forward axis|«Ось вперёд»]]: при таком "
		"выборе вторая ось переключится сама.");
	QLabel *up_label = new QLabel(uiText("Up axis", "Ось вверх"));
	up_label->setToolTip(up_tooltip);
	layout->addWidget(up_label, row, 0);
	QHBoxLayout *up_layout = new QHBoxLayout;
	up_layout->setSpacing(3);
	up_axis_ = makeAxisButtons(up_layout, this, up_tooltip);
	up_layout->addStretch(1);
	layout->addLayout(up_layout, row, 1);
	layout->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() {
		check_button(up_axis_, defaults.up_axis);
		apply_up_axis();
	}), row, 2);
	++row;

	yaw_only_ = new QCheckBox(uiText("Yaw Only", "Только поворот (без наклона)"));
	yaw_only_->setToolTip(loc_.tip(
		"On: objects stay upright - they turn along the path but do not lean on slopes.\n"
		"Off: objects lean with the slope of the path.",
		"Вкл: объекты стоят вертикально - поворачиваются вдоль кривой, но не наклоняются "
		"на склонах.\n"
		"Выкл: объекты наклоняются вместе со склоном кривой."));
	layout->addWidget(yaw_only_, row, 0, 1, 2);
	layout->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() {
		yaw_only_->setChecked(defaults.yaw_only);
	}), row, 2);
	++row;

	skew_ = new QCheckBox(uiText("Skew Along Slope", "Скос по уклону"));
	skew_->setToolTip(loc_.tip(
		"For fences on slopes. On: an object is sheared along the slope instead of being "
		"tilted - its verticals stay vertical, and it reaches exactly to the start of the "
		"next object.\n"
		"Off: objects are not deformed.",
		"Для заборов на склонах. Вкл: объект скашивается вдоль склона, а не наклоняется - "
		"вертикали остаются вертикальными, и он дотягивается ровно до начала следующего "
		"объекта.\n"
		"Выкл: объекты не деформируются."));
	layout->addWidget(skew_, row, 0, 1, 2);
	layout->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() {
		skew_->setChecked(defaults.skew);
	}), row, 2);
	++row;

	snap_to_ground_ = new QCheckBox(uiText("Snap To Ground", "Прижать к земле"));
	snap_to_ground_->setToolTip(loc_.tip(
		"On: every object is dropped onto the surface under the path (searched 5 units "
		"above the path and 100 below).\n"
		"Off: objects stand exactly on the curve.",
		"Вкл: каждый объект ставится на поверхность под кривой (ищется на 5 юнитов выше "
		"кривой и на 100 ниже).\n"
		"Выкл: объекты стоят точно на кривой."));
	layout->addWidget(snap_to_ground_, row, 0, 1, 2);
	layout->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() {
		snap_to_ground_->setChecked(defaults.snap_to_ground);
	}), row, 2);
	++row;

	// Shown only while Snap To Ground is on, see refresh().
	const QString ground_offset_tooltip = loc_.tip(
		"How far above the surface the objects are put, in units - like the offset of "
		"the editor's Snap to Surface. Negative sinks them into the surface.\n"
		"It goes straight up, not along the slope, so the objects stay under the curve "
		"and the sections of a fence stay joined.",
		"На сколько объекты приподняты над поверхностью, в юнитах, - как отступ у Snap to "
		"Surface в редакторе. Минус утапливает их в поверхность.\n"
		"Идёт строго вверх, а не по наклону, поэтому объекты остаются под кривой, а "
		"секции забора - состыкованными.");
	ground_offset_label_ = new QLabel(uiText("Offset From Surface", "Отступ от поверхности"));
	ground_offset_label_->setToolTip(ground_offset_tooltip);
	layout->addWidget(ground_offset_label_, row, 0);
	ground_offset_ = make_spin_box(-100000.0, 100000.0, 0.1, 3, ground_offset_tooltip);
	layout->addWidget(ground_offset_, row, 1);
	ground_offset_reset_ = makeResetButton(reset_tooltip, this, [this, defaults]() {
		ground_offset_->setValue(defaults.ground_offset);
	});
	layout->addWidget(ground_offset_reset_, row, 2);
	++row;

	QHBoxLayout *update_row = new QHBoxLayout;
	live_update_ = new QCheckBox(uiText("Live Update", "Автообновление"));
	live_update_->setChecked(true);
	live_update_->setToolTip(loc_.tip(
		"On: the objects are always kept on the path. They follow the points, and an "
		"object moved by hand goes back to its place at once.\n"
		"Off: nothing moves by itself - use it to adjust objects by hand; Rearrange Now "
		"puts them back on the path.",
		"Вкл: объекты всегда стоят на кривой. Они следуют за точками, а объект, "
		"сдвинутый руками, сразу возвращается на место.\n"
		"Выкл: само ничего не двигается - так можно подправлять объекты руками; кнопка "
		"[[Rearrange Now|«Расставить заново»]] вернёт их на кривую."));
	update_row->addWidget(live_update_, 1);
	rearrange_button_ = new QPushButton(uiText("Rearrange Now", "Расставить заново"));
	rearrange_button_->setIcon(editor_icon(":/images/action_material_inherit.png"));
	rearrange_button_->setIconSize(BUTTON_ICON_SIZE);
	keep_size_for_style(rearrange_button_, attentionButtonStyle());
	rearrange_button_->setToolTip(loc_.tip(
		"Put all objects back on the path right now.",
		"Прямо сейчас вернуть все объекты на кривую."));
	update_row->addWidget(rearrange_button_);
	layout->addLayout(update_row, row, 0, 1, 3);

	connect(forward_axis_, &QButtonGroup::idClicked, this, [this](int) { apply_settings(); });
	connect(up_axis_, &QButtonGroup::idClicked, this, [this](int) { apply_up_axis(); });
	for (QCheckBox *check : {yaw_only_, skew_, snap_to_ground_})
		connect(check, &QCheckBox::toggled, this, [this](bool) { apply_settings(); });
	connect(live_update_, &QCheckBox::toggled, this, [this](bool checked) { placer_->setLiveUpdate(checked); });
	connect(rearrange_button_, &QPushButton::clicked, this, [this]() {
		placer_->rearrange();
		refresh();
	});

	return group;
}

QWidget *PathPlacerPage::build_pattern_group()
{
	QGroupBox *group = new QGroupBox(uiText("Fill pattern", "Паттерн заполнения"));
	QVBoxLayout *layout = new QVBoxLayout(group);

	pattern_empty_label_ = new QLabel(uiText(
		"The pattern is empty. Select meshes or nodes in the Asset Browser and press "
		"Add Selected Assets.",
		"Паттерн пуст. Выберите меши или ноды в Asset Browser и нажмите «Добавить выбранные "
		"ассеты»."));
	pattern_empty_label_->setWordWrap(true);
	pattern_empty_label_->setEnabled(false);
	layout->addWidget(pattern_empty_label_);

	pattern_rows_ = new QVBoxLayout;
	pattern_rows_->setSpacing(2);
	layout->addLayout(pattern_rows_);

	QHBoxLayout *buttons = new QHBoxLayout;
	pattern_add_button_ = new QPushButton(uiText("Add Selected Assets", "Добавить выбранные ассеты"));
	pattern_add_button_->setIcon(editor_icon(":/images/mods/icon_object_mode.png"));
	pattern_add_button_->setIconSize(BUTTON_ICON_SIZE);
	pattern_add_button_->setMinimumHeight(BUTTON_HEIGHT);
	pattern_add_button_->setToolTip(loc_.tip(
		"Add the meshes and nodes selected in the Asset Browser to the end of the pattern - "
		"all of them if several are selected, in the order of their names. The length of "
		"each is measured from its size along the forward axis and can be changed.\n"
		"The pattern is repeated along the path: e.g. fence 3, gate 1, fence 3, fence 1.",
		"Добавить в конец паттерна меши и ноды, выбранные в Asset Browser, - все сразу, "
		"если выбрано несколько, в порядке имён. Длина каждого измеряется по размеру "
		"ассета вдоль оси [[Forward axis|«Ось вперёд»]], её можно поменять.\n"
		"Паттерн повторяется вдоль кривой: например, забор 3, калитка 1, забор 3, забор 1."));
	buttons->addWidget(pattern_add_button_, 1);
	pattern_clear_button_ = new QPushButton(uiText("Clear", "Очистить"));
	pattern_clear_button_->setIcon(editor_icon(":/images/icon_delete_trash.png"));
	pattern_clear_button_->setIconSize(BUTTON_ICON_SIZE);
	pattern_clear_button_->setMinimumHeight(BUTTON_HEIGHT);
	pattern_clear_button_->setToolTip(loc_.tip("Remove all assets from the pattern.",
		"Убрать из паттерна все ассеты."));
	buttons->addWidget(pattern_clear_button_);
	layout->addLayout(buttons);

	fill_button_ = new QPushButton;
	fill_button_->setMinimumHeight(30);
	fill_button_->setIcon(editor_icon(":/images/tools/icon_drop_to_ground.png"));
	fill_button_->setIconSize(BUTTON_ICON_SIZE);
	// Green while the path can be filled, gray while it can not.
	fill_button_->setStyleSheet(goButtonStyle());
	fill_button_->setToolTip(loc_.tip(
		"Fill the whole path with the pattern, repeated as many times as it fits. The "
		"objects that are on the path now are replaced.\n"
		"It is done once: the created objects are ordinary nodes in \"objects\" - delete, "
		"replace or reorder them as you like, they stay on the path.\n"
		"Can be undone with Ctrl+Z.",
		"Заполнить всю кривую паттерном, повторив его столько раз, сколько поместится. "
		"Объекты, которые сейчас на кривой, заменяются.\n"
		"Делается один раз: созданные объекты - обычные ноды в «objects», их можно "
		"удалять, заменять и менять местами, они остаются на кривой.\n"
		"Отменяется через Ctrl+Z."));
	layout->addWidget(fill_button_);

	connect(pattern_add_button_, &QPushButton::clicked, this, [this]() {
		message_.clear();
		placer_->addPatternAssets();
	});
	connect(pattern_clear_button_, &QPushButton::clicked, this, [this]() { placer_->clearPattern(); });
	connect(fill_button_, &QPushButton::clicked, this, [this]() { fill(); });

	return group;
}

QDoubleSpinBox *PathPlacerPage::make_spin_box(double minimum, double maximum, double step, int decimals,
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

void PathPlacerPage::load_settings()
{
	const PathSettings defaults;

	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);

	// What a new path starts with: the settings used last.
	PathSettings last;
	last.step = float(settings.value("step", double(defaults.step)).toDouble());
	last.forward_axis = AxisDirection(settings.value("forward_axis", int(defaults.forward_axis)).toInt());
	last.up_axis = AxisDirection(settings.value("up_axis", int(defaults.up_axis)).toInt());
	last.yaw_only = settings.value("yaw_only", defaults.yaw_only).toBool();
	last.skew = settings.value("skew", defaults.skew).toBool();
	last.snap_to_ground = settings.value("snap_to_ground", defaults.snap_to_ground).toBool();
	last.ground_offset = float(settings.value("ground_offset", double(defaults.ground_offset)).toDouble());
	placer_->setSettings(last);

	hotkeys_header_->setChecked(settings.value("hotkeys_expanded", true).toBool());

	connect(step_, &QDoubleSpinBox::valueChanged, this, [this](double) { apply_settings(); });
	connect(ground_offset_, &QDoubleSpinBox::valueChanged, this, [this](double) { apply_settings(); });
}

void PathPlacerPage::save_settings() const
{
	const PathSettings &last = placer_->getSettings();

	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);
	settings.setValue("step", double(last.step));
	settings.setValue("forward_axis", int(last.forward_axis));
	settings.setValue("up_axis", int(last.up_axis));
	settings.setValue("yaw_only", last.yaw_only);
	settings.setValue("skew", last.skew);
	settings.setValue("snap_to_ground", last.snap_to_ground);
	settings.setValue("ground_offset", double(last.ground_offset));
	settings.setValue("hotkeys_expanded", hotkeys_header_->isChecked());
}

PathSettings PathPlacerPage::read_settings() const
{
	const PathSettings &current = placer_->getSettings();
	const auto field_value = [](const QDoubleSpinBox *field, float kept) {
		// The field shows the kept value rounded: it is the kept one that is meant.
		return qAbs(field->value() - double(kept)) < 0.0006 ? kept : float(field->value());
	};

	PathSettings settings;
	settings.step = field_value(step_, current.step);
	settings.forward_axis = AxisDirection(qMax(0, forward_axis_->checkedId()));
	settings.up_axis = AxisDirection(qMax(0, up_axis_->checkedId()));
	settings.closed = closed_->isChecked();
	settings.yaw_only = yaw_only_->isChecked();
	settings.skew = skew_->isChecked();
	settings.snap_to_ground = snap_to_ground_->isChecked();
	settings.ground_offset = field_value(ground_offset_, current.ground_offset);
	return settings;
}

void PathPlacerPage::apply_up_axis()
{
	if (updating_)
		return;

	// The two axes can not be the same one. The tool settles that by changing the up
	// axis, which would undo this very pick; so the forward axis takes the axis the
	// up one has just left.
	PathSettings settings = read_settings();
	if (int(settings.up_axis) / 2 == int(settings.forward_axis) / 2)
		settings.forward_axis = placer_->getSettings().up_axis;
	placer_->setSettings(settings);
}

void PathPlacerPage::apply_settings()
{
	if (!updating_)
		placer_->setSettings(read_settings());
}

void PathPlacerPage::refresh()
{
	const bool has_path = placer_->hasPath();
	const PathSettings &settings = placer_->getSettings();

	// The controls show what the current path has.
	updating_ = true;
	if (!step_->hasFocus())
		step_->setValue(settings.step);
	check_button(forward_axis_, settings.forward_axis);
	check_button(up_axis_, settings.up_axis);
	closed_->setChecked(settings.closed);
	yaw_only_->setChecked(settings.yaw_only);
	skew_->setChecked(settings.skew);
	snap_to_ground_->setChecked(settings.snap_to_ground);
	if (!ground_offset_->hasFocus())
		ground_offset_->setValue(settings.ground_offset);
	for (QWidget *widget : {static_cast<QWidget *>(ground_offset_label_), static_cast<QWidget *>(ground_offset_),
			static_cast<QWidget *>(ground_offset_reset_)})
		widget->setVisible(settings.snap_to_ground);
	live_update_->setChecked(placer_->isLiveUpdate());
	add_points_button_->setChecked(placer_->isAddingPoints());
	updating_ = false;

	if (has_path)
	{
		path_label_->setText(uiText("%1  -  %2 points, %3 units long, %4 objects", "%1  -  точек: %2, длина: %3, объектов: %4")
			.arg(placer_->getPathName()).arg(placer_->getNumPoints())
			.arg(placer_->getLength(), 0, 'f', 2).arg(placer_->getNumObjects()));
	} else
	{
		path_label_->setText(uiText(
			"No path. Press New Path, or select any node of an existing path.",
			"Кривой нет. Нажмите «Новая кривая» или выделите любую ноду существующей кривой."));
	}

	// New Path is green while there is no path to work with - it is the thing to
	// press then; with a path it is an ordinary gray button.
	set_style(new_path_button_, has_path ? QString() : goButtonStyle());
	// Rearrange Now is blue while Live Update is off: the objects are not put back
	// on the path by themselves then, the button is the way to do it.
	set_style(rearrange_button_, placer_->isLiveUpdate() ? QString() : attentionButtonStyle());

	add_points_button_->setText(placer_->isAddingPoints() ? uiText("Stop Adding Points (Esc)", "Закончить добавление (Esc)")
		: uiText("Add Points", "Добавить точки"));
	add_points_button_->setEnabled(has_path);

	const PathPlacer::PointsState points = placer_->getSelectedPointsState();
	smooth_button_->setEnabled(points != PathPlacer::POINTS_NONE);
	corner_button_->setEnabled(points != PathPlacer::POINTS_NONE);
	smooth_button_->setChecked(points == PathPlacer::POINTS_SMOOTH);
	corner_button_->setChecked(points == PathPlacer::POINTS_CORNER);
	subdivide_button_->setEnabled(placer_->canSubdivide());

	old_script_row_->setVisible(has_path && placer_->hasOldScript());
	rearrange_button_->setEnabled(has_path);

	refresh_pattern();
	refresh_paths();

	const int fill_count = placer_->countFill();
	fill_button_->setText(fill_count > 0 ? uiText("Fill Path (%1 objects)", "Заполнить кривую (объектов: %1)").arg(fill_count)
		: uiText("Fill Path", "Заполнить кривую"));
	fill_button_->setEnabled(fill_count > 0);

	QString status;
	if (has_path && placer_->getNumPoints() < 2)
		status = uiText("The path needs at least two points.", "Кривой нужно минимум две точки.");
	else if (placer_->getNumUnplaced() > 0)
	{
		status = uiText("%1 objects at the end of the list do not fit on the path.", "Объекты в конце списка (%1) не помещаются на кривой.")
			.arg(placer_->getNumUnplaced());
	}
	if (!message_.isEmpty())
		status += (status.isEmpty() ? QString() : QString::fromUtf8("\n")) + message_;
	status_label_->setText(status);
}

namespace
{

const int MAX_VISIBLE_PATHS = 4;
// A fill that makes more objects than this asks first.
const int FILL_WARNING_COUNT = 500;

// A caption that takes the room its row has left: when it does not fit, it is cut in
// the middle with "..." instead of pushing the row wider. The start and the end of an
// asset name are what tells the assets of one set apart.
class ElidedLabel : public QLabel
{
public:
	explicit ElidedLabel(const QString &text)
		: full_text_(text)
	{
		setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		setMinimumWidth(40);
	}

	QSize sizeHint() const override
	{
		return QSize(fontMetrics().horizontalAdvance(full_text_), fontMetrics().height());
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setPen(palette().color(isEnabled() ? QPalette::Active : QPalette::Disabled, foregroundRole()));
		painter.drawText(rect(), Qt::AlignLeft | Qt::AlignVCenter,
			fontMetrics().elidedText(full_text_, Qt::ElideMiddle, width()));
	}

private:
	QString full_text_;
};

QString path_item_text(const QString &name, int points, int objects)
{
	return uiText("%1  -  %2 points, %3 objects", "%1  -  точек: %2, объектов: %3")
		.arg(name).arg(points).arg(objects);
}

} // namespace

void PathPlacerPage::rescan_paths()
{
	paths_ = placer_->findPaths();

	const QSignalBlocker blocker(paths_list_);
	paths_list_->clear();
	for (const PathInfo &path : paths_)
	{
		QListWidgetItem *item = new QListWidgetItem(path_item_text(path.name, path.points, path.objects));
		item->setData(Qt::UserRole, path.id);
		paths_list_->addItem(item);
	}
	if (paths_.isEmpty())
	{
		QListWidgetItem *item = new QListWidgetItem(uiText("No paths in the world.", "В мире нет кривых."));
		item->setFlags(Qt::NoItemFlags);
		paths_list_->addItem(item);
	}

	missing_path_id_ = placer_->getPathId();
	for (const PathInfo &path : paths_)
	{
		if (path.id == missing_path_id_)
			missing_path_id_ = 0;
	}

	// As tall as its rows, up to MAX_VISIBLE_PATHS of them; longer lists are scrolled.
	const int rows = qBound(1, int(paths_.size()), MAX_VISIBLE_PATHS);
	const int row_height = qMax(paths_list_->sizeHintForRow(0), paths_list_->fontMetrics().height() + 2);
	paths_list_->setFixedHeight(rows * row_height + 2 * paths_list_->frameWidth());

	refresh_paths(false);
}

void PathPlacerPage::refresh_paths(bool rescan_if_missing)
{
	const int current_id = placer_->getPathId();

	int row = -1;
	for (int i = 0; i < paths_.size(); ++i)
	{
		if (paths_[i].id == current_id)
			row = i;
	}

	// A path that is not in the list yet: just created, or picked in the World
	// Hierarchy after the list was built.
	if (current_id != 0 && row < 0 && rescan_if_missing && current_id != missing_path_id_)
	{
		rescan_paths();
		return;
	}

	const QSignalBlocker blocker(paths_list_);
	paths_list_->setCurrentRow(row);
	if (row >= 0)
	{
		// The current path changes while it is worked on: its row follows.
		paths_[row].name = placer_->getPathName();
		paths_[row].points = placer_->getNumPoints();
		paths_[row].objects = placer_->getNumObjects();
		paths_list_->item(row)->setText(path_item_text(paths_[row].name, paths_[row].points, paths_[row].objects));
	}
}

// The rows of the pattern list: number, asset name, "show in the Asset Browser", tail
// switch, repeat count, length, the buttons that move the row up and down, remove
// button.
void PathPlacerPage::refresh_pattern()
{
	const QVector<PathPatternItem> &pattern = placer_->getPattern();

	pattern_empty_label_->setVisible(pattern.isEmpty());
	pattern_clear_button_->setEnabled(!pattern.isEmpty());

	// The rows are rebuilt only when the list of assets changes or another path becomes
	// the current one, so that typing a length does not lose the focus.
	QString signature = QString::number(placer_->getPathId()) + QLatin1Char(':');
	for (const PathPatternItem &item : pattern)
		signature += QString::fromUtf8(item.asset.path.get()) + QLatin1Char(item.tail ? '>' : '|');
	if (signature == pattern_signature_)
		return;
	pattern_signature_ = signature;

	while (QLayoutItem *item = pattern_rows_->takeAt(0))
	{
		if (QWidget *widget = item->widget())
			widget->deleteLater();
		delete item;
	}

	const QString length_tooltip = loc_.tip(
		"The length of the path this asset takes, in units: the distance from its start "
		"to the start of the next object.\n"
		"0 - the asset takes no room: the next object starts at the same place. E.g. a "
		"post that stands at the joint of two fence sections.",
		"Длина кривой, которую занимает этот ассет, в юнитах: расстояние от его начала до "
		"начала следующего объекта.\n"
		"0 - ассет не занимает места: следующий объект начинается там же. Например, столб, "
		"который стоит на стыке двух секций забора.");

	const QString tail_tooltip = loc_.tip(
		"On: this asset is not repeated along the path. It is placed once, at the very "
		"end - right after the last object of the pattern. E.g. the last post that closes "
		"a fence.\n"
		"Only one asset can be the tail: while one has it, the button of the others is "
		"locked - turn the tail off first, then turn it on where it is needed. The tail "
		"is kept at the bottom of the list.",
		"Вкл: этот ассет не повторяется вдоль кривой. Он ставится один раз, в самом конце - "
		"сразу за последним объектом паттерна. Например, последний столб, который закрывает "
		"забор.\n"
		"Хвостом может быть только один ассет: пока он у одного, у остальных кнопка "
		"заблокирована - сначала выключите хвост, потом включите там, где нужно. Хвост "
		"всегда стоит внизу списка.");

	const QString count_tooltip = loc_.tip(
		"How many times in a row this asset is placed before the next one of the pattern.",
		"Сколько раз подряд ставится этот ассет, прежде чем пойдёт следующий из паттерна.");

	bool has_tail = false;
	for (const PathPatternItem &item : pattern)
		has_tail = has_tail || item.tail;

	QVector<QWidget *> narrow_fields;

	// The magnifier the editor has on its own asset fields ("go to asset").
	QIcon go_to_asset_icon = findEditorIcon(QString::fromUtf8("icon_go_to_asset.png"));
	if (go_to_asset_icon.isNull())
		go_to_asset_icon = editor_icon(":/images/assets/search.png");

	for (int i = 0; i < pattern.size(); ++i)
	{
		QWidget *row = new QWidget;
		QHBoxLayout *row_layout = new QHBoxLayout(row);
		row_layout->setContentsMargins(0, 0, 0, 0);
		// The row is kept tight, so that the name of the asset gets as much room as
		// it can: the controls are narrow and the name goes without its extension
		// (the full name is in the tooltip).
		row_layout->setSpacing(3);

		QLabel *number = new QLabel(QString::fromUtf8("%1.").arg(i + 1));
		number->setEnabled(false);
		row_layout->addWidget(number);

		QLabel *name = new ElidedLabel(QFileInfo(pattern[i].asset.name).completeBaseName());
		name->setToolTip(QString::fromUtf8("%1 (%2)").arg(pattern[i].asset.name, kindLabel(pattern[i].asset.kind)));
		row_layout->addWidget(name, 1);

		QToolButton *show = new QToolButton;
		show->setIcon(go_to_asset_icon);
		show->setIconSize(QSize(14, 14));
		show->setFixedSize(20, 20);
		show->setAutoRaise(true);
		show->setFocusPolicy(Qt::NoFocus);
		show->setToolTip(loc_.tip("Show this asset in the Asset Browser.", "Показать этот ассет в Asset Browser."));
		row_layout->addWidget(show);
		connect(show, &QToolButton::clicked, this, [this, i]() {
			const QVector<PathPatternItem> &items = placer_->getPattern();
			if (i >= items.size())
				return;
			message_ = showInAssetBrowser(items[i].asset) ? QString()
				: uiText("The asset is not found in the project.", "Ассет не найден в проекте.");
			refresh();
		});

		QToolButton *tail = new QToolButton;
		tail->setText(uiText("Tail", "Хвост"));
		tail->setCheckable(true);
		tail->setChecked(pattern[i].tail);
		// One tail at most: the others can not take it while it is on somewhere.
		tail->setEnabled(pattern[i].tail || !has_tail);
		tail->setFocusPolicy(Qt::NoFocus);
		tail->setStyleSheet(toggleButtonStyle(QString::fromUtf8("#e08a2a"), QString::fromUtf8("#9a9a9a"), 26)
			+ QString::fromUtf8("QToolButton { padding: 1px 3px; }"));
		tail->setToolTip(tail_tooltip);
		row_layout->addWidget(tail);
		connect(tail, &QToolButton::toggled, this, [this, i](bool checked) {
			pattern_signature_.clear();
			placer_->setPatternTail(i, checked);
		});

		// The sign stands next to the field, not inside it as a prefix: a field with
		// a prefix is awkward to retype.
		QLabel *times = new QLabel(QString::fromUtf8("×"));
		times->setEnabled(false);
		times->setToolTip(count_tooltip);
		row_layout->addWidget(times);

		QSpinBox *count = new QSpinBox;
		count->setRange(1, 999);
		count->setValue(pattern[i].count);
		// The tail is placed once.
		count->setEnabled(!pattern[i].tail);
		// The style of the editor gives number fields a width of its own, which beats a
		// fixed width set from the code; the narrow fields get theirs back after every
		// restyle, see eventFilter().
		count->setProperty(FIXED_WIDTH_PROPERTY, 50);
		narrow_fields.append(count);
		count->setToolTip(count_tooltip);
		count->setFocusPolicy(Qt::StrongFocus);
		count->installEventFilter(this);
		enableSpinBoxDrag(count);
		row_layout->addWidget(count);
		connect(count, &QSpinBox::valueChanged, this, [this, i](int value) {
			placer_->setPatternCount(i, value);
		});

		QDoubleSpinBox *length = make_spin_box(0.0, 100000.0, 0.1, 3, length_tooltip);
		length->setValue(pattern[i].length);
		length->setProperty(FIXED_WIDTH_PROPERTY, 58);
		narrow_fields.append(length);
		row_layout->addWidget(length);
		connect(length, &QDoubleSpinBox::valueChanged, this, [this, i](double value) {
			placer_->setPatternLength(i, float(value));
		});

		// Up and down the list. The rows are rebuilt after a move even if the two
		// entries are the same asset: their counts and lengths may differ.
		QToolButton *up = new QToolButton;
		up->setArrowType(Qt::UpArrow);
		up->setFixedSize(16, 20);
		up->setAutoRaise(true);
		up->setFocusPolicy(Qt::NoFocus);
		up->setEnabled(i > 0 && pattern[i - 1].tail == pattern[i].tail);
		up->setToolTip(loc_.tip("Move this asset up the pattern.", "Поднять этот ассет выше в паттерне."));
		row_layout->addWidget(up);
		connect(up, &QToolButton::clicked, this, [this, i]() {
			pattern_signature_.clear();
			placer_->movePatternItem(i, -1);
		});

		QToolButton *down = new QToolButton;
		down->setArrowType(Qt::DownArrow);
		down->setFixedSize(16, 20);
		down->setAutoRaise(true);
		down->setFocusPolicy(Qt::NoFocus);
		down->setEnabled(i + 1 < pattern.size() && pattern[i + 1].tail == pattern[i].tail);
		down->setToolTip(loc_.tip("Move this asset down the pattern.", "Опустить этот ассет ниже в паттерне."));
		row_layout->addWidget(down);
		connect(down, &QToolButton::clicked, this, [this, i]() {
			pattern_signature_.clear();
			placer_->movePatternItem(i, 1);
		});

		QToolButton *remove = new QToolButton;
		remove->setText(QString::fromUtf8("✕"));
		remove->setFixedSize(18, 20);
		remove->setAutoRaise(true);
		remove->setFocusPolicy(Qt::NoFocus);
		remove->setToolTip(loc_.tip("Remove this asset from the pattern.", "Убрать этот ассет из паттерна."));
		row_layout->addWidget(remove);
		connect(remove, &QToolButton::clicked, this, [this, i]() { placer_->removePatternItem(i); });

		pattern_rows_->addWidget(row);
	}

	// The first time the width is set when the rows are in the page and the style has
	// done its part.
	for (QWidget *field : narrow_fields)
	{
		field->ensurePolished();
		keep_fixed_width(field);
	}
}

void PathPlacerPage::fill()
{
	message_.clear();

	const int existing = placer_->getNumObjects();
	if (existing > 0)
	{
		const QMessageBox::StandardButton answer = QMessageBox::question(this,
			uiText("Fill Path", "Заполнить кривую"),
			uiText("The path already has %1 objects. Replace them with the pattern?", "На кривой уже есть объекты (%1). Заменить их паттерном?").arg(existing),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
	}

	// A short asset on a long path makes a great many objects: asked about first.
	const int count = placer_->countFill();
	if (count > FILL_WARNING_COUNT)
	{
		const QMessageBox::StandardButton answer = QMessageBox::question(this,
			uiText("Fill Path", "Заполнить кривую"),
			uiText("The fill will create %1 objects. So many objects can make the editor slow. Continue?",
				"Заполнение создаст объектов: %1. Такое количество может замедлить редактор. Продолжить?")
				.arg(count),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		if (answer != QMessageBox::Yes)
			return;
	}

	placer_->fill();
}

} // namespace ArtistTool
