#include "UiGalleryPage.h"

#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>

#include <UnigineFileSystem.h>
#include <UnigineNode.h>

#include <QAction>
#include <QButtonGroup>
#include <QCheckBox>
#include <QColorDialog>
#include <QComboBox>
#include <QCompleter>
#include <QDial>
#include <QDoubleSpinBox>
#include <QFileDialog>
#include <QFormLayout>
#include <QFrame>
#include <QGridLayout>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QInputDialog>
#include <QLabel>
#include <QLineEdit>
#include <QListWidget>
#include <QMenu>
#include <QMessageBox>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QRandomGenerator>
#include <QScrollArea>
#include <QSlider>
#include <QSpinBox>
#include <QSplitter>
#include <QStackedWidget>
#include <QStyle>
#include <QTabWidget>
#include <QTableWidget>
#include <QTextEdit>
#include <QTimer>
#include <QToolBar>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#include <cmath>
#include <functional>

using ::UnigineEditor::AssetDialogs;
using ::UnigineEditor::Selection;
using ::UnigineEditor::SelectorNodes;

namespace ArtistTool
{

namespace
{

QString str(const char *text)
{
	return QString::fromUtf8(text);
}

enum class IconShape
{
	Circle,
	Square,
	Triangle,
	Diamond,
};

// Icons are drawn in code, so a tool does not need image files for them.
QIcon make_icon(IconShape shape, const QColor &color)
{
	QPixmap pixmap(32, 32);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(color.darker(160), 2));
	painter.setBrush(color);

	switch (shape)
	{
		case IconShape::Circle: painter.drawEllipse(QRectF(5, 5, 22, 22)); break;
		case IconShape::Square: painter.drawRoundedRect(QRectF(5, 5, 22, 22), 4, 4); break;
		case IconShape::Triangle:
		{
			const QPointF points[] = {QPointF(16, 4), QPointF(28, 27), QPointF(4, 27)};
			painter.drawPolygon(points, 3);
			break;
		}
		case IconShape::Diamond:
		{
			const QPointF points[] = {QPointF(16, 3), QPointF(29, 16), QPointF(16, 29), QPointF(3, 16)};
			painter.drawPolygon(points, 4);
			break;
		}
	}
	return QIcon(pixmap);
}

void draw_checkerboard(QPainter &painter, const QRect &rect)
{
	const int cell = 6;
	painter.save();
	painter.setClipRect(rect);
	for (int y = rect.top(); y < rect.bottom(); y += cell)
	{
		for (int x = rect.left(); x < rect.right(); x += cell)
		{
			const bool dark = ((x - rect.left()) / cell + (y - rect.top()) / cell) % 2;
			painter.fillRect(QRect(x, y, cell, cell), dark ? QColor(110, 110, 110) : QColor(170, 170, 170));
		}
	}
	painter.restore();
}

// A button that shows a color and opens the color picker on click.
class ColorButton final : public QPushButton
{
public:
	explicit ColorButton(const QColor &color, QWidget *parent = nullptr)
		: QPushButton(parent)
		, color_(color)
	{
		setMinimumSize(48, 24);
		connect(this, &QPushButton::clicked, this, [this]() {
			const QColor picked = QColorDialog::getColor(color_, this, str("Pick a color"),
				QColorDialog::ShowAlphaChannel);
			if (picked.isValid())
				setColor(picked);
		});
	}

	QColor color() const { return color_; }

	void setColor(const QColor &color)
	{
		color_ = color;
		update();
		if (on_changed)
			on_changed();
	}

	std::function<void()> on_changed;

protected:
	void paintEvent(QPaintEvent *event) override
	{
		QPushButton::paintEvent(event);

		QPainter painter(this);
		const QRect swatch = rect().adjusted(5, 5, -5, -5);
		draw_checkerboard(painter, swatch);
		painter.fillRect(swatch, color_);
		painter.setPen(palette().color(QPalette::Mid));
		painter.drawRect(swatch.adjusted(0, 0, -1, -1));
	}

private:
	QColor color_;
};

class GradientBar final : public QWidget
{
public:
	explicit GradientBar(QWidget *parent = nullptr)
		: QWidget(parent)
	{
		setMinimumHeight(24);
	}

	void setColors(const QColor &from, const QColor &to)
	{
		from_ = from;
		to_ = to;
		update();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		const QRect bar = rect().adjusted(0, 0, -1, -1);
		draw_checkerboard(painter, bar);

		QLinearGradient gradient(bar.topLeft(), bar.topRight());
		gradient.setColorAt(0.0, from_);
		gradient.setColorAt(1.0, to_);
		painter.fillRect(bar, gradient);

		painter.setPen(palette().color(QPalette::Mid));
		painter.drawRect(bar);
	}

private:
	QColor from_;
	QColor to_;
};

// A small curve editor. Drag the points, double-click to add a point, right-click a
// point to remove it. Both axes are 0..1.
class CurveWidget final : public QWidget
{
public:
	explicit CurveWidget(QWidget *parent = nullptr)
		: QWidget(parent)
	{
		points_ = {QPointF(0.0, 1.0), QPointF(0.3, 0.9), QPointF(0.65, 0.35), QPointF(1.0, 0.0)};
		setMinimumHeight(150);
		setMouseTracking(true);
	}

	// Gets the text about the point under the cursor; empty when there is none.
	std::function<void(const QString &)> on_hover;

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);

		const QPalette &pal = palette();
		painter.fillRect(rect(), pal.color(QPalette::Base));

		const QRectF a = area();
		QPen grid_pen(pal.color(QPalette::Mid));
		grid_pen.setStyle(Qt::DotLine);
		painter.setPen(grid_pen);
		for (int i = 0; i <= 4; ++i)
		{
			const double x = a.left() + a.width() * i / 4.0;
			const double y = a.top() + a.height() * i / 4.0;
			painter.drawLine(QPointF(x, a.top()), QPointF(x, a.bottom()));
			painter.drawLine(QPointF(a.left(), y), QPointF(a.right(), y));
		}

		// Catmull-Rom spline through the points, as cubic Bezier segments.
		const int count = int(points_.size());
		QPainterPath curve(to_screen(points_[0]));
		for (int i = 0; i + 1 < count; ++i)
		{
			const QPointF p0 = to_screen(points_[qMax(i - 1, 0)]);
			const QPointF p1 = to_screen(points_[i]);
			const QPointF p2 = to_screen(points_[i + 1]);
			const QPointF p3 = to_screen(points_[qMin(i + 2, count - 1)]);
			curve.cubicTo(p1 + (p2 - p0) / 6.0, p2 - (p3 - p1) / 6.0, p2);
		}

