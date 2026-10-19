#include "Loupe.h"

#include <QCursor>
#include <QGuiApplication>
#include <QPainter>
#include <QPainterPath>
#include <QScreen>

namespace ArtistTool
{

namespace
{

// The size of the magnifier on the screen and how many times it enlarges.
const int LOUPE_SIZE = 128;
const int LOUPE_ZOOM = 4;
// How far from the mouse it is.
const int LOUPE_OFFSET = 24;
// The gap the crosshair leaves around its center, so that the very pixel is seen.
const int CROSS_GAP = LOUPE_ZOOM + 2;

} // namespace

Loupe::Loupe()
	: QWidget(nullptr, Qt::ToolTip | Qt::FramelessWindowHint | Qt::WindowTransparentForInput
		| Qt::WindowDoesNotAcceptFocus)
{
	setAttribute(Qt::WA_ShowWithoutActivating);
	setAttribute(Qt::WA_TransparentForMouseEvents);
	// Round: the corners of the window are see-through.
	setAttribute(Qt::WA_TranslucentBackground);
	setFixedSize(LOUPE_SIZE, LOUPE_SIZE);
}

void Loupe::showAt(const QPoint &center, const QColor &color)
{
	QScreen *screen = QGuiApplication::screenAt(center);
	if (!screen)
	{
		hide();
		return;
	}

	// What is on the screen around the point, LOUPE_ZOOM times smaller than the
	// magnifier. It is taken before the magnifier moves, so that it never catches
	// itself; an odd number of pixels, so that the point is in the middle one.
	const int grab = LOUPE_SIZE / LOUPE_ZOOM + 1;
	const QPoint local = center - screen->geometry().topLeft();
	view_ = screen->grabWindow(0, local.x() - grab / 2, local.y() - grab / 2, grab, grab);
	color_ = color;

	// Up and to the right of the mouse; at the edges of the screen - on the other side.
	const QPoint mouse = QCursor::pos();
	const QRect area = screen->geometry();
	QPoint position(mouse.x() + LOUPE_OFFSET, mouse.y() - LOUPE_OFFSET - LOUPE_SIZE);
	if (position.x() + LOUPE_SIZE > area.right())
		position.setX(mouse.x() - LOUPE_OFFSET - LOUPE_SIZE);
	if (position.y() < area.top())
		position.setY(mouse.y() + LOUPE_OFFSET);
	if (pos() != position)
		move(position);

	if (!isVisible())
		show();
	update();
}

void Loupe::paintEvent(QPaintEvent *)
{
	// Everything is inside the circle; only the ring of the frame is smoothed, the
	// pixels and the crosshair stay sharp.
	QPainter painter(this);
	const QRectF circle(1.5, 1.5, LOUPE_SIZE - 3, LOUPE_SIZE - 3);
	QPainterPath clip;
	clip.addEllipse(circle);
	painter.setClipPath(clip);
	painter.fillRect(rect(), Qt::black);
	// Every pixel of the screen becomes a square: no smoothing.
	painter.setRenderHint(QPainter::SmoothPixmapTransform, false);
	// The middle pixel is centered on the crosshair; the one extra pixel is cut in
	// halves at the edges.
	if (!view_.isNull())
	{
		const int size = (LOUPE_SIZE / LOUPE_ZOOM + 1) * LOUPE_ZOOM;
		const int shift = (size - LOUPE_SIZE) / 2;
		painter.drawPixmap(QRect(-shift, -shift, size, size), view_);
	}

	const int middle = LOUPE_SIZE / 2;
	// A dark line under the light one keeps the crosshair seen on any background.
	for (int pass = 0; pass < 2; ++pass)
	{
		painter.setPen(QPen(pass == 0 ? QColor(0, 0, 0, 160) : color_, pass == 0 ? 3 : 1));
		painter.drawLine(middle, 0, middle, middle - CROSS_GAP);
		painter.drawLine(middle, middle + CROSS_GAP, middle, LOUPE_SIZE);
		painter.drawLine(0, middle, middle - CROSS_GAP, middle);
		painter.drawLine(middle + CROSS_GAP, middle, LOUPE_SIZE, middle);
	}

	painter.setClipping(false);
	painter.setRenderHint(QPainter::Antialiasing);
	painter.setPen(QPen(color_, 2));
	painter.setBrush(Qt::NoBrush);
	painter.drawEllipse(circle);
}

} // namespace ArtistTool
