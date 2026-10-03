#include "ClutterUnpackPage.h"
#include "UiHelpers.h"

#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>

#include <QButtonGroup>
#include <QCheckBox>
#include <QFormLayout>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QLocale>
#include <QMessageBox>
#include <QPlainTextEdit>
#include <QProgressBar>
#include <QPushButton>
#include <QRadioButton>
#include <QSettings>
#include <QTreeWidget>
#include <QVBoxLayout>

using namespace Unigine;
using ::UnigineEditor::Selection;
using ::UnigineEditor::SelectorNodes;

namespace ArtistTool
{

namespace
{

const char SETTINGS_ORGANIZATION[] = "zloy_pingvin";
const char SETTINGS_APPLICATION[] = "artist_tool";
const char SETTINGS_GROUP[] = "clutter_unpack";

enum SelectionColumn
{
	COLUMN_NODE = 0,
	COLUMN_TYPE,
	COLUMN_CONTENT,
};

QString format_count(int value)
{
	return QLocale(QLocale::English).toString(value);
}

} // namespace

ClutterUnpackPage::ClutterUnpackPage(QWidget *parent)
	: QWidget(parent)
{
	unpacker_ = new ClutterUnpacker(this);
	unpacker_->setConfirmFunc([this](const QString &node_name, int instances) {
		const QMessageBox::StandardButton answer = QMessageBox::question(this,
			uiText("Unpack Clutter", "Распаковка клаттеров"),
			uiText("'%1' will create %2 nodes.\n\nThis many nodes can make the editor "
				"slow and the world file large. Continue?",
				"'%1' создаст нод: %2.\n\nТакое количество нод может замедлить редактор и сильно "
				"увеличить файл мира. Продолжить?")
				.arg(node_name, format_count(instances)),
			QMessageBox::Yes | QMessageBox::No, QMessageBox::No);
		return answer == QMessageBox::Yes;
	});

	build_ui();
	load_settings();

	connect(unpacker_, &ClutterUnpacker::started, this, [this](int total_nodes) {
		queued_count_ = total_nodes;
		set_busy(true);
	});
	connect(unpacker_, &ClutterUnpacker::progress, this, [this](int percent, const QString &status) {
		progress_->setValue(percent);
		status_label_->setText(status);
	});
	connect(unpacker_, &ClutterUnpacker::message, this, [this](int level, const QString &text) {
		append_log(level, text);
	});
	connect(unpacker_, &ClutterUnpacker::finished, this,
		[this](int succeeded, int failed, int instances, bool canceled) {
			set_busy(false);

			QString summary = canceled ? uiText("Canceled. ", "Отменено. ") : QString();
			summary += uiText("Unpacked %1 of %2 nodes, %3 instances created.", "Распаковано нод: %1 из %2, создано экземпляров: %3.")
				// Of all that were to be unpacked: after a cancel some were not even tried.
				.arg(succeeded).arg(qMax(queued_count_, succeeded + failed)).arg(format_count(instances));
			status_label_->setText(summary);
			append_log(failed || canceled ? ClutterUnpacker::MSG_WARNING : ClutterUnpacker::MSG_INFO,
				summary);

			refresh_selection();
		});

	connect(Selection::instance(), &Selection::changed, this, [this]() {
		refresh_selection();
	});

	set_busy(false);
	refresh_selection();
}

ClutterUnpackPage::~ClutterUnpackPage()
{
	save_settings();
}

void ClutterUnpackPage::cancel()
{
	unpacker_->cancel();
}

void ClutterUnpackPage::build_ui()
{
	QVBoxLayout *layout = new QVBoxLayout(this);

	QLabel *description = new QLabel(uiText(
		"Converts procedural clutters into regular nodes you can move, delete and edit "
		"one by one. Select clutters in the World Hierarchy and press Unpack.",
		"Превращает процедурные клаттеры в обычные ноды, которые можно двигать, удалять и "
		"редактировать по одной. Выделите клаттеры в World Hierarchy и нажмите «Распаковать»."));
	description->setWordWrap(true);
	layout->addWidget(description);

	// Selection.
	QGroupBox *selection_group = new QGroupBox(uiText("Selected clutters", "Выделенные клаттеры"));
	QVBoxLayout *selection_layout = new QVBoxLayout(selection_group);

	selection_tree_ = new QTreeWidget;
	selection_tree_->setHeaderLabels({uiText("Node", "Нода"), uiText("Type", "Тип"),
		uiText("Content", "Содержимое")});
	selection_tree_->setRootIsDecorated(false);
	selection_tree_->setSelectionMode(QAbstractItemView::NoSelection);
	selection_tree_->setFocusPolicy(Qt::NoFocus);
	selection_tree_->setUniformRowHeights(true);
	selection_tree_->setMinimumHeight(90);
	// Easy to read, like the World Nodes window: striped rows, roomier lines and an
	// icon of the clutter type.
	selection_tree_->setAlternatingRowColors(true);
	selection_tree_->setIconSize(QSize(16, 16));
	selection_tree_->setStyleSheet(QString::fromUtf8(
		"QTreeWidget { background-color: #26282b; alternate-background-color: #303338; }"
		"QTreeWidget::item { padding: 3px 2px; }"));

	// Both icons are built into the editor, so the plugin needs no image files of its
	// own: one for mesh clutters and clusters, one for node (world) clutters.
	mesh_clutter_icon_ = QIcon(QString::fromUtf8(":/sandworm/images/icon_point.png"));
	node_clutter_icon_ = QIcon(QString::fromUtf8(":/sandworm/images/icon_point_group_color.png"));
	// Columns are resized by dragging the header dividers; the last one takes the
	// remaining width. The widths are remembered between sessions.
	selection_tree_->header()->setSectionResizeMode(QHeaderView::Interactive);
	selection_tree_->header()->setStretchLastSection(true);
	selection_tree_->header()->setSectionsMovable(false);
	selection_tree_->header()->setMinimumSectionSize(60);
	selection_tree_->header()->resizeSection(COLUMN_NODE, 220);
	selection_tree_->header()->resizeSection(COLUMN_TYPE, 130);
	selection_tree_->setToolTip(loc_.tip(
		"Nodes selected in the World Hierarchy that can be unpacked:\n"
		"World Clutter, Mesh Clutter and Mesh Cluster. Other selected nodes are ignored.",
		"Выделенные в World Hierarchy ноды, которые можно распаковать:\n"
		"World Clutter, Mesh Clutter и Mesh Cluster. Остальные выделенные ноды игнорируются."));
	selection_layout->addWidget(selection_tree_);

	selection_label_ = new QLabel;
	selection_label_->setWordWrap(true);
	selection_layout->addWidget(selection_label_);

	layout->addWidget(selection_group, 1);

	// Options.
	QGroupBox *options_group = new QGroupBox(uiText("Options", "Настройки"));
	options_widget_ = options_group;
	QFormLayout *options_layout = new QFormLayout(options_group);

	// One of three, as a row of radio buttons.
	const QString source_mode_tooltip = loc_.tip(
		"What to do with the source clutter after it is unpacked.\n"
		"Disable - turn it off: it stays in the world, so you can re-enable it and unpack again.\n"
		"Keep enabled - leave it as it is.\n"
		"Delete - remove it from the world (can be undone with Ctrl+Z).",
		"Что сделать с исходным клаттером после распаковки.\n"
		"[[Disable|Выключить]] - выключить: клаттер остаётся в мире, его можно включить и распаковать заново.\n"
		"[[Keep enabled|Оставить включённым]] - оставить как есть.\n"
		"[[Delete|Удалить]] - удалить из мира (отменяется через Ctrl+Z).");

	// The chosen one is written in its own color: green - turned off (the usual
	// choice: the unpacked copy replaces the source, which can still be brought back),
	// yellow - left on (the source and its copy are both seen then), red - deleted.
	struct SourceModeName
	{
		UnpackOptions::SourceMode mode;
		QString name;
		const char *color;
	};
	const SourceModeName source_modes[] = {
		{UnpackOptions::SOURCE_DISABLE, uiText("Disable", "Выключить"), "#6cc06c"},
		{UnpackOptions::SOURCE_KEEP, uiText("Keep enabled", "Оставить включённым"), "#e6c84a"},
		{UnpackOptions::SOURCE_DELETE, uiText("Delete", "Удалить"), "#e05a5a"},
	};

	source_mode_ = new QButtonGroup(this);
	QHBoxLayout *source_mode_layout = new QHBoxLayout;
	source_mode_layout->setSpacing(12);
	for (const SourceModeName &source_mode : source_modes)
	{
		QRadioButton *button = new QRadioButton(source_mode.name);
		button->setToolTip(source_mode_tooltip);
		button->setStyleSheet(QString::fromUtf8("QRadioButton:checked { color: %1; }")
			.arg(QString::fromUtf8(source_mode.color)));
		source_mode_->addButton(button, int(source_mode.mode));
		source_mode_layout->addWidget(button);
	}
	source_mode_layout->addStretch(1);

	QLabel *source_mode_label = new QLabel(uiText("Source clutter", "Исходный клаттер"));
	source_mode_label->setToolTip(source_mode_tooltip);
	options_layout->addRow(source_mode_label, source_mode_layout);

	suffix_ = new QLineEdit;
	suffix_->setToolTip(loc_.tip(
		"Added to the clutter name to name the group node that holds the unpacked nodes.",
		"Добавляется к имени клаттера - так будет называться нода-группа с распакованными нодами."));
	options_layout->addRow(uiText("Result name suffix", "Суффикс имени результата"), suffix_);

	keep_parent_ = new QCheckBox(uiText("Place result next to the source", "Поместить результат рядом с исходным"));
	keep_parent_->setToolTip(loc_.tip(
		"On: the result appears in the World Hierarchy right below the clutter, "
		"inside the same parent.\n"
		"Off: the result is added to the end of the world root list.",
		"Вкл: результат появляется в World Hierarchy сразу под клаттером, "
		"внутри того же родителя.\n"
		"Выкл: результат добавляется в конец списка, в корень мира."));
	options_layout->addRow(keep_parent_);

	select_result_ = new QCheckBox(uiText("Select result when done", "Выделить результат по окончании"));
	select_result_->setToolTip(loc_.tip(
		"Select the created group nodes in the World Hierarchy after unpacking.",
		"После распаковки выделить созданные ноды-группы в World Hierarchy."));
	options_layout->addRow(select_result_);

	group_by_asset_ = new QCheckBox(uiText("Group instances by asset (World Clutter)", "Группировать экземпляры по ассетам (World Clutter)"));
	group_by_asset_->setToolTip(loc_.tip(
		"World Clutter only.\n"
		"On: nodes are sorted into sub-groups, one per node reference asset.\n"
		"Off: all nodes are placed directly in the result group.",
		"Только для World Clutter.\n"
		"Вкл: ноды раскладываются по подгруппам - по одной на каждый ассет (node reference).\n"
		"Выкл: все ноды лежат прямо в группе результата."));
	options_layout->addRow(group_by_asset_);

	layout->addWidget(options_group);

	// Run.
	QHBoxLayout *buttons_layout = new QHBoxLayout;
	// The main button of the page: larger than the rest, like Start placing of the
	// Object Placer, and blue (gray while there is nothing to unpack).
	unpack_button_ = new QPushButton(uiText("Unpack", "Распаковать"));
	unpack_button_->setMinimumHeight(36);
	unpack_button_->setStyleSheet(primaryButtonStyle());
	unpack_button_->setIcon(QIcon(QString::fromUtf8(":/images/mods/icon_object_mode.png")));
	unpack_button_->setIconSize(QSize(24, 24));
	unpack_button_->setToolTip(loc_.tip(
		"Unpack the selected clutters into regular nodes.\n"
		"While a World Clutter is unpacked the viewport briefly shows it from above - "
		"your camera is not moved and comes back by itself.\n"
		"Can be undone with Ctrl+Z.",
		"Распаковать выделенные клаттеры в обычные ноды.\n"
		"Пока распаковывается World Clutter, вьюпорт ненадолго показывает его сверху - "
		"ваша камера не сдвигается и сама возвращается обратно.\n"
		"Отменяется через Ctrl+Z."));
	cancel_button_ = new QPushButton(uiText("Cancel", "Отмена"));
	cancel_button_->setMinimumHeight(36);
	cancel_button_->setIcon(QIcon(QString::fromUtf8(":/images/icon_delete_cross.png")));
	cancel_button_->setIconSize(QSize(16, 16));
	cancel_button_->setToolTip(loc_.tip(
		"Stop unpacking. Clutters that are already unpacked stay unpacked, the one in "
		"progress is put back as it was.",
		"Остановить распаковку. Уже распакованные клаттеры остаются распакованными, "
		"текущий возвращается в исходное состояние."));
	buttons_layout->addWidget(unpack_button_, 1);
	buttons_layout->addWidget(cancel_button_);
	layout->addLayout(buttons_layout);

	progress_ = new QProgressBar;
	progress_->setRange(0, 100);
	progress_->setTextVisible(false);
	progress_->setFixedHeight(6);
	layout->addWidget(progress_);

	status_label_ = new QLabel;
	status_label_->setWordWrap(true);
	layout->addWidget(status_label_);

	// Log.
	QGroupBox *log_group = new QGroupBox(uiText("Log", "Журнал"));
	QVBoxLayout *log_layout = new QVBoxLayout(log_group);
	log_ = new QPlainTextEdit;
	log_->setReadOnly(true);
	log_->setMaximumBlockCount(500);
	log_->setMinimumHeight(70);
	log_->setToolTip(loc_.tip(
		"What was unpacked and what went wrong.",
		"Что было распаковано и какие были ошибки."));
	log_layout->addWidget(log_);
	layout->addWidget(log_group, 1);

	connect(unpack_button_, &QPushButton::clicked, this, [this]() { start_unpack(); });
	connect(cancel_button_, &QPushButton::clicked, this, [this]() { cancel(); });
}

void ClutterUnpackPage::load_settings()
{
	const UnpackOptions defaults;

	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);