		QPainterPath fill = curve;
		fill.lineTo(a.bottomRight());
		fill.lineTo(a.bottomLeft());
		fill.closeSubpath();
		QColor fill_color = pal.color(QPalette::Highlight);
		fill_color.setAlpha(50);
		painter.fillPath(fill, fill_color);

		painter.setPen(QPen(pal.color(QPalette::Highlight), 2));
		painter.setBrush(Qt::NoBrush);
		painter.drawPath(curve);

		for (int i = 0; i < count; ++i)
		{
			const bool active = i == hover_ || i == drag_;
			const double radius = active ? 6.0 : 4.5;
			painter.setPen(QPen(pal.color(QPalette::Text), 1));
			painter.setBrush(active ? pal.color(QPalette::Highlight) : pal.color(QPalette::Button));
			painter.drawEllipse(to_screen(points_[i]), radius, radius);
		}
	}

	void mousePressEvent(QMouseEvent *event) override
	{
		const int index = hit(event->position());
		const int last = int(points_.size()) - 1;
		if (event->button() == Qt::LeftButton)
		{
			drag_ = index;
		}
		else if (event->button() == Qt::RightButton && index > 0 && index < last)
		{
			points_.removeAt(index);
			hover_ = -1;
			report(-1);
			update();
		}
	}

	void mouseMoveEvent(QMouseEvent *event) override
	{
		if (drag_ >= 0)
		{
			// The end points stay on the borders, the others stay between their neighbours.
			QPointF value = to_value(event->position());
			const int last = int(points_.size()) - 1;
			if (drag_ == 0)
				value.setX(0.0);
			else if (drag_ == last)
				value.setX(1.0);
			else
				value.setX(qBound(points_[drag_ - 1].x() + 0.01, value.x(), points_[drag_ + 1].x() - 0.01));
			points_[drag_] = value;
			update();
		}
		else
		{
			const int index = hit(event->position());
			if (index != hover_)
			{
				hover_ = index;
				setCursor(index >= 0 ? Qt::PointingHandCursor : Qt::ArrowCursor);
				update();
			}
		}
		report(drag_ >= 0 ? drag_ : hover_);
	}

	void mouseReleaseEvent(QMouseEvent *) override
	{
		drag_ = -1;
		update();
	}

	void mouseDoubleClickEvent(QMouseEvent *event) override
	{
		if (event->button() != Qt::LeftButton || hit(event->position()) >= 0)
			return;

		QPointF value = to_value(event->position());
		value.setX(qBound(0.01, value.x(), 0.99));
		int index = 1;
		while (index < int(points_.size()) - 1 && points_[index].x() < value.x())
			++index;
		points_.insert(index, value);
		hover_ = index;
		report(index);
		update();
	}

	void leaveEvent(QEvent *) override
	{
		hover_ = -1;
		report(-1);
		update();
	}

private:
	QRectF area() const { return QRectF(rect()).adjusted(10, 10, -10, -10); }

	QPointF to_screen(const QPointF &value) const
	{
		const QRectF a = area();
		return QPointF(a.left() + value.x() * a.width(), a.bottom() - value.y() * a.height());
	}

	QPointF to_value(const QPointF &position) const
	{
		const QRectF a = area();
		return QPointF(qBound(0.0, (position.x() - a.left()) / a.width(), 1.0),
			qBound(0.0, (a.bottom() - position.y()) / a.height(), 1.0));
	}

	int hit(const QPointF &position) const
	{
		for (int i = 0; i < int(points_.size()); ++i)
		{
			if (QLineF(position, to_screen(points_[i])).length() <= 8.0)
				return i;
		}
		return -1;
	}

	void report(int index)
	{
		if (!on_hover)
			return;
		if (index < 0)
			on_hover(QString());
		else
			on_hover(str("Point %1:   x %2   y %3")
				.arg(index + 1)
				.arg(points_[index].x(), 0, 'f', 2)
				.arg(points_[index].y(), 0, 'f', 2));
	}

	QList<QPointF> points_;
	int hover_{-1};
	int drag_{-1};
};

// Top view of a scatter: random objects inside a circle, keeping a minimum distance
// between them. Redrawn whenever the count, the spacing or the seed change.
class ScatterPreview final : public QWidget
{
public:
	explicit ScatterPreview(QWidget *parent = nullptr)
		: QWidget(parent)
	{
		setMinimumHeight(180);
		regenerate();
	}

	void setCount(int count)
	{
		count_ = count;
		regenerate();
	}

	void setSpacing(double spacing)
	{
		spacing_ = spacing;
		regenerate();
	}

	void shuffle()
	{
		seed_ = QRandomGenerator::global()->generate();
		regenerate();
	}

protected:
	void paintEvent(QPaintEvent *) override
	{
		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);

		const QPalette &pal = palette();
		painter.fillRect(rect(), pal.color(QPalette::Base));

		const double radius = qMin(width(), height()) / 2.0 - 8.0;
		const QPointF center = QRectF(rect()).center();

		QPen border(pal.color(QPalette::Mid));
		border.setStyle(Qt::DashLine);
		painter.setPen(border);
		painter.setBrush(Qt::NoBrush);
		painter.drawEllipse(center, radius, radius);

		// Bigger objects are drawn brighter.
		painter.setPen(Qt::NoPen);
		for (const Item &item : items_)
		{
			painter.setBrush(QColor::fromHsvF(0.3f - 0.1f * item.size, 0.55f, 0.45f + 0.45f * item.size));
			const double r = 2.0 + 4.0 * item.size;
			painter.drawEllipse(center + item.position * radius, r, r);
		}

		painter.setPen(pal.color(QPalette::Text));
		painter.drawText(rect().adjusted(6, 4, -6, -4), Qt::AlignTop | Qt::AlignLeft,
			str("%1 objects").arg(items_.size()));
	}

private:
	struct Item
	{
		QPointF position; // inside the unit circle
		float size{0.0f}; // 0..1
	};

	void regenerate()
	{
		QRandomGenerator random(seed_);
		items_.clear();

		const double two_pi = 6.283185307179586;
		int attempts = count_ * 30;
		while (int(items_.size()) < count_ && attempts-- > 0)
		{
			const double angle = random.generateDouble() * two_pi;
			const double distance = std::sqrt(random.generateDouble());
			const QPointF position(distance * std::cos(angle), distance * std::sin(angle));

			bool free = true;
			for (const Item &item : items_)
			{
				if (QLineF(position, item.position).length() < spacing_)
				{
					free = false;
					break;
				}
			}
			if (free)
				items_.append(Item{position, float(random.generateDouble())});
		}
		update();
	}

	QList<Item> items_;
	int count_{120};
	double spacing_{0.08};
	quint32 seed_{1};
};

} // namespace

