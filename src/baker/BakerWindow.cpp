#include "BakerWindow.h"
#include "BakeCore.h"
#include "BakeGpu.h"
#include "Localization.h"
#include "UiHelpers.h"

#include <editor/UnigineActions.h>
#include <editor/UnigineAssetManager.h>
#include <editor/UnigineCollection.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>
#include <editor/UnigineShortcutManager.h>
#include <editor/UnigineWindowManager.h>

#include <UnigineEngine.h>
#include <UnigineLog.h>
#include <UnigineObjects.h>
#include <UnigineRender.h>
#include <UnigineSystemInfo.h>
#include <UnigineNodes.h>
#include <UnigineWorld.h>

#include <QAction>
#include <QApplication>
#include <QButtonGroup>
#include <QCheckBox>
#include <QComboBox>
#include <QCoreApplication>
#include <QDir>
#include <QDoubleSpinBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QFile>
#include <QFormLayout>
#include <QGridLayout>
#include <QGroupBox>
#include <QJsonDocument>
#include <QJsonObject>
#include <QHBoxLayout>
#include <QHeaderView>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QMimeData>
#include <QProgressBar>
#include <QPushButton>
#include <QRegularExpression>
#include <QScrollArea>
#include <QSettings>
#include <QTabWidget>
#include <QTimer>
#include <QToolButton>
#include <QTreeWidget>
#include <QVBoxLayout>

#if defined(_WIN32)
#	include <windows.h>
#elif defined(_LINUX)
#	include <cstdio>
#endif

#include <algorithm>
#include <cmath>
#include <functional>
#include <set>
#include <cstring>
#include <thread>
#include <vector>

using namespace Unigine;

// Physical memory the machine can still hand out, in bytes; 0 = unknown.
// Unigine::Memory only reports the engine's OWN allocations, so this needs a
// platform call.
static double availablePhysicalMemory()
{
#if defined(_WIN32)
	MEMORYSTATUSEX status;
	status.dwLength = sizeof(status);
	if (GlobalMemoryStatusEx(&status))
		return double(status.ullAvailPhys);
#elif defined(_LINUX)
	// MemAvailable is the kernel's own estimate of what can be allocated
	// without pushing the machine into swap; sysinfo()'s freeram ignores
	// reclaimable page cache and would under-report by gigabytes.
	if (FILE *f = std::fopen("/proc/meminfo", "r"))
	{
		char line[256];
		double kb = 0.0;
		while (std::fgets(line, sizeof(line), f))
			if (std::sscanf(line, "MemAvailable: %lf kB", &kb) == 1)
				break;
		std::fclose(f);
		if (kb > 0.0)
			return kb * 1024.0;
	}
#endif
	return 0.0;
}

// Video memory the capture set may draw on. This is the limit that actually
// gets hit: performCaptures() issues EVERY surface before collecting any of
// them, and each pending capture holds its render targets resident until the
// readback lands, so the whole set is alive on the GPU at once.
static double availableVideoMemory()
{
	const double free = double(Unigine::SystemInfo::getGpuVRamFree());
	// the driver's budget for this process can sit below the card's free memory
	// (shared/laptop GPUs especially); whichever is smaller is the real ceiling
	const double budget = double(Unigine::SystemInfo::getGpuVRamBudget());
	const double usage = double(Unigine::SystemInfo::getGpuVRamUsage());
	double avail = free;
	if (budget > 0.0 && budget - usage < avail)
		avail = budget - usage;
	return avail > 0.0 ? avail : 0.0;
}

// The editor has no API to query its UI language: read its own config
// (%LOCALAPPDATA%/unigine/Editor/editor1.1.cfg, JSON): "editor/language" is
// written when the language differs from the default English;
// "editor/keep_original_ui" = "1" keeps labels English and translates only
// tooltips. Fallback: detect Cyrillic in the main menu "File" action.
static void detectEditorLanguage(bool &ruLabels, bool &ruTooltips)
{
	ruLabels = false;
	ruTooltips = false;

	const QString cfgPath = qEnvironmentVariable("LOCALAPPDATA") + "/unigine/Editor/editor1.1.cfg";
	QFile file(cfgPath);
	if (file.open(QIODevice::ReadOnly))
	{
		const QJsonObject editor = QJsonDocument::fromJson(file.readAll()).object()
			.value("editor").toObject();
		const bool ru = editor.value("language").toString().startsWith("ru", Qt::CaseInsensitive);
		const bool tooltipsOnly = editor.value("keep_original_ui").toString() == QLatin1String("1");
		ruTooltips = ru;
		ruLabels = ru && !tooltipsOnly;
		return;
	}

	for (QWidget *top : QApplication::topLevelWidgets())
	{
		for (QAction *act : top->findChildren<QAction *>())
		{
			if (act->objectName() != QLatin1String("menuFile"))
				continue;
			for (QChar c : act->text())
				if (c.unicode() >= 0x0400 && c.unicode() <= 0x04FF)
				{
					ruLabels = true;
					ruTooltips = true;
					return;
				}
			return;
		}
	}
}

// The language of the other tabs of Artist Tool: captions by uiText(), tooltips wrapped
// as theirs are.
namespace
{

// Keeps a row of choice buttons enabled while the box it stands for is.
class ChoiceEnabled final : public QObject
{
public:
	ChoiceEnabled(QWidget *row, QComboBox *combo)
		: QObject(row)
		, row_(row)
		, combo_(combo)
	{
		combo->installEventFilter(this);
	}

	bool eventFilter(QObject *watched, QEvent *event) override
	{
		if (watched == combo_ && event->type() == QEvent::EnabledChange)
			row_->setEnabled(combo_->isEnabled());
		return QObject::eventFilter(watched, event);
	}

private:
	QWidget *row_;
	QComboBox *combo_;
};

// A row of choice buttons with icons in place of the drop-down box, as the other tabs
// of Artist Tool pick a mode: one button per item of the box (icon by icon; fewer icons
// - the last one again). The box itself stays, hidden: it keeps the value the settings
// are saved from and the bake reads, the buttons show it and set it.
QWidget *makeChoiceRow(QComboBox *combo, const std::vector<const char *> &icons)
{
	QWidget *row = new QWidget();
	QHBoxLayout *layout = new QHBoxLayout(row);
	layout->setContentsMargins(0, 0, 0, 0);
	layout->setSpacing(3);
	QButtonGroup *group = new QButtonGroup(row);
	group->setExclusive(true);
	for (int i = 0; i < combo->count(); ++i)
	{
		QToolButton *button = new QToolButton();
		button->setText(combo->itemText(i));
		if (!icons.empty())
			button->setIcon(QIcon(QString::fromUtf8(icons[std::min<size_t>(size_t(i), icons.size() - 1)])));
		button->setIconSize(QSize(16, 16));
		button->setToolButtonStyle(Qt::ToolButtonTextBesideIcon);
		button->setCheckable(true);
		button->setFocusPolicy(Qt::NoFocus);
		button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
		button->setStyleSheet(ArtistTool::choiceButtonStyle());
		button->setToolTip(combo->toolTip());
		group->addButton(button, i);
		layout->addWidget(button);
	}
	combo->setParent(row);
	combo->hide();
	QObject::connect(group, &QButtonGroup::idClicked, combo, &QComboBox::setCurrentIndex);
	const auto show = [group, combo]() {
		if (QAbstractButton *button = group->button(combo->currentIndex()))
			button->setChecked(true);
	};
	QObject::connect(combo, &QComboBox::currentIndexChanged, row, show);
	show();
	new ChoiceEnabled(row, combo);
	row->setEnabled(combo->isEnabled());
	return row;
}

} // namespace

QString BakerWindow::tr2(const char *ru, const char *en) const
{
	return ArtistTool::uiText(en, ru);
}

QString BakerWindow::tip2(const char *ru, const char *en) const
{
	return ArtistTool::Localization().tip(en, ru);
}

