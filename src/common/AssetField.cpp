#include "AssetField.h"
#include "UiHelpers.h"

#include <editor/UnigineAssetDialogs.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>

#include <UnigineCallback.h>

#include <QAction>
#include <QDataStream>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QHBoxLayout>
#include <QIcon>
#include <QLineEdit>
#include <QMimeData>
#include <QPushButton>
#include <QToolButton>

using namespace Unigine;

namespace ArtistTool
{

namespace
{

// What the Asset Browser and the Materials window of the editor put into a drag: the
// GUIDs of the assets and of their runtimes as strings (QString), the GUIDs of materials
// as C strings.
const char ASSETS_MIME[] = "application/unigine.assets.list.assets";
const char RUNTIMES_MIME[] = "application/unigine.assets.list.runtimes";
const char MATERIALS_MIME[] = "application/unigine.material.list";

Vector<UGUID> dropped_guids(const QMimeData *mime)
{
	Vector<UGUID> guids;
	if (!mime)
		return guids;
	for (const char *format : {RUNTIMES_MIME, ASSETS_MIME})
	{
		QByteArray bytes = mime->data(QString::fromUtf8(format));
		QDataStream stream(&bytes, QIODevice::ReadOnly);
		while (!stream.atEnd())
		{
			QString text;
			stream >> text;
			const UGUID guid(text.toUtf8().constData());
			if (guid.isValid())
				guids.append(guid);
		}
	}
	QByteArray bytes = mime->data(QString::fromUtf8(MATERIALS_MIME));
	QDataStream stream(&bytes, QIODevice::ReadOnly);
	while (!stream.atEnd())
	{
		char *text = nullptr;
		stream >> text;
		if (!text)
			break;
		const UGUID guid(text);
		delete[] text;
		if (guid.isValid())
			guids.append(guid);
	}
	return guids;
}

} // namespace

AssetField::AssetField(const Options &options, QWidget *parent)
	: QWidget(parent)
	, options_(options)
{
	setAcceptDrops(true);
	QHBoxLayout *layout = new QHBoxLayout(this);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(3);

	name_ = new QLineEdit;
	name_->setReadOnly(true);
	name_->setFocusPolicy(Qt::NoFocus);
	// The drops go to the field itself.
	name_->setAcceptDrops(false);
	name_->setToolTip(options.tooltip);
	if (!options.icon.isEmpty())
		icon_ = name_->addAction(QIcon(options.icon), QLineEdit::LeadingPosition);
	layout->addWidget(name_, 1);

	// What is selected in the Asset Browser, as "Add Selected Assets" of the lists.
	if (!options.take_text.isEmpty())
	{
		QPushButton *take = new QPushButton(options.take_text);
		take->setIcon(QIcon(QString::fromUtf8(":/images/mods/icon_object_mode.png")));
		take->setIconSize(QSize(16, 16));
		take->setFocusPolicy(Qt::NoFocus);
		take->setToolTip(options.take_tooltip);
		layout->addWidget(take);
		connect(take, &QPushButton::clicked, this, [this]() {
			Vector<UGUID> guids;
			if (const ::UnigineEditor::SelectorGUIDs *runtimes = ::UnigineEditor::Selection::getSelectorRuntimes())
				guids.append(runtimes->getGUIDs());
			if (const ::UnigineEditor::SelectorGUIDs *materials = ::UnigineEditor::Selection::getSelectorMaterials())
				guids.append(materials->getGUIDs());
			if (on_picked)
				on_picked(guids);
		});
	}

	// The small buttons of the editor's asset fields.
	const auto add_button = [&](const char *icon, const QString &tooltip) {
		QToolButton *button = new QToolButton;
		button->setFixedSize(16, 16);
		button->setIcon(QIcon(QString::fromUtf8(icon)));
		button->setAutoRaise(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setToolTip(tooltip);
		layout->addWidget(button);
		return button;
	};
	QToolButton *browse = add_button(":/images/icon_folder.png", options.dialog_title);
	connect(browse, &QToolButton::clicked, this, [this]() {
		if (on_browse)
		{
			on_browse();
			return;
		}
		const QByteArray title = options_.dialog_title.toUtf8();
		const QByteArray filter = options_.filter.toUtf8();
		::UnigineEditor::AssetDialogs::browseAsset(MakeCallback([this](const ::UnigineEditor::AssetDialogs::SelectedAsset &asset) {
			Vector<UGUID> guids;
			if (asset.runtime_guid.isValid())
				guids.append(asset.runtime_guid);
			if (asset.asset_guid.isValid())
				guids.append(asset.asset_guid);
			if (on_picked && !guids.empty())
				on_picked(guids);
		}), title.isEmpty() ? nullptr : title.constData(), filter.isEmpty() ? nullptr : filter.constData());
	});
	show_ = add_button(":/images/icon_go_to_asset.png", options.show_tooltip);
	connect(show_, &QToolButton::clicked, this, [this]() {
		if (on_show)
			on_show();
	});
	if (!options.reset_tooltip.isEmpty())
	{
		reset_ = makeResetButton(options.reset_tooltip, this, [this]() {
			if (on_reset)
				on_reset();
		});
		layout->addWidget(reset_);
	}
}

void AssetField::setValue(const QString &text, bool is_set)
{
	if (name_->text() != text)
		name_->setText(text);
	// The default is dimmed, as an unset field of the editor.
	name_->setStyleSheet(is_set ? QString() : QString::fromUtf8("QLineEdit { color: #8a8a8a; }"));
	if (reset_)
		reset_->setEnabled(is_set);
}

void AssetField::setShowEnabled(bool enabled)
{
	show_->setEnabled(enabled);
}

void AssetField::setIcon(const QIcon &icon)
{
	if (icon.isNull())
		return;
	if (icon_)
		icon_->setIcon(icon);
	else
		icon_ = name_->addAction(icon, QLineEdit::LeadingPosition);
}

void AssetField::dragEnterEvent(QDragEnterEvent *event)
{
	if (on_picked && !dropped_guids(event->mimeData()).empty())
		event->acceptProposedAction();
}

void AssetField::dropEvent(QDropEvent *event)
{
	const Vector<UGUID> guids = dropped_guids(event->mimeData());
	if (on_picked && !guids.empty())
	{
		event->acceptProposedAction();
		on_picked(guids);
	}
}

} // namespace ArtistTool