UiGalleryPage::UiGalleryPage(QWidget *parent)
	: QWidget(parent)
{
	QVBoxLayout *layout = new QVBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);

	QScrollArea *scroll = new QScrollArea;
	scroll->setWidgetResizable(true);
	scroll->setFrameShape(QFrame::NoFrame);
	layout->addWidget(scroll);

	QWidget *contents = new QWidget;
	sections_layout_ = new QVBoxLayout(contents);
	sections_layout_->setSpacing(4);

	QLabel *description = new QLabel(loc_.tip(
		"Interface elements a tool window can be built from. Everything is live: click, drag, "
		"type. Nothing in the world is changed. Click a section title to fold it.",
		"Элементы интерфейса, из которых можно собрать окно инструмента. Всё живое: нажимайте, "
		"тяните, вводите. В мире ничего не меняется. Клик по заголовку раздела сворачивает его."));
	description->setWordWrap(true);
	sections_layout_->addWidget(description);

	build_buttons();
	build_numbers();
	build_text();
	build_colors();
	build_lists();
	build_progress();
	build_drawing();
	build_layouts();
	build_editor();

	sections_layout_->addStretch(1);
	scroll->setWidget(contents);

	connect(Selection::instance(), &Selection::changed, this, [this]() { refresh_selection(); });
	refresh_selection();
}

void UiGalleryPage::stop()
{
	progress_timer_->stop();
}

QWidget *UiGalleryPage::add_section(const QString &title, const QString &hint)
{
	QToolButton *header = new QToolButton;
	header->setText(title);
	header->setCheckable(true);
	header->setChecked(true);
	header->setArrowType(Qt::DownArrow);
	header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	header->setAutoRaise(true);
	header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	QFont font = header->font();
	font.setBold(true);
	header->setFont(font);

	QFrame *body = new QFrame;
	body->setFrameShape(QFrame::StyledPanel);
	QVBoxLayout *body_layout = new QVBoxLayout(body);

	QLabel *hint_label = new QLabel(hint);
	hint_label->setWordWrap(true);
	hint_label->setEnabled(false); // dimmed text of the editor theme
	body_layout->addWidget(hint_label);

	QWidget *content = new QWidget;
	body_layout->addWidget(content);

	connect(header, &QToolButton::toggled, body, [header, body](bool expanded) {
		header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
		body->setVisible(expanded);
	});

	sections_layout_->addWidget(header);
	sections_layout_->addWidget(body);
	return content;
}

void UiGalleryPage::build_buttons()
{
	QWidget *content = add_section(str("Buttons and switches"), loc_.tip(
		"Run actions, turn modes on and off, pick one option of several.",
		"Запуск действий, включение режимов, выбор одного варианта из нескольких."));
	QVBoxLayout *layout = new QVBoxLayout(content);
	layout->setContentsMargins(0, 0, 0, 0);

	// Plain, toggle and drop-down buttons.
	QHBoxLayout *buttons = new QHBoxLayout;

	QPushButton *run = new QPushButton(style()->standardIcon(QStyle::SP_MediaPlay), str("Run"));
	run->setToolTip(loc_.tip(
		"A plain button: one click - one action.",
		"Обычная кнопка: один клик - одно действие."));
	buttons->addWidget(run);

	QPushButton *toggle = new QPushButton(str("Start placing"));
	toggle->setCheckable(true);
	toggle->setToolTip(loc_.tip(
		"A toggle button: stays pressed until it is clicked again.",
		"Кнопка-переключатель: остаётся нажатой, пока не нажмёшь ещё раз."));
	connect(toggle, &QPushButton::toggled, toggle, [toggle](bool checked) {
		toggle->setText(str(checked ? "Stop placing" : "Start placing"));
	});
	buttons->addWidget(toggle);

	QToolButton *export_button = new QToolButton;
	export_button->setText(str("Export"));
	export_button->setPopupMode(QToolButton::MenuButtonPopup);
	export_button->setToolTip(loc_.tip(
		"A button with a menu: the main action is on the button, the variants are under the arrow.",
		"Кнопка с меню: основное действие - на самой кнопке, варианты - под стрелкой."));
	QMenu *export_menu = new QMenu(export_button);
	export_menu->addAction(str("As .node"));
	export_menu->addAction(str("As .mesh"));
	export_menu->addSeparator();
	QAction *with_children = export_menu->addAction(str("Include children"));
	with_children->setCheckable(true);
	with_children->setChecked(true);
	export_button->setMenu(export_menu);
	buttons->addWidget(export_button);

	buttons->addStretch(1);
	layout->addLayout(buttons);

	// Check boxes.
	QHBoxLayout *checks = new QHBoxLayout;
	QCheckBox *align = new QCheckBox(str("Align to normal"));
	align->setChecked(true);
	checks->addWidget(align);
	checks->addWidget(new QCheckBox(str("Random rotation")));
	QCheckBox *mixed = new QCheckBox(str("Mixed"));
	mixed->setTristate(true);
	mixed->setCheckState(Qt::PartiallyChecked);
	mixed->setToolTip(loc_.tip(
		"A check box can have a third, mixed state - for example when the option is on for "
		"only part of the selected objects.",
		"У галочки бывает третье, смешанное состояние - например, когда опция включена "
		"только у части выделенных объектов."));
	checks->addWidget(mixed);
	checks->addStretch(1);
	layout->addLayout(checks);

	// Radio buttons: one of several.
	QHBoxLayout *radios = new QHBoxLayout;
	QLabel *mode_label = new QLabel(str("Mode"));
	mode_label->setToolTip(loc_.tip(
		"Radio buttons: exactly one option is on.",
		"Радиокнопки: включён ровно один вариант."));
	radios->addWidget(mode_label);
	QButtonGroup *mode_group = new QButtonGroup(content);
	const char *modes[] = {"Add", "Replace", "Remove"};
	for (int i = 0; i < 3; ++i)
	{
		QRadioButton *radio = new QRadioButton(str(modes[i]));
		mode_group->addButton(radio, i);
		radios->addWidget(radio);
	}
	mode_group->button(0)->setChecked(true);
	radios->addStretch(1);
	layout->addLayout(radios);

	// Tool buttons with icons drawn in code, only one of them on.
	struct Tool
	{
		const char *name;
		IconShape shape;
		QColor color;
	};
	const Tool tools[] = {
		{"Brush", IconShape::Circle, QColor(100, 180, 100)},
		{"Erase", IconShape::Square, QColor(210, 95, 85)},
		{"Smooth", IconShape::Diamond, QColor(95, 145, 215)},
		{"Scatter", IconShape::Triangle, QColor(225, 175, 65)},
	};
	QHBoxLayout *tool_row = new QHBoxLayout;
	tool_row->setSpacing(2);
	QButtonGroup *tool_group = new QButtonGroup(content);
	for (const Tool &tool : tools)
	{
		QToolButton *button = new QToolButton;
		button->setText(str(tool.name));
		button->setIcon(make_icon(tool.shape, tool.color));
		button->setIconSize(QSize(22, 22));
		button->setToolButtonStyle(Qt::ToolButtonTextUnderIcon);
		button->setCheckable(true);
		button->setAutoRaise(true);
		button->setMinimumWidth(60);
		button->setToolTip(loc_.tip(
			"Tool buttons with icons drawn by the plugin itself; only one is on at a time.",
			"Кнопки инструментов с иконками, нарисованными самим плагином; включена только одна."));
		tool_group->addButton(button);
		tool_row->addWidget(button);
	}
	tool_group->buttons().first()->setChecked(true);
	tool_row->addStretch(1);
	layout->addLayout(tool_row);

	// Drop-down lists.
	QHBoxLayout *combos = new QHBoxLayout;
	combos->addWidget(new QLabel(str("Up axis")));
	QComboBox *axis = new QComboBox;
	axis->addItems({str("Z (world up)"), str("Surface normal"), str("Camera")});
	combos->addWidget(axis, 1);
	combos->addSpacing(8);
	combos->addWidget(new QLabel(str("Layer")));
	QComboBox *layer = new QComboBox;
	layer->addItem(make_icon(IconShape::Circle, QColor(100, 180, 100)), str("Vegetation"));
	layer->addItem(make_icon(IconShape::Circle, QColor(150, 130, 110)), str("Rocks"));
	layer->addItem(make_icon(IconShape::Circle, QColor(95, 145, 215)), str("Water"));
	layer->setToolTip(loc_.tip(
		"A drop-down list with an icon on each line.",
		"Выпадающий список с иконкой у каждой строки."));
	combos->addWidget(layer, 1);
	layout->addLayout(combos);
}

