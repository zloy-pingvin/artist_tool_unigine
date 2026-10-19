#include "artist_tool.h"
#include "ArtistToolWindow.h"
#include "AssetLocator.h"
#include "UndoActions.h"

#include <editor/UnigineConstants.h>
#include <editor/UnigineWindowManager.h>

#include <UnigineLog.h>

#include <QAction>
#include <QMenu>

using ::UnigineEditor::WindowManager;

artist_tool::artist_tool()  = default;
artist_tool::~artist_tool() = default;

bool artist_tool::init()
{
	QMenu *menu = WindowManager::findMenu(::UnigineEditor::Constants::MM_WINDOWS);
	if (!menu)
	{
		Unigine::Log::error("artist_tool: the Windows menu is not found\n");
		return false;
	}

	action_ = menu->addAction("Artist Tools", this, &artist_tool::showWindow);
	asset_locator_ = new ArtistTool::AssetLocator(this);
	return true;
}

void artist_tool::shutdown()
{
	delete asset_locator_;
	asset_locator_ = nullptr;

	if (action_)
	{
		if (QMenu *menu = WindowManager::findMenu(::UnigineEditor::Constants::MM_WINDOWS))
			menu->removeAction(action_);
		delete action_;
		action_ = nullptr;
	}

	if (window_)
	{
		window_->shutdown();
		WindowManager::remove(window_);
		delete window_;
		window_ = nullptr;
	}

	// The steps of the undo history made by the plugin go away with its code.
	ArtistTool::releaseUndoActions();
}

void artist_tool::showWindow()
{
	// The window is created on first use and then only hidden / shown again, so the
	// tool options and the log survive closing it.
	if (!window_)
	{
		window_ = new ArtistTool::ArtistToolWindow;
		WindowManager::add(window_, WindowManager::NEW_FLOATING_AREA);
		WindowManager::resize(window_, 480, 720);
	}

	WindowManager::show(window_);
}
