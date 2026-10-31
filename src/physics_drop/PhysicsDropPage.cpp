#include "PhysicsDropPage.h"
#include "AssetField.h"
#include "SurfaceFilterButton.h"
#include "UiHelpers.h"

#include <UnigineLog.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>

#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QDial>
#include <QDoubleSpinBox>
#include <QEvent>
#include <QFileInfo>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QIcon>
#include <QLabel>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QScrollArea>
#include <QSettings>
#include <QSlider>
#include <QSpinBox>
#include <QStringList>
#include <QTimer>
#include <QToolButton>
#include <QVBoxLayout>
#include <QWheelEvent>

using namespace Unigine;
using ::UnigineEditor::Selection;
using ::UnigineEditor::SelectorNodes;

namespace ArtistTool
{

namespace
{

const char SETTINGS_ORGANIZATION[] = "zloy_pingvin";
const char SETTINGS_APPLICATION[] = "artist_tool";
const char SETTINGS_GROUP[] = "physics_drop";
// The assets of the brush belong to a project.
const char SETTINGS_ASSETS_GROUP[] = "physics_brush_assets";

const char *const AXIS_NAMES[3] = {"X", "Y", "Z"};
const char *const AXIS_KEYS[3] = {"_x", "_y", "_z"};

// Grid columns of the form of the brush: the values of all its rows start in the same places.
enum BrushColumn
{
	COLUMN_LABEL = 0,
	COLUMN_CAPTION_1,
	COLUMN_FIELD_1,
	COLUMN_CAPTION_2,
	COLUMN_FIELD_2,
	COLUMN_RESET,
};

const int DIAL_SIZE = 56;

// The buttons under a list of assets, the same as in the other tools.
const int LIST_BUTTON_HEIGHT = 28;

// How many of the last messages stay under the buttons.
const int MAX_NOTICES = 3;

// New objects seen less than that on average are removed on applying. Measured on a loose pile of 900 in
// walls, about 10 objects across: of them go 25%, 39%, 53%; pixels that change, seen from all around: 0.06%,
// 0.4%, 2.3%.
const float HIDDEN_LEVELS[] = {0.0f, 0.002f, 0.01f, 0.05f};
const int NUM_HIDDEN_LEVELS = 4;
const int DEFAULT_HIDDEN_LEVEL = 2;
// What is dragged was in the world before: it goes only when asked.
const int DEFAULT_DRAG_HIDDEN_LEVEL = 0;

// The letter of an axis in its color, in the middle of a dial, not of a dial with what is written under it.
QLabel *make_axis_letter(int axis)
{
	QLabel *letter = new QLabel(QString::fromUtf8(AXIS_NAMES[axis]));
	letter->setStyleSheet(QString::fromUtf8("color: %1; font-weight: bold;").arg(axisColor(axis)));
	letter->setFixedHeight(DIAL_SIZE);
	return letter;
}

// What the objects fall on by default: not on grass and small things of a clutter.
SurfaceFilter default_filter()
{
	SurfaceFilter filter;
	filter.surface_types = SurfaceFilter::MESH_STATIC | SurfaceFilter::MESH_SKINNED | SurfaceFilter::MESH_DYNAMIC
		| SurfaceFilter::MESH_CLUSTER | SurfaceFilter::TERRAIN;
	return filter;
}

// The icons of the shapes are drawn in code: the editor has none for them.
QIcon shape_icon(int kind)
{
	QPixmap pixmap(32, 32);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(iconColor(), 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	painter.setBrush(Qt::NoBrush);

	switch (kind)
	{
		case PhysicsDrop::SHAPE_BOX:
			painter.drawRect(QRectF(5, 11, 16, 16));
			painter.drawPolyline(QPolygonF({QPointF(5, 11), QPointF(11, 5), QPointF(27, 5), QPointF(27, 21), QPointF(21, 27)}));
			painter.drawLine(QPointF(21, 11), QPointF(27, 5));
			break;
		case PhysicsDrop::SHAPE_SPHERE:
			painter.drawEllipse(QRectF(4, 4, 24, 24));
			painter.drawArc(QRectF(4, 11, 24, 10), 180 * 16, 180 * 16);
			break;
		case PhysicsDrop::SHAPE_CAPSULE:
			painter.drawRoundedRect(QRectF(9, 3, 14, 26), 7, 7);
			break;
		case PhysicsDrop::SHAPE_CYLINDER:
			painter.drawEllipse(QRectF(6, 4, 20, 8));
			painter.drawLine(QPointF(6, 8), QPointF(6, 24));
			painter.drawLine(QPointF(26, 8), QPointF(26, 24));
			painter.drawArc(QRectF(6, 20, 20, 8), 180 * 16, 180 * 16);
			break;
		case PhysicsDrop::SHAPE_HULL:
			painter.drawPolygon(QPolygonF({QPointF(12, 4), QPointF(25, 7), QPointF(29, 19), QPointF(20, 28), QPointF(6, 25), QPointF(3, 12)}));
			break;
		case PhysicsDrop::SHAPE_PIECES:
			painter.drawPolygon(QPolygonF({QPointF(4, 5), QPointF(13, 4), QPointF(14, 16), QPointF(5, 17)}));
			painter.drawPolygon(QPolygonF({QPointF(18, 5), QPointF(28, 8), QPointF(26, 18), QPointF(18, 15)}));
			painter.drawPolygon(QPolygonF({QPointF(7, 22), QPointF(25, 22), QPointF(24, 28), QPointF(9, 28)}));
			break;
		default:
		{
			// Auto: a magic star.
			QPainterPath star;
			star.moveTo(16, 3);
			star.quadTo(16, 16, 29, 16);
			star.quadTo(16, 16, 16, 29);
			star.quadTo(16, 16, 3, 16);
			star.quadTo(16, 16, 16, 3);
			painter.drawPath(star);
			break;
		}
	}
	return QIcon(pixmap);
}

// A hand-like mark for dragging: four arrows out of a point.
QIcon drag_icon()
{
	QPixmap pixmap(32, 32);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(iconColor(), 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	painter.drawLine(QPointF(16, 4), QPointF(16, 28));
	painter.drawLine(QPointF(4, 16), QPointF(28, 16));
	painter.drawPolyline(QPolygonF({QPointF(12, 8), QPointF(16, 4), QPointF(20, 8)}));
	painter.drawPolyline(QPolygonF({QPointF(12, 24), QPointF(16, 28), QPointF(20, 24)}));
	painter.drawPolyline(QPolygonF({QPointF(8, 12), QPointF(4, 16), QPointF(8, 20)}));
	painter.drawPolyline(QPolygonF({QPointF(24, 12), QPointF(28, 16), QPointF(24, 20)}));
	return QIcon(pixmap);
}

// A thing with a shiver on both sides of it.
QIcon shake_icon()
{
	QPixmap pixmap(32, 32);
	pixmap.fill(Qt::transparent);
	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(iconColor(), 2.4, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	painter.drawRoundedRect(QRectF(11, 7, 10, 18), 2, 2);
	painter.drawPolyline(QPolygonF({QPointF(6, 7), QPointF(3, 12), QPointF(6, 16), QPointF(3, 20), QPointF(6, 25)}));
	painter.drawPolyline(QPolygonF({QPointF(26, 7), QPointF(29, 12), QPointF(26, 16), QPointF(29, 20), QPointF(26, 25)}));
	return QIcon(pixmap);
}

} // namespace

PhysicsDropPage::PhysicsDropPage(QWidget *parent)
	: QWidget(parent)
{
	drop_ = new PhysicsDrop(this);
	drop_->on_changed = [this]() { refresh(); };
	drop_->on_message = [this](int level, const QString &text) { append_log(level, text); };
	brush_ = new PhysicsBrush(drop_, this);
	brush_->on_settings_changed = [this]() {
		// The wheel changed something: the fields show it.
		updating_ = true;
		radius_->setValue(brush_->getSettings().radius);
		height_->setValue(brush_->getSettings().height);
		zone_->setValue(brush_->getSettings().zone);
		updating_ = false;
	};

	build_ui();
	load_settings();

	connect(Selection::instance(), &Selection::changed, this, [this]() { refresh(); });
	refresh();
}

PhysicsDropPage::~PhysicsDropPage()
{
	save_settings();
	drop_->on_changed = nullptr;
	drop_->on_message = nullptr;
	brush_->on_settings_changed = nullptr;
}

void PhysicsDropPage::stop()
{
	brush_->stop();
}

void PhysicsDropPage::finishRunning()
{
	if (drop_->isRunning())
		drop_->finish();
}

void PhysicsDropPage::hideEvent(QHideEvent *event)
{
	QWidget::hideEvent(event);

	// Another tool of the window is open, the stack of the tabs has hidden this very page. The mouse and
	// the keys are that tool's now. All the rest (a tab of the dock area, docking) hides what the page is
	// in: see ArtistToolWindow::hideEvent().
	QTimer::singleShot(0, this, [this]() {
		if (isHidden())
			finishRunning();
	});
}

QDoubleSpinBox *PhysicsDropPage::make_spin_box(double minimum, double maximum, double step, int decimals, const QString &tooltip)
{
	QDoubleSpinBox *box = new QDoubleSpinBox;
	box->setRange(minimum, maximum);
	box->setSingleStep(step);
	box->setDecimals(decimals);
	box->setToolTip(tooltip);
	set_up_number_field(box);
	connect(box, &QDoubleSpinBox::valueChanged, this, [this](double) { apply_brush_settings(); });
	return box;
}

QSlider *PhysicsDropPage::make_slider(QSpinBox *&box, int minimum, int maximum, const QString &tooltip)
{
	box = new QSpinBox;
	box->setRange(minimum, maximum);
	box->setToolTip(tooltip);
	set_up_number_field(box);
	connect(box, &QSpinBox::valueChanged, this, [this](int) { apply_brush_settings(); });

	QSlider *slider = new QSlider(Qt::Horizontal);
	slider->setRange(minimum, maximum);
	slider->setFocusPolicy(Qt::NoFocus);
	slider->setToolTip(tooltip);
	// The wheel scrolls the page here too.
	slider->installEventFilter(this);
	connect(slider, &QSlider::valueChanged, box, &QSpinBox::setValue);
	connect(box, &QSpinBox::valueChanged, slider, &QSlider::setValue);
	return slider;
}

QWidget *PhysicsDropPage::make_dial(QDial *&dial, const QString &caption, const QString &tooltip)
{
	dial = new QDial;
	dial->setRange(-180, 180);
	dial->setSingleStep(1);
	dial->setPageStep(15);
	dial->setNotchesVisible(true);
	dial->setNotchTarget(12.0);
	dial->setFixedSize(DIAL_SIZE, DIAL_SIZE);
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
		apply_brush_settings();
	});

	QWidget *cell = new QWidget;
	// Wide enough for the longest text, so the row does not jump.
	cell->setMinimumWidth(qMax(DIAL_SIZE, value->fontMetrics().horizontalAdvance(uiText("Max -180°", "Макс -180°")) + 2));
	QVBoxLayout *layout = new QVBoxLayout(cell);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(1);
	layout->addWidget(dial, 0, Qt::AlignHCenter);
	layout->addWidget(value);
	return cell;
}

QWidget *PhysicsDropPage::make_angle_cell(QDial *&min_dial, QDial *&max_dial, const QString &tooltip)
{
	QWidget *cell = new QWidget;
	QHBoxLayout *layout = new QHBoxLayout(cell);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(2);
	layout->addWidget(make_dial(min_dial, uiText("Min", "Мин"), tooltip));
	layout->addWidget(make_dial(max_dial, uiText("Max", "Макс"), tooltip));
	return cell;
}

QHBoxLayout *PhysicsDropPage::make_hidden_row(QButtonGroup *&group, const QString &tooltip)
{
	group = new QButtonGroup(this);
	QHBoxLayout *row = new QHBoxLayout;
	row->setSpacing(2);
	const QString names[NUM_HIDDEN_LEVELS] = {uiText("No", "Нет"), uiText("Little", "Мало"), uiText("Medium", "Средне"), uiText("Much", "Много")};
	for (int i = 0; i < NUM_HIDDEN_LEVELS; ++i)
	{
		QToolButton *button = new QToolButton;
		button->setText(names[i]);
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		button->setToolTip(tooltip);
		button->setStyleSheet(choiceButtonStyle());
		group->addButton(button, i);
		row->addWidget(button);
	}
	return row;
}

void PhysicsDropPage::set_up_number_field(QAbstractSpinBox *box)
{
	box->setFocusPolicy(Qt::StrongFocus);
	box->installEventFilter(this);
	enableSpinBoxDrag(box);
}

bool PhysicsDropPage::eventFilter(QObject *watched, QEvent *event)
{
	// The wheel scrolls the page, it does not change the field that happens to be under the mouse.
	if (event->type() == QEvent::Wheel)
	{
		const QWidget *widget = qobject_cast<QWidget *>(watched);
		if (widget && !widget->hasFocus())
		{
			event->ignore();
			return true;
		}

		// One wheel notch turns a dial by exactly one degree (Qt would turn it by several, as many as
		// the system scrolls lines per notch).
		if (QDial *dial = qobject_cast<QDial *>(watched))
		{
			const int delta = static_cast<QWheelEvent *>(event)->angleDelta().y();
			if (delta != 0)
				dial->setValue(dial->value() + (delta > 0 ? 1 : -1));
			event->accept();
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

void PhysicsDropPage::build_ui()
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

	// The text is of a different length in every mode: above the buttons it would make them jump.
	layout->addWidget(build_modes());
	description_ = new QLabel;
	description_->setWordWrap(true);
	layout->addWidget(description_);
	layout->addWidget(build_assets_group());
	layout->addWidget(build_brush_group());
	layout->addWidget(build_drag_group());
	layout->addWidget(build_options_group());
	layout->addWidget(build_result_group());

	// These can be changed while they fall too, so they are not among what gets locked.
	const QString dome_tooltip = loc_.tip(
		"The physics goes on only inside this sphere in front of the camera; what is out of it stands "
		"still. The number is its radius, the checkbox only draws it.",
		"Физика идёт только внутри этой сферы перед камерой; что снаружи, стоит на месте. Число - её "
		"радиус, галка только рисует её.");
	QHBoxLayout *view_row = new QHBoxLayout;
	view_row->addWidget(new QLabel(uiText("In the viewport", "Во вьюпорте")));
	show_dome_ = new QCheckBox(uiText("Dome", "Купол"));
	show_dome_->setToolTip(dome_tooltip);
	connect(show_dome_, &QCheckBox::toggled, this, [this](bool show) { drop_->setShowDome(show); });
	view_row->addWidget(show_dome_);
	dome_radius_ = new QDoubleSpinBox;
	dome_radius_->setRange(1.0, 5000.0);
	dome_radius_->setSingleStep(5.0);
	dome_radius_->setDecimals(0);
	dome_radius_->setToolTip(dome_tooltip);
	set_up_number_field(dome_radius_);
	connect(dome_radius_, &QDoubleSpinBox::valueChanged, this, [this](double radius) { drop_->setDomeRadius(float(radius)); });
	view_row->addWidget(dome_radius_);
	view_row->addSpacing(8);
	show_shapes_ = new QCheckBox(uiText("Shapes", "Шейпы"));
	show_shapes_->setToolTip(loc_.tip(
		"Draw the shapes the objects fall in, to see how well they fit.",
		"Рисовать формы, в которых падают объекты: видно, насколько они подходят."));
	connect(show_shapes_, &QCheckBox::toggled, this, [this](bool show) { drop_->setShowShapes(show); });
	view_row->addWidget(show_shapes_);
	view_row->addStretch(1);
	layout->addLayout(view_row);

	layout->addWidget(build_buttons());

	status_label_ = new QLabel;
	status_label_->setWordWrap(true);
	layout->addWidget(status_label_);

	// The log is usually folded: what is not to be missed is here as well.
	notice_label_ = new QLabel;
	notice_label_->setWordWrap(true);
	notice_label_->setVisible(false);
	layout->addWidget(notice_label_);

	hotkeys_ = new QLabel;
	hotkeys_->setWordWrap(true);
	hotkeys_->setContentsMargins(6, 2, 6, 6);
	hotkeys_header_ = makeSectionHeader(loc_.hotkeys("Hotkeys", "Горячие клавиши"), hotkeys_);
	hotkeys_header_->setToolTip(loc_.tip("Keys and mouse actions of the tool.", "Клавиши и действия мышью для этого инструмента."));
	layout->addWidget(hotkeys_header_);
	layout->addWidget(hotkeys_);

	log_ = new QPlainTextEdit;
	log_->setReadOnly(true);
	log_->setMaximumBlockCount(500);
	log_->setFixedHeight(160);
	const QString log_tooltip = loc_.tip(
		"The shape every object got, and how it went.",
		"Какую форму получил каждый объект и как всё прошло.");
	log_->setToolTip(log_tooltip);
	log_header_ = makeSectionHeader(uiText("Log", "Журнал"), log_);
	log_header_->setToolTip(log_tooltip);
	layout->addWidget(log_header_);
	layout->addWidget(log_);
	layout->addStretch(1);
}

QWidget *PhysicsDropPage::build_modes()
{
	QWidget *row = new QWidget;
	QHBoxLayout *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(2);

	struct ModeName
	{
		int id;
		QString name;
		QIcon icon;
		QString tooltip;
	};
	const ModeName modes[] = {
		{PAGE_DROP, uiText("Drop Selected", "Уронить выделенное"), QIcon(QString::fromUtf8(":/images/tools/icon_drop_to_ground.png")),
			loc_.tip("The objects selected in the world fall and settle.", "Выделенные в мире объекты падают и укладываются.")},
		{PAGE_BRUSH, uiText("Brush", "Кисть"), QIcon(QString::fromUtf8(":/images/icon_brush.png")),
			loc_.tip("Pour new objects from a list of assets.", "Сыпать новые объекты из списка ассетов.")},
		{PAGE_DRAG, uiText("Drag", "Перетаскивание"), drag_icon(),
			loc_.tip("Take objects of the world with the mouse, push and pull them.", "Брать объекты мира мышью, толкать и притягивать их.")},
	};
	mode_ = new QButtonGroup(this);
	mode_->setExclusive(true);
	for (const ModeName &mode : modes)
	{
		QToolButton *button = new QToolButton;
		button->setText(mode.name);
		button->setIcon(mode.icon);
		button->setIconSize(QSize(16, 16));
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		button->setToolTip(mode.tooltip);
		button->setStyleSheet(choiceButtonStyle());
		mode_->addButton(button, mode.id);
		mode_buttons_.append(button);
		layout->addWidget(button);
	}
	connect(mode_, &QButtonGroup::idClicked, this, [this](int) {
		apply_brush_settings();
		refresh();
	});
	return row;
}

QGroupBox *PhysicsDropPage::build_assets_group()
{
	assets_group_ = new QGroupBox(uiText("What to Pour", "Что сыпать"));
	QVBoxLayout *layout = new QVBoxLayout(assets_group_);

	assets_empty_ = new QLabel(uiText(
		"The list is empty. Select meshes or nodes in the Asset Browser and press Add Selected Assets.",
		"Список пуст. Выберите меши или ноды в Asset Browser и нажмите «Добавить выбранные ассеты»."));
	assets_empty_->setWordWrap(true);
	assets_empty_->setEnabled(false);
	layout->addWidget(assets_empty_);

	assets_rows_ = new QVBoxLayout;
	assets_rows_->setSpacing(2);
	layout->addLayout(assets_rows_);

	QHBoxLayout *buttons = new QHBoxLayout;
	QPushButton *add = new QPushButton(uiText("Add Selected Assets", "Добавить выбранные ассеты"));
	add->setIcon(QIcon(QString::fromUtf8(":/images/mods/icon_object_mode.png")));
	add->setIconSize(QSize(16, 16));
	add->setMinimumHeight(LIST_BUTTON_HEIGHT);
	add->setFocusPolicy(Qt::NoFocus);
	add->setToolTip(loc_.tip(
		"Add the meshes and nodes selected in the Asset Browser. Every new object is a random asset of the list.",
		"Добавить меши и ноды, выбранные в Asset Browser. Каждый новый объект - случайный ассет из списка."));
	connect(add, &QPushButton::clicked, this, [this]() { add_selected_assets(); });
	buttons->addWidget(add, 1);
	assets_clear_ = new QPushButton(uiText("Clear", "Очистить"));
	assets_clear_->setIcon(QIcon(QString::fromUtf8(":/images/icon_delete_trash.png")));
	assets_clear_->setIconSize(QSize(16, 16));
	assets_clear_->setMinimumHeight(LIST_BUTTON_HEIGHT);
	assets_clear_->setFocusPolicy(Qt::NoFocus);
	assets_clear_->setToolTip(loc_.tip("Remove all the assets from the list.", "Убрать все ассеты из списка."));
	connect(assets_clear_, &QPushButton::clicked, this, [this]() {
		assets_.clear();
		refresh_assets();
	});
	buttons->addWidget(assets_clear_);
	layout->addLayout(buttons);
	return assets_group_;
}

QGroupBox *PhysicsDropPage::build_brush_group()
{
	const PhysicsBrush::Settings defaults;
	const QString reset_tooltip = loc_.tip("Reset to default", "Сбросить на значение по умолчанию");

	brush_group_ = new QGroupBox(uiText("How to Pour", "Как сыпать"));
	QGridLayout *form = new QGridLayout(brush_group_);
	form->setColumnStretch(COLUMN_FIELD_1, 1);
	form->setColumnStretch(COLUMN_FIELD_2, 1);
	const auto add_label = [form](int row, const QString &name, const QString &tooltip) {
		QLabel *label = new QLabel(name);
		label->setToolTip(tooltip);
		form->addWidget(label, row, COLUMN_LABEL);
	};
	const auto add_second_caption = [form](int row, const QString &name, const QString &tooltip) {
		QLabel *label = new QLabel(name);
		label->setToolTip(tooltip);
		label->setContentsMargins(8, 0, 0, 0);
		form->addWidget(label, row, COLUMN_CAPTION_2);
	};
	int form_row = 0;

	const QString count_tooltip = loc_.tip("How many objects one click puts down.", "Сколько объектов насыпает один клик.");
	add_label(form_row, uiText("Per click", "За клик"), count_tooltip);
	form->addWidget(make_slider(count_, 1, 50, count_tooltip), form_row, COLUMN_FIELD_1, 1, 2);
	form->addWidget(count_, form_row++, COLUMN_FIELD_2);

	const QString rate_tooltip = loc_.tip(
		"How many objects a second are poured while the button is held.",
		"Сколько объектов в секунду сыплется, пока кнопка зажата.");
	add_label(form_row, uiText("Per second", "В секунду"), rate_tooltip);
	form->addWidget(make_slider(rate_, 1, 50, rate_tooltip), form_row, COLUMN_FIELD_1, 1, 2);
	form->addWidget(rate_, form_row++, COLUMN_FIELD_2);

	radius_ = make_spin_box(0.02, 100.0, 0.1, 2, loc_.tip(
		"The radius of the brush, in units. The wheel over the viewport changes it.",
		"Радиус кисти в юнитах. Меняется колесом над вьюпортом."));
	height_ = make_spin_box(0.0, 100.0, 0.1, 2, loc_.tip(
		"How high above the surface the objects appear, in units. Ctrl + wheel over the viewport changes it.",
		"На какой высоте над поверхностью появляются объекты, в юнитах. Меняется через Ctrl + колесо над вьюпортом."));
	add_label(form_row, uiText("Radius", "Радиус"), radius_->toolTip());
	form->addWidget(radius_, form_row, COLUMN_FIELD_1);
	add_second_caption(form_row, uiText("Height", "Высота"), height_->toolTip());
	form->addWidget(height_, form_row++, COLUMN_FIELD_2);

	const QString scale_tooltip = loc_.tip(
		"Every object gets a random scale between Min and Max. 1 - the original size.",
		"Каждый объект получает случайный масштаб между [[Min|Мин]] и [[Max|Макс]]. 1 - исходный размер.");
	add_label(form_row, uiText("Scale", "Масштаб"), scale_tooltip);
	scale_min_ = make_spin_box(0.001, 1000.0, 0.05, 3, scale_tooltip);
	scale_max_ = make_spin_box(0.001, 1000.0, 0.05, 3, scale_tooltip);
	QLabel *scale_min_caption = new QLabel(uiText("Min", "Мин"));
	scale_min_caption->setToolTip(scale_tooltip);
	form->addWidget(scale_min_caption, form_row, COLUMN_CAPTION_1);
	form->addWidget(scale_min_, form_row, COLUMN_FIELD_1);
	add_second_caption(form_row, uiText("Max", "Макс"), scale_tooltip);
	form->addWidget(scale_max_, form_row, COLUMN_FIELD_2);
	form->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() {
		updating_ = true;
		scale_min_->setValue(defaults.scale_min);
		scale_max_->setValue(defaults.scale_max);
		updating_ = false;
		apply_brush_settings();
	}), form_row++, COLUMN_RESET);

	const QString rotation_tooltip = loc_.tip(
		"Every object is turned by a random angle between Min and Max, in degrees. Min -180 and Max 180 - any direction.",
		"Каждый объект поворачивается на случайный угол между [[Min|Мин]] и [[Max|Макс]], в градусах. "
		"[[Min|Мин]] -180 и [[Max|Макс]] 180 - любое направление.");
	add_label(form_row, uiText("Rotation", "Поворот"), rotation_tooltip);
	// From the left edge of the fields to their right edge, with even gaps; X has its letter among the
	// captions, for its dials to start where the fields do.
	form->addWidget(make_axis_letter(0), form_row, COLUMN_CAPTION_1, Qt::AlignRight | Qt::AlignTop);
	QHBoxLayout *rotation_row = new QHBoxLayout;
	for (int axis = 0; axis < 3; ++axis)
	{
		if (axis > 0)
		{
			rotation_row->addStretch(1);
			rotation_row->addWidget(make_axis_letter(axis), 0, Qt::AlignTop);
		}
		rotation_row->addWidget(make_angle_cell(rotation_min_[axis], rotation_max_[axis], rotation_tooltip));
	}
	form->addLayout(rotation_row, form_row, COLUMN_FIELD_1, 1, COLUMN_RESET - COLUMN_FIELD_1);
	form->addWidget(makeResetButton(reset_tooltip, this, [this, defaults]() {
		updating_ = true;
		for (int axis = 0; axis < 3; ++axis)
		{
			rotation_min_[axis]->setValue(qRound(defaults.rotation_min[axis]));
			rotation_max_[axis]->setValue(qRound(defaults.rotation_max[axis]));
		}
		updating_ = false;
		apply_brush_settings();
	}), form_row++, COLUMN_RESET);

	clear_new_ = new QPushButton(uiText("Remove All Poured", "Убрать всё насыпанное"));
	clear_new_->setIcon(QIcon(QString::fromUtf8(":/images/icon_delete_trash.png")));
	clear_new_->setFocusPolicy(Qt::NoFocus);
	clear_new_->setToolTip(loc_.tip(
		"Remove the new objects of this session; the session goes on.",
		"Убрать новые объекты этого сеанса; сеанс продолжается."));
	connect(clear_new_, &QPushButton::clicked, this, [this]() {
		brush_->clearPending();
		drop_->clearNew();
	});
	form->addWidget(clear_new_, form_row++, COLUMN_LABEL, 1, COLUMN_RESET + 1);
	return brush_group_;
}

QGroupBox *PhysicsDropPage::build_drag_group()
{
	drag_group_ = new QGroupBox(uiText("What Moves", "Что двигается"));
	QFormLayout *form = new QFormLayout(drag_group_);

	zone_ = make_spin_box(0.05, 100.0, 0.25, 2, loc_.tip(
		"The radius of the zone around the mouse, in units. Objects in it wake up and take part in "
		"the physics, the others stay still. The wheel over the viewport changes it.",
		"Радиус зоны вокруг мыши в юнитах. Объекты в ней просыпаются и участвуют в физике, остальные "
		"стоят на месте. Меняется колесом над вьюпортом."));

	const QString take_tooltip = loc_.tip(
		"Mouse - a click takes an object, and it follows the mouse.\n"
		"Gizmo - a click selects an object, and it is moved or turned with the gizmo of the editor.",
		"Мышью - клик берёт объект, и он идёт за курсором.\n"
		"Гизмо - клик выделяет объект, а двигать и поворачивать его можно гизмо редактора.");
	take_with_ = new QButtonGroup(this);
	QHBoxLayout *take_row = new QHBoxLayout;
	take_row->setSpacing(2);
	const QString take_names[] = {uiText("Mouse", "Мышью"), uiText("Gizmo", "Гизмо")};
	const QIcon take_icons[] = {QIcon(QString::fromUtf8(":/images/icons_smudge.png")),
		QIcon(QString::fromUtf8(":/images/tools/icon_translate.png"))};
	for (int i = 0; i < 2; ++i)
	{
		QToolButton *button = new QToolButton;
		button->setText(take_names[i]);
		button->setIcon(take_icons[i]);
		button->setIconSize(QSize(16, 16));
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		button->setToolTip(take_tooltip);
		button->setStyleSheet(choiceButtonStyle());
		take_with_->addButton(button, i);
		take_row->addWidget(button);
	}
	connect(take_with_, &QButtonGroup::idClicked, this, [this](int) {
		apply_brush_settings();
		refresh();
	});
	QLabel *take_label = new QLabel(uiText("Take with", "Брать"));
	take_label->setToolTip(take_tooltip);
	form->addRow(take_label, take_row);

	form->addRow(uiText("Zone", "Зона"), zone_);

	max_size_ = make_spin_box(0.05, 100.0, 0.25, 2, loc_.tip(
		"Bigger objects do not move: they are what the others lie on. Size across, in units. "
		"Objects with the Immovable flag do not move either.",
		"Объекты крупнее не двигаются: на них лежат остальные. Размер по диагонали, в юнитах. "
		"Объекты с флагом Immovable тоже не двигаются."));
	form->addRow(uiText("Max size", "Макс. размер"), max_size_);

	force_ = make_spin_box(0.5, 100.0, 1.0, 1, loc_.tip(
		"How hard Shift pushes and Ctrl pulls.",
		"Насколько сильно толкает Shift и притягивает Ctrl."));
	form->addRow(uiText("Force", "Сила"), force_);

	only_selected_ = new QCheckBox(uiText("Only Selected", "Только выделенные"));
	only_selected_->setToolTip(loc_.tip(
		"Only the objects selected in the world move.",
		"Двигаются только выделенные в мире объекты."));
	connect(only_selected_, &QCheckBox::toggled, this, [this](bool) { apply_brush_settings(); });
	form->addRow(only_selected_);
	return drag_group_;
}

QGroupBox *PhysicsDropPage::build_options_group()
{
	options_group_ = new QGroupBox(uiText("Physics", "Физика"));
	options_layout_ = new QFormLayout(options_group_);

	// The shape.
	struct ShapeName
	{
		int id;
		QString name;
	};
	const ShapeName shapes[] = {
		{PhysicsDrop::SHAPE_AUTO, uiText("Auto", "Авто")},
		{PhysicsDrop::SHAPE_HULL, uiText("Hull", "Оболочка")},
		{PhysicsDrop::SHAPE_PIECES, uiText("Pieces", "Куски")},
		{PhysicsDrop::SHAPE_BOX, uiText("Box", "Коробка")},
		{PhysicsDrop::SHAPE_SPHERE, uiText("Sphere", "Шар")},
		{PhysicsDrop::SHAPE_CAPSULE, uiText("Capsule", "Капсула")},
		{PhysicsDrop::SHAPE_CYLINDER, uiText("Cylinder", "Цилиндр")},
	};
	const QString shape_tooltip = loc_.tip(
		"The shape an object falls in, measured from its mesh.\n"
		"Auto - the object's own shapes if it has them; else a simple shape that fits the mesh "
		"closely, or Hull.\n"
		"Hull - follows the mesh, without its hollows.\n"
		"Pieces - for hollow things (a bucket, a chair); slower to start.",
		"Форма, в которой объект падает; строится по его мешу.\n"
		"[[Auto|Авто]] - свои шейпы объекта, если они есть; иначе простая форма, плотно "
		"облегающая меш, или [[Hull|Оболочка]].\n"
		"[[Hull|Оболочка]] - по форме меша, но без впадин.\n"
		"[[Pieces|Куски]] - для полых вещей (ведро, стул); запускается дольше.");
	shape_ = new QButtonGroup(this);
	shape_->setExclusive(true);
	QGridLayout *shape_grid = new QGridLayout;
	shape_grid->setContentsMargins(0, 0, 0, 0);
	shape_grid->setSpacing(2);
	int shape_index = 0;
	for (const ShapeName &shape : shapes)
	{
		QToolButton *button = new QToolButton;
		button->setText(shape.name);
		button->setIcon(shape_icon(shape.id));
		button->setIconSize(QSize(16, 16));
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		button->setToolTip(shape_tooltip);
		button->setStyleSheet(choiceButtonStyle());
		shape_->addButton(button, shape.id);
		// The first row - what follows the mesh, the second - the simple shapes; both go the whole width.
		const int row = shape_index < 3 ? 0 : 1;
		const int span = shape_index < 3 ? 4 : 3;
		const int column = (shape_index < 3 ? shape_index : shape_index - 3) * span;
		shape_grid->addWidget(button, row, column, 1, span);
		++shape_index;
	}
	for (int column = 0; column < 12; ++column)
		shape_grid->setColumnStretch(column, 1);
	QLabel *shape_label = new QLabel(uiText("Shape", "Форма"));
	shape_label->setToolTip(shape_tooltip);
	options_layout_->addRow(shape_label, shape_grid);

	ignore_own_shapes_ = new QCheckBox(uiText("Ignore Prepared Shapes", "Без заготовленных шейпов"));
	ignore_own_shapes_->setToolTip(loc_.tip(
		"The shapes the falling objects came with are not used: the shape is measured from the mesh.",
		"Шейпы, заготовленные у падающих объектов, не используются: форма строится по мешу."));
	connect(shape_, &QButtonGroup::idClicked, this, [this](int) { refresh_shape_options(); });

	// What they fall on.
	SurfaceFilterButton::Texts texts;
	texts.tooltip = loc_.tip(
		"What the objects fall on. An object of a checked kind is solid for the time of the "
		"fall, even without collision surfaces; the other kinds are passed through.",
		"На что объекты падают. Объект отмеченного типа на время падения твёрдый, даже если "
		"у него нет поверхностей коллизии; сквозь остальные типы объекты пролетают.");
	texts.only_immovable = uiText("Fall Only On Immovable Nodes", "Только на ноды с флагом Immovable");
	texts.only_immovable_tooltip = loc_.tip(
		"Fall only on nodes that have the Immovable flag enabled.",
		"Падать только на ноды с включённым флагом Immovable.");
	texts.only_intersection = uiText("Fall Only On Surfaces With Intersection", "Только на поверхности с Intersection");
	texts.only_intersection_tooltip = loc_.tip(
		"Fall only on surfaces that have the Intersection flag and any bit of the "
		"Intersection Mask enabled.",
		"Падать только на поверхности с включённым флагом Intersection и хотя бы одним "
		"битом Intersection Mask.");
	texts.reset_tooltip = loc_.tip("Reset to default", "Сбросить на значение по умолчанию");
	texts.default_filter = default_filter();
	// They have no collision surfaces.
	texts.hidden_types = SurfaceFilter::WATER | SurfaceFilter::CLOUDS;
	filter_ = new SurfaceFilterButton(texts);
	options_layout_->addRow(uiText("Fall on", "Падать на"), filter_);

	friction_ = new QDoubleSpinBox;
	friction_->setRange(0.05, 2.0);
	friction_->setSingleStep(0.05);
	friction_->setDecimals(2);
	friction_->setToolTip(loc_.tip(
		"Less - objects slide to steady places and settle sooner, the pile is flatter.\n"
		"More - they hold on to each other: a taller pile, but with odd poses.",
		"Меньше - объекты соскальзывают в устойчивые положения и быстрее укладываются, куча ниже.\n"
		"Больше - цепляются друг за друга: куча выше, но бывают странные позы."));
	set_up_number_field(friction_);

	// How long.
	max_time_ = new QDoubleSpinBox;
	max_time_->setRange(1.0, 600.0);
	max_time_->setSingleStep(1.0);
	max_time_->setDecimals(0);
	max_time_->setSuffix(uiText(" s", " с"));
	max_time_->setToolTip(loc_.tip(
		"What has not settled by then stays where it is at that moment.",
		"То, что не улеглось к этому времени, останется там, где окажется."));
	set_up_number_field(max_time_);
	max_time_label_ = new QLabel(uiText("Time limit", "Предел времени"));
	max_time_label_->setToolTip(max_time_->toolTip());
	max_time_label_->setContentsMargins(8, 0, 0, 0);

	lock_rotation_ = new QCheckBox(uiText("No Turning", "Без вращения"));
	lock_rotation_->setToolTip(loc_.tip(
		"The objects fall and move without turning, whatever hits them.",
		"Объекты падают и двигаются, не поворачиваясь, что бы их ни задело."));

	QHBoxLayout *motion_row = new QHBoxLayout;
	motion_row->addWidget(friction_);
	motion_row->addWidget(max_time_label_);
	motion_row->addWidget(max_time_);
	motion_row->addSpacing(8);
	motion_row->addWidget(lock_rotation_);
	motion_row->addSpacing(8);
	motion_row->addWidget(ignore_own_shapes_);
	motion_row->addStretch(1);
	options_layout_->addRow(uiText("Friction", "Трение"), motion_row);
	return options_group_;
}

QGroupBox *PhysicsDropPage::build_result_group()
{
	result_group_ = new QGroupBox(uiText("On Apply", "При применении"));
	result_layout_ = new QFormLayout(result_group_);

	// What the new objects become.
	const QString result_tooltip = loc_.tip(
		"Nodes - every object can be moved later.\n"
		"Cluster - lighter for the scene: a cluster per mesh, but objects can not be moved one "
		"by one. What has more than meshes inside stays a node.",
		"Ноды - каждый объект можно потом двигать.\n"
		"Кластер - легче для сцены: по кластеру на меш, но двигать поштучно нельзя. Объекты, "
		"где есть не только меши, остаются нодами.");
	result_ = new QButtonGroup(this);
	result_row_ = new QHBoxLayout;
	result_row_->setSpacing(2);
	const QString results[] = {uiText("Nodes", "Ноды"), uiText("Cluster", "Кластер")};
	for (int i = 0; i < 2; ++i)
	{
		QToolButton *button = new QToolButton;
		button->setText(results[i]);
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		button->setToolTip(result_tooltip);
		button->setStyleSheet(choiceButtonStyle());
		result_->addButton(button, i);
		result_row_->addWidget(button);
	}
	connect(result_, &QButtonGroup::idClicked, this, [this](int id) { drop_->setAsClusters(id == 1); });
	QLabel *result_label = new QLabel(uiText("Result", "Результат"));
	result_label->setToolTip(result_tooltip);
	result_layout_->addRow(result_label, result_row_);

	const QString hidden_tooltip = loc_.tip(
		"On Apply, new objects hidden inside the pile are removed: it looks the same with fewer objects.\n"
		"The more goes, the more shows through the gaps. Do not move such a pile: it is hollow.",
		"При применении новые объекты, спрятанные внутри кучи, убираются: она выглядит так же, а объектов меньше.\n"
		"Чем больше убрано, тем заметнее просветы. Такую кучу лучше не двигать: она пустая внутри.");
	hidden_row_ = make_hidden_row(hidden_, hidden_tooltip);
	connect(hidden_, &QButtonGroup::idClicked, this, [this](int id) { drop_->setLeastSeen(HIDDEN_LEVELS[id]); });
	QLabel *hidden_label = new QLabel(uiText("Remove Hidden", "Убрать скрытые"));
	hidden_label->setToolTip(hidden_tooltip);
	result_layout_->addRow(hidden_label, hidden_row_);

	const QString drag_hidden_tooltip = loc_.tip(
		"On Apply, objects that took part and are hidden inside a pile are removed from the world: it looks the "
		"same with fewer objects. Ctrl+Z brings them back. Do not move such a pile: it is hollow.",
		"При применении объекты, которые участвовали и спрятаны внутри кучи, удаляются из мира: она выглядит "
		"так же, а объектов меньше. Ctrl+Z вернёт их. Такую кучу лучше не двигать: она пустая внутри.");
	drag_hidden_row_ = make_hidden_row(drag_hidden_, drag_hidden_tooltip);
	connect(drag_hidden_, &QButtonGroup::idClicked, this, [this](int id) { drop_->setLeastSeenExisting(HIDDEN_LEVELS[id]); });
	QLabel *drag_hidden_label = new QLabel(uiText("Remove Hidden", "Убрать скрытые"));
	drag_hidden_label->setToolTip(drag_hidden_tooltip);
	result_layout_->addRow(drag_hidden_label, drag_hidden_row_);
	return result_group_;
}

QWidget *PhysicsDropPage::build_buttons()
{
	QWidget *row = new QWidget;
	QHBoxLayout *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);

