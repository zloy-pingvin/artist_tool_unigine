#include "AssetBrowserJump.h"
#include "Localization.h"
#include "UiHelpers.h"

#include <UnigineLog.h>

#include <QAbstractItemView>
#include <QAbstractProxyModel>
#include <QApplication>
#include <QBoxLayout>
#include <QEvent>
#include <QIcon>
#include <QPushButton>
#include <QToolButton>

#include <cstring>

namespace ArtistTool
{

namespace
{

// The names the editor gives to its widgets.
const char BROWSE_DIALOG_NAME[] = "as::BrowseDialog";
const char BROWSER_CLASS[] = "as::Browser";
const char ASSET_BROWSER_NAME_PREFIX[] = "assets_w";
const char NAVIGATION_BAR_CLASS[] = "as::NavigationBar";
const char ASSET_LIST_NAME[] = "ListView";
// The private slot of as::Browser that a click on a folder of its navigation bar goes to.
const char OPEN_FOLDER_SLOT[] = "navbar_selection_changed";

const char BUTTON_NAME[] = "artist_tool_jump_to_asset_browser";

bool is_class(const QObject *object, const char *name)
{
	return strcmp(object->metaObject()->className(), name) == 0;
}

bool is_asset_browser(const QWidget *widget)
{
	return is_class(widget, BROWSER_CLASS)
		&& widget->objectName().startsWith(QLatin1String(ASSET_BROWSER_NAME_PREFIX));
}

QWidget *find_navigation_bar(const QWidget *browser)
{
	for (QWidget *child : browser->findChildren<QWidget *>())
	{
		if (is_class(child, NAVIGATION_BAR_CLASS))
			return child;
	}
	return nullptr;
}

// The Asset Browser to take the folder from: the one the user was in last, or else one on
// the screen, or else one in a tab behind another window.
QWidget *find_asset_browser(QWidget *last_browser)
{
	if (last_browser && last_browser->isVisible())
		return last_browser;

	QWidget *hidden = last_browser;
	for (QWidget *widget : QApplication::allWidgets())
	{
		if (!is_asset_browser(widget))
			continue;
		if (widget->isVisible())
			return widget;
		if (!hidden)
			hidden = widget;
	}
	return hidden;
}

// The folder a browser shows, as the names on its navigation bar from the top down. The
// bar is read and not the root of the list because the list has no root during a search.
QStringList open_folder_names(const QWidget *browser)
{
	QStringList names;
	if (const QWidget *bar = find_navigation_bar(browser))
	{
		for (const QPushButton *button : bar->findChildren<QPushButton *>())
		{
			// The bar keeps the buttons of a deeper folder it showed before, hidden.
			if (!button->isHidden())
				names.append(button->text());
		}
	}
	return names;
}

// The model of the assets under the proxies of a browser. All browsers share it, so its
// indexes are good for any of them.
const QAbstractItemModel *asset_model(const QWidget *browser)
{
	const QAbstractItemView *list = browser->findChild<QAbstractItemView *>(QLatin1String(ASSET_LIST_NAME));
	const QAbstractItemModel *model = list ? list->model() : nullptr;
	while (const QAbstractProxyModel *proxy = qobject_cast<const QAbstractProxyModel *>(model))
		model = proxy->sourceModel();
	return model;
}

QModelIndex find_folder(const QAbstractItemModel *model, const QStringList &names)
{
	// The bar has no button for the root item of the model.
	QModelIndex folder = model->index(0, 0);
	for (const QString &name : names)
	{
		QModelIndex next;
		for (int row = 0, count = model->rowCount(folder); row < count && !next.isValid(); row++)
		{
			const QModelIndex child = model->index(row, 0, folder);
			if (child.data().toString() == name)
				next = child;
		}
		if (!next.isValid())
			return QModelIndex();
		folder = next;
	}
	return folder;
}

} // namespace

AssetBrowserJump::AssetBrowserJump(QObject *parent)
	: QObject(parent)
{
	connect(qApp, &QApplication::focusChanged, this, [this](QWidget *, QWidget *now) {
		for (QWidget *widget = now; widget; widget = widget->parentWidget())
		{
			if (is_asset_browser(widget))
			{
				last_browser_ = widget;
				break;
			}
		}
	});
	qApp->installEventFilter(this);
}

AssetBrowserJump::~AssetBrowserJump()
{
	qApp->removeEventFilter(this);
}

bool AssetBrowserJump::eventFilter(QObject *watched, QEvent *event)
{
	if (event->type() == QEvent::Show && watched->isWidgetType()
		&& watched->objectName() == QLatin1String(BROWSE_DIALOG_NAME))
		add_button(static_cast<QWidget *>(watched));
	return false;
}

void AssetBrowserJump::add_button(QWidget *dialog)
{
	if (dialog->findChild<QToolButton *>(QLatin1String(BUTTON_NAME)))
		return;

	QWidget *browser = nullptr;
	for (QWidget *child : dialog->findChildren<QWidget *>())
	{
		if (is_class(child, BROWSER_CLASS))
		{
			browser = child;
			break;
		}
	}
	QWidget *bar = browser ? find_navigation_bar(browser) : nullptr;
	QBoxLayout *layout = bar ? qobject_cast<QBoxLayout *>(bar->layout()) : nullptr;
	if (!layout)
	{
		Unigine::Log::warning("artist_tool: the navigation bar of the select dialog is not found\n");
		return;
	}

	// At the end of the navigation bar, right before the search field. Not in the toolbar
	// itself: the browser rebuilds its layout on every move of the splitter and would lose
	// a widget it does not know. The bar stays as it is. A caption and a background of its
	// own: an icon alone is lost among the small buttons of this row.
	QToolButton *button = new QToolButton(bar);
	button->setObjectName(QLatin1String(BUTTON_NAME));
	button->setText(uiText("Go to Asset Browser Folder", "Перейти в папку Asset Browser"));
	button->setIcon(QIcon(QString::fromUtf8(":/images/icon_folder.png")));
	button->setIconSize(QSize(16, 16));
	button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
	button->setFocusPolicy(Qt::NoFocus);
	button->setStyleSheet(choiceButtonStyle() + QString::fromUtf8("QToolButton { padding: 1px 6px; }"));
	button->setToolTip(Localization().tip("Open here the folder that is open in the Asset Browser.",
		"Открыть здесь папку, открытую в Asset Browser."));
	layout->addWidget(button);
	connect(button, &QToolButton::clicked, this, [this, browser]() { jump(browser); });
}

void AssetBrowserJump::jump(QWidget *dialog_browser)
{
	const QWidget *browser = find_asset_browser(last_browser_);
	const QAbstractItemModel *model = browser ? asset_model(browser) : nullptr;
	const QModelIndex folder = model ? find_folder(model, open_folder_names(browser)) : QModelIndex();
	if (!folder.isValid() || !QMetaObject::invokeMethod(dialog_browser, OPEN_FOLDER_SLOT,
			Qt::DirectConnection, Q_ARG(QModelIndex, folder)))
		Unigine::Log::warning("artist_tool: the folder of the Asset Browser can not be opened in the dialog\n");
}

} // namespace ArtistTool