	const int mode = settings.value("source_mode", int(defaults.source_mode)).toInt();
	QAbstractButton *mode_button = source_mode_->button(mode);
	if (!mode_button)
		mode_button = source_mode_->button(int(defaults.source_mode));
	mode_button->setChecked(true);
	suffix_->setText(settings.value("suffix", defaults.suffix).toString());
	keep_parent_->setChecked(settings.value("keep_parent", defaults.keep_parent).toBool());
	select_result_->setChecked(settings.value("select_result", defaults.select_result).toBool());
	group_by_asset_->setChecked(settings.value("group_by_asset", defaults.group_by_asset).toBool());

	const QByteArray header_state = settings.value("selection_header").toByteArray();
	if (!header_state.isEmpty())
		selection_tree_->header()->restoreState(header_state);
}

void ClutterUnpackPage::save_settings() const
{
	const UnpackOptions options = read_options();

	QSettings settings(SETTINGS_ORGANIZATION, SETTINGS_APPLICATION);
	settings.beginGroup(SETTINGS_GROUP);
	settings.setValue("source_mode", int(options.source_mode));
	settings.setValue("suffix", options.suffix);
	settings.setValue("keep_parent", options.keep_parent);
	settings.setValue("select_result", options.select_result);
	settings.setValue("group_by_asset", options.group_by_asset);
	settings.setValue("selection_header", selection_tree_->header()->saveState());
}