	main_button_ = new QPushButton;
	main_button_->setCheckable(true);
	main_button_->setMinimumHeight(36);
	// Space is for the pause: with the focus here it would press the button.
	main_button_->setFocusPolicy(Qt::NoFocus);
	// Green says what to press to begin; while it goes, orange, as the main buttons of the other pages.
	main_button_->setStyleSheet(goButtonStyle() + QString::fromUtf8(
		"QPushButton:checked { background-color: #e08a2a; }"
		"QPushButton:checked:hover { background-color: #f09c3e; }"));
	main_button_->setIcon(QIcon(QString::fromUtf8(":/images/tools/icon_play_physics.png")));
	main_button_->setIconSize(QSize(24, 24));
	connect(main_button_, &QPushButton::clicked, this, [this]() { press_main_button(); });
	layout->addWidget(main_button_, 1);

	jitter_button_ = new QPushButton(uiText("Shake", "Встряхнуть"));
	jitter_button_->setMinimumHeight(36);
	jitter_button_->setFocusPolicy(Qt::NoFocus);
	jitter_button_->setIcon(shake_icon());
	jitter_button_->setIconSize(QSize(16, 16));
	jitter_button_->setToolTip(loc_.tip(
		"A little random push to all the objects, to unstick what has got stuck.",
		"Небольшой случайный толчок всем объектам: помогает, если что-то застряло."));
	connect(jitter_button_, &QPushButton::clicked, this, [this]() { drop_->jitter(); });
	layout->addWidget(jitter_button_);