BakerWindow::BakerWindow(QWidget *parent)
	: QWidget(parent)
{
	setWindowTitle("Texture Baker");
	setObjectName("TextureBakerWindow");

	detectEditorLanguage(ruUi_, ruTips_);

	// One page in groups, as the other tabs of Artist Tool: it scrolls when the window is
	// short.
	auto pageLayout = new QVBoxLayout(this);
	pageLayout->setContentsMargins(0, 0, 0, 0);
	auto scroll = new QScrollArea();
	scroll->setFrameShape(QFrame::NoFrame);
	scroll->setWidgetResizable(true);
	pageLayout->addWidget(scroll);
	auto content = new QWidget();
	scroll->setWidget(content);
	auto mainLayout = new QVBoxLayout(content);

	auto description = new QLabel(tr2(
		"Запекает вид high-poly модели в текстуры low-poly: альбедо, шейдинг, нормаль, свечение. "
		"Выберите модели и нажмите «Запечь».",
		"Bakes the look of a high-poly model into textures of a low-poly one: albedo, shading, "
		"normal, emission. Pick the models and press Bake."));
	description->setWordWrap(true);
	mainLayout->addWidget(description);

	// --- Models ---
	auto modelsGroup = new QGroupBox(tr2("Модели", "Models"));
	auto modelsBox = new QVBoxLayout(modelsGroup);
	// the single pair: high-poly and low-poly, a row each
	singlePanel_ = new QWidget();
	auto singleGrid = new QGridLayout(singlePanel_);
	singleGrid->setContentsMargins(0, 0, 0, 0);
	singleGrid->setColumnStretch(1, 1);

	const QString notSet = tr2("<не выбрано>", "<not set>");
	const QString pickText = tr2("Добавить выделенное", "Add Selected");
	const QString pickTip = tip2(
		"Взять модель из текущего выделения в сцене.\n"
		"Вложенные меши выбранной ноды считаются её частями.",
		"Take the model from the current scene selection.\n"
		"Nested meshes of the selected node count as its parts.");

	const QString clearTip = tip2("Очистить слот.", "Clear the slot.");
	auto makeClearButton = [&clearTip]() {
		auto btn = new QPushButton(QString::fromUtf8("✕"));
		btn->setFixedWidth(24);
		btn->setToolTip(clearTip);
		return btn;
	};
	// as Add Selected Assets of the other tabs
	auto stylePick = [](QPushButton *btn) {
		btn->setIcon(QIcon(QString::fromUtf8(":/images/mods/icon_object_mode.png")));
		btn->setIconSize(QSize(16, 16));
	};
	const QString hideText = tr2("Скрыть", "Hide");
	const QString hideTip = tip2(
		"Скрыть/показать меши слота во вьюпорте (удобно при рисовании маски).\n"
		"У high-poly вместе с мешами скрываются и его декали. Декали внутри\n"
		"Node Reference поодиночке не скрываются, поэтому такая ссылка гасится\n"
		"целиком (вместе со своими мешами).\n"
		"На запекание не влияет: скрытые модели запекаются как обычно.",
		"Hide/show the slot meshes in the viewport (handy while painting the mask).\n"
		"Does not affect baking: hidden models bake as usual.");
	auto makeHideButton = [&hideText, &hideTip]() {
		auto btn = new QPushButton(hideText);
		btn->setIcon(QIcon(QString::fromUtf8(":/images/icons_eye_off.png")));
		btn->setCheckable(true);
		btn->setToolTip(hideTip);
		return btn;
	};

	// row 0: single-mode high-poly (the Select button ADDS the selection)
	highLabel_ = new QLabel(notSet);
	highPickButton_ = new QPushButton(pickText);
	highPickButton_->setToolTip(pickTip);
	stylePick(highPickButton_);
	connect(highPickButton_, &QPushButton::clicked, this, &BakerWindow::pickHighFromSelection);
	highHideButton_ = makeHideButton();
	connect(highHideButton_, &QPushButton::toggled, this,
		[this](bool on) { setSlotHidden(true, on); });
	auto highClear = makeClearButton();
	connect(highClear, &QPushButton::clicked, this, [this]() { clearSlot(true); });
	auto highRow = new QHBoxLayout();
	highRow->addWidget(highLabel_, 1);
	highRow->addWidget(highPickButton_);
	highRow->addWidget(highHideButton_);
	highRow->addWidget(highClear);
	auto highTitle = new QLabel(tr2("High-poly", "High-Poly"));
	highTitle->setToolTip(tip2("Модель-источник: с неё запекается вид.", "The source model: its look is baked."));
	singleGrid->addWidget(highTitle, 0, 0);
	singleGrid->addLayout(highRow, 0, 1);

	// row 1: single-mode low-poly
	lowLabel_ = new QLabel(notSet);
	lowPickButton_ = new QPushButton(pickText);
	lowPickButton_->setToolTip(pickTip);
	stylePick(lowPickButton_);
	connect(lowPickButton_, &QPushButton::clicked, this, &BakerWindow::pickLowFromSelection);
	lowHideButton_ = makeHideButton();
	connect(lowHideButton_, &QPushButton::toggled, this,
		[this](bool on) { setSlotHidden(false, on); });
	auto lowClear = makeClearButton();
	connect(lowClear, &QPushButton::clicked, this, [this]() { clearSlot(false); });
	auto lowRow = new QHBoxLayout();
	lowRow->addWidget(lowLabel_, 1);
	lowRow->addWidget(lowPickButton_);
	lowRow->addWidget(lowHideButton_);
	lowRow->addWidget(lowClear);
	auto lowTitle = new QLabel(tr2("Low-poly", "Low-Poly"));
	lowTitle->setToolTip(tip2("Модель-цель: её развёртка получает текстуры.", "The target model: its UV set gets the textures."));
	singleGrid->addWidget(lowTitle, 1, 0);
	singleGrid->addLayout(lowRow, 1, 1);

	// row 2: bake groups mode
	groupsModeCombo_ = new QComboBox();
	groupsModeCombo_->addItem(tr2("Выкл (одна пара)", "Off (Single Pair)"), 0);
	groupsModeCombo_->addItem(tr2("Ручные группы", "Manual Groups"), 1);
	groupsModeCombo_->addItem(tr2("По именам (авто)", "By Name (Auto)"), 2);
	groupsModeCombo_->setToolTip(tip2(
		"Бейк-группы: лучи каждой low-части видят только свою high-часть,\n"
		"соседние детали не отпечатываются друг на друге. Все группы\n"
		"запекаются в один общий набор текстур.\n"
		"Ручные группы — пары задаются списком групп.\n"
		"По именам — вложенные меши выбранных нод сопоставляются по имени\n"
		"без суффиксов _low/_high/_lp/_hp/_lod* (пары пишутся в консоль).",
		"Bake groups: rays of each low part see only its own high part, so\n"
		"neighboring pieces do not imprint on each other. All groups bake\n"
		"into one shared texture set.\n"
		"Manual Groups — pairs are set in the group list.\n"
		"By name — nested meshes of the selected nodes are matched by name\n"
		"ignoring the _low/_high/_lp/_hp/_lod* suffixes (pairs are logged)."));
	connect(groupsModeCombo_, &QComboBox::currentIndexChanged, this, &BakerWindow::updateModelsMode);
	{
		auto groupsRow = new QHBoxLayout();
		auto groupsTitle = new QLabel(tr2("Группы", "Groups"));
		groupsTitle->setToolTip(groupsModeCombo_->toolTip());
		groupsRow->addWidget(groupsTitle);
		groupsRow->addWidget(makeChoiceRow(groupsModeCombo_,
			{":/images/icon_rectangle.png", ":/images/icon_clone.png", ":/images/icon_filter.png"}), 1);
		// at the top: switching the mode does not move it
		modelsBox->addLayout(groupsRow);
		modelsBox->addWidget(singlePanel_);
	}

	// row 3: the groups panel (visible only in group mode)
	groupsPanel_ = new QWidget();
	groupsLayout_ = new QVBoxLayout(groupsPanel_);
	groupsLayout_->setContentsMargins(0, 0, 0, 0);
	addGroupButton_ = new QPushButton(tr2("Добавить группу", "Add Group"));
	addGroupButton_->setIcon(QIcon(QString::fromUtf8(":/images/icon_add.png")));
	addGroupButton_->setMinimumHeight(28);
	connect(addGroupButton_, &QPushButton::clicked, this, &BakerWindow::addGroupRow);
	groupsLayout_->addWidget(addGroupButton_);
	modelsBox->addWidget(groupsPanel_);
	groupsPanel_->setVisible(false);

	// row 4: striped participants list (dark rows = high-poly, gray = low-poly),
	// also a drop target for nodes dragged from the World Nodes hierarchy
	meshTree_ = new QTreeWidget();
	// 0 = name, 1/2 = per-part cage (low-poly rows only), 3 = remove button
	meshTree_->setColumnCount(4);
	meshTree_->setHeaderHidden(true);
	meshTree_->header()->setStretchLastSection(false);
	meshTree_->header()->setSectionResizeMode(0, QHeaderView::Stretch);
	meshTree_->header()->setSectionResizeMode(1, QHeaderView::Fixed);
	meshTree_->header()->setSectionResizeMode(2, QHeaderView::Fixed);
	meshTree_->header()->setSectionResizeMode(3, QHeaderView::Fixed);
	meshTree_->setColumnWidth(1, 54);
	meshTree_->setColumnWidth(2, 54);
	meshTree_->setColumnWidth(3, 24);
	// section headers fold, so a long high-poly list can be tucked away while
	// working on the low-poly one
	meshTree_->setRootIsDecorated(true);
	// editing the cage cells needs a current item, so rows are selectable and
	// the widget takes focus (it was a pure display list before)
	meshTree_->setSelectionMode(QAbstractItemView::SingleSelection);
	meshTree_->setEditTriggers(QAbstractItemView::DoubleClicked
		| QAbstractItemView::SelectedClicked | QAbstractItemView::EditKeyPressed);
	// the participants list is the main working area of the window: give it a
	// tall floor and let it absorb any spare vertical space
	meshTree_->setMinimumHeight(320);
	meshTree_->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Expanding);
	meshTree_->setAcceptDrops(true);
	meshTree_->viewport()->setAcceptDrops(true);
	meshTree_->viewport()->installEventFilter(this);
	meshTree_->setToolTip(tip2(
		"Участники запекания. Синий - high-poly, сиреневый - low-poly, зелёный - декали.\n"
		"Колонки вперёд/назад у low-poly - кейдж детали, метры. Серое значение наследует\n"
		"общее, яркое задано вручную. Двойной щелчок - правка, пустое поле - сброс оси.\n"
		"Перетаскивание нод из World Nodes добавляет их в секцию, ✕ убирает.\n"
		"Щелчок по заголовку сворачивает секцию.\n",
		"Bake participants. Blue - high-poly, mauve - low-poly, green - decals.\n"
		"The front/back columns on low-poly rows are that part's cage, meters. A dimmed\n"
		"value inherits the global one, a bright value is explicit. Double-click edits,\n"
		"an empty field resets that axis.\n"
		"Dragging nodes from World Nodes adds them to a section, ✕ removes.\n"
		"Clicking a header collapses the section."));
	// clicking the ✕ column removes the mesh from its slot
	// a cage cell was edited: empty text returns the part to the global value
	connect(meshTree_, &QTreeWidget::itemChanged, this, [this](QTreeWidgetItem *item, int column) {
		if (treeUpdating_ || !item->parent() || (column != 1 && column != 2))
			return;
		const int id = item->data(0, Qt::UserRole).toInt();
		// the cage lives on low-poly rows only; sections are identified by key
		// because groups mode adds a variable number of them
		const QString sectionKey = item->parent()->data(0, Qt::UserRole).toString();
		if (!id || !(sectionKey == "low" || sectionKey.endsWith("lo")))
			return;
		const QString text = item->text(column).trimmed().replace(',', '.');
		bool ok = false;
		const double value = text.toDouble(&ok);
		// each axis is independent: clearing one cell returns only that axis to
		// the global setting
		PartCage pc = partCage_.count(id) ? partCage_[id] : PartCage();
		if (text.isEmpty())
		{
			if (column == 1)
				pc.hasFrontal = false;
			else
				pc.hasRear = false;
		}
		else if (ok)
		{
			const float v = float(qBound(0.0, value, 10.0));
			if (column == 1)
			{
				pc.hasFrontal = true;
				pc.frontal = v;
			}
			else
			{
				pc.hasRear = true;
				pc.rear = v;
			}
		}
		if (pc.any())
			partCage_[id] = pc;
		else
			partCage_.erase(id);
		treeUpdating_ = true;
		fillCageCells(item, id);
		treeUpdating_ = false;
	});

	// remember folding across the rebuilds refreshMeshTree() does
	connect(meshTree_, &QTreeWidget::itemExpanded, this, [this](QTreeWidgetItem *item) {
		if (!treeUpdating_ && !item->parent())
			collapsedSections_.erase(item->data(0, Qt::UserRole).toString());
	});
	connect(meshTree_, &QTreeWidget::itemCollapsed, this, [this](QTreeWidgetItem *item) {
		if (!treeUpdating_ && !item->parent())
			collapsedSections_.insert(item->data(0, Qt::UserRole).toString());
	});

	connect(meshTree_, &QTreeWidget::itemClicked, this, [this](QTreeWidgetItem *item, int column) {
		// the whole header row folds, not just the little arrow
		if (!item->parent())
		{
			item->setExpanded(!item->isExpanded());
			return;
		}
		if (column != 3 || manualGroups())
			return; // group sections are defined by their root node, not per mesh
		const int id = item->data(0, Qt::UserRole).toInt();
		if (!id)
			return;
		const int section = meshTree_->indexOfTopLevelItem(item->parent()); // 0 hi, 1 lo, 2 decals
		if (section == 2)
		{
			decalIds_.erase(std::remove(decalIds_.begin(), decalIds_.end(), id), decalIds_.end());
			refreshMeshTree();
			return;
		}
		const bool high = section == 0;
		std::vector<int> &ids = high ? highIds_ : lowIds_;
		// a mesh leaving a hidden slot must become visible again
		if ((high ? highHideButton_ : lowHideButton_)->isChecked())
			if (NodePtr node = World::getNodeByID(id))
				node->setEnabled(true);
		ids.erase(std::remove(ids.begin(), ids.end(), id), ids.end());
		if (high)
			updateSuggestedName();
		else
			partCage_.erase(id); // drop this part's cage with the part itself
		updateSlotLabels();
		refreshMeshTree();
		setUiLocked(baking_);
	});
	modelsBox->addWidget(meshTree_, 1);
	mainLayout->addWidget(modelsGroup, 1);

	// --- Cage ---
	auto cageGroup = new QGroupBox(tr2("Кейдж", "Cage"));
	auto cageBox = new QVBoxLayout(cageGroup);

	// per-part cage helpers, right under the list they act on
	{
		auto cageRow = new QHBoxLayout();
		autoCageButton_ = new QPushButton(tr2("Подобрать кейдж", "Fit Cage"));
		autoCageButton_->setMinimumHeight(28);
		autoCageButton_->setIcon(QIcon(QString::fromUtf8(":/images/action_material_inherit.png")));
		autoCageButton_->setToolTip(tip2(
			"Промеряет каждую low-poly деталь лучами и ставит минимальные дистанции,\n"
			"при которых её high-poly попадает в запечку.\n"
			"Общие дистанции - потолок: значения только уменьшаются. Ось, которой нужно\n"
			"больше потолка, остаётся унаследованной.\n",
			"Probes each low-poly part with rays and sets the minimum distances that still\n"
			"catch its high-poly.\n"
			"The global distances are the ceiling: values are only lowered. An axis that\n"
			"needs more than the ceiling stays inherited."));
		connect(autoCageButton_, &QPushButton::clicked, this, &BakerWindow::onAutoCage);
		cageRow->addWidget(autoCageButton_);

		resetCageButton_ = new QPushButton(tr2("Сбросить к общему", "Reset To Global"));
		resetCageButton_->setIcon(ArtistTool::makeResetIcon());
		resetCageButton_->setMinimumHeight(28);
		resetCageButton_->setToolTip(tip2(
			"Убирает индивидуальные дистанции у всех деталей — они снова следуют\n"
			"общим настройкам.",
			"Drops the per-part distances so every part follows the global settings again."));
		resetCageButton_->setEnabled(false);
		connect(resetCageButton_, &QPushButton::clicked, this, &BakerWindow::resetAllPartCage);
		cageRow->addWidget(resetCageButton_);

		// the global pair, right above the buttons that act on it: it is both the
		// default for every part and the ceiling the fitting obeys
		frontalSpin_ = new QDoubleSpinBox();
		frontalSpin_->setDecimals(4);
		frontalSpin_->setRange(0.0001, 100.0);
		frontalSpin_->setSingleStep(0.01);
		frontalSpin_->setValue(0.05);
		frontalSpin_->setSuffix(tr2(" м", " m"));
		frontalSpin_->setToolTip(tip2(
			"Дистанция вперёд, метры: насколько выше поверхности low-poly искать high-poly.\n"
			"Значение по умолчанию для деталей и потолок для \"Подобрать кейдж\".\n",
			"Frontal distance, meters: how far above the low-poly surface to search for\n"
			"high-poly detail. Default for parts and the ceiling for Fit cage."));

		rearSpin_ = new QDoubleSpinBox();
		rearSpin_->setDecimals(4);
		rearSpin_->setRange(0.0, 100.0);
		rearSpin_->setSingleStep(0.01);
		rearSpin_->setValue(0.05);
		rearSpin_->setSuffix(tr2(" м", " m"));
		rearSpin_->setToolTip(tip2(
			"Дистанция назад, метры: насколько ниже поверхности low-poly искать.\n"
			"Значение по умолчанию для деталей и потолок для \"Подобрать кейдж\".\n",
			"Rear distance, meters: how far below the low-poly surface to search.\n"
			"Default for parts and the ceiling for Fit cage."));

		// a field each, with the button that puts the default back, in two columns
		auto globalCage = new QGridLayout();
		globalCage->setColumnStretch(1, 1);
		globalCage->setColumnStretch(4, 1);
		const auto addCageField = [this, globalCage](int column, const QString &title, QDoubleSpinBox *spin) {
			ArtistTool::enableSpinBoxDrag(spin);
			auto label = new QLabel(title);
			label->setToolTip(spin->toolTip());
			globalCage->addWidget(label, 0, column);
			globalCage->addWidget(spin, 0, column + 1);
			globalCage->addWidget(ArtistTool::makeResetButton(tip2("По умолчанию (0.05 м)", "Default (0.05 m)"), this,
				[spin]() { spin->setValue(0.05); }), 0, column + 2);
		};
		addCageField(0, tr2("Вперёд", "Front"), frontalSpin_);
		addCageField(3, tr2("Назад", "Back"), rearSpin_);
		cageBox->addLayout(globalCage);
		cageBox->addLayout(cageRow);

		// parts that inherit show the global number, so keep their cells in sync;
		// parts tuned by hand are deliberately left alone
		for (QDoubleSpinBox *spin : {frontalSpin_, rearSpin_})
			connect(spin, &QDoubleSpinBox::valueChanged, this, [this]() { refreshMeshTree(); });
	}

	// row 5: output name (auto-suggested, user-editable)
	nameEdit_ = new QLineEdit();
	nameEdit_->setToolTip(tip2(
		"Имя запекаемых текстур и материала (<имя>_alb/_sh/_n и <имя>_baked.mat).\n"
		"Заполняется по high-poly модели, можно изменить перед запеканием.",
		"Name of the baked textures and material (<name>_alb/_sh/_n and <name>_baked.mat).\n"
		"Auto-filled from the high-poly model, editable before baking."));
	// added to the bottom of the window (just above Bake), not to this tab

	mainLayout->addWidget(cageGroup);

	// --- Bake ---
	auto settingsPanel = new QGroupBox(tr2("Запекание", "Bake"));
	auto settingsLayout = new QFormLayout(settingsPanel);

	resolutionCombo_ = new QComboBox();
	resolutionCombo_->addItem("512", 512);
	resolutionCombo_->addItem("1024", 1024);
	resolutionCombo_->addItem("2048", 2048);
	resolutionCombo_->addItem("4096", 4096);
	resolutionCombo_->setCurrentIndex(2);
	resolutionCombo_->setToolTip(tip2("Размер запекаемых текстур.", "Size of the baked textures."));
	connect(resolutionCombo_, &QComboBox::currentIndexChanged, this,
		[this](int) { updateCaptureInfo(); });
	settingsLayout->addRow(tr2("Разрешение", "Resolution"),
		makeChoiceRow(resolutionCombo_, {":/images/tools/icon_scale.png"}));

	samplesCombo_ = new QComboBox();
	// no single-sample option: one ray per texel aliases every UV edge and is
	// never the right trade-off (an old saved value of 1 simply falls back to
	// the default, findData below returns -1 for it)
	samplesCombo_->addItem(tr2("4 (качество)", "4 (Quality)"), 4);
	samplesCombo_->addItem(tr2("16 (высокое)", "16 (High)"), 16);
	samplesCombo_->addItem(tr2("64 (максимум, медленно)", "64 (Maximum, Slow)"), 64);
	samplesCombo_->setCurrentIndex(0);
	samplesCombo_->setToolTip(tip2("Число лучей на тексель (сглаживание краёв).",
		"Rays per texel (edge anti-aliasing)."));
	settingsLayout->addRow(tr2("Сэмплов на тексель", "Samples Per Texel"),
		makeChoiceRow(samplesCombo_, {":/images/icon_flashlight.png"}));

	skewMaskCheck_ = new QCheckBox(tr2("Учитывать", "Use"));
	skewMaskCheck_->setChecked(true);
	skewMaskCheck_->setToolTip(tip2(
		"Если у low-poly назначена Surface Custom Texture, она задаёт направление лучей:\n"
		"белое — по геометрической нормали треугольника (без скоса проекции),\n"
		"чёрное — по сглаженной нормали, серое — плавный переход.",
		"If the low-poly has a Surface Custom Texture assigned, it drives the ray direction:\n"
		"white — along the triangle's geometric normal (no projection skew),\n"
		"black — along the smoothed normal, gray — a smooth blend."));
	settingsLayout->addRow(tr2("Skew-маска", "Skew Mask"), skewMaskCheck_);

	skewMaskButton_ = new QPushButton(tr2("Нарисовать skew-маску", "Paint Skew Mask"));
	skewMaskButton_->setToolTip(tip2(
		"Создаёт чёрную текстуру-маску рядом с low-poly мешем и назначает её\n"
		"в слот Surface Custom Texture.",
		"Creates a black mask texture next to the low-poly mesh and assigns it\n"
		"to the Surface Custom Texture slot."));
	skewMaskButton_->setIcon(QIcon(QString::fromUtf8(":/images/icon_brush.png")));
	skewMaskButton_->setMinimumHeight(28);
	settingsLayout->addRow(QString(), skewMaskButton_);
	connect(skewMaskButton_, &QPushButton::clicked, this, &BakerWindow::onCreateSkewMask);

	skewMaskHelp_ = new QLabel(tr2(
		"1. Нажмите кнопку — маска создастся, назначится на low-poly,\n"
		"    включится режим рисования (Texture Paint Mode).\n"
		"2. Выберите Custom Surface Texture в параметрах кисти.\n"
		"3. Закрасьте белым места, где детали запекаются со смещением, и сохраните.\n"
		"4. Запеките — белые зоны идут по нормали полигона, чёрные по сглаженной.",
		"1. Press the button — the mask is created, assigned to the low-poly\n"
		"    and the Texture Paint Mode activates.\n"
		"2. Pick Custom Surface Texture in the brush parameters.\n"
		"3. Paint the areas where details bake skewed white, then save.\n"
		"4. Bake — white areas use the face normal, black the smoothed one."));
	skewMaskHelp_->setWordWrap(true);
	skewMaskHelp_->setEnabled(false); // dimmed hint text
	settingsLayout->addRow(QString(), skewMaskHelp_);
	skewMaskHelp_->setVisible(skewMaskCheck_->isChecked());
	connect(skewMaskCheck_, &QCheckBox::toggled, skewMaskHelp_, &QWidget::setVisible);
	// the paint button only makes sense while the mask is in use
	skewMaskButton_->setVisible(skewMaskCheck_->isChecked());
	connect(skewMaskCheck_, &QCheckBox::toggled, skewMaskButton_, &QWidget::setVisible);

	// Which maps the bake writes. Unchecking one leaves the existing texture
	// alone, so a map that came out wrong can be re-baked on its own.
	albedoCheck_ = new QCheckBox(tr2("Альбедо (_alb)", "Albedo (_alb)"));
	shadingCheck_ = new QCheckBox(tr2("Шейдинг (_sh)", "Shading (_sh)"));
	normalCheck_ = new QCheckBox(tr2("Нормаль (_n)", "Normal (_n)"));
	emissionCheck_ = new QCheckBox(tr2("Свечение (_e)", "Emissive (_e)"));
	albedoCheck_->setChecked(true);
	shadingCheck_->setChecked(true);
	normalCheck_->setChecked(true);
	emissionCheck_->setChecked(false); // off by default: most models do not glow
	const QString mapsTip = tip2(
		"Какие карты записывать. Снятая галочка - карта не перезаписывается,\n"
		"у материала остаётся прежняя текстура. Так можно перепечь одну карту\n"
		"отдельным проходом, не трогая остальные.\n"
		"Трассировка лучей идёт целиком в любом случае - карты считаются за один проход,\n"
		"так что отключение карты экономит не время, а только перезапись файла.",
		"Which maps to write. An unchecked map is not overwritten and the material keeps\n"
		"its current texture, so one map can be re-baked on its own without touching the rest.\n"
		"Ray tracing still runs in full either way - the maps share one traversal - so\n"
		"leaving a map out saves the file rewrite, not the bake time.");
	albedoCheck_->setToolTip(mapsTip);
	shadingCheck_->setToolTip(mapsTip);
	normalCheck_->setToolTip(mapsTip);
	emissionCheck_->setToolTip(tip2(
		"Запекает текстуру свечения (_e) и включает стейт Emission у материала low-poly.\n"
		"RGB - цвет свечения, альфа - маска: 1 где стейт Emission включён, 0 где выключен.\n"
		"mesh_base использует только RGB.\n",
		"Bakes the emission texture (_e) and enables the Emission state on the low-poly material.\n"
		"RGB = glow colour, alpha = mask: 1 where the Emission state is on, 0 where off.\n"
		"mesh_base uses RGB only."));
	{
		QWidget *maps = new QWidget();
		QHBoxLayout *mapsLayout = new QHBoxLayout(maps);
		mapsLayout->setContentsMargins(0, 0, 0, 0);
		mapsLayout->addWidget(albedoCheck_);
		mapsLayout->addWidget(shadingCheck_);
		mapsLayout->addWidget(normalCheck_);
		mapsLayout->addWidget(emissionCheck_);
		mapsLayout->addStretch();
		settingsLayout->addRow(tr2("Карты", "Maps"), maps);
	}
	// the emission pass adds a fourth render target per surface, so the capture
	// budget (and the size it resolves to) changes with this checkbox
	connect(emissionCheck_, &QCheckBox::toggled, this, [this](bool) { updateCaptureInfo(); });

	decalsCheck_ = new QCheckBox(tr2("Запекать", "Bake"));
	decalsCheck_->setChecked(false);
	decalsCheck_->setToolTip(tip2(
		"Проецирует декали сцены в запекаемые текстуры.\n"
		"Берутся все декали из иерархии high-poly, включая вложенные в Node Reference;\n"
		"показываются зелёной секцией списка. Выключенные декали тоже запекаются.\n",
		"Projects the scene's decals into the baked textures.\n"
		"Every decal in the high-poly hierarchy is taken, including ones nested in a\n"
		"Node Reference; they are listed as the green section. Disabled decals bake too."));
	settingsLayout->addRow(tr2("Декали", "Decals"), decalsCheck_);

	decalDistanceSpin_ = new QDoubleSpinBox();
	decalDistanceSpin_->setRange(0.1, 50.0);
	decalDistanceSpin_->setValue(1.0);
	decalDistanceSpin_->setSingleStep(0.5);
	decalDistanceSpin_->setDecimals(1);
	decalDistanceSpin_->setSuffix(tr2(" см", " cm"));
	decalDistanceSpin_->setToolTip(tip2(
		"Предел привязки screen projection декали (нода Decal Mesh), см.\n"
		"Меньше - привязка только к ближайшей геометрии, меньше ложных проекций.\n"
		"На DecalOrtho и DecalProj не влияет: у них своя рамка проекции.\n",
		"Binding limit for a screen projection decal (Decal Mesh node), cm.\n"
		"Smaller = binds only to the nearest geometry, fewer ghost projections.\n"
		"Does not affect DecalOrtho/DecalProj: those carry their own projection box."));
	ArtistTool::enableSpinBoxDrag(decalDistanceSpin_);
	{
		decalDistanceRow_ = new QWidget();
		auto row = new QHBoxLayout(decalDistanceRow_);
		row->setContentsMargins(0, 0, 0, 0);
		row->addWidget(decalDistanceSpin_, 1);
		row->addWidget(ArtistTool::makeResetButton(tip2("По умолчанию (1 см)", "Default (1 cm)"), this,
			[this]() { decalDistanceSpin_->setValue(1.0); }));
		settingsLayout->addRow(tr2("Дистанция SP-декалей", "SP Decal Distance"), decalDistanceRow_);
	}

	// decals are gathered automatically from the high-poly hierarchy and shown
	// as a section in the participants list; toggling just shows/hides it
	connect(decalsCheck_, &QCheckBox::toggled, this, [this, settingsLayout](bool on) {
		settingsLayout->setRowVisible(decalDistanceRow_, on);
		refreshMeshTree();
	});
	settingsLayout->setRowVisible(decalDistanceRow_, false);

	// --- Debug: shown in the Debug tab of Artist Tool, see takeDebugControls() ---
	debugPanel_ = new QWidget();
	auto debugLayout = new QFormLayout(debugPanel_);

	gpuCheck_ = new QCheckBox(tr2("Как в рендере (GPU)", "As Rendered (GPU)"));
	gpuCheck_->setChecked(true);
	gpuCheck_->setToolTip(tip2(
		"Материалы high-poly рендерятся движком в развёртку (слои, маски,\n"
		"тайлинг, цвета) и переносятся на low-poly.\n"
		"Выключите для чтения только базовых текстур материала.",
		"The engine renders the high-poly materials into the unwrap (layers,\n"
		"masks, tiling, colors) and transfers the result onto the low-poly.\n"
		"Disable to read only the material's base textures."));
	debugLayout->addRow(tr2("Режим", "Mode"), gpuCheck_);
	connect(gpuCheck_, &QCheckBox::toggled, this, [this](bool) { updateCaptureInfo(); });

	captureUVCombo_ = new QComboBox();
	captureUVCombo_->addItem(tr2("Авто", "Auto"), -1);
	captureUVCombo_->addItem("UV0", 0);
	captureUVCombo_->addItem("UV1", 1);
	captureUVCombo_->setToolTip(tip2(
		"UV-канал для GPU-захвата.\n"
		"Авто: канал с большей долей треугольников, получающих в атласе площадь >= 1\n"
		"текселя; при равенстве - меньшее перекрытие чартов, затем меньше чартов.\n"
		"Выбор пишется в консоль.\n",
		"UV channel for the GPU capture.\n"
		"Auto: the channel with the larger share of triangles getting >= 1 texel of\n"
		"atlas area; ties go to less chart overlap, then fewer charts.\n"
		"The choice is logged to the console."));
	debugLayout->addRow(tr2("Развёртка захвата", "Capture Unwrap"),
		makeChoiceRow(captureUVCombo_, {":/images/tools/icon_gear.png", ":/images/icon_geometry_data.png"}));

	captureSizeCombo_ = new QComboBox();
	captureSizeCombo_->addItem(tr2("Авто (по свободной памяти)", "Auto (By Free Memory)"), -1);
	captureSizeCombo_->addItem("512", 512);
	captureSizeCombo_->addItem("1024", 1024);
	captureSizeCombo_->addItem("2048", 2048);
	captureSizeCombo_->setToolTip(tip2(
		"Размер GPU-захвата на поверхность high-poly. Ограничивает детализацию режима\n"
		"\"как в рендере\" независимо от разрешения запечки.\n"
		"Авто: min(разрешение, 2048), но не больше sqrt(лимит / (поверхности * 13 Б)),\n"
		"где лимит = 50% свободной ОЗУ, максимум 16 ГБ.\n"
		"Ручной размер лимит не проверяет: при нехватке ОЗУ захваты приходят чёрными,\n"
		"и такие поверхности печекаются из текстур материала. Расход показан внизу окна.\n",
		"GPU capture size per high-poly surface. Caps the detail of the \"as rendered\"\n"
		"mode regardless of the bake resolution.\n"
		"Auto: min(resolution, 2048), limited by sqrt(budget / (surfaces * 13 B)),\n"
		"where budget = 50% of free RAM, 16 GB max.\n"
		"A manual size ignores the budget: if RAM runs short, captures come back black\n"
		"and those surfaces bake from the material textures. Usage is shown at the bottom."));
	debugLayout->addRow(tr2("Размер захвата", "Capture Size"),
		makeChoiceRow(captureSizeCombo_, {":/images/tools/icon_gear.png", ":/images/tools/icon_scale.png"}));
	connect(captureSizeCombo_, &QComboBox::currentIndexChanged, this,
		[this](int) { updateCaptureInfo(); });

	flipYCheck_ = new QCheckBox(tr2("Инвертировать G (Y)", "Invert G (Y)"));
	flipYCheck_->setChecked(false);
	flipYCheck_->setToolTip(tip2(
		"Дополнительная инверсия зелёного канала нормали.\n"
		"Обычно не нужна. Включайте, только если рельеф выглядит вывернутым.",
		"Extra inversion of the normal map green channel.\n"
		"Normally not needed. Enable only if the relief looks inverted."));
	debugLayout->addRow(tr2("Нормали", "Normals"), flipYCheck_);

	debugZonesCheck_ = new QCheckBox(tr2("Раскрасить зоны попаданий", "Colorize Hit Zones"));
	debugZonesCheck_->setChecked(false);
	debugZonesCheck_->setToolTip(tip2(
		"Вместо albedo запекает диагностическую раскраску:\n"
		"зелёный — поверхность выше low-poly, синий — ниже,\n"
		"красный — луч попал в изнанку, жёлтый — зона skew-маски,\n"
		"чёрный/растянутый — промах.\n"
		"Дополнительно сохраняет атласы GPU-захвата в %TEMP%/baker_captures.",
		"Bakes a diagnostic colorization instead of albedo:\n"
		"green — surface above the low-poly, blue — below,\n"
		"red — the ray hit a back face, yellow — the skew mask area,\n"
		"black/stretched — a miss.\n"
		"Also dumps the GPU capture atlases to %TEMP%/baker_captures."));
	debugLayout->addRow(QString(), debugZonesCheck_);

	shadingRaysCheck_ = new QCheckBox(tr2("Лучи по нормалям шейдинга", "Rays Along Shading Normals"));
	shadingRaysCheck_->setChecked(false);
	shadingRaysCheck_->setToolTip(tip2(
		"Пускать лучи по нормалям шейдинга вместо сглаженных.\n"
		"Может убрать скос проекции на фасетках, но даёт щели на жёстких рёбрах.",
		"Cast rays along the shading normals instead of the smoothed ones.\n"
		"May remove projection skew on facets but produces gaps at hard edges."));
	debugLayout->addRow(QString(), shadingRaysCheck_);


	// the parameters are always visible now: they are few enough that hiding
	// them behind a rollout only cost a click
	mainLayout->addWidget(settingsPanel);

	// --- Result: the name of the set, directly above the action it names ---
	{
		auto resultGroup = new QGroupBox(tr2("Результат", "Result"));
		auto nameRow = new QHBoxLayout(resultGroup);
		auto nameTitle = new QLabel(tr2("Имя набора", "Output Name"));
		nameTitle->setToolTip(nameEdit_->toolTip());
		nameRow->addWidget(nameTitle);
		nameRow->addWidget(nameEdit_, 1);
		mainLayout->addWidget(resultGroup);
	}

	// --- actions ---
	auto buttonsRow = new QHBoxLayout();
	bakeButton_ = new QPushButton(tr2("Запечь", "Bake"));
	// the main action, as Generate of the other tabs
	bakeButton_->setMinimumHeight(30);
	bakeButton_->setIcon(QIcon(QString::fromUtf8(":/images/icon_ambient_occlusion.png")));
	bakeButton_->setIconSize(QSize(16, 16));
	bakeButton_->setStyleSheet(ArtistTool::goButtonStyle());
	connect(bakeButton_, &QPushButton::clicked, this, &BakerWindow::startBake);
	cancelButton_ = new QPushButton(tr2("Отмена", "Cancel"));
	cancelButton_->setMinimumHeight(30);
	cancelButton_->setEnabled(false);
	connect(cancelButton_, &QPushButton::clicked, this, &BakerWindow::cancelBake);
	buttonsRow->addWidget(bakeButton_, 1);
	buttonsRow->addWidget(cancelButton_);
	mainLayout->addLayout(buttonsRow);

	progressBar_ = new QProgressBar();
	progressBar_->setRange(0, 100);
	progressBar_->setValue(0);
	mainLayout->addWidget(progressBar_);

	statusLabel_ = new QLabel(tr2("Выберите модели и нажмите «Запечь».",
		"Select the models and press Bake."));
	statusLabel_->setWordWrap(true);
	mainLayout->addWidget(statusLabel_);

	// what the GPU capture will actually cost: it is the real limit on "as
	// rendered" detail, and it changes with the model and the free memory
	captureInfoLabel_ = new QLabel();
	captureInfoLabel_->setWordWrap(true);
	captureInfoLabel_->setStyleSheet("color: #9a9a9a;");
	captureInfoLabel_->setToolTip(tip2(
		"Размер GPU-захвата на поверхность и расход ОЗУ.\n"
		"Меняется на вкладке Debug: Baker > Размер захвата.\n",
		"GPU capture size per surface and its RAM usage.\n"
		"Change it in the Debug tab: Baker > Capture Size."));
	mainLayout->addWidget(captureInfoLabel_);

	// no filler stretch here: the spare space goes to the tabs (and through them
	// to the participants list) instead of being wasted above the credit line

	// The credit line and the version are those of Artist Tool, under its tabs.

	captureTimer_ = new QTimer(this);
	connect(captureTimer_, &QTimer::timeout, this, &BakerWindow::onCaptureTick);
	drainTimer_ = new QTimer(this);
	drainTimer_->setInterval(100);
	connect(drainTimer_, &QTimer::timeout, this, &BakerWindow::drain_transfers);

	// remember the editor's "Active Tool" window when it first appears — it can
	// then be reopened programmatically after assigning the skew mask
	connect(UnigineEditor::WindowManager::instance(), &UnigineEditor::WindowManager::windowShown,
		this, [this](QWidget *widget) {
			if (widget && widget->objectName() == QLatin1String("ActiveToolWidget"))
				activeToolWindow_ = widget;
		});

	restoreSettings();
	refreshMeshTree();
	setUiLocked(false);
}

