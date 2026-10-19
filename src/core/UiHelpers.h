#pragma once

#include <QColor>
#include <QIcon>
#include <QString>
#include <QStringList>

#include <functional>

class QAbstractSpinBox;
class QButtonGroup;
class QSettings;
class QDial;
class QHBoxLayout;
class QObject;
class QToolButton;
class QWidget;

// Small pieces of interface the tool pages share, so that they look the same.
namespace ArtistTool
{

// The folder with the plugin's own images, next to its library:
// bin/plugins/zloy_pingvin/artist_tool/icons.
QString pluginIconsDir();

// An image built into the editor, found by its file name wherever it is in the
// editor's resources (e.g. "icon_go_to_asset.png"). A null icon if there is none.
QIcon findEditorIcon(const QString &file_name);

// X - red, Y - green, Z - blue, the same as the axes in the editor.
QString axisColor(int axis);

// The editor theme is dark; its own icons are light.
QColor iconColor();
// Icons are drawn in code, so the tools do not need image files for them.
QIcon makeResetIcon();
QIcon makeFilterIcon();
// A ruler and an angle (two sides with an arc between them), for the Measure tools.
QIcon makeRulerIcon();
QIcon makeAngleIcon();
// A standing person, for the figure of the Measure tools.
QIcon makeFigureIcon();
// An icon of the editor (a path in its resources, ":/images/...") for a toggle button
// that is filled with a light color while it is on: the icon is as it is while the
// button is off and dark while it is on, so that it is seen on the light fill.
QIcon makeToggleIcon(const QString &path);

// A small "back to default" button with a circular arrow.
QToolButton *makeResetButton(const QString &tooltip, QObject *context, const std::function<void()> &reset);

// A toggle button of a row of alternatives: dark when off, filled with 'color' when
// on.
QString toggleButtonStyle(const QString &color, const QString &off_text_color, int min_width);
// A button of a row of alternatives with an icon and a caption: gray when off, blue
// when on.
QString choiceButtonStyle();
// The main button of a page: blue, orange while checked (the mode it turns on is
// working and the button stops it).
QString primaryButtonStyle();
// A button that says "this is what to press now": green, gray while disabled.
QString goButtonStyle();
// A button that asks for attention - what it does is not done by itself now: blue,
// gray while disabled.
QString attentionButtonStyle();

// Six toggle buttons - X+ X- Y+ Y- Z+ Z-, each letter in the color of its axis, one
// of them on. The ids of the buttons are 0..5 in that order (see AxisDirection).
QButtonGroup *makeAxisButtons(QHBoxLayout *layout, QObject *parent, const QString &tooltip);

// Makes the arrows at the right edge of a number field work the way they do in the
// editor's own fields: press them and drag - up or right adds, down or left takes
// away. A plain click still adds one step.
void enableSpinBoxDrag(QAbstractSpinBox *box);

// A dial for an angle in degrees, from 'minimum' to 'maximum'. Every 'major_step'
// degrees it has a longer mark, and while it is turned with the mouse it sticks to
// those angles when it comes within 'snap' degrees of one. The keys still set any
// angle.
QDial *makeAngleDial(int minimum, int maximum, int major_step = 45, int snap = 4);

// A header that folds and unfolds the widget under it, like the sections of the
// editor's own tool windows.
QToolButton *makeSectionHeader(const QString &title, QWidget *body);

// Opens in 'settings' the group a tool keeps the settings of the current project in:
// "<tool_group>/projects/<project>", the project told by the path of its data folder.
// A setting meant for one project - the brush of the Object Placer, the start of a
// new path - must not turn up in another one. What is not about a project (how the
// page looks: 'global_keys') stays in the group of the tool itself.
// Builds before 1.3 kept everything for all the projects at once; those values go
// to the first project that opens the group.
void beginProjectGroup(QSettings &settings, const char *tool_group, const QStringList &global_keys);
// Forgets what a tool remembers for the current project and its 'global_keys'; what it
// remembers for the other projects stays.
void removeProjectSettings(QSettings &settings, const char *tool_group, const QStringList &global_keys);

} // namespace ArtistTool
