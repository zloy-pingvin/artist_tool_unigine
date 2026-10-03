#pragma once

#include <editor/UniginePlugin.h>
#include <QObject>

class QAction;

namespace ArtistTool
{
class ArtistToolWindow;
}

class artist_tool : public QObject, public ::UnigineEditor::Plugin
{
	Q_OBJECT
	Q_DISABLE_COPY(artist_tool)
	Q_PLUGIN_METADATA(IID UNIGINE_EDITOR_PLUGIN_IID FILE "artist_tool.json")
	Q_INTERFACES(UnigineEditor::Plugin)
public:
	artist_tool();
	~artist_tool() override;

	bool init() override;
	void shutdown() override;

public slots:
	void showWindow();

private:
	QAction *action_{nullptr};
	ArtistTool::ArtistToolWindow *window_{nullptr};
};