void UiGalleryPage::build_numbers()
{
	QWidget *content = add_section(str("Numbers"), loc_.tip(
		"Whole and fractional numbers with units, sliders, XYZ vectors, min/max ranges, a "
		"dial for angles. Values change with the mouse wheel, arrows or typing.",
		"Целые и дробные числа с единицами, слайдеры, векторы XYZ, диапазоны мин/макс, "
		"крутилка для углов. Значения меняются колесом мыши, стрелками или вводом."));
	QGridLayout *grid = new QGridLayout(content);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setColumnStretch(1, 1);
	int row = 0;

	grid->addWidget(new QLabel(str("Count")), row, 0);
	QSpinBox *count = new QSpinBox;
	count->setRange(0, 100000);
	count->setValue(250);
	grid->addWidget(count, row++, 1, 1, 3);

	grid->addWidget(new QLabel(str("Radius")), row, 0);
	QDoubleSpinBox *radius = new QDoubleSpinBox;
	radius->setRange(0.0, 1000.0);
	radius->setDecimals(2);
	radius->setSingleStep(0.5);
	radius->setSuffix(str(" m"));
	radius->setValue(12.5);
	grid->addWidget(radius, row++, 1, 1, 3);

	// A slider tied to a number field.
	grid->addWidget(new QLabel(str("Density")), row, 0);
	QSlider *density_slider = new QSlider(Qt::Horizontal);
	density_slider->setRange(0, 100);
	density_slider->setValue(60);
	QSpinBox *density_spin = new QSpinBox;
	density_spin->setRange(0, 100);
	density_spin->setSuffix(str(" %"));
	density_spin->setValue(60);
	density_slider->setToolTip(loc_.tip(
		"A slider and a number field showing the same value.",
		"Слайдер и числовое поле, показывающие одно и то же значение."));
	connect(density_slider, &QSlider::valueChanged, density_spin, &QSpinBox::setValue);
	connect(density_spin, &QSpinBox::valueChanged, density_slider, &QSlider::setValue);
	grid->addWidget(density_slider, row, 1, 1, 2);
	grid->addWidget(density_spin, row++, 3);

	// A vector with the axis colors of the editor.
	grid->addWidget(new QLabel(str("Position")), row, 0);
	QHBoxLayout *vector = new QHBoxLayout;
	vector->setSpacing(4);
	const char *axes[] = {"X", "Y", "Z"};
	const char *axis_colors[] = {"#e05a5a", "#6cc06c", "#5a8ce0"};
	for (int i = 0; i < 3; ++i)
	{
		QLabel *axis_label = new QLabel(str(axes[i]));
		axis_label->setStyleSheet(str("color: %1; font-weight: bold;").arg(str(axis_colors[i])));
		vector->addWidget(axis_label);
		QDoubleSpinBox *value = new QDoubleSpinBox;
		value->setRange(-100000.0, 100000.0);
		value->setDecimals(2);
		vector->addWidget(value, 1);
	}
	grid->addLayout(vector, row++, 1, 1, 3);

	// Min - max range that keeps min <= max.
	grid->addWidget(new QLabel(str("Scale")), row, 0);
	QHBoxLayout *range = new QHBoxLayout;
	range->setSpacing(4);
	QDoubleSpinBox *scale_min = new QDoubleSpinBox;
	QDoubleSpinBox *scale_max = new QDoubleSpinBox;
	for (QDoubleSpinBox *spin : {scale_min, scale_max})
	{
		spin->setRange(0.01, 100.0);
		spin->setSingleStep(0.1);
		spin->setToolTip(loc_.tip(
			"A range: Min can not get bigger than Max - the other value moves along.",
			"Диапазон: Min не может стать больше Max - второе значение подстраивается."));
	}
	scale_min->setValue(0.8);
	scale_max->setValue(1.2);
	connect(scale_min, &QDoubleSpinBox::valueChanged, scale_max, [scale_max](double value) {
		if (scale_max->value() < value)
			scale_max->setValue(value);
	});
	connect(scale_max, &QDoubleSpinBox::valueChanged, scale_min, [scale_min](double value) {
		if (scale_min->value() > value)
			scale_min->setValue(value);
	});
	range->addWidget(new QLabel(str("Min")));
	range->addWidget(scale_min, 1);
	range->addSpacing(8);
	range->addWidget(new QLabel(str("Max")));
	range->addWidget(scale_max, 1);
	grid->addLayout(range, row++, 1, 1, 3);

	// A dial for angles.
	grid->addWidget(new QLabel(str("Angle")), row, 0);
	QHBoxLayout *dial_row = new QHBoxLayout;
	QDial *dial = new QDial;
	dial->setRange(0, 359);
	dial->setWrapping(true);
	dial->setNotchesVisible(true);
	dial->setFixedSize(56, 56);
	dial->setToolTip(loc_.tip(
		"A dial: turn it with the mouse or the wheel, goes round past 359.",
		"Крутилка: вращается мышью или колесом, после 359 идёт снова 0."));
	QLabel *angle_value = new QLabel;
	connect(dial, &QDial::valueChanged, angle_value, [angle_value](int value) {
		angle_value->setText(str("%1°").arg(value));
	});
	dial->setValue(45);
	dial_row->addWidget(dial);
	dial_row->addWidget(angle_value);
	dial_row->addStretch(1);
	grid->addLayout(dial_row, row++, 1, 1, 3);
}