UnpackOptions ClutterUnpackPage::read_options() const
{
	UnpackOptions options;
	options.source_mode = UnpackOptions::SourceMode(qMax(0, source_mode_->checkedId()));
	options.suffix = suffix_->text().trimmed();
	// An empty suffix would give the result the same name as its source.
	if (options.suffix.isEmpty())
		options.suffix = UnpackOptions().suffix;
	options.keep_parent = keep_parent_->isChecked();
	options.select_result = select_result_->isChecked();
	options.group_by_asset = group_by_asset_->isChecked();
	return options;
}

void ClutterUnpackPage::refresh_selection()
{
	selection_tree_->clear();
	supported_count_ = 0;
	int selected_count = 0;

	if (const SelectorNodes *selector = Selection::getSelectorNodes())
	{
		for (const NodePtr &node : selector->getNodes())
		{
			++selected_count;
			if (!ClutterUnpacker::isSupported(node))
				continue;

			QTreeWidgetItem *item = new QTreeWidgetItem(selection_tree_);
			item->setText(COLUMN_NODE, QString::fromUtf8(node->getName()));
			item->setIcon(COLUMN_NODE, node->getType() == Node::WORLD_CLUTTER ? node_clutter_icon_ : mesh_clutter_icon_);
			item->setText(COLUMN_TYPE, ClutterUnpacker::typeLabel(node));
			item->setText(COLUMN_CONTENT, ClutterUnpacker::contentLabel(node));
			if (!node->isEnabled())
			{
				item->setDisabled(true);
				item->setToolTip(COLUMN_NODE, loc_.tip("This node is disabled.", "Эта нода выключена."));
			}
			++supported_count_;
		}
	}

	if (supported_count_ == 0)
	{
		selection_label_->setText(selected_count == 0
			? uiText("Nothing selected. Select World Clutter, Mesh Clutter or Mesh "
				"Cluster nodes in the World Hierarchy.",
				"Ничего не выделено. Выделите ноды World Clutter, Mesh Clutter или Mesh Cluster "
				"в World Hierarchy.")
			: uiText("None of the %1 selected nodes is a World Clutter, Mesh Clutter "
				"or Mesh Cluster.",
				"Среди выделенных нод (%1) нет ни World Clutter, ни Mesh Clutter, ни Mesh "
				"Cluster.").arg(selected_count));
	} else if (supported_count_ < selected_count)
	{
		selection_label_->setText(uiText("%1 other selected nodes are not clutters "
			"and will be ignored.",
			"Остальные выделенные ноды (%1) - не клаттеры, они будут пропущены.")
			.arg(selected_count - supported_count_));
	} else
		selection_label_->clear();

	unpack_button_->setText(supported_count_ > 1
		? uiText("Unpack %1 clutters", "Распаковать клаттеры: %1").arg(supported_count_)
		: uiText("Unpack", "Распаковать"));
	unpack_button_->setEnabled(supported_count_ > 0 && !unpacker_->isBusy());
}

void ClutterUnpackPage::start_unpack()
{
	if (unpacker_->isBusy())
		return;

	Vector<NodePtr> nodes;
	if (const SelectorNodes *selector = Selection::getSelectorNodes())
		nodes = selector->getNodes();

	save_settings();
	progress_->setValue(0);
	unpacker_->start(nodes, read_options());
}

void ClutterUnpackPage::set_busy(bool busy)
{
	options_widget_->setEnabled(!busy);
	cancel_button_->setEnabled(busy);
	unpack_button_->setEnabled(!busy && supported_count_ > 0);
	if (!busy)
		progress_->setValue(0);
}

void ClutterUnpackPage::append_log(int level, const QString &text)
{
	const char *color = nullptr;
	switch (level)
	{
		case ClutterUnpacker::MSG_SUCCESS: color = "#6cc070"; break;
		case ClutterUnpacker::MSG_WARNING: color = "#e0a030"; break;
		case ClutterUnpacker::MSG_ERROR:   color = "#e05555"; break;
		default: break;
	}

	const QString escaped = text.toHtmlEscaped();
	log_->appendHtml(color
		? QString::fromUtf8("<span style=\"color:%1\">%2</span>").arg(color, escaped)
		: escaped);
}

} // namespace ArtistTool
