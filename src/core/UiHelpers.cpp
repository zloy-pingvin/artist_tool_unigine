#include "UiHelpers.h"

#include <UnigineEngine.h>
#include <UnigineFileSystem.h>
#include <UnigineImage.h>

#include <QAbstractSpinBox>
#include <QChildEvent>
#include <QStyle>
#include <QFrame>
#include <QAbstractScrollArea>
#include <QApplication>
#include <QDialog>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QTextEdit>
#include <QButtonGroup>
#include <QCryptographicHash>
#include <QDial>
#include <QDir>
#include <QGroupBox>
#include <QDirIterator>
#include <QFile>
#include <QFileInfo>
#include <QFont>
#include <QHash>
#include <QImage>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QPainter>
#include <QPixmap>
#include <QSettings>
#include <QStringList>
#include <UnigineVisualizer.h>

#include <QToolButton>
#include <QWidget>

#include <cmath>

namespace ArtistTool
{

namespace
{

// The arrows of a number field take about this much of its right edge.
const int SPIN_HANDLE_WIDTH = 18;
// The mouse goes this far for one step of the field.
const double SPIN_PIXELS_PER_STEP = 4.0;
// A press that moves less than this is a click.
const double SPIN_DRAG_START = 3.0;

// Turns a press on the arrows of a number field into a drag that changes its value.
class SpinBoxDrag : public QObject
{
public:
	explicit SpinBoxDrag(QAbstractSpinBox *box)
		: QObject(box)
		, box_(box)
	{
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (watched != box_ || !box_->isEnabled())
			return false;

		switch (event->type())
		{
			case QEvent::MouseButtonPress:
			case QEvent::MouseButtonDblClick:
			{
				const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
				if (mouse->button() != Qt::LeftButton
					|| mouse->position().x() < box_->width() - SPIN_HANDLE_WIDTH)
					return false;

				pressed_ = true;
				dragged_ = false;
				steps_done_ = 0;
				press_position_ = mouse->globalPosition();
				// The upper arrow or the lower one, as the box itself tells them apart.
				press_down_ = mouse->position().y() >= box_->height() / 2.0;
				box_->setFocus(Qt::MouseFocusReason);
				return true;
			}

			case QEvent::MouseMove:
			{
				if (!pressed_)
					return false;

				// Up and right add, down and left take away.
				const QPointF delta = static_cast<QMouseEvent *>(event)->globalPosition() - press_position_;
				const double pixels = delta.x() - delta.y();
				if (!dragged_ && qAbs(pixels) < SPIN_DRAG_START)
					return true;
				dragged_ = true;

				const int steps = int(pixels / SPIN_PIXELS_PER_STEP);
				if (steps != steps_done_)
				{
					box_->stepBy(steps - steps_done_);
					steps_done_ = steps;
				}
				return true;
			}

			case QEvent::MouseButtonRelease:
			{
				if (!pressed_ || static_cast<QMouseEvent *>(event)->button() != Qt::LeftButton)
					return false;

				pressed_ = false;
				// A click without a drag does what the arrow does: a step up or down.
				if (!dragged_)
					box_->stepBy(press_down_ ? -1 : 1);
				return true;
			}

			default:
				break;
		}
		return false;
	}

private:
	QAbstractSpinBox *box_{nullptr};
	bool pressed_{false};
	bool dragged_{false};
	bool press_down_{false};
	int steps_done_{0};
	QPointF press_position_;
};

} // namespace

namespace
{

// A dial with longer marks at the main angles.
class AngleDial final : public QDial
{
public:
	explicit AngleDial(int major_step)
		: major_step_(major_step)
	{
	}

protected:
	void paintEvent(QPaintEvent *event) override
	{
		QDial::paintEvent(event);
		if (major_step_ <= 0 || maximum() <= minimum())
			return;

		QPainter painter(this);
		painter.setRenderHint(QPainter::Antialiasing);
		painter.setPen(QPen(iconColor(), 1.0));

		const qreal pi = 3.14159265358979323846;
		const qreal radius = qMin(width(), height()) / 2.0;
		const QPointF center(width() / 2.0, height() / 2.0);
		const qreal range = qreal(maximum() - minimum());
		for (int value = minimum(); value <= maximum(); ++value)
		{
			if (value % major_step_ != 0)
				continue;
			// Where QDial puts the value: a whole circle with the minimum at the bottom
			// when it wraps, else 300 degrees with the gap at the bottom.
			const qreal along = qreal(value - minimum()) / range;
			const qreal angle = wrapping() ? pi * 1.5 - along * 2.0 * pi : (pi * 8.0 - along * 10.0 * pi) / 6.0;
			const QPointF direction(std::cos(angle), -std::sin(angle));
			painter.drawLine(center + direction * (radius - 8.0), center + direction * (radius - 1.0));
		}
	}