	cancel_button_ = new QPushButton(uiText("Cancel", "Отмена"));
	cancel_button_->setMinimumHeight(36);
	cancel_button_->setFocusPolicy(Qt::NoFocus);
	cancel_button_->setIcon(QIcon(QString::fromUtf8(":/images/icon_delete_cross.png")));
	cancel_button_->setIconSize(QSize(16, 16));
	cancel_button_->setToolTip(loc_.tip(
		"Put everything back as it was and remove the new objects (Esc).",
		"Вернуть всё как было и убрать новые объекты (Esc)."));
	connect(cancel_button_, &QPushButton::clicked, this, [this]() { drop_->cancel(); });
	layout->addWidget(cancel_button_);
	return row;
}

void PhysicsDropPage::load_settings()
{
	const PhysicsDrop::Options defaults;
	const PhysicsBrush::Settings brush_defaults;

	updating_ = true;

	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);

	QAbstractButton *mode_button = mode_->button(settings.value("mode", int(PAGE_DROP)).toInt());
	if (!mode_button)
		mode_button = mode_->button(int(PAGE_DROP));
	mode_button->setChecked(true);

	QAbstractButton *shape_button = shape_->button(settings.value("shape", int(defaults.shape)).toInt());
	if (!shape_button)
		shape_button = shape_->button(int(defaults.shape));
	shape_button->setChecked(true);
	ignore_own_shapes_->setChecked(!settings.value("use_own_shapes", defaults.use_own_shapes).toBool());

