#include "ObjectPlacerPage.h"
#include "UiHelpers.h"

#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>

#include <QButtonGroup>
#include <QCheckBox>
#include <QDial>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMenu>
#include <QPainter>
#include <QPainterPath>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSignalBlocker>
#include <QStringList>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidgetAction>

#include <array>

using namespace Unigine;
using ::UnigineEditor::Selection;
using ::UnigineEditor::SelectorNodes;

namespace ArtistTool
{

namespace
{

const char SETTINGS_ORGANIZATION[] = "zloy_pingvin";
const char SETTINGS_APPLICATION[] = "artist_tool";
const char SETTINGS_GROUP[] = "object_placer";

// X - red, Y - green, Z - blue, the same as the axes in the editor.
const char *const AXIS_COLORS[3] = {"#e05a5a", "#6cc06c", "#5a8ce0"};
const char *const AXIS_NAMES[3] = {"X", "Y", "Z"};
const char *const AXIS_KEYS[3] = {"_x", "_y", "_z"};

// What a click places.
enum Source
{
	SOURCE_ASSET = 0,
	SOURCE_DUMMY,
};

// Grid rows of the Object group.
enum ObjectRow
{
	ROW_SOURCE = 0,
	ROW_ASSET,
	ROW_PARENT,
	ROW_UP_AXIS,
};

// Grid columns of the Placement group.
enum PlacementColumn
{
	COLUMN_LABEL = 0,
	COLUMN_MIN_MAX,
	COLUMN_X,
	COLUMN_Y,
	COLUMN_Z,
	COLUMN_RESET,
};

struct SurfaceTypeName
{
	int type;
	const char *name;
};

const SurfaceTypeName SURFACE_TYPES[] = {
	{PlacerOptions::SURFACE_MESH_STATIC, "Mesh Static"},
	{PlacerOptions::SURFACE_MESH_SKINNED, "Mesh Skinned"},
	{PlacerOptions::SURFACE_MESH_DYNAMIC, "Mesh Dynamic"},
	{PlacerOptions::SURFACE_MESH_CLUSTER, "Mesh Cluster"},
	{PlacerOptions::SURFACE_MESH_CLUTTER, "Mesh Clutter"},
	{PlacerOptions::SURFACE_TERRAIN, "Terrain"},
	{PlacerOptions::SURFACE_WATER, "Water"},
	{PlacerOptions::SURFACE_CLOUDS, "Clouds"},
};

struct UpAxisName
{
	PlacerOptions::UpAxis axis;
	const char *name;
	// Index into AXIS_COLORS.
	int color;
};

const UpAxisName UP_AXES[] = {
	{PlacerOptions::UP_AXIS_X_POSITIVE, "X+", 0},
	{PlacerOptions::UP_AXIS_X_NEGATIVE, "X-", 0},
	{PlacerOptions::UP_AXIS_Y_POSITIVE, "Y+", 1},
	{PlacerOptions::UP_AXIS_Y_NEGATIVE, "Y-", 1},
	{PlacerOptions::UP_AXIS_Z_POSITIVE, "Z+", 2},
	{PlacerOptions::UP_AXIS_Z_NEGATIVE, "Z-", 2},
};

// The icons are shared with the other tool pages, see UiHelpers.
QIcon make_filter_icon(const QColor &)
{
	return makeFilterIcon();
}

QIcon make_reset_icon(const QColor &)
{
	return makeResetIcon();
}

// Adds a check box to a menu as an item that does not close the menu when toggled.
QCheckBox *add_menu_check_box(QMenu *menu, const QString &text)
{
	QCheckBox *check = new QCheckBox(text);
	check->setContentsMargins(8, 3, 8, 3);

	QWidgetAction *action = new QWidgetAction(menu);
	action->setDefaultWidget(check);
	menu->addAction(action);
	return check;
}

// An empty row that visually separates blocks of parameters.
void add_gap(QGridLayout *grid, int height)
{
	grid->addItem(new QSpacerItem(0, height, QSizePolicy::Minimum, QSizePolicy::Fixed), grid->rowCount(), 0);
}

QColor icon_color()
{
	return iconColor();
}

} // namespace

ObjectPlacerPage::ObjectPlacerPage(QWidget *parent)
	: QWidget(parent)
{
	placer_ = new ObjectPlacer(this);

	build_ui();
	load_settings();
	apply_options();

	placer_->on_changed = [this]() { refresh(); };
	placer_->on_message = [this](int, const QString &text) {
		message_ = text;
		refresh();
	};

	connect(Selection::instance(), &Selection::changed, this, [this]() {
		placer_->updateAssetFromSelection();
	});

	placer_->updateAssetFromSelection();
	refresh();
}

ObjectPlacerPage::~ObjectPlacerPage()
{
	placer_->on_changed = nullptr;
	placer_->on_message = nullptr;
	save_settings();
}

void ObjectPlacerPage::stop()
{
	placer_->setEnabled(false);
}

void ObjectPlacerPage::resetFilter()
{
	reset_filter();
}

void ObjectPlacerPage::hideEvent(QHideEvent *event)
{
	stop();
	QWidget::hideEvent(event);
}

bool ObjectPlacerPage::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == filter_button_)
	{
		if (event->type() == QEvent::Resize)
			fit_filter_text();
		return QWidget::eventFilter(watched, event);
	}

	if (event->type() == QEvent::Wheel)
	{
		const QWidget *widget = qobject_cast<QWidget *>(watched);
		if (widget && !widget->hasFocus())
		{
			event->ignore();
			return true;
		}

		// One wheel notch turns a dial by exactly one degree (Qt would turn it by
		// several, as many as the system scrolls lines per notch).
		if (QDial *dial = qobject_cast<QDial *>(watched))
		{
			const int delta = static_cast<QWheelEvent *>(event)->angleDelta().y();
			if (delta != 0)
				dial->setValue(dial->value() + (delta > 0 ? 1 : -1));
			event->accept();
			return true;
		}
	}
	return QWidget::eventFilter(watched, event);
}