BakerWindow::~BakerWindow()
{
	saveSettings();
	if (debugPanel_ && !debugPanel_->parent())
		delete debugPanel_;
}

QWidget *BakerWindow::takeDebugControls()
{
	return debugPanel_;
}

void BakerWindow::saveSettings() const
{
	QSettings s("zloy_pingvin", "TextureBaker");
	s.setValue("resolution", resolutionCombo_->currentData().toInt());
	s.setValue("frontal", frontalSpin_->value());
	s.setValue("rear", rearSpin_->value());
	s.setValue("samples", samplesCombo_->currentData().toInt());
	s.setValue("use_skew_mask", skewMaskCheck_->isChecked());
	s.setValue("bake_albedo", albedoCheck_->isChecked());
	s.setValue("bake_shading", shadingCheck_->isChecked());
	s.setValue("bake_normal", normalCheck_->isChecked());
	s.setValue("bake_emission", emissionCheck_->isChecked());
	s.setValue("bake_decals", decalsCheck_->isChecked());
	s.setValue("decal_distance", decalDistanceSpin_->value());
	s.setValue("groups_mode", groupsModeCombo_->currentData().toInt());
	s.setValue("gpu_mode", gpuCheck_->isChecked());
	s.setValue("capture_uv", captureUVCombo_->currentData().toInt());
	s.setValue("capture_size", captureSizeCombo_->currentData().toInt());
	s.setValue("collapsed_sections",
		QStringList(collapsedSections_.begin(), collapsedSections_.end()).join(','));
	s.setValue("flip_y", flipYCheck_->isChecked());
	s.setValue("shading_rays", shadingRaysCheck_->isChecked());
}