	SurfaceFilter filter = default_filter();
	filter.surface_types = settings.value("filter_types", filter.surface_types).toInt();
	filter.only_immovable = settings.value("filter_immovable", filter.only_immovable).toBool();
	filter.only_intersection = settings.value("filter_intersection", filter.only_intersection).toBool();
	filter_->setFilter(filter);

	friction_->setValue(settings.value("friction", defaults.friction).toDouble());
	lock_rotation_->setChecked(settings.value("lock_rotation", defaults.lock_rotation).toBool());
	max_time_->setValue(settings.value("fall_time", defaults.max_time).toDouble());
	show_shapes_->setChecked(settings.value("show_shapes", defaults.show_shapes).toBool());
	dome_radius_->setValue(settings.value("dome_radius", defaults.dome_radius).toDouble());
	show_dome_->setChecked(settings.value("show_dome", defaults.show_dome).toBool());
	hotkeys_header_->setChecked(settings.value("hotkeys_expanded", true).toBool());
	log_header_->setChecked(settings.value("log_expanded", false).toBool());

	count_->setValue(settings.value("count", brush_defaults.count).toInt());
	rate_->setValue(qRound(settings.value("rate", brush_defaults.rate).toDouble()));
	radius_->setValue(settings.value("radius", brush_defaults.radius).toDouble());
	height_->setValue(settings.value("height", brush_defaults.height).toDouble());
	scale_min_->setValue(settings.value("scale_min", brush_defaults.scale_min).toDouble());
	scale_max_->setValue(settings.value("scale_max", brush_defaults.scale_max).toDouble());
	for (int axis = 0; axis < 3; ++axis)
	{
		const QString axis_key = QString::fromUtf8(AXIS_KEYS[axis]);
		rotation_min_[axis]->setValue(qRound(settings.value(QString::fromUtf8("rotation_min") + axis_key, double(brush_defaults.rotation_min[axis])).toDouble()));
		rotation_max_[axis]->setValue(qRound(settings.value(QString::fromUtf8("rotation_max") + axis_key, double(brush_defaults.rotation_max[axis])).toDouble()));
	}
	if (QAbstractButton *result_button = result_->button(settings.value("as_clusters", defaults.as_clusters).toBool() ? 1 : 0))
		result_button->setChecked(true);
	if (QAbstractButton *hidden_button = hidden_->button(qBound(0, settings.value("hidden_level", DEFAULT_HIDDEN_LEVEL).toInt(), NUM_HIDDEN_LEVELS - 1)))
		hidden_button->setChecked(true);
	if (QAbstractButton *drag_hidden_button = drag_hidden_->button(qBound(0, settings.value("drag_hidden_level", DEFAULT_DRAG_HIDDEN_LEVEL).toInt(), NUM_HIDDEN_LEVELS - 1)))
		drag_hidden_button->setChecked(true);