void ObjectPlacerPage::build_ui()
{
	const PlacerOptions defaults;

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
		"Places objects by clicking in the viewport. Select a mesh or a node in the Asset "
		"Browser, press Start placing and click where the disc is shown.",
		"Ставит объекты кликом во вьюпорте. Выберите меш или ноду в Asset Browser, нажмите "
		"«Начать расстановку» и кликайте там, где показан диск."));
	description->setWordWrap(true);
	layout->addWidget(description);

	// Filter.
	QGroupBox *filter_group = new QGroupBox(uiText("Place on", "Ставить на"));
	QVBoxLayout *filter_layout = new QVBoxLayout(filter_group);
	filter_layout->addWidget(build_filter());
	layout->addWidget(filter_group);

	// Object.
	QGroupBox *object_group = new QGroupBox(uiText("Object", "Объект"));
	QGridLayout *object_layout = new QGridLayout(object_group);
	object_layout->setColumnStretch(1, 1);

	// What is placed: the asset picked in the Asset Browser or an empty dummy.
	const QString source_tooltip = loc_.tip(
		"What a click places.\n"
		"Asset - the mesh or node selected in the Asset Browser.\n"
		"Dummy - an empty Node Dummy: a point in the world with a position and a rotation, "
		"to mark places or to parent other nodes to. No asset is needed for it.",
		"Что ставит клик.\n"
		"[[Asset|Ассет]] - меш или нода, выбранные в Asset Browser.\n"
		"[[Dummy|Пустышка]] - пустая нода Node Dummy: точка в мире с позицией и поворотом, чтобы "
		"размечать места или привязывать к ней другие ноды. Ассет для неё не нужен.");
	QLabel *source_label = new QLabel(uiText("Place", "Ставить"));
	source_label->setToolTip(source_tooltip);
	object_layout->addWidget(source_label, ROW_SOURCE, 0);

	source_ = new QButtonGroup(this);
	source_->setExclusive(true);
	QHBoxLayout *source_layout = new QHBoxLayout;
	source_layout->setSpacing(3);
	// The buttons of the Place and Parent rows look the same and are of one size:
	// an icon of the editor and a caption.
	QVector<QToolButton *> object_buttons;
	const auto make_object_button = [&object_buttons](const QString &text, const char *icon, const QString &tooltip) {
		QToolButton *button = new QToolButton;
		button->setText(text);
		button->setIcon(QIcon(QString::fromUtf8(icon)));
		button->setIconSize(QSize(16, 16));
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(tooltip);
		button->setStyleSheet(QString::fromUtf8(
			"QToolButton { color: #e4e4e4; font-weight: bold; background-color: #3b3e43;"
			" border: 1px solid #5a5d63; border-radius: 3px; padding: 3px 6px; min-width: 64px; }"
			"QToolButton:hover { background-color: #4b4e54; }"
			"QToolButton:pressed { background-color: #2f3236; }"
			"QToolButton:checked { color: white; background-color: #2f6fd6; border-color: #2f6fd6; }"
			"QToolButton:disabled { color: #7a7a7a; }"));
		object_buttons.append(button);
		return button;
	};

	const QString source_names[] = {uiText("Asset", "Ассет"), uiText("Dummy", "Пустышка")};
	const char *const source_icons[] = {":/images/icon_add.png", ":/images/icon_rectangle.png"};
	for (int id = SOURCE_ASSET; id <= SOURCE_DUMMY; ++id)
	{
		QToolButton *button = make_object_button(source_names[id], source_icons[id], source_tooltip);
		button->setCheckable(true);
		source_->addButton(button, id);
		source_layout->addWidget(button);
	}
	source_layout->addStretch(1);
	object_layout->addLayout(source_layout, ROW_SOURCE, 1, 1, 3);

	asset_title_ = new QLabel(uiText("Asset", "Ассет"));
	object_layout->addWidget(asset_title_, ROW_ASSET, 0);
	asset_label_ = new QLabel;
	asset_label_->setWordWrap(true);
	asset_label_->setToolTip(loc_.tip(
		"The object that will be placed - the last one selected in the Asset Browser:\n"
		"a node (.node) is placed as a Node Reference,\n"
		"a mesh (.mesh) - as a Mesh Static,\n"
		"an imported model (.fbx) - as its objects with their materials.",
		"Объект, который будет размещаться, - последний выбранный в Asset Browser:\n"
		"нода (.node) ставится как Node Reference,\n"
		"меш (.mesh) - как Mesh Static,\n"
		"импортированная модель (.fbx) - как её объекты со своими материалами."));
	object_layout->addWidget(asset_label_, ROW_ASSET, 1, 1, 3);

	object_layout->addWidget(new QLabel(uiText("Parent", "Родитель")), ROW_PARENT, 0);
	parent_label_ = new QLabel;
	parent_label_->setToolTip(loc_.tip(
		"Placed objects become children of this node. World root - they are added to the "
		"end of the World Hierarchy.",
		"Размещённые объекты становятся детьми этой ноды. [[World root|Корень мира]] - добавляются в конец "
		"списка World Hierarchy."));
	object_layout->addWidget(parent_label_, ROW_PARENT, 1);

	parent_select_button_ = make_object_button(uiText("Use selected", "Взять выделенную"),
		":/images/icon_stack.png", loc_.tip(
		"Make the node selected in the World Hierarchy the parent of the placed objects.",
		"Сделать ноду, выделенную в World Hierarchy, родителем для размещаемых объектов."));
	object_layout->addWidget(parent_select_button_, ROW_PARENT, 2);

	parent_clear_button_ = make_object_button(uiText("Clear", "Сбросить"),
		":/images/icon_delete_trash.png", loc_.tip(
		"Place objects into the world root again.",
		"Снова размещать объекты в корень мира."));
	object_layout->addWidget(parent_clear_button_, ROW_PARENT, 3);

	// One size for the four of them: that of the widest one.
	QSize object_button_size;
	for (QToolButton *button : object_buttons)
		object_button_size = object_button_size.expandedTo(button->sizeHint());
	for (QToolButton *button : object_buttons)
		button->setFixedSize(object_button_size);

	const QString up_axis_tooltip = loc_.tip(
		"Which axis of the asset points away from the surface.\n"
		"Z+ - the asset stands as it was made. Pick another axis to lay it on its side or "
		"turn it upside down: e.g. Y- makes the Y- side of the asset look up.",
		"Какая ось ассета смотрит от поверхности («вверх»).\n"
		"Z+ - ассет стоит так, как сделан. Выберите другую ось, чтобы положить его на бок "
		"или перевернуть: например, Y- - вверх будет смотреть сторона Y- ассета.");
	QLabel *up_axis_label = new QLabel(uiText("Up axis", "Ось вверх"));
	up_axis_label->setToolTip(up_axis_tooltip);
	object_layout->addWidget(up_axis_label, ROW_UP_AXIS, 0);

	// One toggle button per axis direction, only one of them on. The letter has the
	// color of its axis; the chosen button is filled with that color.
	up_axis_ = new QButtonGroup(this);
	up_axis_->setExclusive(true);

	QHBoxLayout *up_axis_layout = new QHBoxLayout;
	up_axis_layout->setSpacing(3);
	for (const UpAxisName &axis : UP_AXES)
	{
		const QString color = QString::fromUtf8(AXIS_COLORS[axis.color]);

		QToolButton *button = new QToolButton;
		button->setText(QString::fromUtf8(axis.name));
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(up_axis_tooltip);
		button->setStyleSheet(QString::fromUtf8(
			"QToolButton { color: %1; font-weight: bold; background-color: #3b3e43;"
			" border: 1px solid #5a5d63; border-radius: 3px; padding: 3px 0px; min-width: 34px; }"
			"QToolButton:hover { background-color: #4b4e54; }"
			"QToolButton:checked { color: #1c1c1c; background-color: %1; border-color: %1; }").arg(color));
		up_axis_->addButton(button, int(axis.axis));
		up_axis_layout->addWidget(button);
	}
	up_axis_layout->addStretch(1);
	up_axis_layout->addWidget(make_reset_button([this, defaults]() {
		set_up_axis(defaults.up_axis);
		apply_options();
	}));
	object_layout->addLayout(up_axis_layout, ROW_UP_AXIS, 1, 1, 3);

	layout->addWidget(object_group);

	// Placement: the same parameters as the Objects settings of the Cluster Paint mode.
	QGroupBox *placement_group = new QGroupBox(uiText("Placement", "Размещение"));
	QGridLayout *placement_layout = new QGridLayout(placement_group);
	placement_layout->setHorizontalSpacing(4);
	placement_layout->setVerticalSpacing(3);
	for (int column = COLUMN_X; column <= COLUMN_Z; ++column)
		placement_layout->setColumnStretch(column, 1);

	normal_orientation_ = new QCheckBox(uiText("Normal Orientation", "Ориентация по нормали"));
	normal_orientation_->setToolTip(loc_.tip(
		"On: the object stands perpendicular to the surface you click (leans on slopes).\n"
		"Off: the object always stands upright.",
		"Вкл: объект встаёт перпендикулярно поверхности, по которой кликнули (на склоне "
		"наклоняется).\n"
		"Выкл: объект всегда стоит вертикально."));
	placement_layout->addWidget(normal_orientation_, 0, COLUMN_LABEL, 1, COLUMN_RESET);
	placement_layout->addWidget(make_reset_button([this, defaults]() {
		normal_orientation_->setChecked(defaults.normal_orientation);
	}), 0, COLUMN_RESET);

	const QString random_hint = loc_.tip(
		"\nEach placed object gets a random value between Min and Max. Set both to the "
		"same value for no randomness.",
		"\nКаждый размещённый объект получает случайное значение между [[Min|Мин]] и [[Max|Макс]]. "
		"Одинаковые [[Min|Мин]] и [[Max|Макс]] - без случайности.");

	// Local parameters first, then the world ones; the groups are kept apart,
	// otherwise the fields merge into one wall of columns.
	const int group_gap = 14;
	const int block_gap = 6;

	// One value for all objects, like Offset of the editor's Snap to Surface.
	add_gap(placement_layout, block_gap);
	const QString surface_offset_tooltip = loc_.tip(
		"How far from the surface the object is put, in units - the same as the offset "
		"of the editor's Snap to Surface. It goes along the up axis of the object: along "
		"the normal of the surface while Normal Orientation is on, straight up while it "
		"is off. Negative sinks the object into the surface.\n"
		"The same for every object; for a random shift use Local Offset.",
		"На сколько объект отодвигается от поверхности, в юнитах, - как отступ у Snap to "
		"Surface в редакторе. Идёт вдоль оси «вверх» объекта: по нормали поверхности, "
		"когда включена [[Normal Orientation|«Ориентация по нормали»]], и строго вверх, "
		"когда выключена. Минус утапливает объект в поверхность.\n"
		"Одинаков для всех объектов; для случайного сдвига есть "
		"[[Local Offset|«Локальный сдвиг»]].");
	const int surface_offset_row = placement_layout->rowCount();
	QLabel *surface_offset_label = new QLabel(uiText("Offset From Surface", "Отступ от поверхности"));
	surface_offset_label->setToolTip(surface_offset_tooltip);
	placement_layout->addWidget(surface_offset_label, surface_offset_row, COLUMN_LABEL, 1, 2);
	surface_offset_ = make_spin_box(-100000.0, 100000.0, 0.1, 3, surface_offset_tooltip);
	placement_layout->addWidget(surface_offset_, surface_offset_row, COLUMN_X, 1, COLUMN_RESET - COLUMN_X);
	placement_layout->addWidget(make_reset_button([this, defaults]() {
		surface_offset_->setValue(defaults.surface_offset);
	}), surface_offset_row, COLUMN_RESET);

	add_gap(placement_layout, group_gap);
	local_offset_ = add_vec3_range(placement_layout, uiText("Local Offset", "Локальный сдвиг"),
		-100000.0, 100000.0, 0.1, 3, loc_.tip(
			"Shift of the object relative to the surface, in units: Z - away from the "
			"surface (negative - into it), X and Y - along it.",
			"Сдвиг объекта относительно поверхности, в юнитах: Z - от поверхности "
			"(минус - вглубь), X и Y - вдоль неё.") + random_hint,
		defaults.local_offset_min, defaults.local_offset_max);

	const QString dial_hint = loc_.tip(
		"\nTurn the dial with the mouse; click it and use the wheel or the arrow keys for "
		"one degree steps. From -180 to 180: Min -180 and Max 180 - any direction.",
		"\nКрутилка вращается мышью; кликните по ней и крутите колесо или жмите стрелки - "
		"шаг один градус. От -180 до 180: [[Min|Мин]] -180 и [[Max|Макс]] 180 - любое направление.");

	add_gap(placement_layout, block_gap);
	local_rotation_ = add_angle_range(placement_layout, uiText("Local Rotation", "Локальный поворот"), loc_.tip(
			"Rotation of the object relative to the surface, in degrees. Z - the turn around "
			"the axis looking away from the surface.",
			"Поворот объекта относительно поверхности, в градусах. Z - разворот вокруг оси, "
			"смотрящей от поверхности.") + dial_hint + random_hint,
		defaults.local_rotation_min, defaults.local_rotation_max);

	add_gap(placement_layout, group_gap);
	world_offset_ = add_vec3_range(placement_layout, uiText("World Offset", "Мировой сдвиг"),
		-100000.0, 100000.0, 0.1, 3, loc_.tip(
			"Shift of the object along the world axes, in units. Z - straight up, whatever "
			"the slope of the surface.",
			"Сдвиг объекта по мировым осям, в юнитах. Z - строго вверх, каким бы ни был "
			"наклон поверхности.") + random_hint,
		defaults.world_offset_min, defaults.world_offset_max);

	add_gap(placement_layout, block_gap);
	world_rotation_ = add_angle_range(placement_layout, uiText("World Rotation", "Мировой поворот"), loc_.tip(
			"Rotation of the object around the world axes, in degrees. Applied after the "
			"orientation to the surface.",
			"Поворот объекта вокруг мировых осей, в градусах. Применяется после "
			"ориентации по поверхности.") + dial_hint + random_hint,
		defaults.world_rotation_min, defaults.world_rotation_max);

	add_gap(placement_layout, group_gap);
	const QString scale_tooltip = loc_.tip(
		"Size of the object, the same along all axes. 1 - the original size.",
		"Размер объекта, одинаковый по всем осям. 1 - исходный размер.") + random_hint;
	const int scale_row = placement_layout->rowCount();
	QLabel *scale_label = new QLabel(uiText("Scale", "Масштаб"));
	scale_label->setToolTip(scale_tooltip);
	placement_layout->addWidget(scale_label, scale_row, COLUMN_LABEL);

	scale_min_ = make_spin_box(0.001, 1000.0, 0.05, 3, scale_tooltip);
	scale_max_ = make_spin_box(0.001, 1000.0, 0.05, 3, scale_tooltip);
	QHBoxLayout *scale_layout = new QHBoxLayout;
	scale_layout->setSpacing(4);
	scale_layout->addWidget(new QLabel(uiText("Min", "Мин")));
	scale_layout->addWidget(scale_min_, 1);
	scale_layout->addSpacing(8);
	scale_layout->addWidget(new QLabel(uiText("Max", "Макс")));
	scale_layout->addWidget(scale_max_, 1);
	placement_layout->addLayout(scale_layout, scale_row, COLUMN_MIN_MAX, 1, COLUMN_RESET - COLUMN_MIN_MAX);
	placement_layout->addWidget(make_reset_button([this, defaults]() {
		updating_ = true;
		scale_min_->setValue(defaults.scale_min);
		scale_max_->setValue(defaults.scale_max);
		updating_ = false;
		apply_options();
	}), scale_row, COLUMN_RESET);

	layout->addWidget(placement_group);

	// Run.
	place_button_ = new QPushButton;
	place_button_->setCheckable(true);
	place_button_->setMinimumHeight(36);
	place_button_->setIcon(QIcon(QString::fromUtf8(":/images/tools/icon_snap_to_surface.png")));
	place_button_->setIconSize(QSize(24, 24));
	// The main control of the page: green while it offers to start, red while placing
	// is on and the button stops it.
	place_button_->setStyleSheet(QString::fromUtf8(
		"QPushButton { background-color: #3f9a4a; color: white; border: none; border-radius: 3px;"
		" font-weight: bold; padding: 5px; }"
		"QPushButton:hover { background-color: #4bad57; }"
		"QPushButton:checked { background-color: #c9483b; }"
		"QPushButton:checked:hover { background-color: #dc5a4c; }"
		"QPushButton:disabled { background-color: #4a4a4a; color: #8c8c8c; }"));
	place_button_->setToolTip(loc_.tip(
		"Turn placing on and off. While it is on, a disc under the mouse shows where the "
		"object will go, and every left click in the viewport places one object.\n"
		"Blue disc - a click places the object. Red disc - placing there is not allowed by "
		"the Place on filter.\n"
		"Camera controls keep working (Alt + left click is not taken).\n"
		"Esc turns placing off. Every placed object is undone with Ctrl+Z.",
		"Включить или выключить размещение. Пока оно включено, диск под курсором "
		"показывает, куда встанет объект, а каждый клик левой кнопкой во вьюпорте ставит "
		"один объект.\n"
		"Синий диск - клик поставит объект. Красный диск - ставить сюда запрещено фильтром "
		"[[Place on|«Ставить на»]].\n"
		"Управление камерой работает как обычно (Alt + левый клик не перехватывается).\n"
		"Esc выключает размещение. Каждый поставленный объект отменяется через Ctrl+Z."));
	layout->addWidget(place_button_);

	status_label_ = new QLabel;
	status_label_->setWordWrap(true);
	layout->addWidget(status_label_);

	// Hotkeys: a collapsible list at the bottom, like in the paint tools of the editor.
	hotkeys_header_ = new QToolButton;
	hotkeys_header_->setText(uiText("Hotkeys", "Горячие клавиши"));
	hotkeys_header_->setCheckable(true);
	hotkeys_header_->setChecked(true);
	hotkeys_header_->setArrowType(Qt::DownArrow);
	hotkeys_header_->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	hotkeys_header_->setAutoRaise(true);
	hotkeys_header_->setFocusPolicy(Qt::NoFocus);
	hotkeys_header_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	QFont header_font = hotkeys_header_->font();
	header_font.setBold(true);
	hotkeys_header_->setFont(header_font);
	hotkeys_header_->setToolTip(loc_.tip(
		"Keys and mouse actions of the tool. They work in the viewport while placing is on.",
		"Клавиши и действия мышью для этого инструмента. Работают во вьюпорте, пока "
		"размещение включено."));
	layout->addWidget(hotkeys_header_);

	QLabel *hotkeys = new QLabel(uiText(
		"LMB - Place Object\n"
		"LMB (Hold) + Mouse Move - Move Object Over Surfaces\n"
		"LMB (Hold) + Mouse Wheel - Rotate Around Up Axis (step: 5°)\n"
		"Shift + Mouse Wheel - Next / Previous Asset In Folder\n"
		"LMB (Hold) + Shift + Mouse Wheel - Swap Held Object For Next / Previous Asset\n"
		"Alt + LMB - Editor Camera (not taken by the tool)\n"
		"Esc - Stop Placing\n"
		"Ctrl + Z - Undo Placed Object",
		"ЛКМ - поставить объект\n"
		"ЛКМ (держать) + движение мыши - двигать объект по поверхностям\n"
		"ЛКМ (держать) + колесо мыши - поворот вокруг оси «вверх» (шаг 5°)\n"
		"Shift + колесо мыши - следующий / предыдущий ассет в папке\n"
		"ЛКМ (держать) + Shift + колесо мыши - заменить удерживаемый объект на следующий / "
		"предыдущий ассет\n"
		"Alt + ЛКМ - камера редактора (инструмент её не перехватывает)\n"
		"Esc - остановить расстановку\n"
		"Ctrl + Z - отменить поставленный объект"));
	hotkeys->setWordWrap(true);
	hotkeys->setContentsMargins(6, 2, 6, 6);
	hotkeys->setToolTip(loc_.tip(
		"LMB - the left mouse button. \"Hold\" - press and do not release.\n"
		"Moving and rotating a held object need the Move While Button Is Held option "
		"(on the Debug tab, on by default).",
		"[[LMB|ЛКМ]] - левая кнопка мыши. «[[Hold|держать]]» - нажать и не отпускать.\n"
		"Для перетаскивания и поворота зажатого объекта должна быть включена опция "
		"Move While Button Is Held (на вкладке Debug, по умолчанию включена)."));
	layout->addWidget(hotkeys);

	connect(hotkeys_header_, &QToolButton::toggled, hotkeys, [this, hotkeys](bool expanded) {
		hotkeys_header_->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
		hotkeys->setVisible(expanded);
	});

	layout->addStretch(1);

	connect(place_button_, &QPushButton::toggled, this, [this](bool checked) {
		message_.clear();
		apply_options();
		placer_->setEnabled(checked);
	});
	connect(parent_select_button_, &QToolButton::clicked, this, [this]() { use_selected_as_parent(); });
	connect(parent_clear_button_, &QToolButton::clicked, this, [this]() {
		placer_->setParentNode(NodePtr());
	});
	connect(normal_orientation_, &QCheckBox::toggled, this, [this](bool) { apply_options(); });
	connect(up_axis_, &QButtonGroup::idClicked, this, [this](int) { apply_options(); });
	connect(source_, &QButtonGroup::idClicked, this, [this](int id) {
		message_.clear();
		placer_->setPlaceDummy(id == SOURCE_DUMMY);
	});
}