void BakerWindow::restoreSettings()
{
	QSettings s("zloy_pingvin", "TextureBaker");
	int idx = resolutionCombo_->findData(s.value("resolution", 2048).toInt());
	if (idx >= 0)
		resolutionCombo_->setCurrentIndex(idx);
	frontalSpin_->setValue(s.value("frontal", 0.05).toDouble());
	rearSpin_->setValue(s.value("rear", 0.05).toDouble());
	idx = samplesCombo_->findData(s.value("samples", 4).toInt());
	if (idx >= 0)
		samplesCombo_->setCurrentIndex(idx);
	skewMaskCheck_->setChecked(s.value("use_skew_mask", true).toBool());
	albedoCheck_->setChecked(s.value("bake_albedo", true).toBool());
	shadingCheck_->setChecked(s.value("bake_shading", true).toBool());
	normalCheck_->setChecked(s.value("bake_normal", true).toBool());
	emissionCheck_->setChecked(s.value("bake_emission", false).toBool());
	decalsCheck_->setChecked(s.value("bake_decals", false).toBool());
	decalDistanceSpin_->setValue(s.value("decal_distance", 1.0).toDouble());
	// manual groups are not persisted (node IDs are not stable across sessions),
	// so restore the manual mode as "off"
	int gm = s.value("groups_mode", 0).toInt();
	if (gm == 1)
		gm = 0;
	idx = groupsModeCombo_->findData(gm);
	if (idx >= 0)
		groupsModeCombo_->setCurrentIndex(idx);
	gpuCheck_->setChecked(s.value("gpu_mode", true).toBool());
	idx = captureUVCombo_->findData(s.value("capture_uv", -1).toInt());
	if (idx >= 0)
		captureUVCombo_->setCurrentIndex(idx);
	idx = captureSizeCombo_->findData(s.value("capture_size", -1).toInt());
	if (idx >= 0)
		captureSizeCombo_->setCurrentIndex(idx);
	collapsedSections_.clear();
	for (const QString &key :
		s.value("collapsed_sections").toString().split(',', Qt::SkipEmptyParts))
		collapsedSections_.insert(key);
	flipYCheck_->setChecked(s.value("flip_y", false).toBool());
	shadingRaysCheck_->setChecked(s.value("shading_rays", false).toBool());
}

// Picks a node containing Static Meshes (itself or nested children) from the
// scene selection; fills the label with the name (+ mesh count). Returns the
// node ID or 0.
int BakerWindow::grabSelectedMeshGroupId(QLabel *label)
{
	const UnigineEditor::SelectorNodes *selector = UnigineEditor::Selection::getSelectorNodes();
	if (!selector || selector->getNodes().size() == 0)
	{
		statusLabel_->setText(tr2("Сначала выделите ноду в сцене.",
			"Select a node in the scene first."));
		return 0;
	}
	for (const NodePtr &node : selector->getNodes())
	{
		std::vector<Ptr<ObjectMeshStatic>> meshes = BakeCore::collectMeshes(node);
		if (meshes.empty())
			continue;
		QString name = QString::fromUtf8(node->getName());
		if (meshes.size() > 1)
			name += tr2(" (%1 мешей)", " (%1 meshes)").arg(meshes.size());
		label->setText(name);
		statusLabel_->setText("");
		return node->getID();
	}
	statusLabel_->setText(tr2("В выделении нет ноды с мешами типа Static Mesh.",
		"The selection contains no node with Static Meshes."));
	return 0;
}

// Expands the given root nodes into their Static Meshes and APPENDS them to
// the high/low slot (duplicates skipped), then refreshes the dependent UI.
void BakerWindow::addSlotNodes(bool high, const std::vector<int> &rootIds)
{
	std::vector<int> &ids = high ? highIds_ : lowIds_;
	int added = 0;
	for (int rootId : rootIds)
		for (const Ptr<ObjectMeshStatic> &mesh : BakeCore::collectMeshes(World::getNodeByID(rootId)))
		{
			const int id = mesh->getID();
			if (std::find(ids.begin(), ids.end(), id) == ids.end())
			{
				ids.push_back(id);
				added++;
			}
		}
	if (!added)
	{
		statusLabel_->setText(tr2("Мешей типа Static Mesh не добавлено (нет в ноде или уже в списке).",
			"No Static Meshes added (none in the node or already listed)."));
		return;
	}

	// decals live in the same instance hierarchy as the high-poly — gather them
	// automatically so they show in the list and get baked (correct positions)
	if (high)
		for (int rootId : rootIds)
		{
			std::function<void(const NodePtr &)> walk = [&](const NodePtr &n) {
				if (!n)
					return;
				if (n->isDecal())
				{
					int id = n->getID();
					if (std::find(decalIds_.begin(), decalIds_.end(), id) == decalIds_.end())
						decalIds_.push_back(id);
				}
				for (int i = 0; i < n->getNumChildren(); i++)
					walk(n->getChild(i));
				// descend into node reference contents (decals nested in a prefab)
				if (n->getType() == Node::NODE_REFERENCE)
					if (Ptr<NodeReference> nr = checked_ptr_cast<NodeReference>(n))
						walk(nr->getReference());
			};
			walk(World::getNodeByID(rootId));
		}
	// newly added meshes follow the slot's current visibility toggle
	if ((high ? highHideButton_ : lowHideButton_)->isChecked())
		setSlotHidden(high, true);
	statusLabel_->setText("");
	if (high)
		updateSuggestedName();
	updateSlotLabels();
	refreshMeshTree();
	setUiLocked(baking_);
}

void BakerWindow::forEachHighHierarchyNode(const std::function<void(const NodePtr &)> &fn) const
{
	if (!fn)
		return;
	std::set<int> visited;
	std::vector<NodePtr> stack;
	for (int id : highIds_)
		if (NodePtr n = World::getNodeByID(id))
		{
			NodePtr top = n;
			while (top && top->getParent())
				top = top->getParent();
			if (top)
				stack.push_back(top);
		}
	while (!stack.empty())
	{
		NodePtr n = stack.back();
		stack.pop_back();
		if (!n || !visited.insert(n->getID()).second)
			continue;
		for (int i = 0; i < n->getNumChildren(); i++)
			stack.push_back(n->getChild(i));
		if (n->getType() == Node::NODE_REFERENCE)
			if (Ptr<NodeReference> nr = checked_ptr_cast<NodeReference>(n))
				stack.push_back(nr->getReference());
		fn(n);
	}
}

void BakerWindow::forEachHighDecal(
	const std::function<void(const NodePtr &, const NodePtr &)> &fn) const
{
	if (decalIds_.empty() || !fn)
		return;
	const std::set<int> wanted(decalIds_.begin(), decalIds_.end());
	std::set<int> visited;
	// each entry carries the Node Reference it was reached through (null = none)
	struct Entry
	{
		NodePtr node;
		NodePtr enclosingRef;
	};
	std::vector<Entry> stack;
	// start from the top ancestors, exactly like the gather and the bake do
	for (int id : highIds_)
		if (NodePtr n = World::getNodeByID(id))
		{
			NodePtr top = n;
			while (top && top->getParent())
				top = top->getParent();
			if (top)
				stack.push_back({top, NodePtr()});
		}
	while (!stack.empty())
	{
		const Entry e = stack.back();
		stack.pop_back();
		const NodePtr &n = e.node;
		if (!n || !visited.insert(n->getID()).second)
			continue;
		for (int i = 0; i < n->getNumChildren(); i++)
			stack.push_back({n->getChild(i), e.enclosingRef});
		// the whole point: descend into node reference contents, remembering
		// which reference we entered
		if (n->getType() == Node::NODE_REFERENCE)
			if (Ptr<NodeReference> nr = checked_ptr_cast<NodeReference>(n))
				stack.push_back({nr->getReference(), n});
		if (n->isDecal() && wanted.count(n->getID()))
			fn(n, e.enclosingRef);
	}
}

// Viewport-only visibility of the slot meshes; baking ignores it
// (collectMeshes includes disabled nodes on purpose).
void BakerWindow::setSlotHidden(bool high, bool hidden)
{
	for (int id : (high ? highIds_ : lowIds_))
		if (NodePtr node = World::getNodeByID(id))
			node->setEnabled(!hidden);
	// decals are gathered FROM the high-poly hierarchy, so they belong to that
	// slot: hiding the meshes while their stickers stay floating in the viewport
	// is not what "hide the high-poly" means
	if (high)
	{
		// A decal INSIDE a Node Reference keeps rendering even after its own
		// enabled flag is cleared (verified: the flag takes, the decal stays
		// visible) — the engine draws the reference's content as a unit. So the
		// reference itself has to be toggled, which also hides the rest of that
		// prefab: correct for a "hide the high-poly" switch, since the prefab is
		// part of the same high-poly assembly.
		const std::set<int> listedDecals(decalIds_.begin(), decalIds_.end());
		std::vector<NodePtr> refs;
		QStringList unlisted;
		forEachHighHierarchyNode([&](const NodePtr &n) {
			if (n->isDecal())
			{
				n->setEnabled(!hidden);
				// a decal in the hierarchy that never made it into the list would
				// also never be baked — worth naming rather than silently missing
				if (!listedDecals.count(n->getID()))
					unlisted << QString::fromUtf8(n->getName());
			}
			// EVERY reference in the hierarchy, not just ones holding decals:
			// prefabs carry meshes too (indicators, screens), and their content
			// is equally unreachable node-by-node
			if (n->getType() == Node::NODE_REFERENCE)
				refs.push_back(n);
		});
		for (const NodePtr &r : refs)
			r->setEnabled(!hidden);
		Unigine::Log::message("Baker: hide high-poly %s: %d node reference(s) toggled as a whole "
							  "(their content cannot be hidden node by node)\n",
			hidden ? "on" : "off", int(refs.size()));
		if (!unlisted.isEmpty())
			Unigine::Log::warning("Baker: decals in the high-poly hierarchy that are NOT in the "
								  "participants list (they would not be baked either): %s\n",
				unlisted.join(", ").toUtf8().constData());
	}
	QPushButton *btn = high ? highHideButton_ : lowHideButton_;
	btn->setText(hidden ? tr2("Показать", "Show") : tr2("Скрыть", "Hide"));
}

void BakerWindow::clearSlot(bool high)
{
	// restore visibility before dropping the meshes from the list
	QPushButton *hideBtn = high ? highHideButton_ : lowHideButton_;
	if (hideBtn->isChecked())
		hideBtn->setChecked(false);
	(high ? highIds_ : lowIds_).clear();
	if (high)
	{
		decalIds_.clear(); // decals were gathered from the high-poly
		updateSuggestedName();
	}
	else
		partCage_.clear(); // the per-part cage belonged to those low-poly parts
	updateSlotLabels();
	refreshMeshTree();
	setUiLocked(baking_);
}

void BakerWindow::updateSlotLabels()
{
	auto describe = [this](const std::vector<int> &ids) -> QString {
		if (ids.empty())
			return tr2("<не выбрано>", "<not set>");
		QString name;
		if (NodePtr node = World::getNodeByID(ids[0]))
			name = QString::fromUtf8(node->getName());
		if (ids.size() > 1)
			name += tr2(" (+ ещё %1)", " (+ %1 more)").arg(ids.size() - 1);
		return name;
	};
	highLabel_->setText(describe(highIds_));
	lowLabel_->setText(describe(lowIds_));
}

// Rebuilds the striped participants list: a High-poly section with dark rows
// and a Low-poly section with lighter rows (like the World Nodes hierarchy).
// Column 1 holds the per-mesh remove button (✕).
void BakerWindow::fillCageCells(QTreeWidgetItem *item, int nodeId)
{
	if (!item || !frontalSpin_ || !rearSpin_)
		return;
	auto it = partCage_.find(nodeId);
	const PartCage pc = it != partCage_.end() ? it->second : PartCage();
	for (int c = 1; c <= 2; c++)
	{
		const bool isFront = c == 1;
		const bool explicitAxis = isFront ? pc.hasFrontal : pc.hasRear;
		const float value = explicitAxis
			? (isFront ? pc.frontal : pc.rear)
			: float(isFront ? frontalSpin_->value() : rearSpin_->value());
		item->setText(c, QString::number(value, 'f', 3));
		// dim = follows the global setting, bright = set by hand for this part
		item->setForeground(c, explicitAxis ? QColor(0xe0, 0xe0, 0xe0) : QColor(0x82, 0x82, 0x82));
		item->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
		item->setToolTip(c,
			explicitAxis
				? tip2("Задано для этой детали. Очистите поле, чтобы вернуть общее значение.",
					  "Set for this part. Clear the field to return to the global value.")
				: tip2("Следует за общей настройкой. Впишите своё значение (в метрах).",
					  "Follows the global setting. Type your own value (in meters)."));
	}
}