	zone_->setValue(settings.value("zone", brush_defaults.zone).toDouble());
	max_size_->setValue(settings.value("max_size", brush_defaults.max_size).toDouble());
	only_selected_->setChecked(settings.value("only_selected", brush_defaults.only_selected).toBool());
	if (QAbstractButton *take_button = take_with_->button(settings.value("use_gizmo", brush_defaults.use_gizmo).toBool() ? 1 : 0))
		take_button->setChecked(true);
	force_->setValue(settings.value("force", brush_defaults.force).toDouble());
	settings.endGroup();

	// The assets, by the file they come from.
	assets_.clear();
	QSettings asset_settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	beginProjectGroup(asset_settings, SETTINGS_ASSETS_GROUP, {});
	const QStringList paths = asset_settings.value("assets").toStringList();
	for (const QString &path : paths)
	{
		PlaceableAsset asset;
		if (resolveAsset(String(path.toUtf8().constData()), asset))
			assets_.append(asset);
	}

	updating_ = false;
	refresh_assets();
	refresh_shape_options();
	apply_brush_settings();
}

void PhysicsDropPage::save_settings() const
{
	const PhysicsDrop::Options options = read_options();
	const PhysicsBrush::Settings brush = read_brush_settings();

	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);
	settings.setValue("mode", mode_->checkedId());
	settings.setValue("shape", int(options.shape));
	settings.setValue("use_own_shapes", options.use_own_shapes);
	settings.setValue("filter_types", options.filter.surface_types);
	settings.setValue("filter_immovable", options.filter.only_immovable);
	settings.setValue("filter_intersection", options.filter.only_intersection);
	settings.setValue("friction", options.friction);
	settings.setValue("lock_rotation", options.lock_rotation);
	settings.setValue("fall_time", options.max_time);
	settings.setValue("show_shapes", options.show_shapes);
	settings.setValue("dome_radius", options.dome_radius);
	settings.setValue("show_dome", options.show_dome);
	settings.setValue("hotkeys_expanded", hotkeys_header_->isChecked());
	settings.setValue("log_expanded", log_header_->isChecked());
	settings.setValue("as_clusters", options.as_clusters);
	settings.setValue("hidden_level", qMax(0, hidden_->checkedId()));
	settings.setValue("drag_hidden_level", qMax(0, drag_hidden_->checkedId()));
	settings.setValue("count", brush.count);
	settings.setValue("rate", brush.rate);
	settings.setValue("radius", brush.radius);
	settings.setValue("height", brush.height);
	settings.setValue("scale_min", brush.scale_min);
	settings.setValue("scale_max", brush.scale_max);
	for (int axis = 0; axis < 3; ++axis)
	{
		const QString axis_key = QString::fromUtf8(AXIS_KEYS[axis]);
		settings.setValue(QString::fromUtf8("rotation_min") + axis_key, double(brush.rotation_min[axis]));
		settings.setValue(QString::fromUtf8("rotation_max") + axis_key, double(brush.rotation_max[axis]));
	}
	settings.setValue("zone", brush.zone);
	settings.setValue("max_size", brush.max_size);
	settings.setValue("only_selected", brush.only_selected);
	settings.setValue("use_gizmo", brush.use_gizmo);
	settings.setValue("force", brush.force);
	settings.endGroup();

	QStringList paths;
	for (const PlaceableAsset &asset : assets_)
	{
		if (!asset.asset_path.empty())
			paths.append(QString::fromUtf8(asset.asset_path.get()));
	}
	QSettings asset_settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	beginProjectGroup(asset_settings, SETTINGS_ASSETS_GROUP, {});
	asset_settings.setValue("assets", paths);
}