// The controls of the tool that live on the Debug tab of the window. The page keeps
// the value itself, so it does not depend on how long these widgets exist.
QWidget *ObjectPlacerPage::createDebugControls()
{
	const PlacerOptions defaults;

	QWidget *controls = new QWidget;
	QHBoxLayout *layout = new QHBoxLayout(controls);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);

	QCheckBox *check = new QCheckBox(QString::fromUtf8("Move While Button Is Held"));
	check->setChecked(move_while_held_);
	check->setToolTip(loc_.tip(
		"On: press the mouse button and do not release it - the object is placed and "
		"keeps following the mouse over the surfaces, like with Snap to Surface. "
		"Release the button to leave it there.\n"
		"While the button is held, the mouse wheel turns the object around its up axis, "
		"5 degrees per notch.\n"
		"Shift + wheel swaps it for the next or previous asset of the same type from the "
		"same folder (also works before the click, to pick what the next click places).\n"
		"Off: the object stays where you pressed.",
		"Вкл: нажмите кнопку мыши и не отпускайте - объект ставится и продолжает ездить "
		"за курсором по поверхностям, как при Snap to Surface. Отпустите кнопку, чтобы "
		"оставить его на месте.\n"
		"Пока кнопка зажата, колесо мыши поворачивает объект вокруг его оси «вверх», "
		"5 градусов за щелчок.\n"
		"Shift + колесо заменяет его на следующий или предыдущий ассет того же типа из "
		"той же папки (работает и до клика - чтобы выбрать, что поставит следующий клик).\n"
		"Выкл: объект остаётся там, где нажали."));
	layout->addWidget(check, 1);
	layout->addWidget(make_reset_button([check, defaults]() {
		check->setChecked(defaults.move_while_held);
	}));

	connect(check, &QCheckBox::toggled, this, [this](bool checked) {
		move_while_held_ = checked;
		apply_options();
	});
	return controls;
}

