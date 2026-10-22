#pragma once

#include "BakeCore.h"

#include <UnigineEvent.h>

#include <QPointer>
#include <QWidget>

#include <chrono>
#include <functional>
#include <set>
#include <vector>

class QTimer;
class QToolButton;
class QCheckBox;
class QComboBox;
class QDoubleSpinBox;
class QFormLayout;
class QGroupBox;
class QLabel;
class QLineEdit;
class QMimeData;
class QProgressBar;
class QPushButton;
class QTreeWidget;
class QTreeWidgetItem;
class QVBoxLayout;
class QWidget;

class BakerWindow : public QWidget
{
	Q_OBJECT
	Q_DISABLE_COPY(BakerWindow)
public:
	explicit BakerWindow(QWidget *parent = nullptr);
	~BakerWindow() override;

	// The rarely touched switches of the bake, for the Debug tab of Artist Tool.
	QWidget *takeDebugControls();
	// Cancels a bake or a probe that is running; called before the plugin unloads.
	void stop();
	// Every setting back to its default (Reset All Plugin Settings); not while it bakes.
	void resetSettings();

protected:
	// drop target for nodes dragged from the World Nodes hierarchy
	bool eventFilter(QObject *watched, QEvent *event) override;

private slots:
	void pickHighFromSelection();
	void pickLowFromSelection();
	void startBake();
	void cancelBake();
	void onCaptureTick();
	void onCreateSkewMask();
	// probes every low-poly part and fills the per-part cage with the smallest
	// distances that still catch its high-poly detail
	void onAutoCage();

private:
	int grabSelectedMeshGroupId(QLabel *label);
	void addSlotNodes(bool high, const std::vector<int> &rootIds);
	void clearSlot(bool high);
	void updateSlotLabels();
	void setSlotHidden(bool high, bool hidden);
	std::vector<int> decodeNodeDrop(const QMimeData *mime) const;
	void refreshMeshTree();
	void updateSuggestedName();
	void addGroupRow();
	void removeGroupRow(QGroupBox *box);
	void renumberGroups();
	void updateModelsMode();
	int firstLowId() const;
	void setUiLocked(bool locked);
	// UI language follows the editor translation settings: tr2 = labels and
	// status texts, tip2 = tooltips (translated even in "tooltips only" mode)
	QString tr2(const char *ru, const char *en) const;
	QString tip2(const char *ru, const char *en) const;
	void saveSettings() const;
	void restoreSettings();
	void runBake();
	void abortCapture(const QString &reason);

	void performCaptures();

	// two-phase GPU bake: renders+readback requests run at the engine window's
	// end-of-render event (a safe point), then a timer polls until images arrive
	BakeCore::Settings settings_;
	// one entry per surface to capture; flatIndex matches BakeCore's
	// consecutive surface numbering over all high-poly objects
	struct CaptureItem
	{
		int objectId = 0;
		int surface = -1;
		int flatIndex = -1;
	};
	std::vector<CaptureItem> captureItems_;
	// resolved object IDs for the current bake, one entry per bake group
	// (single mode = one implicit group)
	struct IdGroup
	{
		std::vector<int> highIds;
		std::vector<int> lowIds;
	};
	std::vector<IdGroup> bakeGroupIds_;
	std::vector<BakeGpu::PendingCapturePtr> pending_;
	std::vector<BakeGpu::SurfaceCapture> captures_;
	// Captures run in CHUNKS: every pending capture holds its render targets
	// resident on the GPU until the readback lands, so issuing all of them at
	// once makes peak video memory grow with the surface count — which is what
	// resets the device on large models. A chunk is collected and released
	// before the next one is issued, so the peak is bounded by captureChunk_
	// and no longer depends on how many surfaces the model has.
	size_t captureCursor_ = 0; // next entry of captureItems_ to request
	int captureChunk_ = 0;     // max captures in flight at once
	// Share of the progress bar the capture phase owns, in percent. Capturing a
	// big model takes many chunks and used to leave the bar sitting at zero the
	// whole time, which reads as a hang; the ray tracing that follows is then
	// mapped onto the remaining span.
	int progressBase_ = 0;
	bool captureStarted_ = false;
	bool captureKicked_ = false;
	// warm-up render pass before the real captures: triggers texture streaming
	// and shader compilation, then waits for them to settle
	bool warmupDone_ = false;
	std::chrono::steady_clock::time_point warmupTime_;
	int captureSize_ = 2048;
	int captureUVMode_ = -1; // -1 auto, 0/1 force UV channel
	Unigine::EventConnections engineConns_;
	QTimer *captureTimer_ = nullptr;
	std::chrono::steady_clock::time_point captureDeadline_;