void PhysicsDropPage::resetSettings()
{
	// What goes on ends with what it was started with; only then does the page have nothing to do with it.
	finishRunning();

	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.remove(QString::fromUtf8(SETTINGS_GROUP));
	removeProjectSettings(settings, SETTINGS_ASSETS_GROUP, {});

	// With nothing remembered every value is loaded as its default.
	load_settings();
	refresh();
}

PhysicsDrop::Options PhysicsDropPage::read_options() const
{
	PhysicsDrop::Options options;
	options.shape = PhysicsDrop::ShapeKind(qMax(0, shape_->checkedId()));
	options.use_own_shapes = !ignore_own_shapes_->isChecked();
	options.filter = filter_->getFilter();
	options.friction = float(friction_->value());
	options.lock_rotation = lock_rotation_->isChecked();
	options.max_time = max_time_->value();
	options.show_shapes = show_shapes_->isChecked();
	options.dome_radius = float(dome_radius_->value());
	options.show_dome = show_dome_->isChecked();
	options.as_clusters = result_->checkedId() == 1;
	options.least_seen = HIDDEN_LEVELS[qBound(0, hidden_->checkedId(), NUM_HIDDEN_LEVELS - 1)];
	options.least_seen_existing = HIDDEN_LEVELS[qBound(0, drag_hidden_->checkedId(), NUM_HIDDEN_LEVELS - 1)];
	return options;
}