QWidget *ObjectPlacerPage::build_filter()
{
	QWidget *row = new QWidget;
	QHBoxLayout *row_layout = new QHBoxLayout(row);
	row_layout->setContentsMargins(0, 0, 0, 0);
	row_layout->setSpacing(4);

	filter_button_ = new QPushButton;
	filter_button_->setIcon(make_filter_icon(icon_color()));
	filter_button_->setIconSize(QSize(14, 14));
	filter_button_->setMinimumHeight(24);
	// The text is cut to the width of the button, so it must not ask for more room.
	filter_button_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	filter_button_->installEventFilter(this);
	// A lighter field with the list of what is checked, like the filter of the
	// Cluster Paint mode.
	filter_button_->setStyleSheet(QString::fromUtf8(
		"QPushButton { background-color: #5c6066; color: #f0f0f0; border: none; border-radius: 3px;"
		" padding: 3px 22px 3px 7px; text-align: left; }"
		"QPushButton:hover, QPushButton:pressed { background-color: #6b6f76; }"
		"QPushButton::menu-indicator { subcontrol-origin: padding; subcontrol-position: right center;"
		" right: 7px; }"));
	filter_button_->setToolTip(loc_.tip(
		"What objects can be placed on. Over the checked kinds of objects the disc under "
		"the mouse is blue and a click places an object; over the others it is red and a "
		"click does nothing.\n"
		"An object of an unchecked kind blocks placing on whatever is behind it.\n"
		"The filter goes back to its default when the tool window is closed.",
		"На что можно ставить объекты. Над отмеченными типами объектов диск под курсором "
		"синий и клик ставит объект; над остальными он красный и клик ничего не делает.\n"
		"Объект неотмеченного типа не даёт поставить и на то, что находится за ним.\n"
		"При закрытии окна инструмента фильтр возвращается к значению по умолчанию."));

	QMenu *menu = new QMenu(filter_button_);

	only_immovable_ = add_menu_check_box(menu, uiText("Place Only On Immovable Nodes", "Только на ноды с флагом Immovable"));
	only_immovable_->setToolTip(loc_.tip(
		"Place only on nodes that have the Immovable flag enabled.",
		"Ставить только на ноды с включённым флагом Immovable."));
	only_intersection_ = add_menu_check_box(menu, uiText("Place Only On Surfaces With Intersection", "Только на поверхности с Intersection"));
	only_intersection_->setToolTip(loc_.tip(
		"Place only on surfaces that have the Intersection flag and any bit of the "
		"Intersection Mask enabled.",
		"Ставить только на поверхности с включённым флагом Intersection и хотя бы одним "
		"битом Intersection Mask."));
	menu->addSeparator();

	for (const SurfaceTypeName &type : SURFACE_TYPES)
	{
		FilterItem item;
		item.check = add_menu_check_box(menu, QString::fromUtf8(type.name));
		item.surface_type = type.type;
		filter_types_.append(item);
	}

	filter_button_->setMenu(menu);
	row_layout->addWidget(filter_button_, 1);
	row_layout->addWidget(make_reset_button([this]() { reset_filter(); }));

	const auto changed = [this](bool) {
		refresh_filter_summary();
		apply_options();
	};
	connect(only_immovable_, &QCheckBox::toggled, this, changed);
	connect(only_intersection_, &QCheckBox::toggled, this, changed);
	for (const FilterItem &item : filter_types_)
		connect(item.check, &QCheckBox::toggled, this, changed);

	return row;
}

