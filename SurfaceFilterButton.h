#pragma once

#include "ViewportPicking.h"

#include <QString>
#include <QVector>
#include <QWidget>

#include <functional>

class QCheckBox;
class QPushButton;

namespace ArtistTool
{

// The filter of the kinds of objects a tool works on, the way the Cluster Paint mode
// of the editor has it: a field that lists what is checked and opens a menu of check
// boxes (the kinds of objects, "only Immovable", "only with Intersection"), and a
// button that puts it back to its default.
class SurfaceFilterButton final : public QWidget
{
public:
	struct Texts
	{
		QString tooltip;
		QString only_immovable;
		QString only_immovable_tooltip;
		QString only_intersection;
		QString only_intersection_tooltip;
		QString reset_tooltip;
	};

	explicit SurfaceFilterButton(const Texts &texts, QWidget *parent = nullptr);

	SurfaceFilter getFilter() const;
	// Shows the filter; on_changed is not called.
	void setFilter(const SurfaceFilter &filter);

	// Called when the filter is changed in the menu or reset.
	std::function<void()> on_changed;

protected:
	// Refits the text of the field when its width changes.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	struct Item
	{
		QCheckBox *check{nullptr};
		int surface_type{0};
	};

	void refresh_summary();
	void fit_text();

	QPushButton *button_{nullptr};
	// What the field says when it is wide enough to show all of it.
	QString summary_;
	QVector<Item> types_;
	QCheckBox *only_immovable_{nullptr};
	QCheckBox *only_intersection_{nullptr};
	bool updating_{false};
};

} // namespace ArtistTool