PhysicsBrush::Settings PhysicsDropPage::read_brush_settings() const
{
	PhysicsBrush::Settings settings;
	settings.assets = assets_;
	settings.count = count_->value();
	settings.rate = float(rate_->value());
	settings.radius = float(radius_->value());
	settings.height = float(height_->value());
	settings.scale_min = float(scale_min_->value());
	settings.scale_max = float(scale_max_->value());
	settings.rotation_min = Math::vec3(float(rotation_min_[0]->value()), float(rotation_min_[1]->value()), float(rotation_min_[2]->value()));
	settings.rotation_max = Math::vec3(float(rotation_max_[0]->value()), float(rotation_max_[1]->value()), float(rotation_max_[2]->value()));
	settings.zone = float(zone_->value());
	settings.max_size = float(max_size_->value());
	settings.only_selected = only_selected_->isChecked();
	settings.use_gizmo = take_with_->checkedId() == 1;
	settings.force = float(force_->value());
	return settings;
}

void PhysicsDropPage::apply_brush_settings()
{
	if (updating_ || !radius_ || !force_ || !take_with_ || !mode_)
		return;
	brush_->setSettings(read_brush_settings());
	brush_->setMode(page_mode() == PAGE_DRAG ? PhysicsBrush::MODE_DRAG : PhysicsBrush::MODE_BRUSH);
}

PhysicsDropPage::PageMode PhysicsDropPage::page_mode() const
{
	return PageMode(qMax(0, mode_->checkedId()));
}

void PhysicsDropPage::add_selected_assets()
{
	const QVector<PlaceableAsset> assets = resolveSelectedAssets();
	if (assets.isEmpty())
	{
		append_log(PhysicsDrop::MSG_WARNING, uiText("Select meshes or nodes in the Asset Browser first.",
			"Сначала выберите меши или ноды в Asset Browser."));
		return;
	}
	for (const PlaceableAsset &asset : assets)
	{
		bool known = false;
		for (const PlaceableAsset &other : assets_)
			known = known || (other.path == asset.path);
		if (!known)
			assets_.append(asset);
	}
	refresh_assets();
}

void PhysicsDropPage::refresh_assets()
{
	assets_empty_->setVisible(assets_.isEmpty());

	while (QLayoutItem *item = assets_rows_->takeAt(0))
	{
		if (QWidget *widget = item->widget())
			widget->deleteLater();
		delete item;
	}

	for (int i = 0; i < assets_.size(); ++i)
	{
		const PlaceableAsset &asset = assets_[i];

		QWidget *row = new QWidget;
		QHBoxLayout *row_layout = new QHBoxLayout(row);
		row_layout->setContentsMargins(0, 0, 0, 0);
		row_layout->setSpacing(3);

		QLabel *number = new QLabel(QString::fromUtf8("%1.").arg(i + 1));
		number->setEnabled(false);
		row_layout->addWidget(number);

		// The asset field of the editor: another asset is picked in the dialog or dropped onto it.
		AssetField::Options name_options;
		name_options.tooltip = QString::fromUtf8("%1 (%2)").arg(asset.name, kindLabel(asset.kind));
		name_options.dialog_title = uiText("Asset to pour", "Ассет, который сыпать");
		name_options.icon = QString::fromUtf8(":/images/mods/icon_object_mode.png");
		name_options.show_tooltip = loc_.tip("Show this asset in the Asset Browser.", "Показать этот ассет в Asset Browser.");
		AssetField *name = new AssetField(name_options);
		name->setValue(QFileInfo(asset.name).completeBaseName(), true);
		name->setIcon(kindIcon(asset.kind));
		name->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Preferred);
		row_layout->addWidget(name, 1);
		// Later: the row goes away with the list.
		name->on_picked = [this, i](const Vector<UGUID> &guids) {
			QTimer::singleShot(0, this, [this, i, guids]() {
				PlaceableAsset picked;
				if (i >= assets_.size())
					return;
				if (!resolveSelection(guids, picked))
				{
					append_log(PhysicsDrop::MSG_WARNING, uiText("That can not be poured: pick a .mesh, a .node or a model.",
						"Это нельзя сыпать: выберите .mesh, .node или модель."));
					return;
				}
				assets_[i] = picked;
				refresh_assets();
			});
		};
		name->on_show = [this, i]() {
			if (i < assets_.size() && !showInAssetBrowser(assets_[i]))
				append_log(PhysicsDrop::MSG_WARNING, uiText("The asset is not found in the project.", "Ассет не найден в проекте."));
		};

		QToolButton *remove = new QToolButton;
		remove->setText(QString::fromUtf8("✕"));
		remove->setFixedSize(18, 20);
		remove->setAutoRaise(true);
		remove->setFocusPolicy(Qt::NoFocus);
		remove->setToolTip(loc_.tip("Remove this asset from the list.", "Убрать этот ассет из списка."));
		row_layout->addWidget(remove);
		connect(remove, &QToolButton::clicked, this, [this, i]() {
			assets_.removeAt(i);
			refresh_assets();
		});

		assets_rows_->addWidget(row);
	}

	apply_brush_settings();
	refresh();
}

void PhysicsDropPage::refresh_shape_options()
{
	// Only Auto looks at the shapes an object came with.
	ignore_own_shapes_->setVisible(shape_->checkedId() == PhysicsDrop::SHAPE_AUTO);
}