	// individual Static Mesh node IDs of the two slots (picking/dropping a
	// parent node expands it into its meshes); resolved by ID at bake time
	std::vector<int> highIds_;
	std::vector<int> lowIds_;
	// decal node IDs gathered from the high-poly hierarchy (projected onto the bake)
	std::vector<int> decalIds_;

	// EXPLICIT per-part cage overrides, keyed by low-poly node id. Each axis is
	// independent: an unset axis follows the global spinbox, so changing the
	// global value keeps hand-tuned axes untouched and still updates the rest.
	struct PartCage
	{
		bool hasFrontal = false;
		float frontal = 0.05f;
		bool hasRear = false;
		float rear = 0.05f;
		bool any() const { return hasFrontal || hasRear; }
	};
	std::map<int, PartCage> partCage_;
	// guards the tree's itemChanged handler while refreshMeshTree() fills cells
	bool treeUpdating_ = false;
	// Collapsed sections, by key ("high", "low", "decals", "g0hi", "g0lo", ...):
	// refreshMeshTree() rebuilds the items from scratch, so the state has to live
	// outside them, and groups mode has a variable number of sections.
	std::set<QString> collapsedSections_;
	// bake groups as used for a cage probe (no error plumbing: probing what can
	// be resolved is enough)
	std::vector<BakeCore::BakeGroup> buildProbeGroups() const;
	// writes one low-poly row's cage cells: inherited values are dimmed, explicit
	// ones bright, so it is visible at a glance which parts were tuned by hand
	void fillCageCells(QTreeWidgetItem *item, int nodeId);
	void resetAllPartCage();
	// Visits the gathered decals by WALKING the high-poly hierarchy instead of
	// looking them up by id: decals nested inside a Node Reference are not
	// reachable through World::getNodeByID, which is why the bake walks the tree
	// too. Only nodes already listed in decalIds_ are reported.
	// The second argument is the Node Reference the decal lives inside, or null
	// when it sits in the world directly — the engine renders a reference's
	// content regardless of the inner node's own enabled flag, so hiding such a
	// decal means toggling the reference.
	void forEachHighDecal(
		const std::function<void(const Unigine::NodePtr &, const Unigine::NodePtr &)> &fn) const;
	// Every node of the high-poly hierarchy, descending into Node Reference
	// contents (which World::getNodeByID cannot reach).
	void forEachHighHierarchyNode(const std::function<void(const Unigine::NodePtr &)> &fn) const;

	// how many high-poly surfaces a bake would capture with the current selection
	int estimateCaptureSurfaces() const;
	// What the capture-size decision was based on, for the label under the window
	// and for the bake log.
	struct CaptureBudget
	{
		double ramFree = 0.0;     // bytes reported free by the OS
		double ramBudget = 0.0;   // bytes the capture set may occupy in system RAM
		double vramFree = 0.0;    // bytes; 0 when the engine reported nothing
		double vramBudget = 0.0;  // bytes the capture set may occupy in video memory
		int chunk = 0;            // how many captures may be in flight at once
		bool manual = false;      // size forced in the Debug tab, no budgeting done
		bool vramLimited = false; // a single capture did not fit, so the size was cut
	};
	// The single place the capture-size policy lives, so the label under the
	// window and the bake itself can never disagree. Returns the size in texels
	// and, optionally, the numbers behind it.
	int computeCaptureSize(int surfaceCount, CaptureBudget *outBudget = nullptr) const;
	void updateCaptureInfo();

