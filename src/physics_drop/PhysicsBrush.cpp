#include "PhysicsBrush.h"
#include "Localization.h"
#include "PhysicsGlow.h"
#include "UiHelpers.h"
#include "ViewportPicking.h"

#include <editor/UnigineObjectMode.h>
#include <editor/UnigineSelection.h>
#include <editor/UnigineSelector.h>

#include <UnigineEngine.h>
#include <UnigineImage.h>
#include <UnigineLog.h>
#include <UnigineMaterials.h>
#include <UnigineRender.h>
#include <UnigineTextures.h>
#include <UnigineVisualizer.h>

#include <QApplication>
#include <QCursor>
#include <QEvent>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QPainterPath>
#include <QRandomGenerator>
#include <QWheelEvent>
#include <QWidget>

#include <algorithm>
#include <cmath>

using namespace Unigine;
using namespace Unigine::Math;
using ::UnigineEditor::Selection;
using ::UnigineEditor::SelectorNodes;

namespace ArtistTool
{

namespace
{

// Pouring starts after the button has been held that long, in milliseconds: a click is a click.
const int POUR_DELAY_MS = 250;
// More would be no longer about clutter, and the editor would hardly bear it.
const int MAX_OBJECTS = 3000;
// In a frame: how many places on the disc are tried and found taken before it waits for the next frame,
// and how many objects are made at most, so that a big portion does not stop the editor.
const int POUR_TRIES_PER_FRAME = 8;
const int POUR_MOST_PER_FRAME = 20;
// How often the zone takes in what has come into it, in milliseconds, while something is done.
const int ACTIVATE_PERIOD_MS = 250;
const float WHEEL_STEP = 0.1f;
// As if the force had worked for that long, in seconds.
const float CLICK_FORCE_TIME = 0.05f;
// Carrying a pile: the zone goes no faster than that, in units a second, as what it carries does not
// either; and what is left of the difference from the speed of the mouse goes down that many times per
// second, as a mouse moves by jerks.
const float CARRY_MAX_SPEED = 8.0f;
const float CARRY_FOLLOW_SPEED = 15.0f;
// The zone that pulls goes up with the pile, by this part of its radius at most: it still reaches what
// lies on the ground around, and the pile does not climb after it to the camera. Units per second, up and
// down; slowly up, or it leaves the pile behind.
const float PULL_RISE_OF_ZONE = 0.7f;
const float PULL_RISE_SPEED = 0.5f;
const float PULL_FALL_SPEED = 2.0f;
// The wall of the zone: fainter than that of the general dome.
const float ZONE_GLOW = 0.15f;
const int ZONE_GLOW_BANDS = 12;
// The portal the objects come out of: the size of its picture, how often it is redrawn, in
// milliseconds, and how bright it is. Around it the texels are finer than along the way in: that is
// where the tongues are.
const int PORTAL_IMAGE_AROUND = 256;
const int PORTAL_IMAGE_ALONG = 32;
const int PORTAL_REDRAW_MS = 50;
const float PORTAL_BRIGHTNESS = 1.5f;
// It hangs above where the objects appear, so that they fall out of it: by the biggest of them so far,
// that is half its size and a bit, and not less than that before there are any.
const float PORTAL_LIFT = 1.15f;
const float PORTAL_LIFT_LEAST = 0.1f;
const float PORTAL_LIFT_SPEED = 6.0f;
// Above a heap objects have to appear higher, and it goes up with them, and back down at this speed:
// what is left of the way goes down that many times per second.
const float PORTAL_RISE_FADE = 0.5f;
const vec4 COLOR_ARROW(1.0f, 1.0f, 1.0f, 1.0f);
// While the height is changed with the wheel: the arrow goes both ways, this color and bigger, with a
// jump at every step of the wheel that goes down that fast, and it stays that long after Ctrl is let go.
const vec4 COLOR_ARROW_HEIGHT(1.0f, 0.55f, 0.1f, 1.0f);
const float HEIGHT_ARROW_SIZE = 1.5f;
const float HEIGHT_ARROW_JUMP = 0.6f;
const float HEIGHT_ARROW_JUMP_MS = 150.0f;
const int HEIGHT_SHOW_MS = 800;
// The hatching on the ground, drawn straight down from over the brush. How far below the point under
// the mouse it reaches: at least that, or twice the radius.
const float FALL_ZONE_DEPTH = 2.0f;
const int FALL_ZONE_IMAGE_SIZE = 512;
const float FALL_ZONE_STRIPES = 7.0f;
const float FALL_ZONE_STRIPE_WIDTH = 0.28f;
const float FALL_ZONE_ALPHA_STROKE = 0.35f;
const float FALL_ZONE_ALPHA_RIM = 0.5f;
const float FALL_ZONE_RIM_WIDTH = 0.025f;
// The trash can of the eraser: its size, of the circle, how solid it is, and how far off the strokes
// stop, in hundredths of its size.
const double FALL_ZONE_ICON_SIZE = 1.0 / 3.0;
const float FALL_ZONE_ALPHA_ICON = 0.9f;
const double FALL_ZONE_ICON_GAP = 9.0;
// A faint glow on top of the color, to be seen in a shadow. The engine adds it to the picture, it does
// not mix it: at full strength it turns everything pale and nothing looks transparent.
const float FALL_ZONE_GLOW = 0.25f;
// On what looks away from above it melts away: strokes stretched down the sides of objects are ugly.
const float FALL_ZONE_ANGLE_THRESHOLD = 0.35f;
const float FALL_ZONE_ANGLE_POWER = 0.25f;
// The colors of the markers are soft for thin lines; as a thin layer on the ground they would be pale.
const float FALL_ZONE_COLOR_POWER = 1.6f;
// How fast the aim gets to the surface under the mouse: what is left of the way goes down that many
// times per second.
const float AIM_DEPTH_SPEED = 6.0f;
// Less than that between the aim and the surface is the mouse going along the surface, and the aim
// stays on it: of the radius, of the distance. More is the mouse going from one thing to another.
const float AIM_JUMP_OF_RADIUS = 0.25f;
const float AIM_JUMP_OF_DISTANCE = 0.02f;
// How much the height goes with the wheel at the very least, for every step: from zero it has to go.
const float WHEEL_HEIGHT_STEP = 0.1f;

const vec4 COLOR_PAINT(0.35f, 0.85f, 0.45f, 1.0f);
const vec4 COLOR_ERASE(0.95f, 0.35f, 0.3f, 1.0f);
const vec4 COLOR_GRAB(0.95f, 0.95f, 0.95f, 1.0f);
const vec4 COLOR_PUSH(0.95f, 0.65f, 0.2f, 1.0f);
const vec4 COLOR_PULL(0.35f, 0.65f, 0.95f, 1.0f);
const vec4 COLOR_CARRY(0.95f, 0.85f, 0.25f, 1.0f);
const vec4 COLOR_OUTSIDE(0.55f, 0.55f, 0.55f, 1.0f);

float random_between(float low, float high)
{
	return low + float(QRandomGenerator::global()->bounded(1.0)) * (high - low);
}

// A trash can in the middle of two square pictures: the can itself, and the place for the strokes to
// keep off, a little wider than the can.
void draw_trash_can(QImage &can, QImage &keep_off)
{
	// In a box of 100 by 100.
	QPainterPath body;
	body.moveTo(22.0, 32.0);
	body.lineTo(78.0, 32.0);
	body.lineTo(72.0, 92.0);
	body.lineTo(28.0, 92.0);
	body.closeSubpath();
	QPainterPath lid;
	lid.addRoundedRect(QRectF(14.0, 18.0, 72.0, 9.0), 3.0, 3.0);
	QPainterPath handle;
	handle.addRoundedRect(QRectF(37.0, 7.0, 26.0, 15.0), 4.0, 4.0);
	QPainterPath handle_hole;
	handle_hole.addRoundedRect(QRectF(43.0, 13.0, 14.0, 5.0), 1.5, 1.5);
	QPainterPath slits;
	for (const qreal x : {36.0, 47.5, 59.0})
		slits.addRoundedRect(QRectF(x, 42.0, 5.0, 40.0), 2.5, 2.5);

	const int size = can.width();
	QTransform transform;
	transform.translate(size * 0.5, size * 0.5);
	transform.scale(size * FALL_ZONE_ICON_SIZE / 100.0, size * FALL_ZONE_ICON_SIZE / 100.0);
	transform.translate(-50.0, -50.0);

	can.fill(0);
	QPainter can_painter(&can);
	can_painter.setRenderHint(QPainter::Antialiasing);
	can_painter.setTransform(transform);
	can_painter.fillPath(lid.united(handle.subtracted(handle_hole)).united(body.subtracted(slits)), Qt::white);
	can_painter.end();

	keep_off.fill(0);
	QPainter keep_off_painter(&keep_off);
	keep_off_painter.setRenderHint(QPainter::Antialiasing);
	keep_off_painter.setTransform(transform);
	keep_off_painter.setPen(QPen(Qt::white, FALL_ZONE_ICON_GAP * 2.0, Qt::SolidLine, Qt::RoundCap, Qt::RoundJoin));
	keep_off_painter.setBrush(Qt::white);
	keep_off_painter.drawPath(lid.united(handle).united(body));
	keep_off_painter.end();
}

// White, the alpha is the picture: slanted strokes in a circle with a rim, and with 'with_trash_can'
// the strokes stop short of a trash can. No file for it: the plugin has none.
ImagePtr make_fall_zone_image(bool with_trash_can)
{
	const int size = FALL_ZONE_IMAGE_SIZE;
	QImage can(size, size, QImage::Format_Alpha8);
	QImage keep_off(size, size, QImage::Format_Alpha8);
	if (with_trash_can)
		draw_trash_can(can, keep_off);

	ImagePtr image = Image::create();
	image->create2D(size, size, Image::FORMAT_RGBA8);
	const float pixel = 2.0f / float(size);
	const auto coverage = [pixel](float inside) { return clamp(inside / pixel + 0.5f, 0.0f, 1.0f); };
	for (int y = 0; y < size; ++y)
	{
		for (int x = 0; x < size; ++x)
		{
			const float u = (float(x) + 0.5f) * pixel - 1.0f;
			const float v = (float(y) + 0.5f) * pixel - 1.0f;
			const float distance = std::sqrt(u * u + v * v);

			const float rim = coverage(1.0f - distance) * coverage(distance - (1.0f - FALL_ZONE_RIM_WIDTH));

			// Across the strokes, in units of the distance between them.
			const float across = (u + v) * 0.7071f * FALL_ZONE_STRIPES;
			const float from_stroke = std::abs(across - std::floor(across + 0.5f)) / FALL_ZONE_STRIPES;
			const float stroke = coverage(FALL_ZONE_STRIPE_WIDTH * 0.5f / FALL_ZONE_STRIPES - from_stroke) * coverage(1.0f - distance);

			float alpha = std::max(rim * FALL_ZONE_ALPHA_RIM, stroke * FALL_ZONE_ALPHA_STROKE);
			if (with_trash_can)
			{
				const float strokes_left = 1.0f - float(keep_off.constScanLine(y)[x]) / 255.0f;
				const float can_here = float(can.constScanLine(y)[x]) / 255.0f;
				alpha = std::max({rim * FALL_ZONE_ALPHA_RIM, stroke * FALL_ZONE_ALPHA_STROKE * strokes_left, can_here * FALL_ZONE_ALPHA_ICON});
			}
			image->set2D(x, y, Image::Pixel(255, 255, 255, int(alpha * 255.0f + 0.5f)));
		}
	}
	image->createMipmaps();
	return image;
}

vec4 fall_zone_paint(const vec4 &color)
{
	return vec4(std::pow(color.x, FALL_ZONE_COLOR_POWER), std::pow(color.y, FALL_ZONE_COLOR_POWER), std::pow(color.z, FALL_ZONE_COLOR_POWER), 1.0f);
}

MaterialPtr make_fall_zone_material(const ImagePtr &image)
{
	const MaterialPtr base = Materials::findManualMaterial("Unigine::decal_base");
	const MaterialPtr material = base ? base->inherit() : MaterialPtr();
	if (!material)
		return material;
	// The surface under it stays what it is, only its color changes. The emission texture stays
	// white: the shader takes the alpha of the albedo for the glow as well.
	material->setState("emission", 1);
	material->setState("angle_fade", 1);
	material->setTextureImage(material->findTexture("albedo"), image);
	material->setParameterFloat("emission_scale", FALL_ZONE_GLOW);
	material->setParameterFloat("angle_fade_threshold", FALL_ZONE_ANGLE_THRESHOLD);
	material->setParameterFloat("angle_fade_pow", FALL_ZONE_ANGLE_POWER);
	material->setParameterFloat("metalness_visible", 0.0f);
	material->setParameterFloat("roughness_visible", 0.0f);
	material->setParameterFloat("normal_visible", 0.0f);
	return material;
}

} // namespace

PhysicsBrush::PhysicsBrush(PhysicsDrop *drop, QObject *parent)
	: QObject(parent)
	, drop_(drop)
{
}

PhysicsBrush::~PhysicsBrush()
{
	stop_listening();
}

bool PhysicsBrush::begin(const PhysicsDrop::Options &options)
{
	if (!drop_->beginSession(options))
		return false;

	filter_ = options.filter;
	// What the filter leaves out is not aimed at: objects go through it anyway.
	filter_.look_through = true;
	action_ = ACTION_NONE;
	stroke_ = 0;
	told_limit_ = false;
	action_timer_.start();
	activate_timer_.start();
	// Both together: the time of the last picture counts from this start, not from that of the last session.
	portal_timer_.start();
	portal_drawn_at_ = 0;

	if (!listening_)
	{
		listening_ = true;
		manipulators_were_on_ = ::UnigineEditor::ObjectMode::isManipulatorsEnabled();
		requestVisualizer();
		Engine::get()->getEventEndWorldUpdate().connect(connections_, this, &PhysicsBrush::update);
		Render::getEventBegin().connect(connections_, this, &PhysicsBrush::upload_portal);
		qApp->installEventFilter(this);
	}
	apply_gizmo();
	return true;
}

void PhysicsBrush::setSettings(const Settings &settings)
{
	settings_ = settings;
	apply_gizmo();
}

void PhysicsBrush::setMode(Mode mode)
{
	mode_ = mode;
	apply_gizmo();
}

void PhysicsBrush::apply_gizmo()
{
	drop_->setGizmoEnabled(uses_gizmo());
	if (!listening_)
		return;

	using ::UnigineEditor::ObjectMode;
	ObjectMode::setManipulatorsEnabled(uses_gizmo());
	if (uses_gizmo() && ObjectMode::getTransformType() == ObjectMode::TRANSFORM_TYPE_SELECT)
	{
		ObjectMode::setTransformType(ObjectMode::TRANSFORM_TYPE_TRANSLATE);
		translate_turned_on_ = true;
	}
}

void PhysicsBrush::restore_manipulators()
{
	using ::UnigineEditor::ObjectMode;
	ObjectMode::setManipulatorsEnabled(manipulators_were_on_);
	if (translate_turned_on_ && ObjectMode::getTransformType() == ObjectMode::TRANSFORM_TYPE_TRANSLATE)
		ObjectMode::setTransformType(ObjectMode::TRANSFORM_TYPE_SELECT);
	translate_turned_on_ = false;
}

void PhysicsBrush::stop()
{
	stop_listening();
	drop_->cancelNow();
}

void PhysicsBrush::stop_listening()
{
	if (!listening_)
		return;
	listening_ = false;
	action_ = ACTION_NONE;
	pending_ = 0;
	restore_manipulators();
	if (fall_zone_ && !fall_zone_.isDeleted())
		fall_zone_.deleteLater();
	fall_zone_ = nullptr;
	if (portal_ && !portal_.isDeleted())
		portal_.deleteLater();
	portal_ = nullptr;
	connections_.disconnectAll();
	qApp->removeEventFilter(this);
	releaseVisualizer();
	if (hover_viewport_)
		hover_viewport_->unsetCursor();
	hover_viewport_ = nullptr;
}

void PhysicsBrush::report(int level, const QString &text) const
{
	if (drop_->on_message)
		drop_->on_message(level, text);
}

SurfaceHit PhysicsBrush::pick(const Vec3 &p0, const Vec3 &p1) const
{
	if (mode_ == MODE_BRUSH)
		return raycastSurface(p0, p1, filter_, Vector<NodePtr>());

	// What to take has nothing to do with what things fall on.
	SurfaceFilter anything;
	anything.surface_types = SurfaceFilter::ALL & ~(SurfaceFilter::WATER | SurfaceFilter::CLOUDS);
	anything.look_through = true;
	return raycastSurface(p0, p1, anything, Vector<NodePtr>());
}

PhysicsBrush::Action PhysicsBrush::action_for(Qt::KeyboardModifiers modifiers) const
{
	const bool control = (modifiers & Qt::ControlModifier) != 0;
	const bool shift = (modifiers & Qt::ShiftModifier) != 0;
	if (mode_ == MODE_BRUSH)
		return control ? ACTION_ERASE : ACTION_PAINT;
	if (control && shift)
		return ACTION_CARRY;
	if (control)
		return ACTION_PULL;
	if (shift)
		return ACTION_PUSH;
	return settings_.use_gizmo ? ACTION_NONE : ACTION_GRAB;
}

void PhysicsBrush::spawn_some(int count, const SurfaceHit &hit)
{
	if (settings_.assets.isEmpty() || !hit.found)
		return;

	// A held button does not pile up more than that while there is no room.
	pending_ = std::min(pending_ + count, std::max(settings_.count, int(settings_.rate)));
	pending_point_ = hit.point;
	pour_pending();
}

void PhysicsBrush::pour_pending()
{
	if (pending_ <= 0 || settings_.assets.isEmpty())
		return;

	drop_->prepareArea(pending_point_, settings_.radius + settings_.height + 2.0f);
	// What has no room waits for the next frame: what came out before has fallen away by then.
	int tries = 0;
	int made = 0;
	while (pending_ > 0 && tries < POUR_TRIES_PER_FRAME && made < POUR_MOST_PER_FRAME)
	{
		if (drop_->getNumObjects() >= MAX_OBJECTS)
		{
			pending_ = 0;
			if (!told_limit_)
			{
				told_limit_ = true;
				report(PhysicsDrop::MSG_WARNING, uiText("That is enough for one session: %1 objects. Apply them first.",
					"Для одного сеанса достаточно: %1 объектов. Сначала примените их.").arg(MAX_OBJECTS));
			}
			return;
		}

		// Evenly over the disc, not crowding at its middle.
		const float angle = random_between(0.0f, 6.2831853f);
		const float distance = settings_.radius * std::sqrt(random_between(0.0f, 1.0f));
		const Vec3 center = pending_point_ + Vec3(Scalar(std::cos(angle) * distance), Scalar(std::sin(angle) * distance), Scalar(settings_.height));
		if (!drop_->hasRoom(center))
		{
			++tries;
			continue;
		}

		const PlaceableAsset &asset = settings_.assets[QRandomGenerator::global()->bounded(int(settings_.assets.size()))];
		const vec3 &turn_from = settings_.rotation_min;
		const vec3 &turn_to = settings_.rotation_max;
		const quat rotation(rotateZ(random_between(turn_from.z, turn_to.z))
			* rotateY(random_between(turn_from.y, turn_to.y))
			* rotateX(random_between(turn_from.x, turn_to.x)));
		const float scale = random_between(settings_.scale_min, std::max(settings_.scale_min, settings_.scale_max));
		drop_->spawn(asset, center, rotation, scale, stroke_);
		--pending_;
		++made;
	}
}

void PhysicsBrush::activate(const Vec3 &center, const NodePtr &clicked)
{
	activate_timer_.start();

	QVector<NodePtr> selected;
	if (settings_.only_selected)
	{
		if (const SelectorNodes *selector = Selection::getSelectorNodes())
		{
			const Vector<NodePtr> nodes = selector->getNodes();
			for (const NodePtr &node : nodes)
				selected.append(node);
		}
	}
	drop_->activateZone(center, settings_.zone, settings_.max_size, settings_.only_selected ? &selected : nullptr, clicked);
}

void PhysicsBrush::press(QWidget *viewport, const QPointF &position, Qt::KeyboardModifiers modifiers)
{
	Vec3 p0, p1;
	if (!screenRay(viewport, position, p0, p1))
		return;
	const SurfaceHit hit = pick(p0, p1);

	action_ = action_for(modifiers);
	action_timer_.start();
	pour_rest_ = 0.0f;
	told_outside_ = false;
	height_adjusting_ = false;
	if (action_ != ACTION_PAINT)
		pending_ = 0;

	if (hit.found)
	{
		aim_valid_ = true;
		aim_distance_ = hit.distance;
		aim_point_ = hit.point;

		// The physics does not go there. What was poured can be erased wherever it is.
		if (action_ != ACTION_ERASE && !drop_->isInDome(hit.point, mode_ == MODE_DRAG && action_ != ACTION_GRAB ? settings_.zone : 0.0f))
		{
			tell_outside();
			action_ = ACTION_NONE;
			return;
		}
	}

	switch (action_)
	{
		case ACTION_PAINT:
			if (settings_.assets.isEmpty())
			{
				report(PhysicsDrop::MSG_WARNING, uiText("Nothing to pour: add assets to the list.", "Нечего сыпать: добавьте ассеты в список."));
				action_ = ACTION_NONE;
				return;
			}
			++stroke_;
			spawn_some(settings_.count, hit);
			break;

		case ACTION_ERASE:
			// Now as well: a quick click may be over before the next frame.
			if (hit.found)
				drop_->eraseNew(hit.point, settings_.radius);
			break;

		case ACTION_GRAB:
		{
			if (!hit.found || !hit.object)
			{
				action_ = ACTION_NONE;
				return;
			}
			const NodePtr whole = PhysicsDrop::wholeNode(hit.object);
			activate(hit.point, whole);
			const int item = drop_->findItem(whole);
			if (item < 0)
			{
				report(PhysicsDrop::MSG_WARNING, settings_.only_selected
					? uiText("This object does not move: only the selected ones do.", "Этот объект не двигается: двигаются только выделенные.")
					: uiText("This object does not move: it is bigger than the limit or has the Immovable flag.",
						"Этот объект не двигается: он больше предела или с флагом Immovable."));
				action_ = ACTION_NONE;
				return;
			}
			drop_->grab(item, hit.point);
			grab_distance_ = length(hit.point - p0);
			grab_target_ = hit.point;
			break;
		}

		case ACTION_PUSH:
		case ACTION_PULL:
			if (!hit.found)
			{
				action_ = ACTION_NONE;
				return;
			}
			carry_height_ = double(hit.point.z);
			pull_floor_ = carry_height_;
			carry_center_ = hit.point;
			activate(hit.point);
			// A quick click gives a shove of its own.
			drop_->applyForce(hit.point, settings_.zone, settings_.force, action_ == ACTION_PULL, CLICK_FORCE_TIME);
			break;

		case ACTION_CARRY:
			if (!hit.found)
			{
				action_ = ACTION_NONE;
				return;
			}
			carry_height_ = double(hit.point.z);
			carry_center_ = hit.point;
			carry_velocity_ = vec3(0.0f, 0.0f, 0.0f);
			activate(hit.point);
			break;

		default:
			break;
	}
}

void PhysicsBrush::end_action()
{
	if (action_ == ACTION_GRAB)
		drop_->release();
	else if (action_ == ACTION_CARRY)
		drop_->stopZone(carry_center_, settings_.zone);
	action_ = ACTION_NONE;
}

bool PhysicsBrush::ray_on_zone_plane(const Vec3 &p0, const Vec3 &p1, Vec3 &point) const
{
	const double height = double(p1.z - p0.z);
	const double along = std::abs(height) > 1e-9 ? (carry_height_ - double(p0.z)) / height : -1.0;
	if (along <= 0.0 || along >= 1.0)
		return false;
	point = p0 + (p1 - p0) * Scalar(along);
	return true;
}

void PhysicsBrush::raise_pull_zone(const SurfaceHit &hit)
{
	if (!hit.found)
		return;
	const double height = double(hit.point.z);
	pull_floor_ = std::min(pull_floor_, height);
	const double target = std::min(height, pull_floor_ + double(settings_.zone * PULL_RISE_OF_ZONE));
	const double time = double(std::min(Engine::get()->getIFps(), 0.1f));
	carry_height_ += std::clamp(target - carry_height_, -double(PULL_FALL_SPEED) * time, double(PULL_RISE_SPEED) * time);
}

void PhysicsBrush::update_carry(const Vec3 &p0, const Vec3 &p1, float ifps)
{
	vec3 velocity(0.0f, 0.0f, 0.0f);
	Vec3 on_plane;
	if (ray_on_zone_plane(p0, p1, on_plane))
	{
		// Behind the mouse if it is too fast: the zone stays with the pile and does not leave it behind.
		vec3 way = vec3(on_plane - carry_center_);
		way.z = 0.0f;
		const float time = std::max(ifps, 1e-3f);
		const float reach = CARRY_MAX_SPEED * time;
		if (length(way) > reach)
			way *= reach / length(way);
		carry_center_ += Vec3(way);
		velocity = way * (1.0f / time);
	}
	carry_velocity_ += (velocity - carry_velocity_) * (1.0f - std::exp(-ifps * CARRY_FOLLOW_SPEED));

	if (activate_timer_.elapsed() >= ACTIVATE_PERIOD_MS)
		activate(carry_center_);
	drop_->carryZone(carry_center_, settings_.zone, carry_velocity_, ifps);
}

void PhysicsBrush::update()
{
	// It is over: applied, canceled, or the world is gone.
	if (!drop_->isRunning() || !drop_->isSession())
	{
		stop_listening();
		return;
	}

	const QPoint mouse = QCursor::pos();
	QWidget *viewport = hover_viewport_;
	if (viewport && !viewport->rect().contains(viewport->mapFromGlobal(mouse)))
		viewport = nullptr;
	if (!viewport)
	{
		if (QWidget *widget = QApplication::widgetAt(mouse))
			viewport = findViewportWidget(widget);
	}

	// In case the release went to a widget that is not watched.
	if (action_ != ACTION_NONE && !(QApplication::mouseButtons() & Qt::LeftButton))
		end_action();

	if (height_adjusting_ && (mode_ != MODE_BRUSH
		|| (!(QApplication::keyboardModifiers() & Qt::ControlModifier) && height_timer_.elapsed() >= HEIGHT_SHOW_MS)))
		height_adjusting_ = false;

	Vec3 p0, p1;
	const bool has_ray = viewport && screenRay(viewport, viewport->mapFromGlobal(QPointF(mouse)), p0, p1);
	SurfaceHit hit;
	// A held object has a place of its own, whatever is under the mouse. So does a zone that pushes or
	// pulls: what it pulls comes between the mouse and the ground, a zone at the first thing under the
	// mouse gets nearer to the camera with it, and the whole pile climbs along the ray to the camera.
	// The zone that pulls does rise with the pile, which is how a pile gets tall, but only so far.
	const bool zone_on_plane = action_ == ACTION_PUSH || action_ == ACTION_PULL;
	if (has_ray && action_ != ACTION_GRAB && action_ != ACTION_PUSH)
		hit = pick(p0, p1);
	if (has_ray && action_ == ACTION_PULL)
		raise_pull_zone(hit);
	if (has_ray && zone_on_plane)
		ray_on_zone_plane(p0, p1, carry_center_);
	if (has_ray)
		update_aim(p0, p1, hit);

	const float ifps = Engine::get()->getIFps();
	switch (action_)
	{
		case ACTION_PAINT:
			if (hit.found && action_timer_.elapsed() >= POUR_DELAY_MS)
			{
				if (!drop_->isInDome(hit.point))
				{
					tell_outside();
					break;
				}
				pour_rest_ += settings_.rate * ifps;
				const int count = int(pour_rest_);
				pour_rest_ -= float(count);
				if (count > 0)
					spawn_some(count, hit);
			}
			break;

		case ACTION_ERASE:
			if (hit.found)
				drop_->eraseNew(hit.point, settings_.radius);
			break;

		case ACTION_GRAB:
			if (has_ray)
			{
				grab_target_ = aim_point_;
				drop_->setGrabTarget(grab_target_);
			}
			if (activate_timer_.elapsed() >= ACTIVATE_PERIOD_MS)
				activate(grab_target_);
			break;

		case ACTION_PUSH:
		case ACTION_PULL:
			if (!drop_->isInDome(carry_center_, settings_.zone))
			{
				tell_outside();
				break;
			}
			if (activate_timer_.elapsed() >= ACTIVATE_PERIOD_MS)
				activate(carry_center_);
			drop_->applyForce(carry_center_, settings_.zone, settings_.force, action_ == ACTION_PULL, ifps);
			break;

		case ACTION_CARRY:
			if (has_ray)
				update_carry(p0, p1, ifps);
			break;

		default:
			break;
	}

	if (mode_ == MODE_BRUSH)
		pour_pending();
	else
		pending_ = 0;

	update_fall_zone(mode_ == MODE_BRUSH && hit.found);
	update_portal(mode_ == MODE_BRUSH && hit.found && shown_action() != ACTION_ERASE);

	// What the manipulator takes pushes what is around it, as what is held by the mouse does.
	const QVector<Vec3> gizmo_targets = uses_gizmo() ? drop_->getGizmoTargets() : QVector<Vec3>();
	if (!gizmo_targets.isEmpty())
	{
		if (activate_timer_.elapsed() >= ACTIVATE_PERIOD_MS)
		{
			for (const Vec3 &target : gizmo_targets)
				activate(target);
		}
		const Vec3 center = gizmo_targets.first();
		draw_zone(center, double(center.z), drop_->isInDome(center, settings_.zone) ? COLOR_GRAB : COLOR_OUTSIDE);
		return;
	}

	if (hit.found || action_ == ACTION_GRAB || action_ == ACTION_CARRY || zone_on_plane)
		draw();
}

void PhysicsBrush::update_aim(const Vec3 &p0, const Vec3 &p1, const SurfaceHit &hit)
{
	const Vec3 direction = Vec3(normalize(vec3(p1 - p0)));
	if (action_ == ACTION_PUSH || action_ == ACTION_PULL)
	{
		aim_valid_ = true;
		aim_distance_ = length(carry_center_ - p0);
		aim_point_ = carry_center_;
		zone_base_ = action_ == ACTION_PULL ? pull_floor_ : carry_height_;
		return;
	}
	if (action_ == ACTION_GRAB)
	{
		aim_distance_ = grab_distance_;
		aim_valid_ = true;
	} else if (!hit.found)
	{
		aim_valid_ = false;
	} else if (!aim_valid_)
	{
		aim_distance_ = hit.distance;
		aim_valid_ = true;
	} else
	{
		const float ifps = std::min(Engine::get()->getIFps(), 0.1f);
		const double radius = mode_ == MODE_DRAG ? settings_.zone : settings_.radius;
		const double jump = std::max(radius * AIM_JUMP_OF_RADIUS, hit.distance * AIM_JUMP_OF_DISTANCE);
		if (std::abs(hit.distance - aim_distance_) < jump)
			aim_distance_ = hit.distance;
		else
			aim_distance_ += (hit.distance - aim_distance_) * double(1.0f - std::exp(-ifps * AIM_DEPTH_SPEED));
	}
	aim_point_ = p0 + direction * Scalar(aim_distance_);
	if (action_ != ACTION_GRAB)
		zone_base_ = double(aim_point_.z);
}

void PhysicsBrush::tell_outside()
{
	if (told_outside_)
		return;
	told_outside_ = true;
	report(PhysicsDrop::MSG_WARNING, uiText("This is out of the dome, no physics there: fly closer or make the dome bigger.",
		"Это вне купола, физики там нет: подлетите ближе или увеличьте купол."));
}

PhysicsBrush::Action PhysicsBrush::shown_action() const
{
	if (action_ != ACTION_NONE)
		return action_;
	// Ctrl would show the eraser, with nothing in it to see the height by.
	return height_adjusting_ ? ACTION_PAINT : action_for(QApplication::keyboardModifiers());
}

Vec3 PhysicsBrush::portal_position() const
{
	return aim_point_ + Vec3(Scalar(0), Scalar(0), Scalar(settings_.height + portal_lift_));
}

vec4 PhysicsBrush::marker_color(Action action) const
{
	const bool dragging = mode_ == MODE_DRAG;
	if (action != ACTION_ERASE && !drop_->isInDome(aim_point_, dragging ? settings_.zone : 0.0f))
		return COLOR_OUTSIDE;
	switch (action)
	{
		case ACTION_PAINT: return COLOR_PAINT;
		case ACTION_ERASE: return COLOR_ERASE;
		case ACTION_PUSH: return COLOR_PUSH;
		case ACTION_PULL: return COLOR_PULL;
		case ACTION_CARRY: return COLOR_CARRY;
		default: return COLOR_GRAB;
	}
}

void PhysicsBrush::draw() const
{
	const Action action = shown_action();
	// With the manipulator a plain click is the editor's: the zone shows only when it means something.
	if (action == ACTION_NONE)
		return;
	const vec4 color = marker_color(action);

	if (mode_ == MODE_DRAG)
	{
		if (action_ == ACTION_CARRY)
			draw_zone(carry_center_, carry_height_, color);
		else
			draw_zone(aim_point_, std::min(zone_base_, double(aim_point_.z)), color);
		return;
	}

	// From the portal to where they go. Gray out of the dome, like all the rest.
	if (action == ACTION_ERASE)
		return;
	const Vec3 top = portal_position();
	const bool outside = color == COLOR_OUTSIDE;
	if (height_adjusting_)
	{
		const float jump = HEIGHT_ARROW_JUMP * std::exp(-float(height_timer_.elapsed()) / HEIGHT_ARROW_JUMP_MS);
		drawArrow(top, aim_point_, outside ? color : COLOR_ARROW_HEIGHT, HEIGHT_ARROW_SIZE + jump, true);
		// Anchored at the middle of its bottom edge: the number stands above the portal.
		Visualizer::renderMessage3D(top, vec3(0.0f, 1.0f, 0.0f), QString::number(settings_.height, 'f', 2).toUtf8().constData(), outside ? color : COLOR_ARROW_HEIGHT, 1);
	} else
		drawArrow(top, aim_point_, outside ? color : COLOR_ARROW);
}

void PhysicsBrush::update_portal(bool show)
{
	if (!show)
	{
		if (portal_ && !portal_.isDeleted())
			portal_->setEnabled(false);
		return;
	}

	const vec4 color = marker_color(shown_action());
	const bool new_color = color != portal_color_;
	portal_color_ = color;
	const float time = float(portal_timer_.elapsed()) * 0.001f;
	const vec3 paint(std::pow(color.x, FALL_ZONE_COLOR_POWER), std::pow(color.y, FALL_ZONE_COLOR_POWER), std::pow(color.z, FALL_ZONE_COLOR_POWER));

	if (!portal_material_ || portal_material_.isDeleted())
	{
		const MaterialPtr base = Materials::findManualMaterial("Unigine::mesh_base");
		portal_material_ = base ? base->inherit() : MaterialPtr();
		if (!portal_material_)
			return;

		portal_image_ = Image::create();
		portal_image_->create2D(PORTAL_IMAGE_AROUND, PORTAL_IMAGE_ALONG, Image::FORMAT_RGBA8);
		fillPortalImage(portal_image_, paint, time);
		portal_texture_ = Texture::create();
		portal_texture_->create(portal_image_, Texture::SAMPLER_FILTER_LINEAR);

		// Black and lit by nothing: all of its color is the glow, mixed by the opacity of the albedo.
		portal_material_->setTransparent(Material::TRANSPARENT_BLEND);
		portal_material_->setBlendSrcFunc(RenderState::BLEND_SRC_ALPHA);
		portal_material_->setBlendDestFunc(RenderState::BLEND_ONE_MINUS_SRC_ALPHA);
		portal_material_->setOption(Material::OPTION_TWO_SIDED, 1);
		portal_material_->setState("emission", 1);
		portal_material_->setTexture("albedo", portal_texture_);
		portal_material_->setTexture("emission", portal_texture_);
		portal_material_->setParameterFloat4("albedo_color", vec4(0.0f, 0.0f, 0.0f, 1.0f));
		portal_material_->setParameterFloat("specular", 0.0f);
		portal_material_->setParameterFloat("emission_scale", PORTAL_BRIGHTNESS);
		portal_drawn_at_ = portal_timer_.elapsed();
	} else if (new_color || portal_timer_.elapsed() - portal_drawn_at_ >= PORTAL_REDRAW_MS)
	{
		fillPortalImage(portal_image_, paint, time);
		portal_upload_pending_ = true;
		portal_drawn_at_ = portal_timer_.elapsed();
	}

	if (!portal_ || portal_.isDeleted())
	{
		portal_ = ObjectMeshDynamic::create(makePortalMesh());
		// What is named so is not aimed at and is not solid: see isEditorInternal().
		portal_->setName("UnigineEditor.artist_tool_physics_portal");
		portal_->setShowInEditorEnabled(false);
		portal_->setSaveToWorldEnabled(false);
		portal_->setMaterial(portal_material_, 0);
		portal_->setCastShadow(false, 0);
		portal_->setCastWorldShadow(false, 0);
		portal_->setIntersection(false, 0);
		portal_->setCollision(false, 0);
	}

	const float ifps = std::min(Engine::get()->getIFps(), 0.1f);
	portal_rise_ = std::max(drop_->takeSpawnRise(), portal_rise_ * std::exp(-ifps * PORTAL_RISE_FADE));
	const float lift = std::max(drop_->getLargestNewRadius(), PORTAL_LIFT_LEAST) * PORTAL_LIFT + portal_rise_;
	portal_lift_ += (lift - portal_lift_) * (1.0f - std::exp(-ifps * PORTAL_LIFT_SPEED));

	const float radius = settings_.radius;
	portal_->setWorldTransform(translate(portal_position()) * Mat4(Math::scale(vec3(radius, radius, 1.0f))));
	portal_->setEnabled(true);
}

void PhysicsBrush::upload_portal()
{
	// A new picture for a texture is a command for the video card: given outside of the rendering, while
	// the world updates, it did not get there, and the portal stood still.
	if (!portal_upload_pending_ || !portal_texture_)
		return;
	portal_upload_pending_ = false;
	if (!portal_texture_->setImage2D(portal_image_, 0, 0))
		Log::warning("Artist Tools, Physics Drop: the picture of the portal was not taken by its texture\n");
}

void PhysicsBrush::update_fall_zone(bool show)
{
	if (!show)
	{
		if (fall_zone_ && !fall_zone_.isDeleted())
			fall_zone_->setEnabled(false);
		return;
	}

	// Two materials, made once: going from one picture to the other is a press of Ctrl, and it has to
	// be at once.
	const Action action = shown_action();
	const bool erasing = action == ACTION_ERASE;
	if (!fall_zone_material_ || fall_zone_material_.isDeleted())
	{
		fall_zone_material_ = make_fall_zone_material(make_fall_zone_image(false));
		fall_zone_color_ = vec4(0.0f, 0.0f, 0.0f, 0.0f);
	}
	if (!erase_zone_material_ || erase_zone_material_.isDeleted())
	{
		erase_zone_material_ = make_fall_zone_material(make_fall_zone_image(true));
		if (erase_zone_material_)
		{
			erase_zone_material_->setParameterFloat4("albedo", fall_zone_paint(COLOR_ERASE));
			erase_zone_material_->setParameterFloat4("emission_color", fall_zone_paint(COLOR_ERASE));
		}
	}
	if (!fall_zone_material_ || !erase_zone_material_)
		return;

	if (!fall_zone_ || fall_zone_.isDeleted())
	{
		fall_zone_ = DecalOrtho::create();
		fall_zone_->setName("artist_tool_physics_fall_zone");
		fall_zone_->setShowInEditorEnabled(false);
		fall_zone_->setSaveToWorldEnabled(false);
		fall_zone_->setZNear(0.0f);
		fall_zone_->setMaterial(erasing ? erase_zone_material_ : fall_zone_material_);
		fall_zone_erasing_ = erasing;
	}
	if (erasing != fall_zone_erasing_)
	{
		fall_zone_->setMaterial(erasing ? erase_zone_material_ : fall_zone_material_);
		fall_zone_erasing_ = erasing;
	}

	if (!erasing)
	{
		const vec4 color = marker_color(action);
		if (color != fall_zone_color_)
		{
			fall_zone_color_ = color;
			const vec4 paint = fall_zone_paint(color);
			fall_zone_material_->setParameterFloat4("albedo", paint);
			fall_zone_material_->setParameterFloat4("emission_color", paint);
		}
	}

	// Straight down: this is the way the objects fall, whatever the surface looks like.
	const float radius = settings_.radius;
	const float above = std::max(settings_.height + portal_lift_, radius);
	fall_zone_->setWidth(radius * 2.0f);
	fall_zone_->setHeight(radius * 2.0f);
	fall_zone_->setRadius(above + std::max(FALL_ZONE_DEPTH, radius * 2.0f));

	Mat4 transform = translate(aim_point_ + Vec3(Scalar(0), Scalar(0), Scalar(above)));
	if (erasing)
	{
		// The top of the picture is the side of +Y of the decal: it goes away from the camera, so that
		// the trash can stands upright on the screen. Looking straight down there is no such side.
		Vec3 camera;
		vec3 view;
		if (viewportCameraView(camera, view) && std::abs(view.x) + std::abs(view.y) > 0.01f)
			erase_zone_heading_ = normalize(vec3(view.x, view.y, 0.0f));
		transform.setColumn3(0, Vec3(vec3(erase_zone_heading_.y, -erase_zone_heading_.x, 0.0f)));
		transform.setColumn3(1, Vec3(erase_zone_heading_));
	}
	fall_zone_->setWorldTransform(transform);
	fall_zone_->setEnabled(true);
}

void PhysicsBrush::draw_zone(const Vec3 &center, double base, const vec4 &color) const
{
	// The dome of the zone: what is outside it does not move. It looks the same way all the time,
	// whatever surface is under the mouse.
	const float radius = settings_.zone;
	Visualizer::renderCircle(radius, translate(center), color, 0.0f, false);
	drawGlowSphere(center, radius, base, radius, vec4(color.x, color.y, color.z, ZONE_GLOW), ZONE_GLOW_BANDS);
}

bool PhysicsBrush::eventFilter(QObject *watched, QEvent *event)
{
	if (!listening_ || !drop_->isRunning())
		return false;

	switch (event->type())
	{
		case QEvent::ShortcutOverride:
		case QEvent::KeyPress:
		{
			if (isTypingInField(watched))
				break;
			const QKeyEvent *key = static_cast<const QKeyEvent *>(event);
			const bool control = (key->modifiers() & Qt::ControlModifier) != 0;
			const int letter = latinKey(key);
			const bool history = control && (letter == Qt::Key_Z || letter == Qt::Key_Y);
			const bool enter = key->key() == Qt::Key_Return || key->key() == Qt::Key_Enter;
			// Space is the tool's only over a viewport.
			const bool space = key->key() == Qt::Key_Space && hover_viewport_;
			const bool escape = key->key() == Qt::Key_Escape && isEscapeFree(watched);
			if (!history && !enter && !space && !escape)
				break;

			if (event->type() == QEvent::ShortcutOverride)
			{
				event->accept();
				return true;
			}
			// The manipulator has yet to leave its step in the history: ended now, the session could
			// not take it away.
			if (drop_->isGizmoHolding() && (enter || escape))
				return true;
			if (escape)
				drop_->cancel();
			else if (enter)
				drop_->finish();
			else if (space)
			{
				// Held down, it comes again and again.
				if (!key->isAutoRepeat())
					drop_->setUserPaused(!drop_->isUserPaused());
			}
			else if (letter == Qt::Key_Z)
			{
				pending_ = 0;
				if (!drop_->removeLastStroke())
					report(PhysicsDrop::MSG_WARNING, uiText("No strokes to take back. Esc puts everything back.", "Нечего отменять по мазкам. Esc вернёт всё как было."));
			}
			return true;
		}

		case QEvent::Enter:
		case QEvent::MouseMove:
			if (watched != hover_viewport_)
			{
				if (QWidget *viewport = findViewportWidget(watched))
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
		case QEvent::MouseButtonDblClick:
		{
			const QMouseEvent *mouse = static_cast<QMouseEvent *>(event);
			if (mouse->button() != Qt::LeftButton)
				break;
			if (watched->isWidgetType())
				swallow_release_ = false;
			// Alt + LMB stays with the editor camera.
			if (mouse->modifiers() & Qt::AltModifier)
				break;
			QWidget *viewport = findViewportWidget(watched);
			if (!viewport)
				break;
			// It selects, and the manipulator takes it.
			if (action_for(mouse->modifiers()) == ACTION_NONE)
				break;
			press(viewport, viewport->mapFromGlobal(mouse->globalPosition()), mouse->modifiers());
			swallow_release_ = true;
			return true;
		}

		case QEvent::MouseButtonRelease:
			if (static_cast<QMouseEvent *>(event)->button() != Qt::LeftButton)
				break;
			end_action();
			if (swallow_release_ && watched->isWidgetType())
			{
				swallow_release_ = false;
				return findViewportWidget(watched) != nullptr;
			}
			break;

		case QEvent::Wheel:
		{
			if (!watched->isWidgetType())
				break;
			const QWheelEvent *wheel = static_cast<QWheelEvent *>(event);
			const int delta = wheel->angleDelta().y() != 0 ? wheel->angleDelta().y() : wheel->angleDelta().x();
			const float step = float(delta) / 120.0f * WHEEL_STEP;
			// A held object goes nearer or farther.
			if (action_ == ACTION_GRAB)
			{
				grab_distance_ = std::max(0.1, grab_distance_ * (1.0 + double(step)));
				return true;
			}
			// With the right button held it is the speed of the flying camera, with Alt the camera too.
			if (!(wheel->modifiers() & Qt::AltModifier) && !(QApplication::mouseButtons() & Qt::RightButton) && findViewportWidget(watched))
			{
				if (mode_ == MODE_BRUSH && (wheel->modifiers() & Qt::ControlModifier))
				{
					settings_.height = clamp(settings_.height * (1.0f + step) + step * WHEEL_HEIGHT_STEP, 0.0f, 100.0f);
					height_adjusting_ = true;
					height_timer_.start();
				} else
				{
					float &radius = mode_ == MODE_DRAG ? settings_.zone : settings_.radius;
					radius = clamp(radius * (1.0f + step), 0.02f, 100.0f);
				}
				if (on_settings_changed)
					on_settings_changed();
				return true;
			}
			break;
		}

		default:
			break;
	}
	return false;
}

} // namespace ArtistTool