void UiGalleryPage::build_text()
{
	QWidget *content = add_section(str("Text"), loc_.tip(
		"Name fields, search with suggestions, formatted text with links and a colored log.",
		"Ввод имени, поиск с подсказками, оформленный текст со ссылками и цветной лог."));
	QGridLayout *grid = new QGridLayout(content);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setColumnStretch(1, 1);
	int row = 0;

	grid->addWidget(new QLabel(str("Name")), row, 0);
	QLineEdit *name = new QLineEdit;
	name->setPlaceholderText(str("e.g. rocks_group"));
	name->setClearButtonEnabled(true);
	grid->addWidget(name, row++, 1);

	grid->addWidget(new QLabel(str("Search")), row, 0);
	QLineEdit *search = new QLineEdit;
	search->setPlaceholderText(str("type: rock, pine, grass..."));
	search->setClearButtonEnabled(true);
	search->setToolTip(loc_.tip(
		"Suggestions appear while typing; any part of the name matches.",
		"Подсказки появляются при вводе; совпадение ищется в любой части имени."));
	QCompleter *completer = new QCompleter(QStringList{
		str("rock_01.mesh"), str("rock_02.mesh"), str("rock_cliff_big.mesh"),
		str("pine_01.node"), str("pine_02.node"), str("birch_young.node"),
		str("grass_tall.mesh"), str("grass_dry.mesh"), str("bush_round.node")}, search);
	completer->setCaseSensitivity(Qt::CaseInsensitive);
	completer->setFilterMode(Qt::MatchContains);
	search->setCompleter(completer);
	grid->addWidget(search, row++, 1);

	QLabel *rich = new QLabel(str(
		"Formatted text: <b>bold</b>, <i>italic</i>, <span style='color:#e0a040'>colored</span> "
		"and a <a href='https://developer.unigine.com/en/docs/latest/editor2/extensions/'>link "
		"to the UNIGINE docs</a>."));
	rich->setWordWrap(true);
	rich->setTextFormat(Qt::RichText);
	rich->setOpenExternalLinks(true);
	grid->addWidget(rich, row++, 0, 1, 2);

	QTextEdit *log = new QTextEdit;
	log->setReadOnly(true);
	log->setMinimumHeight(90);
	log->setMaximumHeight(130);
	log->setToolTip(loc_.tip(
		"A log with colored lines: errors and warnings are easy to spot.",
		"Лог с цветными строками: ошибки и предупреждения сразу видны."));
	auto add_line = [log](const char *color, const char *text) {
		if (color)
			log->append(str("<span style='color:%1'>%2</span>").arg(str(color), str(text).toHtmlEscaped()));
		else
			log->append(str(text).toHtmlEscaped());
	};
	add_line(nullptr, "Unpacking 12 clutters...");
	add_line("#e0a040", "Warning: 3 objects are outside the terrain");
	add_line("#e06060", "Error: mesh not found: rocks/rock_07.mesh");
	add_line("#70c070", "Done in 1.4 s");
	grid->addWidget(log, row++, 0, 1, 2);

	QHBoxLayout *log_buttons = new QHBoxLayout;
	QPushButton *add_button = new QPushButton(str("Add line"));
	QPushButton *clear_button = new QPushButton(str("Clear"));
	connect(add_button, &QPushButton::clicked, log, [add_line]() {
		switch (QRandomGenerator::global()->bounded(3))
		{
			case 0: add_line(nullptr, "Placed 25 objects"); break;
			case 1: add_line("#e0a040", "Warning: 2 objects overlap"); break;
			default: add_line("#e06060", "Error: no surface under the cursor"); break;
		}
	});
	connect(clear_button, &QPushButton::clicked, log, &QTextEdit::clear);
	log_buttons->addWidget(add_button);
	log_buttons->addWidget(clear_button);
	log_buttons->addStretch(1);
	grid->addLayout(log_buttons, row++, 0, 1, 2);
}

void UiGalleryPage::build_colors()
{
	QWidget *content = add_section(str("Colors"), loc_.tip(
		"Color pickers (with transparency), gradients, ready-made palettes.",
		"Выбор цвета (с прозрачностью), градиенты, готовые палитры."));
	QGridLayout *grid = new QGridLayout(content);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setColumnStretch(2, 1);

	ColorButton *from = new ColorButton(QColor(60, 110, 40));
	ColorButton *to = new ColorButton(QColor(230, 210, 120, 120));
	GradientBar *gradient = new GradientBar;
	from->setToolTip(loc_.tip(
		"Click to pick a color.",
		"Клик - выбрать цвет."));
	to->setToolTip(loc_.tip(
		"This color is half transparent - the checkerboard shows through.",
		"Этот цвет полупрозрачный - сквозь него видна шахматка."));
	auto update_gradient = [from, to, gradient]() { gradient->setColors(from->color(), to->color()); };
	from->on_changed = update_gradient;
	to->on_changed = update_gradient;
	update_gradient();

	grid->addWidget(new QLabel(str("Gradient")), 0, 0);
	grid->addWidget(from, 0, 1);
	grid->addWidget(gradient, 0, 2);
	grid->addWidget(to, 0, 3);

	// Palette: a click puts the color into the left button.
	grid->addWidget(new QLabel(str("Palette")), 1, 0);
	QHBoxLayout *palette_row = new QHBoxLayout;
	palette_row->setSpacing(2);
	const QColor colors[] = {
		QColor(60, 110, 40), QColor(120, 150, 60), QColor(150, 130, 100), QColor(110, 100, 95),
		QColor(70, 120, 170), QColor(200, 80, 60), QColor(230, 190, 80), QColor(235, 235, 235)};
	for (const QColor &color : colors)
	{
		QToolButton *swatch = new QToolButton;
		swatch->setIcon(make_icon(IconShape::Square, color));
		swatch->setIconSize(QSize(18, 18));
		swatch->setAutoRaise(true);
		swatch->setToolTip(loc_.tip(
			"A preset color: click to put it into the left color of the gradient.",
			"Готовый цвет: клик ставит его в левый цвет градиента."));
		connect(swatch, &QToolButton::clicked, from, [from, color]() { from->setColor(color); });
		palette_row->addWidget(swatch);
	}
	palette_row->addStretch(1);
	grid->addLayout(palette_row, 1, 1, 1, 3);
}