void BakerWindow::refreshMeshTree()
{
	if (!meshTree_)
		return;
	treeUpdating_ = true;
	meshTree_->clear();

	// each role gets its own hue so the three sections are told apart at a
	// glance: blue = source, amber = target, green = decals. Kept dark and
	// low-saturation to sit in the editor's theme.
	const QColor highBg(0x25, 0x2e, 0x3d);  // blue
	const QColor lowBg(0x43, 0x38, 0x41);   // mauve
	const QColor decalBg(0x2e, 0x3a, 0x2e); // green
	// section header: the row colour, a shade deeper
	auto headerOf = [](const QColor &c) { return c.darker(135); };

	// removable = the row's ✕ takes the mesh out of the slot. Group sections are
	// defined by their root node, so individual meshes cannot be dropped there.
	auto addSection = [this, &headerOf](const QString &title, const std::vector<int> &ids,
							const QColor &rowBg, bool cage, const QString &key, bool removable) {
		const QColor headerBg = headerOf(rowBg);
		auto top = new QTreeWidgetItem(meshTree_, {title});
		top->setData(0, Qt::UserRole, key); // fold state is remembered by key
		QFont f = top->font(0);
		f.setBold(true);
		top->setFont(0, f);
		// the section header doubles as the column caption for the cage columns
		if (cage)
		{
			top->setText(1, tr2("вперёд", "front"));
			top->setText(2, tr2("назад", "back"));
			for (int c = 1; c <= 2; c++)
			{
				top->setFont(c, f);
				top->setTextAlignment(c, Qt::AlignRight | Qt::AlignVCenter);
				top->setToolTip(c, tip2("Кейдж этой детали, в метрах.", "This part's cage, in meters."));
			}
		}
		for (int c = 0; c < 4; c++)
			top->setBackground(c, headerBg);
		top->setFlags(top->flags() & ~Qt::ItemIsSelectable);
		for (int id : ids)
		{
			NodePtr node = World::getNodeByID(id);
			if (!node)
				continue;
			auto item = new QTreeWidgetItem(top, {QString::fromUtf8(node->getName())});
			for (int c = 0; c < 4; c++)
				item->setBackground(c, rowBg);
			item->setData(0, Qt::UserRole, id);
			if (removable)
			{
				item->setText(3, "✕");
				item->setTextAlignment(3, Qt::AlignCenter);
				item->setForeground(3, QColor(0xa0, 0xa0, 0xa0));
				item->setToolTip(3, tip2("Убрать из списка.", "Remove from the list."));
			}
			if (cage)
			{
				item->setFlags(item->flags() | Qt::ItemIsEditable);
				fillCageCells(item, id);
			}
		}
		if (top->childCount() == 0)
		{
			auto item = new QTreeWidgetItem(top, {tr2("<не выбрано>", "<not set>")});
			for (int c = 0; c < 4; c++)
				item->setBackground(c, rowBg);
			item->setForeground(0, QColor(0x80, 0x80, 0x80));
		}
		top->setExpanded(!collapsedSections_.count(key));
	};

	if (manualGroups())
	{
		// same list, one pair of sections per group: the cage columns, the colours
		// and the folding all work exactly as in single mode
		for (size_t i = 0; i < groupRows_.size(); i++)
		{
			const GroupRow &g = groupRows_[i];
			std::vector<int> gHigh, gLow;
			for (const auto &m : BakeCore::collectMeshes(World::getNodeByID(g.highId)))
				gHigh.push_back(m->getID());
			for (const auto &m : BakeCore::collectMeshes(World::getNodeByID(g.lowId)))
				gLow.push_back(m->getID());
			addSection(tr2("Группа %1 · high-poly", "Group %1 · high-poly").arg(i + 1), gHigh,
				highBg, false, QString("g%1hi").arg(i), false);
			addSection(tr2("Группа %1 · low-poly", "Group %1 · low-poly").arg(i + 1), gLow, lowBg,
				true, QString("g%1lo").arg(i), false);
		}
	}
	else
	{
		addSection(tr2("High-poly (источник)", "High-poly (source)"), highIds_, highBg, false,
			"high", true);
		// the cage offsets the ray ORIGIN from the low-poly surface, so it is a
		// property of the low-poly part — that is the only section carrying it
		addSection(tr2("Low-poly (цель)", "Low-poly (target)"), lowIds_, lowBg, true, "low", true);
		if (decalsCheck_ && decalsCheck_->isChecked())
			addSection(tr2("Декали (из high-poly)", "Decals (from high-poly)"), decalIds_, decalBg,
				false, "decals", true);
	}
	treeUpdating_ = false;
	if (resetCageButton_)
		resetCageButton_->setEnabled(!partCage_.empty());
	updateCaptureInfo();
}

void BakerWindow::updateSuggestedName()
{
	QString suggestion;
	if (manualGroups())
	{
		const int rootId = groupRows_.empty() ? 0 : groupRows_[0].highId;
		std::vector<Ptr<ObjectMeshStatic>> meshes = BakeCore::collectMeshes(World::getNodeByID(rootId));
		if (!meshes.empty())
			suggestion = QString::fromUtf8(BakeCore::suggestBaseName(meshes[0]).get());
	}
	else if (!highIds_.empty())
	{
		Ptr<ObjectMeshStatic> mesh = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(highIds_[0]));
		if (mesh)
			suggestion = QString::fromUtf8(BakeCore::suggestBaseName(mesh).get());
	}
	// respect a user-edited name: overwrite only untouched/auto-filled text
	if (nameEdit_->text().isEmpty() || nameEdit_->text() == lastAutoName_)
		nameEdit_->setText(suggestion);
	lastAutoName_ = suggestion;
}

// The World Nodes hierarchy drags "application/unigine.hierarchy.list". The
// payload layout is not documented: scan it for 32-bit values (both byte
// orders) that resolve to existing scene nodes.
std::vector<int> BakerWindow::decodeNodeDrop(const QMimeData *mime) const
{
	std::vector<int> ids;
	if (!mime)
		return ids;
	const QByteArray data = mime->data(QString::fromUtf8("application/unigine.hierarchy.list"));
	if (data.isEmpty())
		return ids;

	auto tryAdd = [&ids](int v) {
		if (v == 0 || !World::getNodeByID(v))
			return;
		if (std::find(ids.begin(), ids.end(), v) == ids.end())
			ids.push_back(v);
	};
	for (int off = 0; off + 4 <= data.size(); off++)
	{
		quint32 le = 0;
		memcpy(&le, data.constData() + off, 4);
		tryAdd(int(le));
		const quint32 be = ((le & 0xFF) << 24) | ((le & 0xFF00) << 8)
			| ((le >> 8) & 0xFF00) | (le >> 24);
		tryAdd(int(be));
	}

	if (ids.empty())
		Unigine::Log::warning("Baker: could not decode the dropped nodes (payload %d bytes: %s...)\n",
			int(data.size()), data.left(48).toHex(' ').constData());
	return ids;
}

bool BakerWindow::eventFilter(QObject *watched, QEvent *event)
{
	if (meshTree_ && watched == meshTree_->viewport())
	{
		if (event->type() == QEvent::DragEnter || event->type() == QEvent::DragMove)
		{
			auto drag = static_cast<QDropEvent *>(event);
			if (drag->mimeData()->hasFormat(QString::fromUtf8("application/unigine.hierarchy.list")))
			{
				drag->acceptProposedAction();
				return true;
			}
		}
		else if (event->type() == QEvent::Drop)
		{
			auto drop = static_cast<QDropEvent *>(event);
			std::vector<int> ids = decodeNodeDrop(drop->mimeData());
			if (ids.empty())
			{
				statusLabel_->setText(tr2("Не удалось распознать перетащенную ноду.",
					"Could not recognize the dropped node."));
				return true;
			}
			// the section (top-level item) under the cursor decides the slot
			QTreeWidgetItem *item = meshTree_->itemAt(drop->position().toPoint());
			if (!item)
			{
				statusLabel_->setText(tr2("Бросьте ноду на секцию High-poly или Low-poly.",
					"Drop the node onto the High-poly or Low-poly section."));
				return true;
			}
			QTreeWidgetItem *top = item;
			while (top->parent())
				top = top->parent();
			addSlotNodes(meshTree_->indexOfTopLevelItem(top) == 0, ids);
			drop->acceptProposedAction();
			return true;
		}
	}
	return QWidget::eventFilter(watched, event);
}

void BakerWindow::pickHighFromSelection()
{
	const UnigineEditor::SelectorNodes *selector = UnigineEditor::Selection::getSelectorNodes();
	if (!selector || selector->getNodes().size() == 0)
	{
		statusLabel_->setText(tr2("Сначала выделите ноду в сцене.",
			"Select a node in the scene first."));
		return;
	}
	std::vector<int> roots;
	for (const NodePtr &node : selector->getNodes())
		roots.push_back(node->getID());
	addSlotNodes(true, roots);
}

void BakerWindow::addGroupRow()
{
	GroupRow gr;
	gr.box = new QGroupBox();
	auto form = new QFormLayout(gr.box);

	const QString notSet = tr2("<не выбрано>", "<not set>");
	const QString pickText = tr2("Добавить выделенное", "Add Selected");
	const QString clearTip = tip2("Очистить слот.", "Clear the slot.");
	auto makeClearButton = [&clearTip]() {
		auto btn = new QPushButton("✕");
		btn->setFixedWidth(24);
		btn->setToolTip(clearTip);
		return btn;
	};

	// viewport-only toggle: the group's slot is a root node, so disabling that
	// node hides its whole subtree — node references included
	const QString hideTipGroup = tip2(
		"Скрыть/показать ноду слота во вьюпорте вместе со всем её содержимым.\n"
		"На запекание не влияет.",
		"Hide/show the slot node in the viewport with its whole subtree.\n"
		"Does not affect baking.");
	auto makeHideBtn = [&hideTipGroup, this]() {
		auto btn = new QPushButton(tr2("Скрыть", "Hide"));
		btn->setIcon(QIcon(QString::fromUtf8(":/images/icons_eye_off.png")));
		btn->setCheckable(true);
		btn->setToolTip(hideTipGroup);
		return btn;
	};

	gr.highLabel = new QLabel(notSet);
	auto highBtn = new QPushButton(pickText);
	highBtn->setIcon(QIcon(QString::fromUtf8(":/images/mods/icon_object_mode.png")));
	gr.hideHighButton = makeHideBtn();
	auto highClear = makeClearButton();
	auto highRow = new QHBoxLayout();
	highRow->addWidget(gr.highLabel, 1);
	highRow->addWidget(highBtn);
	highRow->addWidget(gr.hideHighButton);
	highRow->addWidget(highClear);
	form->addRow(tr2("High-poly", "High-Poly"), highRow);

	gr.lowLabel = new QLabel(notSet);
	auto lowBtn = new QPushButton(pickText);
	lowBtn->setIcon(QIcon(QString::fromUtf8(":/images/mods/icon_object_mode.png")));
	gr.hideLowButton = makeHideBtn();
	auto lowClear = makeClearButton();
	auto lowRow = new QHBoxLayout();
	lowRow->addWidget(gr.lowLabel, 1);
	lowRow->addWidget(lowBtn);
	lowRow->addWidget(gr.hideLowButton);
	lowRow->addWidget(lowClear);
	form->addRow(tr2("Low-poly", "Low-Poly"), lowRow);

	auto removeBtn = new QPushButton(tr2("Удалить группу", "Remove Group"));
	removeBtn->setIcon(QIcon(QString::fromUtf8(":/images/icon_delete_trash.png")));
	form->addRow(QString(), removeBtn);

	QGroupBox *boxPtr = gr.box;
	for (int slot = 0; slot < 2; slot++)
	{
		QPushButton *btn = slot == 0 ? gr.hideHighButton : gr.hideLowButton;
		connect(btn, &QPushButton::toggled, this, [this, boxPtr, slot, btn](bool on) {
			for (const GroupRow &g : groupRows_)
				if (g.box == boxPtr)
					if (NodePtr node = World::getNodeByID(slot == 0 ? g.highId : g.lowId))
						node->setEnabled(!on);
			btn->setText(on ? tr2("Показать", "Show") : tr2("Скрыть", "Hide"));
		});
	}
	connect(highBtn, &QPushButton::clicked, this, [this, boxPtr]() {
		for (GroupRow &g : groupRows_)
			if (g.box == boxPtr)
			{
				int id = grabSelectedMeshGroupId(g.highLabel);
				if (id)
					g.highId = id;
				break;
			}
		updateSuggestedName();
		refreshMeshTree();
		setUiLocked(baking_);
	});
	connect(lowBtn, &QPushButton::clicked, this, [this, boxPtr]() {
		for (GroupRow &g : groupRows_)
			if (g.box == boxPtr)
			{
				int id = grabSelectedMeshGroupId(g.lowLabel);
				if (id)
					g.lowId = id;
				break;
			}
		refreshMeshTree();
		setUiLocked(baking_);
	});
	connect(highClear, &QPushButton::clicked, this, [this, boxPtr, notSet]() {
		for (GroupRow &g : groupRows_)
			if (g.box == boxPtr)
			{
				g.highId = 0;
				g.highLabel->setText(notSet);
				break;
			}
		updateSuggestedName();
		refreshMeshTree();
		setUiLocked(baking_);
	});
	connect(lowClear, &QPushButton::clicked, this, [this, boxPtr, notSet]() {
		for (GroupRow &g : groupRows_)
			if (g.box == boxPtr)
			{
				g.lowId = 0;
				g.lowLabel->setText(notSet);
				break;
			}
		refreshMeshTree();
		setUiLocked(baking_);
	});
	connect(removeBtn, &QPushButton::clicked, this, [this, boxPtr]() { removeGroupRow(boxPtr); });

	// keep the add button at the bottom of the panel
	groupsLayout_->insertWidget(groupsLayout_->count() - 1, gr.box);
	groupRows_.push_back(gr);
	renumberGroups();
	refreshMeshTree();
	setUiLocked(baking_);
}

void BakerWindow::removeGroupRow(QGroupBox *box)
{
	for (auto it = groupRows_.begin(); it != groupRows_.end(); ++it)
		if (it->box == box)
		{
			box->deleteLater();
			groupRows_.erase(it);
			break;
		}
	renumberGroups();
	refreshMeshTree();
	setUiLocked(baking_);
}

void BakerWindow::renumberGroups()
{
	for (size_t i = 0; i < groupRows_.size(); i++)
		groupRows_[i].box->setTitle(tr2("Группа %1", "Group %1").arg(i + 1));
}

bool BakerWindow::manualGroups() const
{
	return groupsModeCombo_->currentData().toInt() == 1;
}

bool BakerWindow::nameGroups() const
{
	return groupsModeCombo_->currentData().toInt() == 2;
}

void BakerWindow::updateModelsMode()
{
	const bool manual = manualGroups();
	singlePanel_->setVisible(!manual); // the single pair
	groupsPanel_->setVisible(manual);  // the manual groups
	if (manual && groupRows_.empty())
		addGroupRow();
	updateSuggestedName();
	setUiLocked(baking_);
}

int BakerWindow::firstLowId() const
{
	if (!manualGroups())
		return lowIds_.empty() ? 0 : lowIds_[0];
	for (const GroupRow &g : groupRows_)
		if (g.lowId)
			return g.lowId;
	return 0;
}

// Normalized name for by-name pairing: lowercased, the trailing
// _low/_high/_lp/_hp/_lowpoly/_highpoly/_lod<N> decorations stripped.
static QString bakeGroupKey(const char *name)
{
	QString n = QString::fromUtf8(name).toLower();
	static const QRegularExpression re(
		QString::fromUtf8("(_(low|high|lp|hp|lowpoly|highpoly)|_lod[_\\-]?\\d*)+$"));
	n.remove(re);
	return n;
}

void BakerWindow::pickLowFromSelection()
{
	const UnigineEditor::SelectorNodes *selector = UnigineEditor::Selection::getSelectorNodes();
	if (!selector || selector->getNodes().size() == 0)
	{
		statusLabel_->setText(tr2("Сначала выделите ноду в сцене.",
			"Select a node in the scene first."));
		return;
	}
	std::vector<int> roots;
	for (const NodePtr &node : selector->getNodes())
		roots.push_back(node->getID());
	addSlotNodes(false, roots);
}

