#include "ObjectPlacer.h"
#include "Localization.h"
#include "PlaceableAsset.h"
#include "ViewportPicking.h"

#include <editor/UnigineActions.h>
#include <editor/UnigineAssetManager.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>
#include <editor/UnigineUndo.h>
#include <editor/UnigineViewportManager.h>

#include <UnigineEngine.h>
#include <UnigineFileSystem.h>
#include <UnigineLog.h>
#include <UnigineNodes.h>
#include <UnigineObjects.h>
#include <UniginePlayers.h>
#include <UniginePrimitives.h>
#include <UnigineWorld.h>

#include <QApplication>
#include <QCursor>
#include <QFileInfo>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QRandomGenerator>
#include <QStringList>
#include <QWidget>

#include <cmath>
#include <cstring>

using namespace Unigine;
using ::UnigineEditor::AssetManager;
using ::UnigineEditor::ViewportManager;
using ::UnigineEditor::ViewportWindowPtr;

namespace ArtistTool
{

namespace
{

const char CURSOR_NODE_NAME[] = "artist_tool_placer_cursor";

// How long the button must be held before the placed object starts to follow the
// mouse. An ordinary click lasts 80-150 ms.
const int DRAG_DELAY_MS = 200;

// How far one notch of the mouse wheel turns the held object around its up axis, in
// degrees.
const float WHEEL_TURN = 5.0f;

// The surface under the world ray p0 -> p1 for this tool: its filter applies, and its
// own cursor disc and the object being moved by the mouse are not surfaces.
ObjectPlacer::Hit raycast(const Math::Vec3 &p0, const Math::Vec3 &p1, const PlacerOptions &options,
	const NodePtr &cursor, const NodePtr &dragged)
{
	SurfaceFilter filter;
	filter.surface_types = options.surface_types;
	filter.only_immovable = options.only_immovable;
	filter.only_intersection = options.only_intersection;

	Vector<NodePtr> ignore;
	if (cursor)
		ignore.append(cursor);
	if (dragged)
		ignore.append(dragged);

	return raycastSurface(p0, p1, filter, ignore);
}

// Rotation that turns the chosen axis of the asset to +Z.
Math::mat4 up_axis_rotation(PlacerOptions::UpAxis axis)
{
	switch (axis)
	{
		case PlacerOptions::UP_AXIS_Z_NEGATIVE: return Math::rotateX(180.0f);
		case PlacerOptions::UP_AXIS_X_POSITIVE: return Math::rotateY(-90.0f);
		case PlacerOptions::UP_AXIS_X_NEGATIVE: return Math::rotateY(90.0f);
		case PlacerOptions::UP_AXIS_Y_POSITIVE: return Math::rotateX(90.0f);
		case PlacerOptions::UP_AXIS_Y_NEGATIVE: return Math::rotateX(-90.0f);
		default:                                return Math::mat4_identity;
	}
}

float random_range(float from, float to)
{
	return from + (to - from) * float(QRandomGenerator::global()->generateDouble());
}

Math::vec3 random_vec3(const Math::vec3 &from, const Math::vec3 &to)
{
	return Math::vec3(random_range(from.x, to.x), random_range(from.y, to.y), random_range(from.z, to.z));
}

// Random rotation around the X, Y and Z axes, angles in degrees.
Math::mat4 random_rotation(const Math::vec3 &from, const Math::vec3 &to)
{
	const Math::vec3 angles = random_vec3(from, to);
	return Math::rotateZ(angles.z) * Math::rotateY(angles.y) * Math::rotateX(angles.x);
}

} // namespace

ObjectPlacer::ObjectPlacer(QObject *parent)
	: QObject(parent)
{
}

ObjectPlacer::~ObjectPlacer()
{
	on_changed = nullptr;
	on_message = nullptr;
	setEnabled(false);
}

QString ObjectPlacer::kindLabel(AssetKind kind)
{
	return ArtistTool::kindLabel(kind);
}

void ObjectPlacer::updateAssetFromSelection()
{
	const ::UnigineEditor::SelectorGUIDs *selector = ::UnigineEditor::Selection::getSelectorRuntimes();
	if (!selector || selector->empty())
		return;

	Asset asset;
	if (!resolveSelection(selector->getGUIDs(), asset))
		return;

	if (asset.kind != asset_.kind || asset.path != asset_.path)
	{
		Log::message("ArtistTool: Object Placer asset '%s' -> %s (%s)\n", asset.name.toUtf8().constData(),
			kindLabel(asset.kind).toUtf8().constData(), asset.path.get());
	}

	asset_ = asset;
	notify_changed();
}

void ObjectPlacer::setPlaceDummy(bool dummy)
{
	if (place_dummy_ == dummy)
		return;

	place_dummy_ = dummy;
	// Nothing left to place: an asset was never picked.
	if (enabled_ && !hasObject())
		setEnabled(false);
	else
		notify_changed();
}

QString ObjectPlacer::getObjectName() const
{
	return place_dummy_ ? QString::fromUtf8("Node Dummy") : asset_.name;
}

void ObjectPlacer::setParentNode(const NodePtr &node)
{
	parent_ = node;
	notify_changed();
}

NodePtr ObjectPlacer::getParentNode() const
{
	return parent_.isDeleted() ? NodePtr() : parent_;
}

void ObjectPlacer::setEnabled(bool enabled)
{
	if (enabled && !hasObject())
	{
		report(MSG_WARNING, uiText(
			"Select a mesh or a node in the Asset Browser first.",
			"Сначала выберите меш или ноду в Asset Browser."));
		enabled = false;
	}
	if (enabled_ == enabled)
	{
		notify_changed();
		return;
	}

	enabled_ = enabled;
	swallow_release_ = false;
	if (enabled_)
	{
		placed_count_ = 0;
		ignored_clicks_logged_ = 0;
		cursor_logged_ = false;
		qApp->installEventFilter(this);
		// The cursor disc follows the mouse every frame.
		Engine::get()->getEventEndWorldUpdate().connect(connections_, this, &ObjectPlacer::update_cursor);
	} else
	{
		qApp->removeEventFilter(this);
		connections_.disconnectAll();
		end_drag();
		remove_cursor();
		if (hover_viewport_)
			hover_viewport_->unsetCursor();
		hover_viewport_ = nullptr;
	}

	notify_changed();
}

bool ObjectPlacer::eventFilter(QObject *watched, QEvent *event)
{
	if (!enabled_)
		return false;

	switch (event->type())
	{
		case QEvent::KeyPress:
			if (static_cast<QKeyEvent *>(event)->key() == Qt::Key_Escape)
			{
				setEnabled(false);
				return true;
			}
			break;

		case QEvent::Enter:
		case QEvent::MouseMove:
			// Track the viewport under the mouse and show there that the tool is on.
			if (watched != hover_viewport_)
			{
				if (QWidget *viewport = find_viewport_widget(watched))
				{
					if (hover_viewport_)
						hover_viewport_->unsetCursor();
					hover_viewport_ = viewport;
					viewport->setCursor(Qt::CrossCursor);
				}
			}
			break;

		case QEvent::Leave:
			if (watched == hover_viewport_)
			{
				hover_viewport_->unsetCursor();
				hover_viewport_ = nullptr;
			}
			break;

		case QEvent::MouseButtonPress:
		case QEvent::MouseButtonDblClick:	// the second click of a fast pair
		{
			const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
			if (mouse->button() != Qt::LeftButton)
				break;

			// A press that is not taken below goes to the editor with its release: the
			// editor must always see the end of a click it saw the start of.
			if (watched->isWidgetType())
				swallow_release_ = false;

			// Alt + LMB stays with the editor camera.
			if (mouse->modifiers() & Qt::AltModifier)
				break;

			QWidget *viewport = find_viewport_widget(watched);
			if (!viewport)
			{
				log_ignored_click(watched);
				break;
			}

			place(viewport, viewport->mapFromGlobal(mouse->globalPosition()));
			swallow_release_ = true;
			return true;
		}

		case QEvent::Wheel:
		{
			// The event comes twice, for the window and for the widget in it; only the
			// second one is taken.
			if (!watched->isWidgetType())
				break;

			const QWheelEvent *wheel = static_cast<QWheelEvent *>(event);
			const bool holding = drag_node_ && !drag_node_.isDeleted();
			// Shift + wheel scrolls sideways on some systems, hence both directions.
			const int delta = wheel->angleDelta().y() != 0 ? wheel->angleDelta().y() : wheel->angleDelta().x();

			// Shift + wheel: the next / previous asset of the same folder, like the
			// Quick asset change of the editor. Works over a viewport, held or not.
			if (wheel->modifiers() & Qt::ShiftModifier)
			{
				if (!holding && !find_viewport_widget(watched))
					break;
				if (delta != 0)
					switch_asset(delta > 0 ? -1 : 1);
				return true;
			}

			// While an object is held, the wheel turns it around its up axis instead of
			// doing what it usually does in the viewport.
			if (holding)
			{
				turn_around_up(WHEEL_TURN * float(delta) / 120.0f);
				return true;
			}
			break;
		}

		case QEvent::MouseButtonRelease:
			if (static_cast<QMouseEvent *>(event)->button() != Qt::LeftButton)
				break;
			end_drag();
			if (swallow_release_ && watched->isWidgetType())
			{
				swallow_release_ = false;
				return find_viewport_widget(watched) != nullptr;
			}
			break;

		default:
			break;
	}

	return false;
}

QWidget *ObjectPlacer::find_viewport_widget(QObject *watched) const
{
	return findViewportWidget(watched);
}

// Which widget got a click the tool did not take - written to the editor console to
// tell why clicking in a viewport places nothing, should that ever happen.
void ObjectPlacer::log_ignored_click(QObject *watched)
{
	if (ignored_clicks_logged_ >= 8 || !watched->isWidgetType())
		return;

	QString chain;
	for (const QWidget *widget = static_cast<QWidget *>(watched); widget; widget = widget->parentWidget())
	{
		// Clicks on the tool's own controls are of no interest.
		if (widget->objectName() == QLatin1String("ArtistToolWindow"))
			return;
		chain += QString::fromUtf8("%1(%2x%3) ").arg(widget->metaObject()->className())
			.arg(widget->width()).arg(widget->height());
	}
	++ignored_clicks_logged_;

	QString sizes;
	Vector<ViewportWindowPtr> viewports;
	ViewportManager::getViewports(viewports);
	for (const ViewportWindowPtr &viewport : viewports)
		sizes += QString::fromUtf8("%1x%2 ").arg(viewport->getWidth()).arg(viewport->getHeight());

	Log::message("ArtistTool: Object Placer ignored a click on: %s| viewports: %s\n",
		chain.toUtf8().constData(), sizes.toUtf8().constData());
}

// The surface point under the mouse, with the filter applied (see Hit::allowed).
ObjectPlacer::Hit ObjectPlacer::pick(QWidget *viewport, const QPointF &position) const
{
	if (!World::isLoaded())
		return {};

	Math::Vec3 p0, p1;
	if (!screenRay(viewport, position, p0, p1))
		return {};

	return raycast(p0, p1, options_, cursor_.isDeleted() ? NodePtr() : NodePtr(cursor_),
		drag_node_.isDeleted() ? NodePtr() : drag_node_);
}

// Called every frame while the tool is on: moves the disc to the surface under the
// mouse. Blue - a click places the object there, red - the filter does not allow it.
void ObjectPlacer::update_cursor()
{
	if (!enabled_)
		return;

	const QPoint mouse = QCursor::pos();

	// The viewport under the mouse: the one tracked from the mouse events, or, if no
	// event told about it yet, whatever widget is there.
	QWidget *viewport = hover_viewport_;
	if (viewport && !viewport->rect().contains(viewport->mapFromGlobal(mouse)))
		viewport = nullptr;
	if (!viewport)
	{
		if (QWidget *widget = QApplication::widgetAt(mouse))
			viewport = find_viewport_widget(widget);
	}

	// The object dropped by the press that is still held follows the mouse. The
	// button state is checked here as well, in case the release went to a widget
	// the tool does not watch.
	if (drag_node_ && (drag_node_.isDeleted() || !(QApplication::mouseButtons() & Qt::LeftButton)))
		end_drag();

	Hit hit;
	if (viewport)
		hit = pick(viewport, viewport->mapFromGlobal(QPointF(mouse)));

	if (drag_node_ && !drag_moving_)
	{
		const bool held_long_enough = drag_timer_.elapsed() >= DRAG_DELAY_MS;
		const bool moved_far_enough = (mouse - drag_press_position_).manhattanLength()
			>= QApplication::startDragDistance();
		drag_moving_ = held_long_enough && moved_far_enough;
	}

	// Over a place where it can not stand the object waits at its last good one.
	if (drag_node_ && drag_moving_ && hit.found && hit.allowed)
	{
		drag_hit_ = hit;
		drag_node_->setWorldTransform(make_transform(hit, drag_placement_));
	}

	if (hit.found)
		show_cursor(hit);
	else
		hide_cursor();
}

// Turns the held object around its up axis: the normal of the surface it stands on,
// or the vertical if it is not oriented by the surface.
void ObjectPlacer::turn_around_up(float angle)
{
	drag_turn_ += angle;
	drag_placement_.local_rotation = Math::rotateZ(angle) * drag_placement_.local_rotation;
	drag_node_->setWorldTransform(make_transform(drag_hit_, drag_placement_));

	report(MSG_INFO, uiText("Turned around the up axis: %1°", "Поворот вокруг оси «вверх»: %1°").arg(qRound(drag_turn_)));
}

void ObjectPlacer::end_drag()
{
	if (drag_node_ && !drag_node_.isDeleted())
		register_node(drag_node_);

	drag_node_ = NodePtr();
	drag_moving_ = false;
}

// Gives a placed node to the editor: it appears in the World Hierarchy and becomes
// one undo step.
void ObjectPlacer::register_node(const NodePtr &node)
{
	::UnigineEditor::Undo::apply(new ::UnigineEditor::CreateNodesAction(node));
}

// Makes the next (direction 1) or previous (-1) asset of the same type in the folder
// of the current one the asset to place. An object that is being held is swapped for
// the new asset right where it stands.
void ObjectPlacer::switch_asset(int direction)
{
	if (place_dummy_)
		return;
	if (asset_.asset_path.empty())
	{
		report(MSG_WARNING, uiText(
			"Can not switch: select the asset itself in the Asset Browser, not a part of it.",
			"Переключить нельзя: выберите в Asset Browser сам ассет, а не его часть."));
		return;
	}

	const QStringList siblings = siblingAssets(asset_.asset_path);
	const int count = siblings.size();
	int index = siblings.indexOf(QString::fromUtf8(asset_.asset_path.get()));
	if (count < 2 || index < 0)
	{
		report(MSG_INFO, uiText("No other assets of this type in the folder.", "В папке нет других ассетов этого типа."));
		return;
	}

	// Skip the assets that can not be placed (e.g. a model that failed to import).
	Asset asset;
	bool found = false;
	for (int step = 1; step < count && !found; ++step)
	{
		index = (index + direction + count) % count;
		found = resolveAsset(String(siblings[index].toUtf8().constData()), asset);
	}
	if (!found)
	{
		report(MSG_INFO, uiText("No other assets of this type in the folder.", "В папке нет других ассетов этого типа."));
		return;
	}

	asset_ = asset;

	if (drag_node_ && !drag_node_.isDeleted())
	{
		if (NodePtr node = create_node())
		{
			if (NodePtr parent = getParentNode())
				node->setWorldParent(parent);
			node->setWorldTransform(make_transform(drag_hit_, drag_placement_));
			node->setShowInEditorEnabledRecursive(true);
			node->setSaveToWorldEnabledRecursive(true);

			// The old one was never given to the editor, so it is simply removed.
			NodePtr old = drag_node_;
			drag_node_ = node;
			old.deleteLater();
		}
	}

	report(MSG_INFO, uiText("Asset: %1 (%2 of %3)", "Ассет: %1 (%2 из %3)").arg(asset_.name).arg(index + 1).arg(count));
	notify_changed();
}

// The random part of a placement, drawn once per object so that it does not change
// while the object is moved by the mouse.
ObjectPlacer::Placement ObjectPlacer::random_placement() const
{
	Placement placement;
	placement.local_rotation = random_rotation(options_.local_rotation_min, options_.local_rotation_max);
	placement.world_rotation = random_rotation(options_.world_rotation_min, options_.world_rotation_max);
	placement.local_offset = random_vec3(options_.local_offset_min, options_.local_offset_max);
	placement.world_offset = random_vec3(options_.world_offset_min, options_.world_offset_max);
	placement.scale = random_range(options_.scale_min, options_.scale_max);
	return placement;
}

// From the innermost: scale, turn the chosen axis of the asset up, local rotation,
// surface orientation, world rotation.
Math::Mat4 ObjectPlacer::make_transform(const Hit &hit, const Placement &placement) const
{
	const Math::vec3 up = options_.normal_orientation ? hit.normal : Math::vec3(0.0f, 0.0f, 1.0f);
	const Math::mat4 surface_basis = basisFromUp(up);
	const Math::mat4 rotation = placement.world_rotation
		* surface_basis
		* placement.local_rotation
		* up_axis_rotation(options_.up_axis)
		* Math::scale(Math::vec3(placement.scale));

	// The offset from the surface goes the way the up axis of the object looks: along
	// the normal of the surface, or straight up if the object is not oriented by it.
	const Math::vec3 offset = up * options_.surface_offset
		+ surface_basis * placement.local_offset + placement.world_offset;
	return Math::translate(hit.point + Math::Vec3(offset)) * Math::Mat4(rotation);
}

void ObjectPlacer::show_cursor(const Hit &hit)
{
	if (!cursor_ || cursor_.isDeleted())
	{
		// A flat disc of radius 1 around the Z axis, scaled every frame.
		cursor_ = Primitives::createCylinder(1.0f, 0.04f, 1, 48);
		if (!cursor_)
			return;

		cursor_->setName(CURSOR_NODE_NAME);
		cursor_->setShowInEditorEnabled(false);
		cursor_->setSaveToWorldEnabled(false);

		for (int surface = 0, num = cursor_->getNumSurfaces(); surface < num; ++surface)
		{
			// Glows on its own, so it stays visible in shadow; is never hit by rays.
			cursor_->setMaterialState("emission", 1, surface);
			cursor_->setMaterialParameterFloat("emission_scale", 2.0f, surface);
			cursor_->setCastShadow(false, surface);
			cursor_->setCastWorldShadow(false, surface);
			cursor_->setIntersection(false, surface);
			cursor_->setCollision(false, surface);
			cursor_->setPhysicsIntersection(false, surface);
		}
		set_cursor_color(hit.allowed);
	}

	if (cursor_allowed_ != hit.allowed)
		set_cursor_color(hit.allowed);

	// The same apparent size at any distance, lying just above the surface.
	const float radius = Math::clamp(float(hit.distance) * 0.012f, 0.005f, 1000.0f);
	const Math::Mat4 transform = Math::translate(hit.point + Math::Vec3(hit.normal * radius * 0.03f))
		* Math::Mat4(basisFromUp(hit.normal) * Math::scale(Math::vec3(radius)));
	cursor_->setWorldTransform(transform);
	cursor_->setEnabled(true);

	if (!cursor_logged_)
	{
		cursor_logged_ = true;
		Log::message("ArtistTool: Object Placer cursor shown (surfaces: %d)\n", cursor_->getNumSurfaces());
	}
}

void ObjectPlacer::set_cursor_color(bool allowed)
{
	cursor_allowed_ = allowed;

	const Math::vec4 color = allowed ? Math::vec4(0.25f, 0.7f, 1.0f, 1.0f) : Math::vec4(1.0f, 0.12f, 0.1f, 1.0f);
	for (int surface = 0, num = cursor_->getNumSurfaces(); surface < num; ++surface)
	{
		cursor_->setMaterialParameterFloat4("albedo_color", color, surface);
		cursor_->setMaterialParameterFloat4("emission_color", color, surface);
	}
}

void ObjectPlacer::hide_cursor()
{
	if (cursor_ && !cursor_.isDeleted())
		cursor_->setEnabled(false);
}

void ObjectPlacer::remove_cursor()
{
	if (cursor_ && !cursor_.isDeleted())
		cursor_.deleteLater();
	cursor_ = ObjectMeshDynamicPtr();
}

bool ObjectPlacer::place(QWidget *viewport, const QPointF &position)
{
	// The disc under the mouse is shown for the same hit: red or no disc - no object.
	const Hit hit = pick(viewport, position);
	if (!hit.found || !hit.allowed)
		return false;

	NodePtr node = create_node();
	if (!node)
	{
		report(MSG_ERROR, uiText("Could not create a node from '%1'.", "Не удалось создать ноду из '%1'.").arg(getObjectName()));
		return false;
	}

	const Placement placement = random_placement();
	report(MSG_INFO, QString());

	if (NodePtr parent = getParentNode())
		node->setWorldParent(parent);
	node->setWorldTransform(make_transform(hit, placement));
	node->setShowInEditorEnabledRecursive(true);
	node->setSaveToWorldEnabledRecursive(true);

	// While the button is held the object keeps following the mouse and can still be
	// swapped for another asset, so it is given to the editor only on release.
	if (!options_.move_while_held)
		register_node(node);
	else
	{
		drag_node_ = node;
		drag_placement_ = placement;
		drag_hit_ = hit;
		drag_turn_ = 0.0f;
		drag_moving_ = false;
		drag_press_position_ = QCursor::pos();
		drag_timer_.start();
	}

	++placed_count_;
	notify_changed();
	return true;
}

NodePtr ObjectPlacer::create_node() const
{
	if (place_dummy_)
	{
		NodeDummyPtr dummy = NodeDummy::create();
		dummy->setName("NodeDummy");
		return dummy;
	}

	return createNode(asset_);
}

void ObjectPlacer::notify_changed() const
{
	if (on_changed)
		on_changed();
}

void ObjectPlacer::report(int level, const QString &text) const
{
	if (on_message)
		on_message(level, text);
}

} // namespace ArtistTool
