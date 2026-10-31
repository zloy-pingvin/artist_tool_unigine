#pragma once

#include "PhysicsDrop.h"
#include "PlaceableAsset.h"

#include <UnigineCallback.h>
#include <UnigineDecals.h>
#include <UnigineMaterial.h>
#include <UnigineImage.h>
#include <UnigineMathLib.h>
#include <UnigineObjects.h>
#include <UnigineTextures.h>

#include <QElapsedTimer>
#include <QObject>
#include <QPointer>
#include <QSet>
#include <QString>
#include <QVector>

#include <functional>

class QWidget;

namespace ArtistTool
{

// What the mouse does in a viewport during a session of PhysicsDrop.
//  - The brush: a click puts down a number of objects, holding the button goes on pouring
//    them under the mouse; with Ctrl it erases what it has put down.
//  - Dragging: a click takes an object and it follows the mouse, pushing what is on its way, or the
//    click stays with the editor and the object is moved with its manipulator; with Shift everything
//    in the zone is pushed away, with Ctrl pulled in, with both it goes along with the mouse.
// Only what is in the zone around the mouse takes part in dragging, and only objects small
// enough: the rest of the world stays where it is, as solid as it was.
class PhysicsBrush final : public QObject
{
public:
	enum Mode
	{
		MODE_BRUSH = 0,
		MODE_DRAG,
	};

	struct Settings
	{
		QVector<PlaceableAsset> assets;
		// A click.
		int count{10};
		// Objects a second while the button is held.
		float rate{10.0f};
		float radius{0.5f};
		// How high above the surface they appear.
		float height{1.0f};
		float scale_min{1.0f};
		float scale_max{1.0f};
		// Every new object is turned by a random angle between these, in degrees around X, Y and Z.
		Unigine::Math::vec3 rotation_min{-180.0f, -180.0f, -180.0f};
		Unigine::Math::vec3 rotation_max{180.0f, 180.0f, 180.0f};
		// Of dragging: the radius of the zone, the biggest object that moves (its size across),
		// only the selected ones, and how hard Shift and Ctrl push (units per second squared).
		float zone{1.5f};
		float max_size{2.0f};
		bool only_selected{false};
		float force{12.0f};
		// Objects are moved with the manipulator of the editor, not taken with the mouse.
		bool use_gizmo{false};
	};

	explicit PhysicsBrush(PhysicsDrop *drop, QObject *parent = nullptr);
	~PhysicsBrush() override;

	bool begin(const PhysicsDrop::Options &options);
	// The session is over, the objects go back: the plugin is being unloaded.
	void stop();

	void setSettings(const Settings &settings);
	const Settings &getSettings() const { return settings_; }
	// What was still to come out does not.
	void clearPending() { pending_ = 0; }
	void setMode(Mode mode);

	// The radius or the height was changed with the wheel.
	std::function<void()> on_settings_changed;

protected:
	bool eventFilter(QObject *watched, QEvent *event) override;

private:
	enum Action
	{
		ACTION_NONE = 0,
		ACTION_PAINT,
		ACTION_ERASE,
		ACTION_GRAB,
		ACTION_PUSH,
		ACTION_PULL,
		ACTION_CARRY,
	};