void BakerWindow::setUiLocked(bool locked)
{
	uiLocked_ = locked;
	update_busy();
	highPickButton_->setEnabled(!locked);
	lowPickButton_->setEnabled(!locked);
	groupsModeCombo_->setEnabled(!locked);
	addGroupButton_->setEnabled(!locked);
	for (const GroupRow &g : groupRows_)
		g.box->setEnabled(!locked);
	resolutionCombo_->setEnabled(!locked);
	frontalSpin_->setEnabled(!locked);
	rearSpin_->setEnabled(!locked);
	samplesCombo_->setEnabled(!locked);
	flipYCheck_->setEnabled(!locked);
	gpuCheck_->setEnabled(!locked);
	captureUVCombo_->setEnabled(!locked);
	debugZonesCheck_->setEnabled(!locked);
	shadingRaysCheck_->setEnabled(!locked);
	skewMaskCheck_->setEnabled(!locked);
	albedoCheck_->setEnabled(!locked);
	shadingCheck_->setEnabled(!locked);
	normalCheck_->setEnabled(!locked);
	emissionCheck_->setEnabled(!locked);
	decalsCheck_->setEnabled(!locked);
	decalDistanceSpin_->setEnabled(!locked);
	skewMaskButton_->setEnabled(!locked && firstLowId() != 0);
	if (autoCageButton_)
		autoCageButton_->setEnabled(!locked && firstLowId() != 0);
	if (resetCageButton_)
		resetCageButton_->setEnabled(!locked && !partCage_.empty());
	if (meshTree_)
		meshTree_->setEnabled(!locked);
	bool modelsReady;
	if (manualGroups())
	{
		modelsReady = !groupRows_.empty();
		for (const GroupRow &g : groupRows_)
			if (!g.highId || !g.lowId)
				modelsReady = false;
	}
	else
		modelsReady = !highIds_.empty() && !lowIds_.empty();
	bakeButton_->setEnabled(!locked && modelsReady);
	cancelButton_->setEnabled(locked);
}

void BakerWindow::update_busy()
{
	// While it bakes or probes, the plugin must not be reloaded: the code of the bake is
	// on the stack under the events it lets through (see the reloader). The end of a bake
	// (the import of the textures, the material) lets events through too: runBake() keeps
	// the window busy until it returns. And while readbacks of GPU captures are still on
	// their way (a bake cancelled or timed out): their callbacks are code of the plugin.
	const int in_flight = BakeGpu::transfersInFlight();
	setProperty("artist_tool_busy", uiLocked_ || finishing_ || in_flight > 0);
	if (drainTimer_ && in_flight > 0 && !uiLocked_ && !finishing_ && !drainTimer_->isActive())
		drainTimer_->start();
}

void BakerWindow::drain_transfers()
{
	// The readbacks come with a render: one is made now and then, until all are in.
	if (BakeGpu::transfersInFlight() > 0 && ++drainTicks_ % 10 == 0)
		BakeGpu::flushTransfers();
	if (BakeGpu::transfersInFlight() <= 0)
	{
		drainTimer_->stop();
		drainTicks_ = 0;
	}
	update_busy();
}

void BakerWindow::resetSettings()
{
	if (property("artist_tool_busy").toBool())
		return;
	{
		QSettings s("zloy_pingvin", "TextureBaker");
		s.clear();
	}
	// With nothing saved, every field takes its default.
	restoreSettings();
	debugZonesCheck_->setChecked(false);
	refreshMeshTree();
}

void BakerWindow::stop()
{
	if (property("artist_tool_busy").toBool())
		cancelBake();
	// The plugin goes away: the readbacks still on their way are taken in now.
	for (int attempt = 0; attempt < 20 && BakeGpu::transfersInFlight() > 0; ++attempt)
	{
		BakeGpu::flushTransfers();
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}

void BakerWindow::cancelBake()
{
	cancelRequested_ = true;
	statusLabel_->setText(tr2("Отмена...", "Cancelling..."));
}

void BakerWindow::onCreateSkewMask()
{
	if (baking_)
		return;

	const int lowId = firstLowId();
	Ptr<ObjectMeshStatic> low;
	{
		std::vector<Ptr<ObjectMeshStatic>> meshes = BakeCore::collectMeshes(World::getNodeByID(lowId));
		if (!meshes.empty())
			low = meshes[0];
	}
	if (!low)
	{
		statusLabel_->setText(tr2("Low-poly модель не выбрана (или нода удалена).",
			"The low-poly model is not set (or the node was deleted)."));
		return;
	}

	String error;
	String virtualPath = BakeCore::createSkewMask(low, 1024, error);
	if (virtualPath.empty())
	{
		statusLabel_->setText(QString::fromUtf8(error.get()));
		return;
	}
	// import UNCOMPRESSED ("unchanged"): the default texture import compresses
	// to BC (ATI1), and the editor's paint tools cannot paint compressed textures.
	// An already-correct asset is NOT touched at all: reimport re-processes the
	// asset after this call returns and wipes the freshly assigned slot.
	const bool wasAsset = UnigineEditor::AssetManager::isAsset(virtualPath.get());
	bool needsImport = !wasAsset;
	bool needsReimport = false;
	if (wasAsset)
	{
		UnigineEditor::CollectionPtr current =
			UnigineEditor::AssetManager::getAssetImportParameters(virtualPath.get());
		needsReimport = !(current && current->getBool("unchanged", false));
	}
	if (needsImport || needsReimport)
	{
		UnigineEditor::CollectionPtr importParams = UnigineEditor::Collection::create();
		importParams->setBool("unchanged", true);
		importParams->setBool("srgb_correction", false);
		const bool imported = needsImport
			? UnigineEditor::AssetManager::importAssetSync(virtualPath.get(), importParams)
			: UnigineEditor::AssetManager::reimportAssetSync(virtualPath.get(), importParams);
		Unigine::Log::message("Baker: skew mask %s \"%s\": %s\n",
			needsImport ? "import" : "reimport (was compressed)",
			virtualPath.get(), imported ? "ok" : "FAILED");
		if (needsImport && !imported)
		{
			// without a first import the engine cannot resolve the path at all
			statusLabel_->setText(tr2("Не удалось импортировать маску: %1",
				"Failed to import the mask: %1")
				.arg(QString::fromUtf8(virtualPath.get())));
			return;
		}
	}
	else
	{
		Unigine::Log::message("Baker: skew mask \"%s\" already imported, reusing as is\n",
			virtualPath.get());
	}

	// one mask in the shared UV layout, assigned to every low-poly part
	if (manualGroups())
	{
		for (const GroupRow &g : groupRows_)
			for (const Ptr<ObjectMeshStatic> &part : BakeCore::collectMeshes(World::getNodeByID(g.lowId)))
				BakeCore::assignSkewMask(part, virtualPath.get());
	}
	else
		for (int id : lowIds_)
			if (Ptr<ObjectMeshStatic> part = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(id)))
				BakeCore::assignSkewMask(part, virtualPath.get());

	// hide the high-poly so it does not block painting on the low-poly
	// (viewport-only: baking ignores the hidden state)
	if (manualGroups())
	{
		for (const GroupRow &g : groupRows_)
			for (const Ptr<ObjectMeshStatic> &part : BakeCore::collectMeshes(World::getNodeByID(g.highId)))
				part->setEnabled(false);
	}
	else if (!highIds_.empty() && !highHideButton_->isChecked())
		highHideButton_->setChecked(true); // triggers setSlotHidden(true, true)

	// select the low-poly so the Texture Editor picks it up; refresh so the
	// Parameters window shows the newly assigned custom texture
	auto selectLow = [lowId]() {
		Unigine::Vector<NodePtr> nodes;
		nodes.append(World::getNodeByID(lowId));
		if (UnigineEditor::SelectorNodes *sel = UnigineEditor::SelectorNodes::createObjectsSelector(nodes))
			UnigineEditor::SelectionAction::applySelection(sel);
		UnigineEditor::SelectionAction::refreshSelection();
	};
	selectLow();

	// open the editor's "Active Tool" window (tool hotkeys are unreachable —
	// engine level — but the tool WINDOW is managed by WindowManager)
	QWidget *toolWindow = activeToolWindow_;
	if (!toolWindow)
		for (QWidget *w : UnigineEditor::WindowManager::allWindows())
			if (w->objectName() == QLatin1String("ActiveToolWidget"))
			{
				toolWindow = w;
				break;
			}
	if (toolWindow)
	{
		UnigineEditor::WindowManager::show(toolWindow);
		UnigineEditor::WindowManager::activate(toolWindow);
	}

	// activate the Texture Paint mode by firing its editor shortcut handler
	// directly (the hotkey itself is handled at the engine level and cannot be
	// synthesized, but the ShortcutManager exposes the shortcut events)
	bool paintActivated = false;
	if (UnigineEditor::ShortcutContextPtr ctx = UnigineEditor::ShortcutManager::getContext("global"))
		if (UnigineEditor::ShortcutPtr sc = ctx->getShortcut("texture_paint_mode"))
		{
			static_cast<Unigine::EventInvoker<> &>(sc->getEventPressed()).run();
			paintActivated = true;
			Unigine::Log::message("Baker: Texture Paint Mode activated\n");
		}
	if (!paintActivated)
		Unigine::Log::warning("Baker: texture_paint_mode shortcut not found, activate the paint mode manually\n");

	// the paint mode hooks the mesh on a selection CHANGE event, but our
	// selection was applied before the mode became active — re-selecting the
	// same node does nothing (identical selection emits no "changed"). Deselect,
	// let the editor fully process the empty state (a synchronous clear+select
	// is coalesced and does not re-hook the tool), then re-select (this is the
	// manual "switch away and back" workaround, automated).
	if (paintActivated)
	{
		QTimer::singleShot(150, this, []() {
			UnigineEditor::Selection::clear();
			UnigineEditor::SelectionAction::refreshSelection();
		});
		QTimer::singleShot(450, this, [selectLow]() { selectLow(); });
	}

	statusLabel_->setText(
		(paintActivated
			? tr2("Маска назначена (%1), режим рисования включён.\n"
				  "Выберите Custom Surface Texture, закрасьте белым проблемные\n"
				  "места и сохраните текстуру.",
				  "Mask assigned (%1), the paint mode is on.\n"
				  "Pick Custom Surface Texture, paint the problem areas white\n"
				  "and save the texture.")
			: tr2("Маска назначена (%1). Нажмите Shift+2 (Texture Paint Mode), выберите\n"
				  "Custom Surface Texture, закрасьте белым проблемные места и сохраните.",
				  "Mask assigned (%1). Press Shift+2 (Texture Paint Mode), pick\n"
				  "Custom Surface Texture, paint the problem areas white and save."))
			.arg(QString::fromUtf8(virtualPath.get())));
}

std::vector<BakeCore::BakeGroup> BakerWindow::buildProbeGroups() const
{
	std::vector<BakeCore::BakeGroup> groups;
	if (manualGroups())
	{
		// each row is already an explicit high/low pair
		for (const GroupRow &row : groupRows_)
		{
			BakeCore::BakeGroup g;
			g.highs = BakeCore::collectMeshes(World::getNodeByID(row.highId));
			g.lows = BakeCore::collectMeshes(World::getNodeByID(row.lowId));
			if (!g.highs.empty() && !g.lows.empty())
				groups.push_back(std::move(g));
		}
		return groups;
	}
	// single pair (and name groups: probing against the whole high-poly set only
	// risks a slightly smaller cage, and the nearest hit is the part's own
	// geometry anyway)
	BakeCore::BakeGroup g;
	for (int id : highIds_)
		if (Ptr<ObjectMeshStatic> mesh = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(id)))
			g.highs.push_back(mesh);
	for (int id : lowIds_)
		if (Ptr<ObjectMeshStatic> mesh = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(id)))
			g.lows.push_back(mesh);
	if (!g.highs.empty() && !g.lows.empty())
		groups.push_back(std::move(g));
	return groups;
}

int BakerWindow::estimateCaptureSurfaces() const
{
	int n = 0;
	for (const BakeCore::BakeGroup &g : buildProbeGroups())
		for (const auto &high : g.highs)
		{
			Ptr<ConstMesh> mesh = high->getMeshForceRAM();
			if (!mesh)
				continue;
			for (int s = 0; s < mesh->getNumSurfaces(); s++)
			{
				if (!BakeCore::isSurfaceBakeable(high, s))
					continue;
				if (mesh->getNumTexCoords0(s) <= 0)
					continue;
				n++;
			}
		}
	return n;
}

int BakerWindow::computeCaptureSize(int surfaceCount, CaptureBudget *outBudget) const
{
	// per surface and per texel in SYSTEM RAM: 3 RGBA8 gbuffer images read back
	// from the GPU + 1 coverage byte
	const double bytesPerTexel = 3.0 * 4.0 + 1.0;
	// ...and in VIDEO memory: one RGBA8 render target per gbuffer image, plus a
	// fourth one when the emission pass is on.
	//
	// The x2 is a deliberate safety factor, not arithmetic. Rendering a surface
	// needs more GPU memory than the targets this plugin reads back: the engine
	// allocates its own depth buffer and intermediate gbuffer for the pass. A
	// field report pins the scale — 112 surfaces at 2048 (5.6 GB by the bare
	// count, on a card with ~9 GB free) reset the device, while 112 at 1024
	// completed. So the bare count understates the real cost by roughly this
	// much, and the estimate has to carry it or auto-sizing walks into the same
	// crash. Replace it with a measurement once the before/after VRAM numbers
	// logged around the capture loop come back from a large bake.
	const bool emission = emissionCheck_ && emissionCheck_->isChecked();
	const double bytesPerTexelVram = (emission ? 4.0 : 3.0) * 4.0 * 2.0;

	CaptureBudget b;
	b.ramFree = availablePhysicalMemory();
	b.vramFree = availableVideoMemory();

	int size = resolutionCombo_ ? resolutionCombo_->currentData().toInt() : 2048;
	size = size < 512 ? 512 : (size > 2048 ? 2048 : size);

	const int forced = captureSizeCombo_ ? captureSizeCombo_->currentData().toInt() : -1;
	b.manual = forced > 0;
	if (forced > 0)
	{
		if (outBudget)
			*outBudget = b;
		return forced;
	}

	// Budget the capture set against what the machine can actually give. A fixed
	// constant is wrong in both directions: it wastes resolution on a big box and
	// thrashes on a small one. Half of the free physical memory leaves room for
	// the bake accumulators, the output images and the editor itself.
	b.ramBudget = qBound(1.0e9, b.ramFree > 0.0 ? b.ramFree * 0.5 : 1.5e9, 16.0e9);
	// Video memory gets its own, tighter share. Overrunning RAM only makes the
	// bake slow; overrunning VRAM resets the device (HRESULT 0x887A0007) and
	// takes the editor down with it, so leave the renderer a wide margin. Free
	// VRAM already excludes the loaded scene, which is what has to keep fitting.
	b.vramBudget = b.vramFree > 0.0 ? b.vramFree * 0.5 : 0.0;

	// System RAM holds every finished capture for the whole bake, so it is what
	// caps the SIZE — the full set has to fit at once and chunking cannot help.
	if (surfaceCount > 0)
	{
		const int maxSize = int(std::sqrt(b.ramBudget / (double(surfaceCount) * bytesPerTexel)));
		if (maxSize < size)
			size = maxSize < 256 ? 256 : maxSize;
	}

	// Video memory, by contrast, only has to hold the captures still in flight,
	// and performCaptures() issues them a chunk at a time. So VRAM sets HOW MANY
	// run together rather than how big they are, and a small GPU costs extra
	// passes instead of quality. It can still force the size down, but only in
	// the one case chunking cannot fix: a single capture that does not fit.
	const double oneCapture = double(size) * double(size) * bytesPerTexelVram;
	if (b.vramBudget > 0.0 && oneCapture > b.vramBudget)
	{
		const int maxSize = int(std::sqrt(b.vramBudget / bytesPerTexelVram));
		size = maxSize < 256 ? 256 : maxSize;
		b.vramLimited = true;
	}
	if (b.vramBudget > 0.0)
	{
		const double fit = b.vramBudget / (double(size) * double(size) * bytesPerTexelVram);
		b.chunk = fit < 1.0 ? 1 : int(fit);
	}
	else
		b.chunk = 16; // no VRAM reading: a conservative fixed chunk
	if (surfaceCount > 0 && b.chunk > surfaceCount)
		b.chunk = surfaceCount;

	if (outBudget)
		*outBudget = b;
	return size;
}