QToolButton *ObjectPlacerPage::make_reset_button(const std::function<void()> &reset)
{
	QToolButton *button = new QToolButton;
	button->setAutoRaise(true);
	button->setIcon(make_reset_icon(icon_color()));
	button->setIconSize(QSize(16, 16));
	button->setFixedSize(22, 22);
	button->setFocusPolicy(Qt::NoFocus);
	button->setToolTip(loc_.tip("Reset to default", "Сбросить на значение по умолчанию"));
	connect(button, &QToolButton::clicked, this, [reset]() { reset(); });
	return button;
}

QDoubleSpinBox *ObjectPlacerPage::make_spin_box(double minimum, double maximum, double step,
	int decimals, const QString &tooltip)
{
	QDoubleSpinBox *box = new QDoubleSpinBox;
	box->setRange(minimum, maximum);
	box->setSingleStep(step);
	box->setDecimals(decimals);
	box->setMinimumWidth(50);
	box->setToolTip(tooltip);
	box->setFocusPolicy(Qt::StrongFocus);
	box->installEventFilter(this);
	// The arrows at the right edge: press and drag to change the value both ways,
	// like in the fields of the editor.
	enableSpinBoxDrag(box);
	connect(box, &QDoubleSpinBox::valueChanged, this, [this](double) { apply_options(); });
	return box;
}

