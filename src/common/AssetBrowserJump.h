#pragma once

#include <QObject>
#include <QPointer>

class QWidget;

namespace ArtistTool
{

// A button in the select dialogs of the editor (Select Mesh, Select Node...) that opens in
// the dialog the folder open in the Asset Browser. The editor has no API for this, so it
// goes through the Qt objects of as::Browser, the widget both windows are made of.
class AssetBrowserJump : public QObject
{
public:
	explicit AssetBrowserJump(QObject *parent = nullptr);
	~AssetBrowserJump() override;

protected:
	// Adds the button to a select dialog as it opens.
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	void add_button(QWidget *dialog);
	void jump(QWidget *dialog_browser);

	// The Asset Browser the user was in last: the editor can have several of them.
	QPointer<QWidget> last_browser_;
};

} // namespace ArtistTool
