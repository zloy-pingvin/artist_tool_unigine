#pragma once

#include <QColor>
#include <QPixmap>
#include <QPoint>
#include <QWidget>

namespace ArtistTool
{

// A magnifier next to the mouse, as in the point tools of ImageModeler: what is on the
// screen around a point, enlarged several times with sharp pixels, with a crosshair
// right on that point. It lets a point be put exactly.
//
// It is a small window of its own over the viewport that takes no clicks and no focus.
class Loupe final : public QWidget
{
public:
	Loupe();

	// Shows the screen around 'center' (global screen coordinates) next to the mouse.
	// The frame and the crosshair are of 'color'.
	void showAt(const QPoint &center, const QColor &color);

protected:
	void paintEvent(QPaintEvent *event) override;

private:
	QPixmap view_;
	QColor color_;
};

} // namespace ArtistTool