void UiGalleryPage::build_lists()
{
	QWidget *content = add_section(str("Lists, trees, tables"), loc_.tip(
		"A list with check boxes, filter and drag-to-reorder; a hierarchy tree; an editable "
		"table sorted by a click on a column title. Tabs inside a section are possible too.",
		"Список с галочками, фильтром и перетаскиванием; дерево-иерархия; редактируемая "
		"таблица с сортировкой по клику на заголовок. Вкладки внутри раздела тоже можно."));
	QVBoxLayout *layout = new QVBoxLayout(content);
	layout->setContentsMargins(0, 0, 0, 0);

	QTabWidget *tabs = new QTabWidget;
	tabs->setMinimumHeight(210);
	layout->addWidget(tabs);

	// List.
	QWidget *list_page = new QWidget;
	QVBoxLayout *list_layout = new QVBoxLayout(list_page);
	QLineEdit *filter = new QLineEdit;
	filter->setPlaceholderText(str("Filter..."));
	filter->setClearButtonEnabled(true);
	list_layout->addWidget(filter);

	QListWidget *list = new QListWidget;
	list->setDragDropMode(QAbstractItemView::InternalMove);
	list->setSelectionMode(QAbstractItemView::ExtendedSelection);
	list->setToolTip(loc_.tip(
		"Check boxes, several items with Ctrl / Shift, drag items to change the order.",
		"Галочки, несколько строк через Ctrl / Shift, перетаскивание меняет порядок."));
	struct Entry
	{
		const char *name;
		IconShape shape;
		QColor color;
		bool checked;
	};
	const Entry entries[] = {
		{"rock_01.mesh", IconShape::Square, QColor(150, 130, 110), true},
		{"rock_cliff_big.mesh", IconShape::Square, QColor(120, 110, 100), false},
		{"pine_01.node", IconShape::Triangle, QColor(60, 120, 60), true},
		{"birch_young.node", IconShape::Triangle, QColor(130, 170, 80), true},
		{"grass_tall.mesh", IconShape::Diamond, QColor(150, 190, 80), false},
		{"bush_round.node", IconShape::Circle, QColor(90, 140, 70), true},
	};
	for (const Entry &entry : entries)
	{
		QListWidgetItem *item = new QListWidgetItem(make_icon(entry.shape, entry.color), str(entry.name), list);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(entry.checked ? Qt::Checked : Qt::Unchecked);
	}
	connect(filter, &QLineEdit::textChanged, list, [list](const QString &text) {
		for (int i = 0; i < list->count(); ++i)
			list->item(i)->setHidden(!list->item(i)->text().contains(text, Qt::CaseInsensitive));
	});
	list_layout->addWidget(list);
	tabs->addTab(list_page, str("List"));

	// Tree: checking a group checks everything inside it.
	QTreeWidget *tree = new QTreeWidget;
	tree->setHeaderLabels({str("Name"), str("Type")});
	tree->setToolTip(loc_.tip(
		"A hierarchy. Checking a group checks everything inside; a partly checked group shows "
		"the mixed state.",
		"Иерархия. Галочка на группе ставится всему внутри; частично отмеченная группа "
		"показывает смешанное состояние."));
	const QIcon folder_icon = style()->standardIcon(QStyle::SP_DirIcon);
	const QIcon file_icon = style()->standardIcon(QStyle::SP_FileIcon);
	auto add_group = [&](QTreeWidgetItem *parent, const char *name) {
		QTreeWidgetItem *item = parent ? new QTreeWidgetItem(parent) : new QTreeWidgetItem(tree);
		item->setText(0, str(name));
		item->setText(1, str("Group"));
		item->setIcon(0, folder_icon);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable | Qt::ItemIsAutoTristate);
		item->setCheckState(0, Qt::Checked);
		return item;
	};
	auto add_leaf = [&](QTreeWidgetItem *parent, const char *name, const char *type, bool checked) {
		QTreeWidgetItem *item = new QTreeWidgetItem(parent);
		item->setText(0, str(name));
		item->setText(1, str(type));
		item->setIcon(0, file_icon);
		item->setFlags(item->flags() | Qt::ItemIsUserCheckable);
		item->setCheckState(0, checked ? Qt::Checked : Qt::Unchecked);
	};
	QTreeWidgetItem *rocks = add_group(nullptr, "Rocks");
	add_leaf(rocks, "rock_01", "Mesh", true);
	add_leaf(rocks, "rock_02", "Mesh", true);
	QTreeWidgetItem *vegetation = add_group(nullptr, "Vegetation");
	QTreeWidgetItem *trees = add_group(vegetation, "Trees");
	add_leaf(trees, "pine_01", "Node", true);
	add_leaf(trees, "birch_young", "Node", false);
	QTreeWidgetItem *grass = add_group(vegetation, "Grass");
	add_leaf(grass, "grass_tall", "Clutter", true);
	add_leaf(grass, "grass_dry", "Clutter", true);
	tree->expandAll();
	tree->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	tree->header()->setStretchLastSection(false);
	tabs->addTab(tree, str("Tree"));

	// Table: double-click a cell to edit; numbers get a number field automatically.
	QTableWidget *table = new QTableWidget(5, 3);
	table->setHorizontalHeaderLabels({str("Asset"), str("Weight"), str("Count")});
	table->setToolTip(loc_.tip(
		"Double-click a cell to edit it. Click a column title to sort.",
		"Двойной клик по ячейке - редактировать. Клик по заголовку столбца - сортировка."));
	const char *assets[] = {"rock_01", "rock_02", "pine_01", "birch_young", "grass_tall"};
	const double weights[] = {1.0, 0.5, 2.0, 0.75, 4.0};
	const int counts[] = {120, 60, 40, 25, 900};
	for (int i = 0; i < 5; ++i)
	{
		table->setItem(i, 0, new QTableWidgetItem(str(assets[i])));
		QTableWidgetItem *weight = new QTableWidgetItem;
		weight->setData(Qt::EditRole, weights[i]);
		table->setItem(i, 1, weight);
		QTableWidgetItem *count = new QTableWidgetItem;
		count->setData(Qt::EditRole, counts[i]);
		table->setItem(i, 2, count);
	}
	table->setSortingEnabled(true);
	table->setAlternatingRowColors(true);
	table->verticalHeader()->hide();
	table->horizontalHeader()->setSectionResizeMode(0, QHeaderView::Stretch);
	tabs->addTab(table, str("Table"));
}