	bool baking_ = false;
	bool cancelRequested_ = false;
	// runBake() is running: the window stays busy after the UI is unlocked.
	bool finishing_ = false;
	bool uiLocked_ = false;
	// Takes in the readbacks still on their way after a bake (see update_busy()).
	QTimer *drainTimer_ = nullptr;
	int drainTicks_ = 0;
	void update_busy();
	void drain_transfers();
	bool ruUi_ = false;   // labels/status in Russian
	bool ruTips_ = false; // tooltips in Russian

	QLabel *highLabel_ = nullptr;
	QLabel *lowLabel_ = nullptr;
	QPushButton *highPickButton_ = nullptr;
	QPushButton *lowPickButton_ = nullptr;
	// viewport-only visibility toggles (checkable); baking ignores hidden state
	QPushButton *highHideButton_ = nullptr;
	QPushButton *lowHideButton_ = nullptr;
	// bake groups (optional): each group's low parts trace only its high parts
	struct GroupRow
	{
		int highId = 0;
		int lowId = 0;
		QGroupBox *box = nullptr;
		QLabel *highLabel = nullptr;
		QLabel *lowLabel = nullptr;
		// same viewport-only toggles the single-mode slots have
		QPushButton *hideHighButton = nullptr;
		QPushButton *hideLowButton = nullptr;
	};
	std::vector<GroupRow> groupRows_;
	// group mode combo: 0 = off (single pair), 1 = manual groups, 2 = by name
	QComboBox *groupsModeCombo_ = nullptr;
	bool manualGroups() const;
	bool nameGroups() const;
	QWidget *groupsPanel_ = nullptr;
	QVBoxLayout *groupsLayout_ = nullptr;
	QPushButton *addGroupButton_ = nullptr;
	// the single pair (high-poly and low-poly rows), the debug switches, the row of the
	// distance of the decals
	QWidget *singlePanel_ = nullptr;
	QPointer<QWidget> debugPanel_;
	QWidget *decalDistanceRow_ = nullptr;
	QTreeWidget *meshTree_ = nullptr; // striped high/low participants list, drop target
	QLineEdit *nameEdit_ = nullptr;   // output name for textures/material
	QString lastAutoName_;            // last auto-suggested name (user edits win)
	// the editor's "active tool" window (objectName "active_tool_window"),
	// caught via WindowManager::windowShown — used to open the paint tool
	QPointer<QWidget> activeToolWindow_;
	QComboBox *resolutionCombo_ = nullptr;
	QDoubleSpinBox *frontalSpin_ = nullptr;
	QDoubleSpinBox *rearSpin_ = nullptr;
	QComboBox *samplesCombo_ = nullptr;
	QCheckBox *gpuCheck_ = nullptr;
	QComboBox *captureUVCombo_ = nullptr;
	QComboBox *captureSizeCombo_ = nullptr; // auto / 512 / 1024 / 2048
	QLabel *captureInfoLabel_ = nullptr;    // resolved capture size + RAM estimate
	QCheckBox *flipYCheck_ = nullptr;
	QCheckBox *debugZonesCheck_ = nullptr;
	QCheckBox *shadingRaysCheck_ = nullptr;
	QCheckBox *skewMaskCheck_ = nullptr;
	QPushButton *skewMaskButton_ = nullptr;
	QLabel *skewMaskHelp_ = nullptr;
	// per-map write switches; an unchecked map keeps the texture it already has
	QCheckBox *albedoCheck_ = nullptr;
	QCheckBox *shadingCheck_ = nullptr;
	QCheckBox *normalCheck_ = nullptr;
	QCheckBox *emissionCheck_ = nullptr;
	QCheckBox *decalsCheck_ = nullptr;
	QDoubleSpinBox *decalDistanceSpin_ = nullptr;
	QPushButton *autoCageButton_ = nullptr;
	QPushButton *resetCageButton_ = nullptr;
	QPushButton *bakeButton_ = nullptr;
	QPushButton *cancelButton_ = nullptr;
	QProgressBar *progressBar_ = nullptr;
	QLabel *statusLabel_ = nullptr;
};