void PhysicsDropPage::refresh()
{
	if (!main_button_ || !mode_)
		return;

	const bool running = drop_->isRunning();
	const bool session = running && drop_->isSession();
	const PageMode mode = page_mode();

	// The helper that reloads the plugin after a build waits for this: the world is not as
	// it should be while the objects fall.
	setProperty("artist_tool_busy", running);

	mode_buttons_[PAGE_DROP]->setEnabled(!running);
	mode_buttons_[PAGE_BRUSH]->setEnabled(!running || session);
	mode_buttons_[PAGE_DRAG]->setEnabled(!running || session);

	assets_group_->setVisible(mode == PAGE_BRUSH);
	brush_group_->setVisible(mode == PAGE_BRUSH);
	drag_group_->setVisible(mode == PAGE_DRAG);
	result_group_->setVisible(mode != PAGE_DROP);
	result_layout_->setRowVisible(result_row_, mode == PAGE_BRUSH);
	result_layout_->setRowVisible(hidden_row_, mode == PAGE_BRUSH);
	result_layout_->setRowVisible(drag_hidden_row_, mode == PAGE_DRAG);
	max_time_label_->setVisible(mode == PAGE_DROP);
	max_time_->setVisible(mode == PAGE_DROP);
	// What can not be used now goes gray and stays where it is: hidden, it would make the buttons jump.
	options_group_->setEnabled(!running);
	cancel_button_->setEnabled(running);
	jitter_button_->setEnabled(running);
	clear_new_->setEnabled(session && drop_->hasNewObjects());
	assets_clear_->setEnabled(!assets_.isEmpty());

	main_button_->setChecked(running);

	switch (mode)
	{
		case PAGE_BRUSH:
			description_->setText(uiText(
				"Press Start and pour new objects into the viewport: they fall, roll and pile up. "
				"It goes on until you press Apply; all of it is one step of Ctrl+Z.",
				"Нажмите «Начать» и сыпьте новые объекты во вьюпорт: они падают, катятся и "
				"собираются в кучи. Всё идёт, пока не нажмёте «Применить»; это один шаг Ctrl+Z."));
			hotkeys_->setText(loc_.hotkeys(
				"LMB - pour: a click puts down \"Per click\", holding goes on\n"
				"Ctrl + LMB - erase what was poured\n"
				"Wheel - radius, Ctrl + wheel - height\n"
				"Ctrl + Z - take back the last stroke\n"
				"Space - pause, Enter - apply, Esc - cancel",
				"ЛКМ - сыпать: клик кладёт [[Per click|«За клик»]], удержание сыплет дальше\n"
				"Ctrl + ЛКМ - стереть насыпанное\n"
				"Колесо - радиус, Ctrl + колесо - высота\n"
				"Ctrl + Z - убрать последний мазок\n"
				"Пробел - пауза, Enter - применить, Esc - отмена"));
			break;
		case PAGE_DRAG:
			description_->setText(uiText(
				"Press Start, then take objects with the mouse: what they hit moves too. Only what is "
				"inside the zone around the mouse takes part. It goes on until you press Apply.",
				"Нажмите «Начать» и берите объекты мышью: то, что они задевают, тоже двигается. "
				"Участвует только то, что внутри зоны вокруг мыши. Всё идёт, пока не нажмёте «Применить»."));
			hotkeys_->setText(take_with_->checkedId() == 1
				? loc_.hotkeys(
					"LMB - select an object, as usual; the gizmo of the editor moves and turns it\n"
					"Shift + LMB - push all in the zone away\n"
					"Ctrl + LMB - pull all in the zone in\n"
					"Ctrl + Shift + LMB - carry all in the zone, a whole pile\n"
					"Wheel - radius of the zone\n"
					"Space - pause, Enter - apply, Esc - cancel",
					"ЛКМ - выделить объект, как обычно; гизмо редактора двигает и поворачивает его\n"
					"Shift + ЛКМ - оттолкнуть всё в зоне\n"
					"Ctrl + ЛКМ - притянуть всё в зоне\n"
					"Ctrl + Shift + ЛКМ - тащить всё в зоне, целую кучу\n"
					"Колесо - радиус зоны\n"
					"Пробел - пауза, Enter - применить, Esc - отмена")
				: loc_.hotkeys(
					"LMB on an object - take it, the wheel moves it nearer or farther\n"
					"Shift + LMB - push all in the zone away\n"
					"Ctrl + LMB - pull all in the zone in\n"
					"Ctrl + Shift + LMB - carry all in the zone, a whole pile\n"
					"Wheel - radius of the zone\n"
					"Space - pause, Enter - apply, Esc - cancel",
					"ЛКМ по объекту - взять, колесо двигает его ближе или дальше\n"
					"Shift + ЛКМ - оттолкнуть всё в зоне\n"
					"Ctrl + ЛКМ - притянуть всё в зоне\n"
					"Ctrl + Shift + ЛКМ - тащить всё в зоне, целую кучу\n"
					"Колесо - радиус зоны\n"
					"Пробел - пауза, Enter - применить, Esc - отмена"));
			break;
		default:
			description_->setText(uiText(
				"The selected objects fall and settle under physics, then stay where they came to rest. "
				"They need no bodies or shapes of their own, and get none: the physics is there only "
				"while they fall.",
				"Выделенные объекты падают и укладываются под физикой, а потом остаются там, где "
				"улеглись. Свои тела и шейпы им не нужны и не добавляются: физика есть только пока "
				"они падают."));
			hotkeys_->setText(loc_.hotkeys(
				"Esc or Ctrl + Z while they fall - put everything back",
				"Esc или Ctrl + Z, пока они падают - вернуть всё как было"));
			break;
	}

	main_button_->setToolTip(mode == PAGE_DROP
		? loc_.tip(
			"Drop the selected objects. Each one falls as one solid thing with all that is inside it.\n"
			"When they settle they stay there; Ctrl+Z brings them back. While they fall, this "
			"button stops them where they are.",
			"Уронить выделенные объекты. Каждый падает как одно целое со всем, что у него внутри.\n"
			"Когда они улягутся, то останутся там; Ctrl+Z вернёт их обратно. Пока они падают, эта "
			"кнопка останавливает их там, где они есть.")
		: loc_.tip(
			"Start: the physics goes on until you press this again.\n"
			"Apply: everything stays where it is, as one step of Ctrl+Z (Enter).",
			"Начать: физика идёт, пока снова не нажмёте эту кнопку.\n"
			"Применить: всё остаётся на местах, одним шагом Ctrl+Z (Enter)."));

	if (session)
	{
		main_button_->setEnabled(true);
		main_button_->setText(uiText("Apply", "Применить"));
		QString status = uiText("Objects: %1, still moving: %2.", "Объектов: %1, ещё движутся: %2.")
			.arg(drop_->getNumObjects()).arg(drop_->getNumMoving());
		if (drop_->getNumParked() > 0)
			status += uiText(" Out of the dome: %1.", " Вне купола: %1.").arg(drop_->getNumParked());
		if (drop_->isUserPaused())
			status += uiText(" Paused.", " Пауза.");
		else if (drop_->isIdlePaused())
			status += uiText(" All at rest: the physics waits.", " Всё улеглось: физика ждёт.");
		status_label_->setText(status);
		return;
	}
	if (running)
	{
		main_button_->setEnabled(true);
		main_button_->setText(uiText("Stop Here", "Остановить здесь"));
		QString status = uiText("Falling: %1 s, still moving: %2 of %3.", "Падают: %1 с, ещё движутся: %2 из %3.")
			.arg(drop_->getElapsed(), 0, 'f', 1).arg(drop_->getNumMoving()).arg(drop_->getNumObjects());
		if (drop_->getNumParked() > 0)
			status += uiText(" Out of the dome: %1.", " Вне купола: %1.").arg(drop_->getNumParked());
		status_label_->setText(status);
		return;
	}

	status_label_->clear();
	if (mode == PAGE_BRUSH)
	{
		main_button_->setEnabled(!assets_.isEmpty());
		main_button_->setText(assets_.isEmpty() ? uiText("Add Assets To Pour", "Добавьте ассеты в список")
			: uiText("Start Pouring", "Начать сыпать"));
		return;
	}
	if (mode == PAGE_DRAG)
	{
		main_button_->setEnabled(true);
		main_button_->setText(uiText("Start Dragging", "Начать перетаскивание"));
		return;
	}

	can_fall_count_ = 0;
	if (const SelectorNodes *selector = Selection::getSelectorNodes())
	{
		const Vector<NodePtr> nodes = selector->getNodes();
		for (const NodePtr &node : nodes)
		{
			if (node && !node.isDeleted() && PhysicsDrop::canFall(node))
				++can_fall_count_;
		}
	}
	main_button_->setEnabled(can_fall_count_ > 0);
	main_button_->setText(can_fall_count_ > 0
		? uiText("Drop Selected: %1", "Уронить выделенное: %1").arg(can_fall_count_)
		: uiText("Select Objects To Drop", "Выделите объекты, чтобы уронить"));
}

void PhysicsDropPage::press_main_button()
{
	// A field with the cursor in it would keep the keys of what goes on now.
	if (QWidget *focused = QApplication::focusWidget())
	{
		if (isAncestorOf(focused))
			focused->clearFocus();
	}

	if (drop_->isRunning())
	{
		drop_->finish();
		// It stays pressed until it is really over.
		main_button_->setChecked(true);
		return;
	}

	save_settings();
	log_->clear();
	notices_.clear();
	notice_label_->setVisible(false);

	if (page_mode() == PAGE_DROP)
	{
		Vector<NodePtr> nodes;
		if (const SelectorNodes *selector = Selection::getSelectorNodes())
			nodes = selector->getNodes();
		drop_->start(nodes, read_options());
	} else
	{
		apply_brush_settings();
		brush_->begin(read_options());
	}
	refresh();
}

void PhysicsDropPage::append_log(int level, const QString &text)
{
	const char *color = nullptr;
	switch (level)
	{
		case PhysicsDrop::MSG_SUCCESS: color = "#6cc070"; break;
		case PhysicsDrop::MSG_WARNING: color = "#e0a030"; break;
		case PhysicsDrop::MSG_ERROR:   color = "#e05555"; break;
		default: break;
	}

	const QString escaped = text.toHtmlEscaped();
	const QString line = color
		? QString::fromUtf8("<span style=\"color:%1\">%2</span>").arg(color, escaped)
		: escaped;
	log_->appendHtml(line);

	if (level == PhysicsDrop::MSG_INFO)
		return;
	notices_.append(line);
	while (notices_.size() > MAX_NOTICES)
		notices_.removeFirst();
	notice_label_->setText(notices_.join(QString::fromUtf8("<br>")));
	notice_label_->setVisible(true);
}

} // namespace ArtistTool