void UiGalleryPage::build_progress()
{
	QWidget *content = add_section(str("Progress and status"), loc_.tip(
		"Progress of long operations, a \"working...\" indicator when the end is unknown, "
		"colored status marks.",
		"Прогресс долгих операций, индикатор «работаю...», когда конец неизвестен, цветные "
		"отметки статуса."));
	QVBoxLayout *layout = new QVBoxLayout(content);
	layout->setContentsMargins(0, 0, 0, 0);

	QHBoxLayout *progress_row = new QHBoxLayout;
	progress_ = new QProgressBar;
	progress_->setRange(0, 100);
	progress_->setValue(35);
	progress_row->addWidget(progress_, 1);
	QPushButton *start = new QPushButton(str("Start"));
	start->setCheckable(true);
	progress_row->addWidget(start);
	layout->addLayout(progress_row);

	progress_timer_ = new QTimer(this);
	progress_timer_->setInterval(40);
	connect(progress_timer_, &QTimer::timeout, progress_, [this]() {
		progress_->setValue((progress_->value() + 1) % 101);
	});
	connect(start, &QPushButton::toggled, progress_timer_, [this, start](bool checked) {
		start->setText(str(checked ? "Stop" : "Start"));
		if (checked)
			progress_timer_->start();
		else
			progress_timer_->stop();
	});

	QHBoxLayout *busy_row = new QHBoxLayout;
	busy_row->addWidget(new QLabel(str("Working...")));
	QProgressBar *busy = new QProgressBar;
	busy->setRange(0, 0);
	busy->setTextVisible(false);
	busy->setFixedHeight(6);
	busy->setToolTip(loc_.tip(
		"Shown while the end of the work is unknown.",
		"Показывается, пока неизвестно, сколько ещё работать."));
	busy_row->addWidget(busy, 1);
	layout->addLayout(busy_row);

	QHBoxLayout *status_row = new QHBoxLayout;
	struct Status
	{
		const char *text;
		const char *color;
	};
	const Status statuses[] = {{"OK", "#3f7a3f"}, {"Warning", "#9a6a1a"}, {"Error", "#9a3434"}, {"Skipped", "#555a60"}};
	for (const Status &status : statuses)
	{
		QLabel *chip = new QLabel(str(status.text));
		chip->setStyleSheet(str("background: %1; color: white; border-radius: 3px; padding: 2px 8px;")
			.arg(str(status.color)));
		status_row->addWidget(chip);
	}
	status_row->addStretch(1);
	layout->addLayout(status_row);
}

void UiGalleryPage::build_drawing()
{
	QWidget *content = add_section(str("Custom drawing"), loc_.tip(
		"Whatever is missing can be drawn by hand and made to react to the mouse: curves, "
		"graphs, a top-view preview, masks, histograms.",
		"Всё, чего нет в готовом виде, рисуется вручную и реагирует на мышь: кривые, "
		"графики, превью сверху, маски, гистограммы."));
	QVBoxLayout *layout = new QVBoxLayout(content);
	layout->setContentsMargins(0, 0, 0, 0);

	// Curve editor.
	layout->addWidget(new QLabel(str("Falloff curve")));
	CurveWidget *curve = new CurveWidget;
	curve->setToolTip(loc_.tip(
		"Drag the points. Double-click - add a point, right-click a point - remove it.",
		"Тяните точки. Двойной клик - добавить точку, правый клик по точке - удалить её."));
	layout->addWidget(curve);
	QLabel *curve_value = new QLabel;
	curve_value->setEnabled(false);
	curve->on_hover = [curve_value](const QString &text) { curve_value->setText(text); };
	layout->addWidget(curve_value);

	// Scatter preview.
	layout->addWidget(new QLabel(str("Scatter preview (top view)")));
	ScatterPreview *scatter = new ScatterPreview;
	scatter->setToolTip(loc_.tip(
		"How objects would be spread in the brush circle; bigger ones are brighter.",
		"Как объекты разлеглись бы в круге кисти; крупные - светлее."));
	layout->addWidget(scatter);

	QHBoxLayout *scatter_row = new QHBoxLayout;
	scatter_row->addWidget(new QLabel(str("Count")));
	QSpinBox *scatter_count = new QSpinBox;
	scatter_count->setRange(1, 500);
	scatter_count->setValue(120);
	scatter_row->addWidget(scatter_count);
	scatter_row->addSpacing(8);
	scatter_row->addWidget(new QLabel(str("Spacing")));
	QSlider *spacing = new QSlider(Qt::Horizontal);
	spacing->setRange(0, 30);
	spacing->setValue(8);
	scatter_row->addWidget(spacing, 1);
	QPushButton *shuffle = new QPushButton(str("Shuffle"));
	scatter_row->addWidget(shuffle);
	layout->addLayout(scatter_row);

	connect(scatter_count, &QSpinBox::valueChanged, scatter, [scatter](int value) { scatter->setCount(value); });
	connect(spacing, &QSlider::valueChanged, scatter, [scatter](int value) { scatter->setSpacing(value / 100.0); });
	connect(shuffle, &QPushButton::clicked, scatter, [scatter]() { scatter->shuffle(); });
}

void UiGalleryPage::build_layouts()
{
	QWidget *content = add_section(str("Layout"), loc_.tip(
		"A toolbar, panels with a draggable divider, a list that switches pages - for tools "
		"with many settings. The sections of this page themselves are foldable blocks.",
		"Панель инструментов, панели с перетаскиваемым разделителем, список, переключающий "
		"страницы, - для инструментов с большим количеством настроек. Сами разделы этой "
		"страницы - сворачиваемые блоки."));
	QVBoxLayout *layout = new QVBoxLayout(content);
	layout->setContentsMargins(0, 0, 0, 0);

	QToolBar *toolbar = new QToolBar;
	toolbar->setIconSize(QSize(16, 16));
	toolbar->addAction(style()->standardIcon(QStyle::SP_FileIcon), str("New"));
	toolbar->addAction(style()->standardIcon(QStyle::SP_DialogOpenButton), str("Open"));
	toolbar->addAction(style()->standardIcon(QStyle::SP_DialogSaveButton), str("Save"));
	toolbar->addSeparator();
	toolbar->addAction(style()->standardIcon(QStyle::SP_BrowserReload), str("Refresh"));
	toolbar->addAction(style()->standardIcon(QStyle::SP_TrashIcon), str("Delete"));
	layout->addWidget(toolbar);

	QSplitter *splitter = new QSplitter(Qt::Horizontal);
	splitter->setMinimumHeight(130);
	splitter->setToolTip(loc_.tip(
		"Drag the divider between the panels.",
		"Разделитель между панелями можно тянуть мышью."));

	QListWidget *categories = new QListWidget;
	QStackedWidget *pages = new QStackedWidget;

	auto add_page = [&](const char *name, QWidget *a, const char *a_label, QWidget *b, const char *b_label) {
		categories->addItem(str(name));
		QWidget *page = new QWidget;
		QFormLayout *form = new QFormLayout(page);
		form->addRow(str(a_label), a);
		form->addRow(str(b_label), b);
		pages->addWidget(page);
	};

	QComboBox *quality = new QComboBox;
	quality->addItems({str("Low"), str("Medium"), str("High")});
	add_page("General", new QLineEdit(str("My tool")), "Name", quality, "Quality");

	QDoubleSpinBox *step = new QDoubleSpinBox;
	step->setSuffix(str(" m"));
	step->setValue(1.0);
	add_page("Placement", step, "Grid step", new QCheckBox(str("Snap to grid")), "Snap");

	QDoubleSpinBox *mass = new QDoubleSpinBox;
	mass->setSuffix(str(" kg"));
	mass->setValue(50.0);
	add_page("Physics", mass, "Mass", new QCheckBox(str("Static")), "Body");

	categories->setCurrentRow(0);
	connect(categories, &QListWidget::currentRowChanged, pages, &QStackedWidget::setCurrentIndex);

	splitter->addWidget(categories);
	splitter->addWidget(pages);
	splitter->setStretchFactor(1, 1);
	splitter->setSizes({110, 260});
	layout->addWidget(splitter);
}