// A value field with the letter of its axis, in the axis color, on the left.
// 'extra' is an optional widget shown after the field.
QWidget *ObjectPlacerPage::make_axis_cell(QWidget *field, QWidget *extra, int axis) const
{
	QWidget *cell = new QWidget;
	QHBoxLayout *layout = new QHBoxLayout(cell);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);

	QLabel *letter = new QLabel(QString::fromUtf8(AXIS_NAMES[axis]));
	letter->setStyleSheet(QString::fromUtf8("color: %1; font-weight: bold;").arg(QString::fromUtf8(AXIS_COLORS[axis])));
	layout->addWidget(letter);

	if (extra)
	{
		layout->addWidget(field);
		layout->addWidget(extra);
		layout->addStretch(1);
	} else
		layout->addWidget(field, 1);
	return cell;
}

// A dial for an angle from -180 to 180 with its caption and value written under it.
QWidget *ObjectPlacerPage::make_dial(QDial *&dial, const QString &caption, const QString &tooltip)
{
	dial = new QDial;
	dial->setRange(-180, 180);
	dial->setSingleStep(1);
	dial->setPageStep(15);
	dial->setNotchesVisible(true);
	dial->setNotchTarget(12.0);
	dial->setFixedSize(56, 56);
	dial->setToolTip(tooltip);
	dial->setFocusPolicy(Qt::StrongFocus);
	dial->installEventFilter(this);

	QLabel *value = new QLabel;
	value->setAlignment(Qt::AlignHCenter);
	value->setToolTip(tooltip);

	const auto show_value = [value, caption](int angle) {
		value->setText(QString::fromUtf8("%1 %2°").arg(caption).arg(angle));
	};
	show_value(dial->value());
	connect(dial, &QDial::valueChanged, this, [this, show_value](int angle) {
		show_value(angle);
		apply_options();
	});

	QWidget *cell = new QWidget;
	// Wide enough for the longest text, "Min -180°", so the row does not jump.
	cell->setMinimumWidth(qMax(56, value->fontMetrics().horizontalAdvance(uiText("Max -180°", "Макс -180°")) + 2));
	QVBoxLayout *layout = new QVBoxLayout(cell);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(1);
	layout->addWidget(dial, 0, Qt::AlignHCenter);
	layout->addWidget(value);
	return cell;
}

// The Min and Max dials of one axis side by side, after the letter of the axis.
QWidget *ObjectPlacerPage::make_angle_cell(QDial *&min_dial, QDial *&max_dial, int axis, const QString &tooltip)
{
	QWidget *dials = new QWidget;
	QHBoxLayout *layout = new QHBoxLayout(dials);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(2);
	layout->addWidget(make_dial(min_dial, uiText("Min", "Мин"), tooltip));
	layout->addWidget(make_dial(max_dial, uiText("Max", "Макс"), tooltip));

	return make_axis_cell(dials, nullptr, axis);
}

// Two rows, Min and Max, each with X / Y / Z fields and its own reset button.
ObjectPlacerPage::Vec3Range ObjectPlacerPage::add_vec3_range(QGridLayout *grid, const QString &label,
	double minimum, double maximum, double step, int decimals, const QString &tooltip,
	const Math::vec3 &default_min, const Math::vec3 &default_max)
{
	const int row = grid->rowCount();

	QLabel *name = new QLabel(label);
	name->setToolTip(tooltip);
	grid->addWidget(name, row, COLUMN_LABEL, 2, 1);
	grid->addWidget(new QLabel(uiText("Min", "Мин")), row, COLUMN_MIN_MAX);
	grid->addWidget(new QLabel(uiText("Max", "Макс")), row + 1, COLUMN_MIN_MAX);

	Vec3Range range;
	for (int axis = 0; axis < 3; ++axis)
	{
		const QString axis_tooltip = QString::fromUtf8("%1 - %2\n").arg(label, QString::fromUtf8(AXIS_NAMES[axis]))
			+ tooltip;
		range.min[axis] = make_spin_box(minimum, maximum, step, decimals, axis_tooltip);
		range.max[axis] = make_spin_box(minimum, maximum, step, decimals, axis_tooltip);
		grid->addWidget(make_axis_cell(range.min[axis], nullptr, axis), row, COLUMN_X + axis);
		grid->addWidget(make_axis_cell(range.max[axis], nullptr, axis), row + 1, COLUMN_X + axis);
	}

	const auto reset = [this](QDoubleSpinBox *const boxes[3], const Math::vec3 &value) {
		updating_ = true;
		write_vec3(boxes, value);
		updating_ = false;
		apply_options();
	};
	const std::array<QDoubleSpinBox *, 3> min_boxes = {range.min[0], range.min[1], range.min[2]};
	const std::array<QDoubleSpinBox *, 3> max_boxes = {range.max[0], range.max[1], range.max[2]};
	grid->addWidget(make_reset_button([reset, min_boxes, default_min]() { reset(min_boxes.data(), default_min); }),
		row, COLUMN_RESET);
	grid->addWidget(make_reset_button([reset, max_boxes, default_max]() { reset(max_boxes.data(), default_max); }),
		row + 1, COLUMN_RESET);

	return range;
}