void BakerWindow::updateCaptureInfo()
{
	if (!captureInfoLabel_)
		return;
	// nothing is captured in CPU mode, so the line has nothing to say: hide it
	// instead of leaving a dead row in the window
	if (gpuCheck_ && !gpuCheck_->isChecked())
	{
		captureInfoLabel_->setVisible(false);
		return;
	}
	captureInfoLabel_->setVisible(true);
	const int n = estimateCaptureSurfaces();
	if (n <= 0)
	{
		captureInfoLabel_->setText(
			tr2("Захват: модели не выбраны.", "Capture: no models set."));
		return;
	}
	CaptureBudget b;
	const int size = computeCaptureSize(n, &b);
	const double texels = double(n) * double(size) * double(size);
	const double useGb = texels * 13.0 / 1e9;
	const bool emission = emissionCheck_ && emissionCheck_->isChecked();
	const int chunk = b.chunk > 0 ? b.chunk : 1;
	const double peakVram
		= double(chunk) * double(size) * double(size) * (emission ? 32.0 : 24.0) / 1e9;
	QString text
		= tr2("Захват: %1×%1 × %2 поверхн. ≈ %3 ГБ ОЗУ; по %4 за раз ≈ %5 ГБ видеопамяти "
			  "(%6; свободно: ОЗУ %7 ГБ, видео %8 ГБ)",
			"Capture: %1x%1 x %2 surfaces = %3 GB RAM; %4 at a time = %5 GB VRAM "
			"(%6; free: RAM %7 GB, VRAM %8 GB)")
			  .arg(size)
			  .arg(n)
			  .arg(useGb, 0, 'f', 1)
			  .arg(chunk)
			  .arg(peakVram, 0, 'f', 1)
			  .arg(b.manual ? tr2("вручную", "manual") : tr2("авто", "auto"))
			  .arg(b.ramFree / 1e9, 0, 'f', 1)
			  .arg(b.vramFree / 1e9, 0, 'f', 1);
	// only says this in the one case chunking cannot absorb — a single capture
	// too big for the GPU — so a reduced size never looks unexplained
	if (b.vramLimited)
		text += tr2(" — размер урезан под видеопамять", " - size cut to fit video memory");
	captureInfoLabel_->setText(text);
}

void BakerWindow::resetAllPartCage()
{
	if (partCage_.empty())
		return;
	partCage_.clear();
	refreshMeshTree();
	statusLabel_->setText(tr2("Индивидуальные дистанции сброшены — все детали следуют общим настройкам.",
		"Per-part distances cleared - every part follows the global settings again."));
}

void BakerWindow::onAutoCage()
{
	if (baking_)
		return;
	std::vector<BakeCore::BakeGroup> groups = buildProbeGroups();
	if (groups.empty())
	{
		statusLabel_->setText(tr2("Сначала выберите high-poly и low-poly модели.",
			"Set the high-poly and low-poly models first."));
		return;
	}

	cancelRequested_ = false;
	setUiLocked(true);
	statusLabel_->setText(tr2("Промер кейджа...", "Probing the cage..."));
	QCoreApplication::processEvents();

	// Cancel stops the probe, as it stops a bake.
	auto progress = [this](int percent, const char *text) -> bool {
		progressBar_->setValue(percent);
		statusLabel_->setText(QString::fromUtf8(text));
		QCoreApplication::processEvents();
		return !cancelRequested_;
	};
	// look no further than a generous slice of the model: a probe that reaches
	// across the whole scene would just latch onto unrelated geometry
	float maxProbe = 0.5f;
	{
		using namespace Unigine::Math;
		vec3 mn(1e9f, 1e9f, 1e9f), mx(-1e9f, -1e9f, -1e9f);
		bool any = false;
		for (const BakeCore::BakeGroup &g : groups)
			for (const auto &low : g.lows)
			{
				const auto bb = low->getWorldBoundBox();
				mn = min(mn, vec3(bb.minimum));
				mx = max(mx, vec3(bb.maximum));
				any = true;
			}
		if (any)
			maxProbe = clamp(length(mx - mn) * 0.1f, 0.02f, 2.0f);
	}

	const std::map<int, Unigine::Math::vec2> probed =
		BakeCore::suggestPartCage(groups, maxProbe, progress);

	progressBar_->setValue(0);
	setUiLocked(false);
	if (cancelRequested_)
	{
		cancelRequested_ = false;
		statusLabel_->setText(tr2("Промер отменён — дистанции не изменены.", "Probe cancelled - distances left unchanged."));
		return;
	}
	if (probed.empty())
	{
		statusLabel_->setText(tr2("Промер не нашёл high-poly под деталями — дистанции не изменены.",
			"The probe found no high-poly under the parts - distances left unchanged."));
		return;
	}
	// The global pair is the CEILING: fitting may only tighten a part, never
	// grant it a bigger cage than the one already accepted globally. An axis the
	// probe wants wider is left inherited, so it keeps following the global value.
	const float gFront = float(frontalSpin_->value());
	const float gRear = float(rearSpin_->value());
	int tightened = 0, atCeiling = 0;
	for (const auto &entry : probed)
	{
		PartCage pc = partCage_.count(entry.first) ? partCage_[entry.first] : PartCage();
		bool changed = false;
		if (entry.second.x < gFront)
		{
			pc.hasFrontal = true;
			pc.frontal = entry.second.x;
			changed = true;
		}
		else
			pc.hasFrontal = false;
		if (entry.second.y < gRear)
		{
			pc.hasRear = true;
			pc.rear = entry.second.y;
			changed = true;
		}
		else
			pc.hasRear = false;
		if (pc.any())
			partCage_[entry.first] = pc;
		else
			partCage_.erase(entry.first);
		changed ? tightened++ : atCeiling++;
	}
	refreshMeshTree();
	statusLabel_->setText(
		tr2("Кейдж поджат у %1 детал(и/ей), %2 остались на общем значении (оно потолок).",
			"Cage tightened on %1 part(s), %2 stay at the global value (it is the ceiling).")
			.arg(tightened)
			.arg(atCeiling));
}

void BakerWindow::startBake()
{
	if (baking_)
		return;

	// with every map unchecked the bake would trace the whole model and write
	// nothing, which looks like a failure rather than an empty selection
	if (!albedoCheck_->isChecked() && !shadingCheck_->isChecked() && !normalCheck_->isChecked()
		&& !emissionCheck_->isChecked())
	{
		statusLabel_->setText(
			tr2("Не выбрана ни одна карта.", "No maps selected."));
		return;
	}

	// resolve the bake groups (single mode = one implicit group).
	// baking a mesh onto itself is allowed: it is a useful null-test
	// (the resulting normal map must be perfectly flat)
	std::vector<BakeCore::BakeGroup> bakeGroups;
	if (manualGroups())
	{
		for (size_t i = 0; i < groupRows_.size(); i++)
		{
			BakeCore::BakeGroup g;
			g.highs = BakeCore::collectMeshes(World::getNodeByID(groupRows_[i].highId));
			g.lows = BakeCore::collectMeshes(World::getNodeByID(groupRows_[i].lowId));
			if (g.highs.empty() || g.lows.empty())
			{
				statusLabel_->setText(tr2("В группе %1 не выбраны модели (или нода удалена).",
					"Group %1 has models missing (or a node was deleted).").arg(i + 1));
				return;
			}
			bakeGroups.push_back(std::move(g));
		}
		if (bakeGroups.empty())
		{
			statusLabel_->setText(tr2("Добавьте хотя бы одну группу.", "Add at least one group."));
			return;
		}
	}
	else
	{
		std::vector<Ptr<ObjectMeshStatic>> highs, lows;
		for (int id : highIds_)
			if (Ptr<ObjectMeshStatic> mesh = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(id)))
				highs.push_back(mesh);
		for (int id : lowIds_)
			if (Ptr<ObjectMeshStatic> mesh = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(id)))
				lows.push_back(mesh);
		if (highs.empty())
		{
			statusLabel_->setText(tr2("High-poly модель не выбрана (или нода удалена).",
				"The high-poly model is not set (or the node was deleted)."));
			return;
		}
		if (lows.empty())
		{
			statusLabel_->setText(tr2("Low-poly модель не выбрана (или нода удалена).",
				"The low-poly model is not set (or the node was deleted)."));
			return;
		}

		if (nameGroups())
		{
			// pair the parts by normalized names; group order follows the lows
			std::vector<QString> keys;
			auto findKey = [&keys](const QString &k) -> int {
				for (size_t i = 0; i < keys.size(); i++)
					if (keys[i] == k)
						return int(i);
				return -1;
			};
			for (const auto &low : lows)
			{
				const QString k = bakeGroupKey(low->getName());
				int i = findKey(k);
				if (i < 0)
				{
					keys.push_back(k);
					bakeGroups.push_back(BakeCore::BakeGroup());
					i = int(keys.size()) - 1;
				}
				bakeGroups[i].lows.push_back(low);
			}
			QStringList unmatchedHighs;
			for (const auto &high : highs)
			{
				const int i = findKey(bakeGroupKey(high->getName()));
				if (i >= 0)
					bakeGroups[i].highs.push_back(high);
				else
					unmatchedHighs << QString::fromUtf8(high->getName());
			}
			QStringList unmatchedLows;
			for (size_t i = 0; i < bakeGroups.size(); i++)
				if (bakeGroups[i].highs.empty())
					for (const auto &low : bakeGroups[i].lows)
						unmatchedLows << QString::fromUtf8(low->getName());
			if (!unmatchedLows.isEmpty())
			{
				statusLabel_->setText(tr2("Нет high-пары по имени для: %1",
					"No high match by name for: %1").arg(unmatchedLows.join(", ")));
				return;
			}
			if (!unmatchedHighs.isEmpty())
				Unigine::Log::warning("Baker: high parts with no low match by name (skipped): %s\n",
					unmatchedHighs.join(", ").toUtf8().constData());
			for (size_t i = 0; i < bakeGroups.size(); i++)
				Unigine::Log::message("Baker: name group \"%s\": %d high, %d low\n",
					keys[i].toUtf8().constData(),
					int(bakeGroups[i].highs.size()), int(bakeGroups[i].lows.size()));
		}
		else
		{
			BakeCore::BakeGroup g;
			g.highs = std::move(highs);
			g.lows = std::move(lows);
			bakeGroups.push_back(std::move(g));
		}
	}

	bakeGroupIds_.clear();
	for (const BakeCore::BakeGroup &g : bakeGroups)
	{
		IdGroup ids;
		for (const auto &obj : g.highs)
			ids.highIds.push_back(obj->getID());
		for (const auto &obj : g.lows)
			ids.lowIds.push_back(obj->getID());
		bakeGroupIds_.push_back(std::move(ids));
	}

	saveSettings();
	settings_ = BakeCore::Settings();
	settings_.resolution = resolutionCombo_->currentData().toInt();
	settings_.frontalDistance = float(frontalSpin_->value());
	settings_.rearDistance = float(rearSpin_->value());
	// resolve per-axis inheritance into the concrete pairs the core expects
	settings_.partCage.clear();
	for (const auto &entry : partCage_)
	{
		const PartCage &pc = entry.second;
		const Unigine::Math::vec2 v(
			pc.hasFrontal ? pc.frontal : settings_.frontalDistance,
			pc.hasRear ? pc.rear : settings_.rearDistance);
		settings_.partCage[entry.first] = v;
		if (NodePtr node = World::getNodeByID(entry.first))
			Unigine::Log::message("Baker: part cage \"%s\": frontal=%.4f%s rear=%.4f%s\n",
				node->getName(), v.x, pc.hasFrontal ? "" : " (global)", v.y,
				pc.hasRear ? "" : " (global)");
	}
	settings_.supersamples = samplesCombo_->currentData().toInt();
	settings_.flipNormalY = flipYCheck_->isChecked();
	settings_.gpuMode = gpuCheck_->isChecked();
	captureUVMode_ = captureUVCombo_->currentData().toInt();
	settings_.debugZones = debugZonesCheck_->isChecked();
	settings_.raysAlongShading = shadingRaysCheck_->isChecked();
	settings_.useSkewMask = skewMaskCheck_->isChecked();
	settings_.bakeAlbedo = albedoCheck_->isChecked();
	settings_.bakeShading = shadingCheck_->isChecked();
	settings_.bakeNormal = normalCheck_->isChecked();
	settings_.bakeEmission = emissionCheck_->isChecked();
	settings_.bakeDecals = decalsCheck_->isChecked();
	settings_.decalNodeIds = decalsCheck_->isChecked() ? decalIds_ : std::vector<int>();
	settings_.decalDistance = float(decalDistanceSpin_->value()) * 0.01f; // cm -> m
	settings_.outputName = nameEdit_->text().trimmed().toUtf8().constData();

	baking_ = true;
	cancelRequested_ = false;
	setUiLocked(true);
	progressBar_->setValue(0);
	progressBase_ = 0; // CPU mode: ray tracing owns the whole bar

	pending_.clear();
	captures_.clear();

	// GPU mode phase 1: renders and readback requests must run at a safe point
	// of the engine frame (a Qt slot can fire mid-render: "frame is already
	// rendering"). Subscribe to the main window's end-of-render event, do the
	// captures there, then poll for readback completion with a timer.
	if (settings_.gpuMode)
	{
		// flat surface numbering spans all groups and objects in order —
		// must match BakeCore's numbering of srcSurfaces
		captureItems_.clear();
		int flatBase = 0;
		for (const BakeCore::BakeGroup &g : bakeGroups)
		for (const auto &obj : g.highs)
		{
			Ptr<ConstMesh> highMesh = obj->getMeshForceRAM();
			if (!highMesh)
				continue;
			for (int s = 0; s < highMesh->getNumSurfaces(); s++)
			{
				if (!BakeCore::isSurfaceBakeable(obj, s))
					continue;
				if (highMesh->getNumTexCoords0(s) <= 0)
					continue;
				captureItems_.push_back({obj->getID(), s, flatBase + s});
			}
			flatBase += highMesh->getNumSurfaces();
		}
		if (!captureItems_.empty())
		{
			// Every captured surface holds 3 images (albedo/shading/normal) at
			// captureSize² AND a GPU texture set of the same size in flight, so
			// the size is budgeted against free RAM *and* free video memory (or
			// forced in the Debug tab). One shared policy with the label under
			// the window, so what the user reads there is exactly what the bake
			// uses.
			{
				const int n = int(captureItems_.size());
				CaptureBudget b;
				captureSize_ = computeCaptureSize(n, &b);
				captureChunk_ = b.chunk > 0 ? b.chunk : 1;
				captureCursor_ = 0;
				progressBase_ = 10;
				// sized for the whole bake up front; the chunks fill it in place
				int maxFlat = -1;
				for (const CaptureItem &it : captureItems_)
					maxFlat = std::max(maxFlat, it.flatIndex);
				captures_.assign(size_t(maxFlat) + 1, BakeGpu::SurfaceCapture());
				const double texels = double(n) * double(captureSize_) * double(captureSize_);
				const double perTexelVram = settings_.bakeEmission ? 32.0 : 24.0;
				Unigine::Log::message(
					"Baker: %d surfaces to capture, capture size = %d (%s%s, free RAM %.1f GB "
					"-> budget %.1f GB, free VRAM %.1f GB -> budget %.1f GB), %.1f GB RAM for "
					"the whole set, %d capture(s) per chunk = %.2f GB VRAM peak\n",
					n, captureSize_, b.manual ? "manual" : "auto",
					b.vramLimited ? ", size cut to fit VRAM" : "", b.ramFree / 1e9,
					b.ramBudget / 1e9, b.vramFree / 1e9, b.vramBudget / 1e9, texels * 13.0 / 1e9,
					captureChunk_,
					double(captureChunk_) * double(captureSize_) * double(captureSize_)
						* perTexelVram / 1e9);
			}
			captureStarted_ = false;
			captureKicked_ = false;
			warmupDone_ = false;
			engineConns_.disconnectAll();
			// end of the engine UPDATE phase: no frame is being rendered, the
			// classic point where samples do render-to-texture
			Engine::get()->getEventEndUpdate().connect(engineConns_, [this]() { performCaptures(); });
			statusLabel_->setText(tr2("Захват материалов (GPU)...", "Capturing materials (GPU)..."));
			captureDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(25);
			captureTimer_->start(50);
			return; // continues in performCaptures() + onCaptureTick()
		}
	}

	runBake();
}