void UiGalleryPage::build_editor()
{
	QWidget *content = add_section(str("Editor and dialogs"), loc_.tip(
		"Real editor windows (the asset picker), the current selection in the world, "
		"standard questions, folder picker, right-click menus.",
		"Настоящие окна редактора (выбор ассета), текущее выделение в мире, стандартные "
		"вопросы, выбор папки, меню по правому клику."));
	QGridLayout *grid = new QGridLayout(content);
	grid->setContentsMargins(0, 0, 0, 0);
	grid->setColumnStretch(1, 1);
	int row = 0;

	QPushButton *pick_asset = new QPushButton(str("Pick asset..."));
	pick_asset->setToolTip(loc_.tip(
		"Opens the editor's own asset picker (meshes, nodes, materials).",
		"Открывает родное окно выбора ассета редактора (меши, ноды, материалы)."));
	connect(pick_asset, &QPushButton::clicked, this, [this]() {
		AssetDialogs::browseAsset(Unigine::MakeCallback(this, &UiGalleryPage::on_asset_picked),
			"Pick an asset", ".mesh.node.mat", nullptr, AssetDialogs::DialogMode::Modal);
	});
	grid->addWidget(pick_asset, row, 0);
	asset_label_ = new QLabel(str("(none)"));
	asset_label_->setWordWrap(true);
	grid->addWidget(asset_label_, row++, 1);

	QLabel *selection_title = new QLabel(str("Selected"));
	selection_title->setToolTip(loc_.tip(
		"Follows the selection in the World Hierarchy and the viewport.",
		"Следит за выделением в World Hierarchy и во вьюпорте."));
	grid->addWidget(selection_title, row, 0);
	selection_label_ = new QLabel;
	selection_label_->setWordWrap(true);
	grid->addWidget(selection_label_, row++, 1);

	QHBoxLayout *dialogs = new QHBoxLayout;
	QPushButton *question = new QPushButton(str("Question"));
	QPushButton *input = new QPushButton(str("Input"));
	QPushButton *folder = new QPushButton(str("Folder..."));
	dialogs->addWidget(question);
	dialogs->addWidget(input);
	dialogs->addWidget(folder);
	dialogs->addStretch(1);
	grid->addLayout(dialogs, row++, 0, 1, 2);

	QLabel *menu_area = new QLabel(loc_.tip(
		"Right-click here for a menu",
		"Правый клик здесь - меню"));
	menu_area->setAlignment(Qt::AlignCenter);
	menu_area->setFrameShape(QFrame::StyledPanel);
	menu_area->setMinimumHeight(40);
	menu_area->setContextMenuPolicy(Qt::CustomContextMenu);
	grid->addWidget(menu_area, row++, 0, 1, 2);

	dialog_result_ = new QLabel;
	dialog_result_->setWordWrap(true);
	grid->addWidget(dialog_result_, row++, 0, 1, 2);

	connect(question, &QPushButton::clicked, this, [this]() {
		const QMessageBox::StandardButton answer = QMessageBox::question(this, str("Artist Tools"),
			str("Replace 25 objects in the selection?"));
		dialog_result_->setText(str(answer == QMessageBox::Yes ? "Answer: Yes" : "Answer: No"));
	});
	connect(input, &QPushButton::clicked, this, [this]() {
		bool ok = false;
		const QString text = QInputDialog::getText(this, str("Artist Tools"), str("Group name:"),
			QLineEdit::Normal, str("rocks_group"), &ok);
		dialog_result_->setText(ok ? str("Entered: %1").arg(text) : str("Input cancelled"));
	});
	connect(folder, &QPushButton::clicked, this, [this]() {
		const QString path = QFileDialog::getExistingDirectory(this, str("Choose a folder"));
		dialog_result_->setText(path.isEmpty() ? str("No folder chosen") : str("Folder: %1").arg(path));
	});
	connect(menu_area, &QLabel::customContextMenuRequested, this, [this, menu_area](const QPoint &position) {
		QMenu menu;
		menu.addAction(style()->standardIcon(QStyle::SP_FileDialogNewFolder), str("Group"));
		menu.addAction(str("Duplicate"));
		QMenu *align = menu.addMenu(str("Align"));
		align->addAction(str("To ground"));
		align->addAction(str("To normal"));
		menu.addSeparator();
		menu.addAction(style()->standardIcon(QStyle::SP_TrashIcon), str("Delete"));
		if (QAction *chosen = menu.exec(menu_area->mapToGlobal(position)))
			dialog_result_->setText(str("Menu: %1").arg(chosen->text()));
	});
}

void UiGalleryPage::refresh_selection()
{
	const SelectorNodes *selector = Selection::getSelectorNodes();
	if (!selector || selector->getNodes().size() == 0)
	{
		selection_label_->setText(str("(nothing)"));
		return;
	}

	const auto &nodes = selector->getNodes();
	QStringList names;
	for (int i = 0; i < nodes.size() && i < 3; ++i)
		names.append(QString::fromUtf8(nodes[i]->getName()));
	QString text = names.join(str(", "));
	if (nodes.size() > 3)
		text += str(" and %1 more").arg(nodes.size() - 3);
	selection_label_->setText(text);
}

void UiGalleryPage::on_asset_picked(const AssetDialogs::SelectedAsset &asset)
{
	asset_label_->setText(QString::fromUtf8(Unigine::FileSystem::getVirtualPath(asset.asset_guid).get()));
}

} // namespace ArtistTool