	// One degree per step of the wheel (QDial takes several), round the circle when
	// the dial wraps.
	void wheelEvent(QWheelEvent *event) override
	{
		wheel_delta_ += event->angleDelta().y();
		const int steps = wheel_delta_ / 120;
		wheel_delta_ -= steps * 120;
		if (steps != 0)
		{
			int value = this->value() + steps * singleStep();
			const int range = maximum() - minimum();
			if (wrapping() && range > 0)
			{
				while (value > maximum())
					value -= range;
				while (value < minimum())
					value += range;
			}
			setValue(qBound(minimum(), value, maximum()));
		}
		event->accept();
	}

private:
	int major_step_{45};
	int wheel_delta_{0};
};

} // namespace

QDial *makeAngleDial(int minimum, int maximum, int major_step, int snap)
{
	QDial *dial = new AngleDial(major_step);
	dial->setRange(minimum, maximum);
	dial->setSingleStep(1);
	dial->setPageStep(15);
	dial->setNotchesVisible(true);
	dial->setNotchTarget(12.0);

	// Turned with the mouse, it sticks to the main angles.
	QObject::connect(dial, &QDial::valueChanged, dial, [dial, major_step, snap](int value) {
		if (!dial->isSliderDown() || major_step <= 0)
			return;
		const int nearest = int(std::lround(double(value) / major_step)) * major_step;
		if (nearest != value && std::abs(nearest - value) <= snap && nearest >= dial->minimum() && nearest <= dial->maximum())
			dial->setValue(nearest);
	});
	return dial;
}

void enableSpinBoxDrag(QAbstractSpinBox *box)
{
	box->installEventFilter(new SpinBoxDrag(box));
}

QString pluginIconsDir()
{
	const QDir app_dir(QString::fromUtf8(Unigine::Engine::get()->getAppPath()));
	return QDir::cleanPath(app_dir.filePath(QString::fromUtf8("plugins/zloy_pingvin/artist_tool/icons")));
}

QIcon findEditorIcon(const QString &file_name)
{
	// Most icons of the editor are in ":/images".
	const QString usual = QString::fromUtf8(":/images/") + file_name;
	if (QFile::exists(usual))
		return QIcon(usual);

	QDirIterator it(QString::fromUtf8(":/"), {file_name}, QDir::Files, QDirIterator::Subdirectories);
	return it.hasNext() ? QIcon(it.next()) : QIcon();
}

QIcon nodeTypeIcon(const char *type_name)
{
	static QHash<QString, QIcon> icons;
	const QString name = QString::fromUtf8(type_name);
	const auto found = icons.constFind(name);
	if (found != icons.constEnd())
		return found.value();

	// The icons are in core.ung, which Qt can not open: they are read by the engine, the
	// way the editor does it.
	QIcon icon;
	const QByteArray path = QString::fromUtf8("core/gui/node_icons/%1_x16.png").arg(name).toUtf8();
	Unigine::ImagePtr image = Unigine::Image::create();
	if (Unigine::FileSystem::isFileExist(path.constData()) && image->load(path.constData())
		&& (!image->isCompressedFormat() && image->getFormat() <= Unigine::Image::FORMAT_RGBA8
			|| image->convertToFormat(Unigine::Image::FORMAT_RGBA8)))
	{
		const int width = image->getWidth();
		const int height = image->getHeight();
		const int channels = image->getNumChannels();
		const unsigned char *pixels = image->getPixels2D();
		if (width > 0 && height > 0 && pixels)
		{
			QImage picture(width, height, QImage::Format_RGBA8888);
			for (int y = 0; y < height; ++y)
			{
				unsigned char *line = picture.scanLine(y);
				for (int x = 0; x < width; ++x)
				{
					const unsigned char *p = pixels + (y * width + x) * channels;
					// Gray, gray with alpha, RGB, RGBA.
					line[x * 4 + 0] = p[0];
					line[x * 4 + 1] = channels >= 3 ? p[1] : p[0];
					line[x * 4 + 2] = channels >= 3 ? p[2] : p[0];
					line[x * 4 + 3] = channels == 4 ? p[3] : channels == 2 ? p[1] : 255;
				}
			}
			icon = QIcon(QPixmap::fromImage(picture));
		}
	}
	icons.insert(name, icon);
	return icon;
}

bool isEscapeFree(QObject *watched)
{
	if (QApplication::activeModalWidget() || QApplication::activePopupWidget())
		return false;
	for (QObject *object : {watched, static_cast<QObject *>(QApplication::focusWidget())})
	{
		if (qobject_cast<QLineEdit *>(object) || qobject_cast<QTextEdit *>(object)
			|| qobject_cast<QPlainTextEdit *>(object) || qobject_cast<QAbstractSpinBox *>(object))
			return false;
		if (const QWidget *widget = qobject_cast<QWidget *>(object))
		{
			if (qobject_cast<const QDialog *>(widget->window()))
				return false;
		}
	}
	return true;
}

QString axisColor(int axis)
{
	const char *const colors[3] = {"#e05a5a", "#6cc06c", "#5a8ce0"};
	return QString::fromUtf8(colors[axis < 0 || axis > 2 ? 0 : axis]);
}

QColor iconColor()
{
	return QColor(228, 228, 228);
}

QIcon makeFilterIcon()
{
	QPixmap pixmap(32, 32);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(iconColor());

	// A funnel.
	const QPointF points[] = {QPointF(3, 5), QPointF(29, 5), QPointF(19, 17), QPointF(19, 28),
		QPointF(13, 24), QPointF(13, 17)};
	painter.drawPolygon(points, 6);
	return QIcon(pixmap);
}

QIcon makeRulerIcon()
{
	QPixmap pixmap(32, 32);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);

