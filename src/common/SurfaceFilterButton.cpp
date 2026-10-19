#include "SurfaceFilterButton.h"
#include "Localization.h"
#include "UiHelpers.h"

#include <QCheckBox>
#include <QEvent>
#include <QHBoxLayout>
#include <QMenu>
#include <QPushButton>
#include <QStringList>
#include <QToolButton>
#include <QWidgetAction>

namespace ArtistTool
{

namespace
{

struct SurfaceTypeName
{
	int type;
	const char *name;
};

const SurfaceTypeName SURFACE_TYPES[] = {
	{SurfaceFilter::MESH_STATIC, "Mesh Static"},
	{SurfaceFilter::MESH_SKINNED, "Mesh Skinned"},
	{SurfaceFilter::MESH_DYNAMIC, "Mesh Dynamic"},
	{SurfaceFilter::MESH_CLUSTER, "Mesh Cluster"},
	{SurfaceFilter::MESH_CLUTTER, "Mesh Clutter"},
	{SurfaceFilter::TERRAIN, "Terrain"},
	{SurfaceFilter::WATER, "Water"},
	{SurfaceFilter::CLOUDS, "Clouds"},
};

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

} // namespace

SurfaceFilterButton::SurfaceFilterButton(const Texts &texts, QWidget *parent)
	: QWidget(parent)
{
	QHBoxLayout *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(4);

	button_ = new QPushButton;
	button_->setIcon(makeFilterIcon());
	button_->setIconSize(QSize(14, 14));
	button_->setMinimumHeight(24);
	// The text is cut to the width of the field, so it must not ask for more room.
	button_->setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed);
	button_->installEventFilter(this);
	// A lighter field with the list of what is checked, like the filter of the
	// Cluster Paint mode.
	button_->setStyleSheet(QString::fromUtf8(
		"QPushButton { background-color: #5c6066; color: #f0f0f0; border: none; border-radius: 3px;"
		" padding: 3px 22px 3px 7px; text-align: left; }"
		"QPushButton:hover, QPushButton:pressed { background-color: #6b6f76; }"
		"QPushButton::menu-indicator { subcontrol-origin: padding; subcontrol-position: right center;"
		" right: 7px; }"));
	button_->setToolTip(texts.tooltip);

	QMenu *menu = new QMenu(button_);
	only_immovable_ = add_menu_check_box(menu, texts.only_immovable);
	only_immovable_->setToolTip(texts.only_immovable_tooltip);
	only_intersection_ = add_menu_check_box(menu, texts.only_intersection);
	only_intersection_->setToolTip(texts.only_intersection_tooltip);
	menu->addSeparator();
	for (const SurfaceTypeName &type : SURFACE_TYPES)
	{
		Item item;
		item.check = add_menu_check_box(menu, QString::fromUtf8(type.name));
		item.surface_type = type.type;
		types_.append(item);
	}
	button_->setMenu(menu);
	layout->addWidget(button_, 1);

	layout->addWidget(makeResetButton(texts.reset_tooltip, this, [this]() {
		setFilter(SurfaceFilter());
		if (on_changed)
			on_changed();
	}));

	const auto changed = [this](bool) {
		if (updating_)
			return;
		refresh_summary();
		if (on_changed)
			on_changed();
	};
	connect(only_immovable_, &QCheckBox::toggled, this, changed);
	connect(only_intersection_, &QCheckBox::toggled, this, changed);
	for (const Item &item : types_)
		connect(item.check, &QCheckBox::toggled, this, changed);

	setFilter(SurfaceFilter());
}

SurfaceFilter SurfaceFilterButton::getFilter() const
{
	SurfaceFilter filter;
	filter.surface_types = 0;
	for (const Item &item : types_)
	{
		if (item.check->isChecked())
			filter.surface_types |= item.surface_type;
	}
	filter.only_immovable = only_immovable_->isChecked();
	filter.only_intersection = only_intersection_->isChecked();
	return filter;
}

void SurfaceFilterButton::setFilter(const SurfaceFilter &filter)
{
	updating_ = true;
	for (const Item &item : types_)
		item.check->setChecked((filter.surface_types & item.surface_type) != 0);
	only_immovable_->setChecked(filter.only_immovable);
	only_intersection_->setChecked(filter.only_intersection);
	updating_ = false;
	refresh_summary();
}

bool SurfaceFilterButton::eventFilter(QObject *watched, QEvent *event)
{
	if (watched == button_ && event->type() == QEvent::Resize)
		fit_text();
	return QWidget::eventFilter(watched, event);
}

void SurfaceFilterButton::refresh_summary()
{
	QStringList names;
	for (const Item &item : types_)
	{
		if (item.check->isChecked())
			names.append(item.check->text());
	}

	// Everything that is checked, by name - the way the Cluster Paint filter shows it.
	if (only_immovable_->isChecked())
		names.prepend(uiText("Immovable Only", "Только Immovable"));
	if (only_intersection_->isChecked())
		names.prepend(uiText("With Intersection Only", "Только с Intersection"));

	summary_ = names.isEmpty() ? uiText("Nothing", "Ничего") : names.join(QString::fromUtf8(", "));
	fit_text();
}

// Shows as much of the summary as fits the field, with "..." at the end.
void SurfaceFilterButton::fit_text()
{
	// Icon, paddings and the drop-down arrow take about this much of the width.
	const int reserved = 56;
	const int available = qMax(40, button_->width() - reserved);
	button_->setText(button_->fontMetrics().elidedText(summary_, Qt::ElideRight, available));
}

} // namespace ArtistTool