// One row of dials: the Min and Max of X, then of Y, then of Z, and a reset button
// that puts all of them back.
ObjectPlacerPage::AngleRange ObjectPlacerPage::add_angle_range(QGridLayout *grid, const QString &label,
	const QString &tooltip, const Math::vec3 &default_min, const Math::vec3 &default_max)
{
	const int row = grid->rowCount();

	QLabel *name = new QLabel(label);
	name->setToolTip(tooltip);
	grid->addWidget(name, row, COLUMN_LABEL);

	AngleRange range;
	for (int axis = 0; axis < 3; ++axis)
	{
		const QString axis_tooltip = QString::fromUtf8("%1 - %2\n").arg(label, QString::fromUtf8(AXIS_NAMES[axis]))
			+ tooltip;
		grid->addWidget(make_angle_cell(range.min[axis], range.max[axis], axis, axis_tooltip),
			row, COLUMN_X + axis);
	}

	const std::array<QDial *, 3> min_dials = {range.min[0], range.min[1], range.min[2]};
	const std::array<QDial *, 3> max_dials = {range.max[0], range.max[1], range.max[2]};
	grid->addWidget(make_reset_button([this, min_dials, max_dials, default_min, default_max]() {
		updating_ = true;
		write_angles(min_dials.data(), default_min);
		write_angles(max_dials.data(), default_max);
		updating_ = false;
		apply_options();
	}), row, COLUMN_RESET);

	return range;
}

void ObjectPlacerPage::load_angles(const QSettings &settings, const QString &key, QDial *const dials[3],
	const Math::vec3 &default_value)
{
	for (int axis = 0; axis < 3; ++axis)
	{
		dials[axis]->setValue(qRound(settings.value(key + QString::fromUtf8(AXIS_KEYS[axis]),
			double(default_value[axis])).toDouble()));
	}
}

Math::vec3 ObjectPlacerPage::read_angles(QDial *const dials[3])
{
	return Math::vec3(float(dials[0]->value()), float(dials[1]->value()), float(dials[2]->value()));
}

void ObjectPlacerPage::write_angles(QDial *const dials[3], const Math::vec3 &value)
{
	for (int axis = 0; axis < 3; ++axis)
		dials[axis]->setValue(qRound(value[axis]));
}

void ObjectPlacerPage::load_vec3(const QSettings &settings, const QString &key,
	QDoubleSpinBox *const boxes[3], const Math::vec3 &default_value)
{
	for (int axis = 0; axis < 3; ++axis)
	{
		boxes[axis]->setValue(settings.value(key + QString::fromUtf8(AXIS_KEYS[axis]),
			double(default_value[axis])).toDouble());
	}
}

void ObjectPlacerPage::save_vec3(QSettings &settings, const QString &key, const Math::vec3 &value)
{
	for (int axis = 0; axis < 3; ++axis)
		settings.setValue(key + QString::fromUtf8(AXIS_KEYS[axis]), double(value[axis]));
}

Math::vec3 ObjectPlacerPage::read_vec3(QDoubleSpinBox *const boxes[3])
{
	return Math::vec3(float(boxes[0]->value()), float(boxes[1]->value()), float(boxes[2]->value()));
}

void ObjectPlacerPage::write_vec3(QDoubleSpinBox *const boxes[3], const Math::vec3 &value)
{
	for (int axis = 0; axis < 3; ++axis)
		boxes[axis]->setValue(double(value[axis]));
}

void ObjectPlacerPage::load_settings()
{
	const PlacerOptions defaults;

	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);

	// Every value set below would otherwise push half-loaded options to the tool.
	updating_ = true;

	hotkeys_header_->setChecked(settings.value("hotkeys_expanded", true).toBool());
	placer_->setPlaceDummy(settings.value("place_dummy", false).toBool());
	set_up_axis(PlacerOptions::UpAxis(settings.value("up_axis", int(defaults.up_axis)).toInt()));
	normal_orientation_->setChecked(settings.value("normal_orientation", defaults.normal_orientation).toBool());
	move_while_held_ = settings.value("move_while_held", defaults.move_while_held).toBool();
	surface_offset_->setValue(settings.value("surface_offset", double(defaults.surface_offset)).toDouble());

	load_vec3(settings, "local_offset_min", local_offset_.min, defaults.local_offset_min);
	load_vec3(settings, "local_offset_max", local_offset_.max, defaults.local_offset_max);
	load_vec3(settings, "world_offset_min", world_offset_.min, defaults.world_offset_min);
	load_vec3(settings, "world_offset_max", world_offset_.max, defaults.world_offset_max);
	load_angles(settings, "local_rotation_min", local_rotation_.min, defaults.local_rotation_min);
	load_angles(settings, "local_rotation_max", local_rotation_.max, defaults.local_rotation_max);
	load_angles(settings, "world_rotation_min", world_rotation_.min, defaults.world_rotation_min);
	load_angles(settings, "world_rotation_max", world_rotation_.max, defaults.world_rotation_max);
	scale_min_->setValue(settings.value("scale_min", double(defaults.scale_min)).toDouble());
	scale_max_->setValue(settings.value("scale_max", double(defaults.scale_max)).toDouble());

	updating_ = false;

	// The filter is not remembered: it always starts from its default.
	reset_filter();
}

