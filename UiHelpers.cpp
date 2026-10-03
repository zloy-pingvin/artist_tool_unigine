#include "UiHelpers.h"

#include <UnigineEngine.h>

#include <QAbstractSpinBox>
#include <QButtonGroup>
#include <QDir>
#include <QDirIterator>
#include <QFile>
#include <QFont>
#include <QHBoxLayout>
#include <QMouseEvent>
#include <QPainter>
#include <QPixmap>
#include <QToolButton>
#include <QWidget>

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
				// A click without a drag does what it did before: one step up.
				if (!dragged_)
					box_->stepBy(1);
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
	int steps_done_{0};
	QPointF press_position_;
};

} // namespace

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

QString goButtonStyle()
{
	return filled_button_style("#3f9a4a", "#4bad57", "#358540");
}

QString attentionButtonStyle()
{
	return filled_button_style("#2f6fd6", "#3d7fe8", "#285fb8");
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

} // namespace ArtistTool