	// A ruler lying from the bottom left to the top right, with ticks along its upper
	// edge, every other one longer.
	painter.translate(16, 16);
	painter.rotate(-45);
	painter.setPen(QPen(iconColor(), 2.4));
	painter.setBrush(Qt::NoBrush);
	painter.drawRoundedRect(QRectF(-15, -5.5, 30, 11), 1.5, 1.5);
	painter.setPen(QPen(iconColor(), 2.0, Qt::SolidLine, Qt::FlatCap));
	for (int i = 0; i < 5; ++i)
	{
		const qreal x = -10 + i * 5;
		painter.drawLine(QPointF(x, -5.5), QPointF(x, i % 2 ? -1.5 : 0.5));
	}
	return QIcon(pixmap);
}

QIcon makeAngleIcon()
{
	QPixmap pixmap(32, 32);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);

	// Two sides from the corner at the bottom left: one along the bottom, one up to
	// the right, and an arc between them.
	painter.setPen(QPen(iconColor(), 2.6, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	painter.setBrush(Qt::NoBrush);
	const QPointF lines[] = {QPointF(29, 27), QPointF(4, 27), QPointF(22, 5)};
	painter.drawPolyline(lines, 3);
	painter.setPen(QPen(iconColor(), 2.0, Qt::SolidLine, Qt::FlatCap));
	painter.drawArc(QRectF(4 - 13, 27 - 13, 26, 26), 0, 51 * 16);
	return QIcon(pixmap);
}

QIcon makeFigureIcon()
{
	QPixmap pixmap(32, 32);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(Qt::NoPen);
	painter.setBrush(iconColor());

	// A head, and the body with the arms and the legs as one shape.
	painter.drawEllipse(QRectF(12.5, 1, 7, 7));
	const QPointF body[] = {
		QPointF(10, 9.5), QPointF(22, 9.5), QPointF(24.5, 12), QPointF(25, 20), QPointF(22.5, 20),
		QPointF(22, 14), QPointF(21, 14), QPointF(20.5, 31), QPointF(17, 31), QPointF(16, 21),
		QPointF(15, 31), QPointF(11.5, 31), QPointF(11, 14), QPointF(10, 14), QPointF(9.5, 20),
		QPointF(7, 20), QPointF(7.5, 12),
	};
	painter.drawPolygon(body, int(sizeof(body) / sizeof(body[0])));
	return QIcon(pixmap);
}

QIcon makeResetIcon()
{
	QPixmap pixmap(32, 32);
	pixmap.fill(Qt::transparent);

	QPainter painter(&pixmap);
	painter.setRenderHint(QPainter::Antialiasing);

	// A clockwise circular arrow: a ring open at the top right, with the arrow head
	// at its top end pointing to the right.
	painter.setPen(QPen(iconColor(), 3.6, Qt::SolidLine, Qt::FlatCap));
	painter.setBrush(Qt::NoBrush);
	painter.drawArc(QRectF(5, 7, 20, 20), 80 * 16, 290 * 16);

	painter.setPen(Qt::NoPen);
	painter.setBrush(iconColor());
	const QPointF head[] = {QPointF(16, 0.5), QPointF(16, 13.5), QPointF(25.5, 7)};
	painter.drawPolygon(head, 3);
	return QIcon(pixmap);
}

QIcon makeToggleIcon(const QString &path)
{
	const QPixmap light(path);
	if (light.isNull())
		return QIcon();

	QPixmap dark = light;
	{
		QPainter painter(&dark);
		painter.setCompositionMode(QPainter::CompositionMode_SourceIn);
		painter.fillRect(dark.rect(), QColor(28, 28, 28));
	}

	QIcon icon;
	icon.addPixmap(light, QIcon::Normal, QIcon::Off);
	icon.addPixmap(dark, QIcon::Normal, QIcon::On);
	return icon;
}

QToolButton *makeResetButton(const QString &tooltip, QObject *context, const std::function<void()> &reset)
{
	QToolButton *button = new QToolButton;
	button->setAutoRaise(true);
	button->setIcon(makeResetIcon());
	button->setIconSize(QSize(16, 16));
	button->setFixedSize(22, 22);
	button->setFocusPolicy(Qt::NoFocus);
	button->setToolTip(tooltip);
	QObject::connect(button, &QToolButton::clicked, context, [reset]() { reset(); });
	return button;
}

QString toggleButtonStyle(const QString &color, const QString &off_text_color, int min_width)
{
	return QString::fromUtf8(
		"QToolButton { color: %2; font-weight: bold; background-color: #3b3e43;"
		" border: 1px solid #5a5d63; border-radius: 3px; padding: 3px 0px; min-width: %3px; }"
		"QToolButton:hover { background-color: #4b4e54; }"
		"QToolButton:checked { color: #1c1c1c; background-color: %1; border-color: %1; }"
		"QToolButton:disabled { color: #7a7a7a; }")
		.arg(color, off_text_color).arg(min_width);
}

QString choiceButtonStyle()
{
	return QString::fromUtf8(
		"QToolButton { color: #e4e4e4; font-weight: bold; background-color: #3b3e43;"
		" border: 1px solid #5a5d63; border-radius: 3px; padding: 3px 6px; min-width: 64px; }"
		"QToolButton:hover { background-color: #4b4e54; }"
		"QToolButton:pressed { background-color: #2f3236; }"
		"QToolButton:checked { color: white; background-color: #2f6fd6; border-color: #2f6fd6; }"
		"QToolButton:disabled { color: #7a7a7a; }");
}

QString primaryButtonStyle()
{
	return QString::fromUtf8(
		"QPushButton { background-color: #2f6fd6; color: white; border: none; border-radius: 3px;"
		" font-weight: bold; padding: 5px; }"
		"QPushButton:hover { background-color: #3d7fe8; }"
		"QPushButton:checked { background-color: #e08a2a; }"
		"QPushButton:checked:hover { background-color: #f09c3e; }"
		"QPushButton:disabled { background-color: #4a4a4a; color: #8c8c8c; }");
}

namespace
{

QString filled_button_style(const char *color, const char *hover, const char *pressed)
{
	return QString::fromUtf8(
		"QPushButton { background-color: %1; color: white; border: none; border-radius: 3px;"
		" font-weight: bold; padding: 5px; }"
		"QPushButton:hover { background-color: %2; }"
		"QPushButton:pressed { background-color: %3; }"
		"QPushButton:disabled { background-color: #4a4a4a; color: #8c8c8c; }")
		.arg(QString::fromUtf8(color), QString::fromUtf8(hover), QString::fromUtf8(pressed));
}

} // namespace

namespace
{
int visualizer_requests = 0;
bool visualizer_turned_on = false;
} // namespace

void requestVisualizer()
{
	if (visualizer_requests++ > 0)
		return;
	visualizer_turned_on = Unigine::Visualizer::getMode() == Unigine::Visualizer::MODE_DISABLED;
	if (visualizer_turned_on)
		Unigine::Visualizer::setMode(Unigine::Visualizer::MODE_ENABLED_DEPTH_TEST_ENABLED);
}

void releaseVisualizer()
{
	if (visualizer_requests <= 0 || --visualizer_requests > 0)
		return;
	if (visualizer_turned_on && Unigine::Visualizer::getMode() == Unigine::Visualizer::MODE_ENABLED_DEPTH_TEST_ENABLED)
		Unigine::Visualizer::setMode(Unigine::Visualizer::MODE_DISABLED);
	visualizer_turned_on = false;
}

QString goButtonStyle()
{
	return filled_button_style("#3f9a4a", "#4bad57", "#358540");
}

QString attentionButtonStyle()
{
	return filled_button_style("#2f6fd6", "#3d7fe8", "#285fb8");
}

QString handWorkButtonStyle()
{
	return filled_button_style("#c56a1c", "#d87b2a", "#a95a15");
}

namespace
{

const char TINTED_FRAME_NAME[] = "artist_tool_tinted_frame";
// The plain containers of the rows inside a tinted frame.
const char CLEAR_CONTAINER_NAME[] = "artist_tool_clear";

// A widget that only holds others (a row, a cell): a QWidget or a QFrame itself, not the
// viewport of a list or a text field (they keep their background).
bool is_plain_container(const QWidget *widget)
{
	const QMetaObject *type = widget->metaObject();
	return (type == &QWidget::staticMetaObject || type == &QFrame::staticMetaObject)
		&& !qobject_cast<const QAbstractScrollArea *>(widget->parentWidget());
}

// Names the plain containers inside a tinted frame, also the ones added to it later (the
// rows of a list made anew): the style sheet of the frame makes them see-through.
class ContainerNamer final : public QObject
{
public:
	using QObject::QObject;

	void watch(QWidget *widget)
	{
		widget->installEventFilter(this);
		if (widget->objectName().isEmpty() && is_plain_container(widget))
			widget->setObjectName(QString::fromUtf8(CLEAR_CONTAINER_NAME));
	}

protected:
	bool eventFilter(QObject *watched, QEvent *event) override
	{
		// After it is made and polished: its class is known then. Polished with the old
		// name, it is polished again.
		// The widgets inside it were polished before it, unwatched: they are named too.
		if (event->type() == QEvent::ChildPolished)
		{
			if (QWidget *child = qobject_cast<QWidget *>(static_cast<QChildEvent *>(event)->child()))
			{
				adopt(child);
				for (QWidget *inner : child->findChildren<QWidget *>())
					adopt(inner);
			}
		}
		return QObject::eventFilter(watched, event);
	}

private:
	void adopt(QWidget *widget)
	{
		const bool rename = widget->objectName().isEmpty() && is_plain_container(widget);
		watch(widget);
		if (rename)
		{
			widget->style()->unpolish(widget);
			widget->style()->polish(widget);
			widget->update();
		}
	}
};

} // namespace

void makeTintedFrame(QWidget *frame)
{
	// The style of the editor gives every widget the gray of the panel (a palette is
	// overridden by it), so the frame gets its gray by a style sheet. What inside it would
	// paint the panel gray over it - the containers of the rows, labels, check and radio
	// buttons, dials - is see-through. A frame inside it keeps the panel gray (the user
	// wants it so), as do fields, lists and buttons.
	frame->setObjectName(QString::fromUtf8(TINTED_FRAME_NAME));
	ContainerNamer *namer = new ContainerNamer(frame);
	for (QWidget *widget : frame->findChildren<QWidget *>())
		namer->watch(widget);
	namer->watch(frame);

	QStringList inside;
	for (const char *type : {"#artist_tool_clear", "QLabel", "QCheckBox", "QRadioButton", "QDial"})
		inside << QString::fromUtf8("#%1 %2").arg(QString::fromUtf8(TINTED_FRAME_NAME), QString::fromUtf8(type));
	frame->setStyleSheet(QString::fromUtf8("#%1 { background-color: rgba(0, 0, 0, 28); } %2 { background-color: transparent; }")
		.arg(QString::fromUtf8(TINTED_FRAME_NAME), inside.join(QString::fromUtf8(", "))));
}

void tintPageFrames(QWidget *page)
{
	for (QGroupBox *box : page->findChildren<QGroupBox *>())
	{
		bool nested = false;
		for (QWidget *parent = box->parentWidget(); parent && parent != page; parent = parent->parentWidget())
			nested = nested || qobject_cast<QGroupBox *>(parent);
		if (!nested)
			makeTintedFrame(box);
	}
}

QButtonGroup *makeAxisButtons(QHBoxLayout *layout, QObject *parent, const QString &tooltip)
{
	QButtonGroup *group = new QButtonGroup(parent);
	group->setExclusive(true);

	const char *const names[6] = {"X+", "X-", "Y+", "Y-", "Z+", "Z-"};
	for (int id = 0; id < 6; ++id)
	{
		const QString color = axisColor(id / 2);

		QToolButton *button = new QToolButton;
		button->setText(QString::fromUtf8(names[id]));
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(tooltip);
		button->setStyleSheet(toggleButtonStyle(color, color, 34));
		group->addButton(button, id);
		layout->addWidget(button);
	}
	return group;
}

QToolButton *makeSectionHeader(const QString &title, QWidget *body)
{
	QToolButton *header = new QToolButton;
	header->setText(title);
	header->setCheckable(true);
	header->setChecked(true);
	header->setArrowType(Qt::DownArrow);
	header->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	header->setAutoRaise(true);
	header->setFocusPolicy(Qt::NoFocus);
	header->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
	QFont font = header->font();
	font.setBold(true);
	header->setFont(font);

	QObject::connect(header, &QToolButton::toggled, body, [header, body](bool expanded) {
		header->setArrowType(expanded ? Qt::DownArrow : Qt::RightArrow);
		body->setVisible(expanded);
	});
	return header;
}

namespace
{

// "projects/<name>_<hash>": the name of the project folder for reading, and a hash of
// the whole path, so that two projects of the same name do not share it.
QString project_group()
{
	const QString data = QDir::cleanPath(QDir::fromNativeSeparators(QString::fromUtf8(Unigine::Engine::get()->getDataPath()))).toLower();
	QString name = QDir(data).dirName() == QLatin1String("data") ? QFileInfo(data).dir().dirName() : QDir(data).dirName();
	for (QChar &c : name)
	{
		if (!c.isLetterOrNumber() || c.unicode() >= 128)
			c = QLatin1Char('_');
	}
	const QString hash = QString::fromLatin1(QCryptographicHash::hash(data.toUtf8(), QCryptographicHash::Md5).toHex().left(8));
	return QString::fromUtf8("projects/") + name + QLatin1Char('_') + hash;
}

} // namespace

void removeProjectSettings(QSettings &settings, const char *tool_group, const QStringList &global_keys)
{
	settings.beginGroup(QString::fromUtf8(tool_group));
	for (const QString &key : global_keys)
		settings.remove(key);
	settings.remove(project_group());
	settings.endGroup();
}

void beginProjectGroup(QSettings &settings, const char *tool_group, const QStringList &global_keys)
{
	const QString group = project_group();

	settings.beginGroup(QString::fromUtf8(tool_group));
	for (const QString &key : settings.childKeys())
	{
		if (global_keys.contains(key))
			continue;
		const QString moved = group + QLatin1Char('/') + key;
		if (!settings.contains(moved))
			settings.setValue(moved, settings.value(key));
		settings.remove(key);
	}
	settings.beginGroup(group);
}

} // namespace ArtistTool