void ObjectPlacerPage::save_settings() const
{
	const PlacerOptions options = read_options();

	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);
	settings.setValue("hotkeys_expanded", hotkeys_header_->isChecked());
	settings.setValue("place_dummy", placer_->isPlaceDummy());
	settings.setValue("up_axis", int(options.up_axis));
	settings.setValue("normal_orientation", options.normal_orientation);
	settings.setValue("move_while_held", options.move_while_held);
	settings.setValue("surface_offset", double(options.surface_offset));
	save_vec3(settings, "local_offset_min", options.local_offset_min);
	save_vec3(settings, "local_offset_max", options.local_offset_max);
	save_vec3(settings, "world_offset_min", options.world_offset_min);
	save_vec3(settings, "world_offset_max", options.world_offset_max);
	save_vec3(settings, "local_rotation_min", options.local_rotation_min);
	save_vec3(settings, "local_rotation_max", options.local_rotation_max);
	save_vec3(settings, "world_rotation_min", options.world_rotation_min);
	save_vec3(settings, "world_rotation_max", options.world_rotation_max);
	settings.setValue("scale_min", double(options.scale_min));
	settings.setValue("scale_max", double(options.scale_max));
}

PlacerOptions ObjectPlacerPage::read_options() const
{
	PlacerOptions options;

	options.surface_types = 0;
	for (const FilterItem &item : filter_types_)
	{
		if (item.check->isChecked())
			options.surface_types |= item.surface_type;
	}
	options.only_immovable = only_immovable_->isChecked();
	options.only_intersection = only_intersection_->isChecked();

	options.up_axis = PlacerOptions::UpAxis(qMax(0, up_axis_->checkedId()));
	options.normal_orientation = normal_orientation_->isChecked();
	options.move_while_held = move_while_held_;
	options.surface_offset = float(surface_offset_->value());
	options.local_offset_min = read_vec3(local_offset_.min);
	options.local_offset_max = read_vec3(local_offset_.max);
	options.world_offset_min = read_vec3(world_offset_.min);
	options.world_offset_max = read_vec3(world_offset_.max);
	options.local_rotation_min = read_angles(local_rotation_.min);
	options.local_rotation_max = read_angles(local_rotation_.max);
	options.world_rotation_min = read_angles(world_rotation_.min);
	options.world_rotation_max = read_angles(world_rotation_.max);
	options.scale_min = float(scale_min_->value());
	options.scale_max = float(scale_max_->value());
	return options;
}

// Checks the button of the axis; an unknown value falls back to the default one.
void ObjectPlacerPage::set_up_axis(PlacerOptions::UpAxis axis)
{
	QAbstractButton *button = up_axis_->button(int(axis));
	if (!button)
		button = up_axis_->button(int(PlacerOptions().up_axis));
	if (button)
		button->setChecked(true);
}

void ObjectPlacerPage::apply_options()
{
	if (!updating_)
		placer_->setOptions(read_options());
}

void ObjectPlacerPage::refresh()
{
	const ObjectPlacer::Asset &asset = placer_->getAsset();
	const bool has_asset = asset.kind != ObjectPlacer::ASSET_NONE;
	const bool has_object = placer_->hasObject();
	const bool dummy = placer_->isPlaceDummy();
	const bool enabled = placer_->isEnabled();

	if (QAbstractButton *button = source_->button(dummy ? SOURCE_DUMMY : SOURCE_ASSET))
		button->setChecked(true);

	if (has_asset)
		asset_label_->setText(QString::fromUtf8("%1  (%2)").arg(asset.name, ObjectPlacer::kindLabel(asset.kind)));
	else
		asset_label_->setText(uiText("Select a mesh or a node in the Asset Browser", "Выберите меш или ноду в Asset Browser"));
	// The asset is kept while dummies are placed, but it is not what a click places.
	asset_title_->setEnabled(!dummy);
	asset_label_->setEnabled(!dummy);

	const NodePtr parent = placer_->getParentNode();
	parent_label_->setText(parent ? QString::fromUtf8(parent->getName()) : uiText("World root", "Корень мира"));
	parent_clear_button_->setEnabled(bool(parent));

	{
		QSignalBlocker blocker(place_button_);
		place_button_->setChecked(enabled);
	}
	place_button_->setText(enabled ? uiText("Stop placing (Esc)", "Остановить (Esc)") : uiText("Start placing", "Начать расстановку"));
	place_button_->setEnabled(has_object || enabled);

	QString status;
	if (enabled)
	{
		status = uiText("Click in the viewport to place '%1'. Placed: %2.", "Кликайте во вьюпорте, чтобы поставить '%1'. Поставлено: %2.")
			.arg(placer_->getObjectName()).arg(placer_->getPlacedCount());
	} else if (placer_->getPlacedCount() > 0)
		status = uiText("Placed: %1.", "Поставлено: %1.").arg(placer_->getPlacedCount());
	if (!message_.isEmpty())
		status += (status.isEmpty() ? QString() : QString::fromUtf8("\n")) + message_;
	status_label_->setText(status);
}

void ObjectPlacerPage::reset_filter()
{
	const PlacerOptions defaults;

	updating_ = true;
	for (const FilterItem &item : filter_types_)
		item.check->setChecked((defaults.surface_types & item.surface_type) != 0);
	only_immovable_->setChecked(defaults.only_immovable);
	only_intersection_->setChecked(defaults.only_intersection);
	updating_ = false;

	refresh_filter_summary();
	apply_options();
}

void ObjectPlacerPage::refresh_filter_summary()
{
	QStringList names;
	for (const FilterItem &item : filter_types_)
	{
		if (item.check->isChecked())
			names.append(item.check->text());
	}

	// Everything that is checked, by name - the way the Cluster Paint filter shows it.
	if (only_immovable_->isChecked())
		names.prepend(uiText("Immovable Only", "Только Immovable"));
	if (only_intersection_->isChecked())
		names.prepend(uiText("With Intersection Only", "Только с Intersection"));

	filter_summary_ = names.isEmpty() ? uiText("Nothing", "Ничего") : names.join(QString::fromUtf8(", "));
	fit_filter_text();
}

// Shows as much of the summary as fits the button, with "..." at the end.
void ObjectPlacerPage::fit_filter_text()
{
	// Icon, paddings and the drop-down arrow take about this much of the width.
	const int reserved = 56;
	const int available = qMax(40, filter_button_->width() - reserved);
	filter_button_->setText(filter_button_->fontMetrics().elidedText(filter_summary_, Qt::ElideRight, available));
}

void ObjectPlacerPage::use_selected_as_parent()
{
	const SelectorNodes *selector = Selection::getSelectorNodes();
	if (!selector || selector->empty())
	{
		message_ = uiText("Select a node in the World Hierarchy to use it as the parent.", "Выделите ноду в World Hierarchy, чтобы сделать её родителем.");
		refresh();
		return;
	}

	message_.clear();
	placer_->setParentNode(selector->getNodes()[0]);
}

} // namespace ArtistTool