// runs at the end of the engine update phase — a safe moment
// for our own off-screen renders and readback requests
void BakerWindow::performCaptures()
{
	// the captures take seconds and Qt timers can fire re-entrantly meanwhile,
	// so guard the entry and signal completion (captureKicked_) only at the end
	if (captureStarted_)
		return;

	// cold start protection: on the first bake after editor launch the material
	// textures may not be streamed in yet and the shaders may still be compiling
	// asynchronously — a capture then grabs placeholder data (broken normals).
	// Pass 1 renders every surface once (discarding the results) to trigger the
	// loading, then the real captures run after the resources settle.
	if (!warmupDone_)
	{
		const bool prevForceStreaming = Render::isForceStreaming();
		const Render::SHADERS_COMPILE_MODE prevCompileMode = Render::getShadersCompileMode();
		Render::setForceStreaming(true);
		Render::setShadersCompileMode(Render::SHADERS_COMPILE_MODE_FORCE);
		// Streaming and shader compilation do not care how big the target is, so
		// warm up at a small size: at the real capture size this pass allocates a
		// render target set for EVERY surface before a single frame is drawn to
		// release them, which is a memory spike as bad as the captures it exists
		// to protect.
		const int warmupSize = captureSize_ < 256 ? captureSize_ : 256;
		for (const CaptureItem &item : captureItems_)
		{
			Ptr<ObjectMeshStatic> obj = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(item.objectId));
			Ptr<ConstMesh> mesh = obj ? obj->getMeshForceRAM() : Ptr<ConstMesh>();
			if (mesh)
				BakeGpu::requestSurfaceCapture(obj, mesh, item.surface, warmupSize, captureUVMode_);
		}
		Render::setShadersCompileMode(prevCompileMode);
		Render::setForceStreaming(prevForceStreaming);
		warmupDone_ = true;
		warmupTime_ = std::chrono::steady_clock::now();
		return; // stay subscribed; the real captures run on a later frame
	}
	// give texture streaming and shader compilation time to finish
	if (std::chrono::steady_clock::now() - warmupTime_ < std::chrono::milliseconds(1200))
		return;

	captureStarted_ = true;

	// Measure what the capture set actually costs on the GPU. computeCaptureSize()
	// can only estimate it, and the estimate is what stands between a big scene
	// and a device reset — these two numbers are how the estimate gets corrected.
	const double vramBefore = double(Unigine::SystemInfo::getGpuVRamUsage());

	const bool prevForceStreaming = Render::isForceStreaming();
	const Render::SHADERS_COMPILE_MODE prevCompileMode = Render::getShadersCompileMode();
	Render::setForceStreaming(true);
	Render::setShadersCompileMode(Render::SHADERS_COMPILE_MODE_FORCE);

	// one chunk only: the rest is issued after this one has been collected and
	// its GPU memory released (see onCaptureTick)
	const size_t chunkEnd
		= std::min(captureItems_.size(), captureCursor_ + size_t(captureChunk_ > 0 ? captureChunk_ : 1));
	const size_t chunkBegin = captureCursor_;
	for (size_t i = chunkBegin; i < chunkEnd; i++)
	{
		const CaptureItem &item = captureItems_[i];
		Ptr<ObjectMeshStatic> obj = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(item.objectId));
		Ptr<ConstMesh> mesh = obj ? obj->getMeshForceRAM() : Ptr<ConstMesh>();
		if (!mesh)
			continue;
		BakeGpu::PendingCapturePtr p = BakeGpu::requestSurfaceCapture(obj, mesh, item.surface,
			captureSize_, captureUVMode_, settings_.bakeEmission);
		if (p)
		{
			p->surface = item.flatIndex; // capture list index = BakeCore's flat surface index
			pending_.push_back(p);
		}
		else
			Unigine::Log::warning("Baker: GPU capture request failed for \"%s\" surface %d\n",
				obj->getName(), item.surface);
	}
	captureCursor_ = chunkEnd;
	// each render's swap delivers the previous render's readbacks;
	// a final dummy render flushes the last one
	if (chunkEnd > chunkBegin)
		BakeGpu::flushTransfers();

	Render::setShadersCompileMode(prevCompileMode);
	Render::setForceStreaming(prevForceStreaming);

	{
		const double used = double(Unigine::SystemInfo::getGpuVRamUsage()) - vramBefore;
		const double texels
			= double(pending_.size()) * double(captureSize_) * double(captureSize_);
		Unigine::Log::message(
			"Baker: chunk %d-%d of %d in flight at %d, VRAM usage +%.2f GB (%.1f bytes/texel "
			"measured), %.2f GB still free\n",
			int(chunkBegin), int(chunkEnd) - 1, int(captureItems_.size()), captureSize_,
			used / 1e9, texels > 0.0 ? used / texels : 0.0,
			double(Unigine::SystemInfo::getGpuVRamFree()) / 1e9);
	}

	captureKicked_ = true;
}

void BakerWindow::abortCapture(const QString &reason)
{
	engineConns_.disconnectAll();
	captureTimer_->stop();
	pending_.clear();
	baking_ = false;
	setUiLocked(false);
	// The readbacks already requested are taken in, so that the plugin is free again.
	if (BakeGpu::transfersInFlight() > 0)
		BakeGpu::flushTransfers();
	update_busy();
	progressBar_->setValue(0);
	statusLabel_->setText(reason);
}

void BakerWindow::onCaptureTick()
{
	if (cancelRequested_)
	{
		engineConns_.disconnectAll();
		abortCapture(tr2("Запекание отменено.", "Baking cancelled."));
		return;
	}

	const bool deadlinePassed = std::chrono::steady_clock::now() >= captureDeadline_;

	// still waiting for the end-of-render event to fire
	if (!captureKicked_)
	{
		// Before the first chunk this covers the warm-up pass, which renders
		// every surface once to trigger texture streaming and shader
		// compilation — seconds of apparent silence on a big model. Between
		// chunks the bar already stands where the last chunk left it, so it is
		// left alone rather than reset.
		if (captureCursor_ == 0 && progressBase_ > 0)
			statusLabel_->setText(tr2("Подготовка: стриминг текстур и компиляция шейдеров...",
				"Warming up: texture streaming and shader compilation..."));
		if (deadlinePassed)
		{
			engineConns_.disconnectAll();
			captureTimer_->stop();
			Unigine::Log::warning("Baker: end-of-update event never fired, GPU captures skipped\n");
			runBake();
		}
		return;
	}
	engineConns_.disconnectAll(); // one-shot: captures already requested

	int readyCount = 0;
	for (const auto &p : pending_)
		if (p->ready())
			readyCount++;
	const bool allReady = readyCount == int(pending_.size());

	// Move the bar on every tick, not once per chunk: within a chunk the
	// readbacks land one by one, and a bar that only steps between chunks still
	// looks stuck on a model with few, large chunks.
	if (!captureItems_.empty() && progressBase_ > 0)
	{
		const int collected = int(captureCursor_) - int(pending_.size()) + readyCount;
		progressBar_->setValue(collected * progressBase_ / int(captureItems_.size()));
		statusLabel_->setText(tr2("Захват материалов (GPU): %1 из %2...",
			"Capturing materials (GPU): %1 of %2...")
								  .arg(collected)
								  .arg(captureItems_.size()));
	}

	if (!allReady && !deadlinePassed)
		return;

	captureTimer_->stop();

	// captures_ spans every surface of the bake and is filled across chunks, so
	// it is grown here rather than re-assigned — assigning per chunk would drop
	// everything the earlier chunks collected
	int maxSurface = -1;
	for (const auto &p : pending_)
		maxSurface = std::max(maxSurface, p->surface);
	if (int(captures_.size()) < maxSurface + 1)
		captures_.resize(maxSurface + 1);

	int ready = 0, timedOut = 0;
	for (const auto &p : pending_)
	{
		if (p->ready())
		{
			captures_[p->surface] = BakeGpu::finishCapture(p);
			ready++;
		}
		else
		{
			Unigine::Log::warning("Baker: GPU readback timed out for surface %d\n", p->surface);
			timedOut++;
		}
	}
	Unigine::Log::message("Baker: captures ready=%d, timed out=%d (timed-out surfaces bake from "
						  "the base material on the CPU)\n",
		ready, timedOut);
	// releases this chunk's GPU targets — the whole point of chunking, and it
	// has to happen before the next chunk is requested
	pending_.clear();

	// more surfaces left: hand control back to the engine so the released
	// targets are actually recycled, then issue the next chunk the same way the
	// first one was issued
	if (captureCursor_ < captureItems_.size())
	{
		captureStarted_ = false;
		captureKicked_ = false;
		// the status text and the bar are updated on every tick above
		engineConns_.disconnectAll();
		Engine::get()->getEventEndUpdate().connect(engineConns_, [this]() { performCaptures(); });
		// same allowance the first chunk gets, and a chunk is smaller than the
		// whole set used to be
		captureDeadline_ = std::chrono::steady_clock::now() + std::chrono::seconds(25);
		captureTimer_->start();
		return;
	}

	// diagnostic dump of the capture atlases (shares the debug zones checkbox)
	if (debugZonesCheck_->isChecked())
	{
		const QString dir = QDir::tempPath() + "/baker_captures";
		QDir().mkpath(dir);
		for (int s = 0; s < int(captures_.size()); s++)
		{
			const BakeGpu::SurfaceCapture &c = captures_[s];
			if (!c.valid())
				continue;
			c.albedo->save(QString("%1/baker_gpu_s%2_alb.png").arg(dir).arg(s).toUtf8().constData());
			c.shading->save(QString("%1/baker_gpu_s%2_sh.png").arg(dir).arg(s).toUtf8().constData());
			c.normal->save(QString("%1/baker_gpu_s%2_n.png").arg(dir).arg(s).toUtf8().constData());
			if (c.emission)
				c.emission->save(QString("%1/baker_gpu_s%2_e.png").arg(dir).arg(s).toUtf8().constData());
		}
		Unigine::Log::message("Baker: capture atlases dumped to %s\n", dir.toUtf8().constData());
	}

	runBake();
}

void BakerWindow::runBake()
{
	// Busy until the very end, whatever way it returns.
	struct Finishing
	{
		BakerWindow *window;
		explicit Finishing(BakerWindow *w) : window(w)
		{
			window->finishing_ = true;
			window->setProperty("artist_tool_busy", true);
		}
		~Finishing()
		{
			window->finishing_ = false;
			window->update_busy();
		}
	} finishing(this);

	// re-resolve everything by ID: nodes may have been deleted while the GPU
	// captures were in flight
	std::vector<BakeCore::BakeGroup> groups;
	for (const IdGroup &ids : bakeGroupIds_)
	{
		BakeCore::BakeGroup g;
		for (int id : ids.highIds)
			if (Ptr<ObjectMeshStatic> obj = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(id)))
				g.highs.push_back(obj);
		for (int id : ids.lowIds)
			if (Ptr<ObjectMeshStatic> obj = checked_ptr_cast<ObjectMeshStatic>(World::getNodeByID(id)))
				g.lows.push_back(obj);
		if (g.highs.size() != ids.highIds.size() || g.lows.size() != ids.lowIds.size())
		{
			abortCapture(tr2("Нода удалена во время запекания.", "A node was deleted during baking."));
			return;
		}
		groups.push_back(std::move(g));
	}

	settings_.gpuCaptures = &captures_;

	auto progress = [this](int percent, const char *text) -> bool {
		// the capture phase already filled the first progressBase_ percent
		progressBar_->setValue(progressBase_ + percent * (100 - progressBase_) / 100);
		statusLabel_->setText(QString::fromUtf8(text));
		QCoreApplication::processEvents();
		return !cancelRequested_;
	};

	BakeCore::Result result = BakeCore::bake(groups, settings_, progress);
	settings_.gpuCaptures = nullptr;
	captures_.clear();

	baking_ = false;
	setUiLocked(false);

	if (result.cancelled)
	{
		progressBar_->setValue(0);
		statusLabel_->setText(tr2("Запекание отменено.", "Baking cancelled."));
		return;
	}
	if (!result.success)
	{
		progressBar_->setValue(0);
		statusLabel_->setText(tr2("Ошибка: %1", "Error: %1").arg(QString::fromUtf8(result.error.get())));
		QMessageBox::warning(this, "Texture Baker", statusLabel_->text());
		return;
	}

	// import the textures BEFORE assigning them to the material: a material
	// referencing a not-yet-imported file binds it raw, bypassing the import
	// pipeline (normal maps render broken until reimported). New files need
	// importAssetSync — reimport fails on unknown assets.
	statusLabel_->setText(tr2("Импорт текстур...", "Importing textures..."));
	QCoreApplication::processEvents();
	for (const Unigine::String &path : {result.albedoPath, result.shadingPath, result.normalPath, result.emissionPath})
	{
		if (path.empty())
			continue;
		const bool wasAsset = UnigineEditor::AssetManager::isAsset(path.get());
		const bool imported = wasAsset
			? UnigineEditor::AssetManager::reimportAssetSync(path.get())
			: UnigineEditor::AssetManager::importAssetSync(path.get());
		if (!imported)
			Unigine::Log::warning("Baker: %s failed for \"%s\"\n",
				wasAsset ? "reimport" : "import", path.get());
	}

	statusLabel_->setText(tr2("Назначение материала...", "Assigning the material..."));
	QCoreApplication::processEvents();
	BakeCore::assignMaterial(groups, result);

	QString report = tr2("Готово. Сохранено:", "Done. Saved:");
	if (!result.albedoPath.empty())
		report += QString("\n  %1").arg(QString::fromUtf8(result.albedoPath.get()));
	if (!result.shadingPath.empty())
		report += QString("\n  %1").arg(QString::fromUtf8(result.shadingPath.get()));
	if (!result.normalPath.empty())
		report += QString("\n  %1").arg(QString::fromUtf8(result.normalPath.get()));
	if (!result.emissionPath.empty())
		report += QString("\n  %1").arg(QString::fromUtf8(result.emissionPath.get()));
	if (!result.materialPath.empty())
		report += tr2("\nМатериал: %1", "\nMaterial: %1").arg(QString::fromUtf8(result.materialPath.get()));
	statusLabel_->setText(report);
}