	void stop_listening();
	void update();
	void press(QWidget *viewport, const QPointF &position, Qt::KeyboardModifiers modifiers);
	void end_action();
	// Where the ray of the mouse goes through the level plane of the zone; above the horizon it does not.
	bool ray_on_zone_plane(const Unigine::Math::Vec3 &p0, const Unigine::Math::Vec3 &p1, Unigine::Math::Vec3 &point) const;
	// The zone that pulls goes up with the top of the pile under the mouse, slowly and not far.
	void raise_pull_zone(const SurfaceHit &hit);
	void update_carry(const Unigine::Math::Vec3 &p0, const Unigine::Math::Vec3 &p1, float ifps);
	void spawn_some(int count, const SurfaceHit &hit);
	void pour_pending();
	void activate(const Unigine::Math::Vec3 &center, const Unigine::NodePtr &clicked = Unigine::NodePtr());
	SurfaceHit pick(const Unigine::Math::Vec3 &p0, const Unigine::Math::Vec3 &p1) const;
	Action action_for(Qt::KeyboardModifiers modifiers) const;
	// Where the mouse aims: it is always under the mouse, and goes deeper or nearer smoothly, so
	// that it does not jump from one thing under the mouse to another.
	void update_aim(const Unigine::Math::Vec3 &p0, const Unigine::Math::Vec3 &p1, const SurfaceHit &hit);
	Action shown_action() const;
	Unigine::Math::Vec3 portal_position() const;
	// The color of the marker of the action, gray out of the dome.
	Unigine::Math::vec4 marker_color(Action action) const;
	void draw() const;
	void draw_zone(const Unigine::Math::Vec3 &center, double base, const Unigine::Math::vec4 &color) const;
	// The hatching on what is under the brush, where the objects land.
	void update_fall_zone(bool show);
	void update_portal(bool show);
	void upload_portal();
	void tell_outside();
	bool uses_gizmo() const { return mode_ == MODE_DRAG && settings_.use_gizmo; }
	// In a session the manipulator is there when it is what objects are moved with, whatever the editor
	// had chosen, and not there otherwise: the click is taken, and it would do nothing.
	void apply_gizmo();
	void restore_manipulators();
	void report(int level, const QString &text) const;

	PhysicsDrop *drop_{nullptr};
	Settings settings_;
	Mode mode_{MODE_BRUSH};
	bool listening_{false};
	SurfaceFilter filter_;

	Action action_{ACTION_NONE};
	bool swallow_release_{false};
	int stroke_{0};
	// What the button has been held for, and the part of an object not poured yet.
	QElapsedTimer action_timer_;
	float pour_rest_{0.0f};
	// Objects still to come out, and where.
	int pending_{0};
	Unigine::Math::Vec3 pending_point_;
	bool told_limit_{false};
	bool told_outside_{false};
	// From the camera to the held object, along the ray of the mouse.
	double grab_distance_{0.0};
	Unigine::Math::Vec3 grab_target_;
	QElapsedTimer activate_timer_;
	// The zone that carries, pushes or pulls goes over a level plane, at the height it was taken at:
	// what the mouse passes over, and what the zone itself moves, does not throw it up and down.
	double carry_height_{0.0};
	// The lowest under the zone that pulls since it was taken: it rises from there.
	double pull_floor_{0.0};
	Unigine::Math::Vec3 carry_center_;
	Unigine::Math::vec3 carry_velocity_{0.0f, 0.0f, 0.0f};

	bool aim_valid_{false};
	double aim_distance_{0.0};
	Unigine::Math::Vec3 aim_point_;
	// The height the dome of the zone rises from: a held object does not take it up with it.
	double zone_base_{0.0};

	// What the objects come out of.
	QElapsedTimer portal_timer_;
	Unigine::ObjectMeshDynamicPtr portal_;
	Unigine::MaterialPtr portal_material_;
	Unigine::TexturePtr portal_texture_;
	Unigine::ImagePtr portal_image_;
	Unigine::Math::vec4 portal_color_{0.0f, 0.0f, 0.0f, 0.0f};
	qint64 portal_drawn_at_{0};
	bool portal_upload_pending_{false};
	float portal_lift_{0.0f};
	float portal_rise_{0.0f};
	// The height is being changed with the wheel: what is drawn shows the height.
	bool height_adjusting_{false};
	QElapsedTimer height_timer_;
	Unigine::DecalOrthoPtr fall_zone_;
	Unigine::MaterialPtr fall_zone_material_;
	Unigine::Math::vec4 fall_zone_color_{0.0f, 0.0f, 0.0f, 0.0f};
	// The eraser: the same with a trash can, turned so that the can stands upright on the screen.
	Unigine::MaterialPtr erase_zone_material_;
	bool fall_zone_erasing_{false};
	Unigine::Math::vec3 erase_zone_heading_{0.0f, 1.0f, 0.0f};

	// What the editor had, to be put back when the session is over.
	bool manipulators_were_on_{true};
	bool translate_turned_on_{false};

	QPointer<QWidget> hover_viewport_;
	Unigine::EventConnections connections_;
};

} // namespace ArtistTool
