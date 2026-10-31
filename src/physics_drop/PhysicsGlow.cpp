#include "PhysicsGlow.h"
#include "ViewportPicking.h"

#include <UnigineVisualizer.h>

#include <algorithm>
#include <cmath>
#include <vector>

using namespace Unigine;
using namespace Unigine::Math;

namespace ArtistTool
{

namespace
{

const int SEGMENTS = 64;
// A sphere goes on under the height it starts from, at full strength, that much of the fade: the
// ground is not the same height everywhere, and where it is higher it hides this part anyway.
const float SKIRT_DEPTH = 0.5f;
const int SKIRT_BANDS = 3;
// Of the height of what the camera sees at the tip of an arrow.
const float ARROW_SHAFT_WIDTH = 0.0025f;
const float ARROW_HEAD_WIDTH = 0.011f;
const float ARROW_HEAD_LENGTH = 0.028f;
// The visualizer does not smooth edges: an arrow is drawn several times, a little wider and fainter each
// time, out to that many pixels, which makes a soft edge.
const float ARROW_SOFT_EDGE = 2.0f;
const int ARROW_SOFT_STEPS = 4;
// The portal: how far its picture goes, of the radius, and how finely the ring is cut. In the middle
// the ring has a tiny hole: triangles of no size at all would give the engine nothing to make tangents
// of.
const float PORTAL_OUTER = 1.2f;
const float PORTAL_HOLE = 0.001f;
const int PORTAL_MESH_SEGMENTS = 128;
const int PORTAL_MESH_RINGS = 6;
// The opacity of the inside, where there is no fire.
const float PORTAL_VEIL = 0.14f;
// Two layers of noise make the tongues. How many of them go around, how long they are along the way
// in (the less, the longer), how fast they run inward, and how strong each layer is. The more of the
// fine layer and the more gain, the sharper the tongues.
const float PORTAL_WIDE_SIZE = 4.0f;
const float PORTAL_WIDE_STRETCH = 2.0f;
const float PORTAL_WIDE_SPEED = 0.5f;
const float PORTAL_WIDE_PART = 0.75f;
const float PORTAL_FINE_SIZE = 10.0f;
const float PORTAL_FINE_STRETCH = 3.0f;
const float PORTAL_FINE_SPEED = 1.1f;
const float PORTAL_FINE_PART = 0.25f;
const float PORTAL_NOISE_GAIN = 1.15f;
// The fire inside the rim: the shortest and the longest tongue, of the radius, and how it fades.
const float PORTAL_TONGUE_SHORT = 0.12f;
const float PORTAL_TONGUE_LONG = 0.46f;
const float PORTAL_TONGUE_POWER = 1.7f;
const float PORTAL_ALPHA_POWER = 1.05f;
// The fire outside the rim goes down fast, the halo slowly, and that is all gone by the end.
const float PORTAL_EDGE_WIDTH = 0.024f;
const float PORTAL_HALO = 0.28f;
const float PORTAL_HALO_WIDTH = 0.075f;
const float PORTAL_HALO_END = 0.04f;
// How white the hottest part is, and how narrow it is.
const float PORTAL_HOT = 0.65f;
const float PORTAL_HOT_POWER = 2.8f;

struct UnitCircle
{
	float cosine[SEGMENTS + 1];
	float sine[SEGMENTS + 1];
};

const UnitCircle &unit_circle()
{
	static const UnitCircle circle = []() {
		UnitCircle result;
		for (int i = 0; i <= SEGMENTS; ++i)
		{
			const float angle = 6.2831853f * float(i) / float(SEGMENTS);
			result.cosine[i] = std::cos(angle);
			result.sine[i] = std::sin(angle);
		}
		return result;
	}();
	return circle;
}

// One step of the gradient, around the vertical axis. 'inner' gives the direction to the inside from
// a point of it: what looks away from the camera is left out. The visualizer has one color per
// triangle, so a gradient is made of bands.
template<typename Inner>
void draw_band(const Vec3 &axis, double z0, float radius0, double z1, float radius1, const vec4 &color, const Vec3 &camera, const Inner &inner)
{
	const UnitCircle &circle = unit_circle();
	const auto at = [&](int i, double z, float radius) {
		return Vec3(axis.x + Scalar(circle.cosine[i] * radius), axis.y + Scalar(circle.sine[i] * radius), Scalar(z));
	};
	for (int i = 0; i < SEGMENTS; ++i)
	{
		// Counterclockwise as seen from the inside.
		const Vec3 v0 = at(i + 1, z0, radius0);
		const Vec3 v1 = at(i, z0, radius0);
		const Vec3 v2 = at(i, z1, radius1);
		const Vec3 v3 = at(i + 1, z1, radius1);
		const Vec3 middle = (v0 + v2) * Scalar(0.5);
		if (dot(vec3(camera - middle), inner(middle)) > 0.0f)
			Visualizer::renderQuad3D(v0, v1, v2, v3, color, 0.0f, true);
	}
}

vec4 with_strength(const vec4 &color, float strength)
{
	return vec4(color.x, color.y, color.z, color.w * strength);
}

// How much of the strength is left that far up the fade, from 0 to 1.
float strength_at(float height)
{
	const float left = 1.0f - height;
	return left * left;
}

float noise_gradient(int x, int y, int z, float dx, float dy, float dz)
{
	unsigned int hash = unsigned(x) * 73856093u ^ unsigned(y) * 19349663u ^ unsigned(z) * 83492791u;
	hash ^= hash >> 13;
	hash *= 1274126177u;
	hash ^= hash >> 16;
	const unsigned int kind = hash & 15u;
	const float u = kind < 8u ? dx : dy;
	const float v = kind < 4u ? dy : (kind == 12u || kind == 14u ? dx : dz);
	return (kind & 1u ? -u : u) + (kind & 2u ? -v : v);
}

// Perlin noise, about from -1 to 1.
float perlin_noise(float x, float y, float z)
{
	const float floor_x = std::floor(x);
	const float floor_y = std::floor(y);
	const float floor_z = std::floor(z);
	const int ix = int(floor_x);
	const int iy = int(floor_y);
	const int iz = int(floor_z);
	const float dx = x - floor_x;
	const float dy = y - floor_y;
	const float dz = z - floor_z;
	const auto smooth = [](float t) { return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f); };
	const auto mix = [](float a, float b, float t) { return a + (b - a) * t; };
	const float u = smooth(dx);
	const float v = smooth(dy);
	const float w = smooth(dz);
	return mix(
		mix(mix(noise_gradient(ix, iy, iz, dx, dy, dz), noise_gradient(ix + 1, iy, iz, dx - 1.0f, dy, dz), u),
			mix(noise_gradient(ix, iy + 1, iz, dx, dy - 1.0f, dz), noise_gradient(ix + 1, iy + 1, iz, dx - 1.0f, dy - 1.0f, dz), u), v),
		mix(mix(noise_gradient(ix, iy, iz + 1, dx, dy, dz - 1.0f), noise_gradient(ix + 1, iy, iz + 1, dx - 1.0f, dy, dz - 1.0f), u),
			mix(noise_gradient(ix, iy + 1, iz + 1, dx, dy - 1.0f, dz - 1.0f), noise_gradient(ix + 1, iy + 1, iz + 1, dx - 1.0f, dy - 1.0f, dz - 1.0f), u), v),
		w);
}

// The head of an arrow, pointing along 'along', with all its edges moved out by 'out'. 'side' is across
// the arrow, so that the triangle is counterclockwise as seen by the camera.
void draw_arrow_head(const Vec3 &tip, const vec3 &along, const vec3 &side, float head_length, float half_width, float out, const vec4 &color)
{
	const float forward = out * std::sqrt(head_length * head_length + half_width * half_width) / half_width;
	const float grown_width = (head_length + forward + out) * half_width / head_length;
	const Vec3 base = tip - Vec3(along * (head_length + out));
	const Vec3 base_side = Vec3(side * grown_width);
	Visualizer::renderTriangle3D(tip + Vec3(along * forward), base - base_side, base + base_side, color, 0.0f, false);
}

} // namespace

void drawGlowSphere(const Vec3 &center, float radius, double base, float fade, const vec4 &color, int bands)
{
	Vec3 camera;
	vec3 direction;
	if (fade <= 0.0f || !viewportCameraView(camera, direction))
		return;

	const double middle = double(center.z);
	const double bottom = middle - radius;
	const double top = middle + radius;
	const double start = std::min(std::max(base, bottom), top);
	const auto radius_at = [&](double z) {
		const double height = z - middle;
		return float(std::sqrt(std::max(0.0, double(radius) * radius - height * height)));
	};
	const auto inner = [&center](const Vec3 &point) { return vec3(center - point); };
	const auto band = [&](double z0, double z1, float strength) {
		draw_band(center, z0, radius_at(z0), z1, radius_at(z1), with_strength(color, strength), camera, inner);
	};

	const double skirt = std::max(bottom, start - double(fade * SKIRT_DEPTH));
	if (skirt < start)
	{
		for (int i = 0; i < SKIRT_BANDS; ++i)
			band(skirt + (start - skirt) * i / SKIRT_BANDS, skirt + (start - skirt) * (i + 1) / SKIRT_BANDS, 1.0f);
	}

	const double end = std::min(top, start + double(fade));
	if (end <= start)
		return;
	for (int i = 0; i < bands; ++i)
	{
		const double z0 = start + (end - start) * i / bands;
		const double z1 = start + (end - start) * (i + 1) / bands;
		band(z0, z1, strength_at(float(((z0 + z1) * 0.5 - start) / fade)));
	}
}

MeshPtr makePortalMesh()
{
	MeshPtr mesh = Mesh::create();
	const int surface = mesh->addSurface("portal");
	for (int ring = 0; ring <= PORTAL_MESH_RINGS; ++ring)
	{
		const float along = float(ring) / float(PORTAL_MESH_RINGS);
		const float distance = std::max(PORTAL_OUTER * along, PORTAL_HOLE);
		for (int i = 0; i <= PORTAL_MESH_SEGMENTS; ++i)
		{
			const float around = float(i) / float(PORTAL_MESH_SEGMENTS);
			const float angle = 6.2831853f * around;
			mesh->addVertex(vec3(std::cos(angle) * distance, std::sin(angle) * distance, 0.0f), surface);
			mesh->addTexCoord0(vec2(around, along), surface);
			mesh->addNormal(vec3(0.0f, 0.0f, 1.0f), surface);
		}
	}
	const int row = PORTAL_MESH_SEGMENTS + 1;
	for (int ring = 0; ring < PORTAL_MESH_RINGS; ++ring)
	{
		for (int i = 0; i < PORTAL_MESH_SEGMENTS; ++i)
		{
			const int a = ring * row + i;
			const int b = a + 1;
			const int c = a + row;
			const int d = c + 1;
			for (int index : {a, b, d, d, c, a})
				mesh->addIndex(index, surface);
		}
	}
	mesh->createTangents(surface);
	mesh->createBounds(surface);
	return mesh;
}

void fillPortalImage(const ImagePtr &image, const vec3 &color, float time)
{
	const int width = image->getWidth();
	const int height = image->getHeight();

	// The noise goes along a circle: this way there is no seam where the picture closes.
	std::vector<float> cosines(width);
	std::vector<float> sines(width);
	for (int x = 0; x < width; ++x)
	{
		const float angle = 6.2831853f * (float(x) + 0.5f) / float(width);
		cosines[x] = std::cos(angle);
		sines[x] = std::sin(angle);
	}

	const auto to_byte = [](float value) { return int(clamp(value, 0.0f, 1.0f) * 255.0f + 0.5f); };
	for (int y = 0; y < height; ++y)
	{
		const float distance = PORTAL_OUTER * (float(y) + 0.5f) / float(height);
		// How far inside the rim, negative outside it.
		const float inward = 1.0f - distance;
		// Where no tongue reaches there is no need for the noise.
		if (inward >= PORTAL_TONGUE_LONG)
		{
			for (int x = 0; x < width; ++x)
				image->set2D(x, y, Image::Pixel(to_byte(color.x), to_byte(color.y), to_byte(color.z), to_byte(PORTAL_VEIL)));
			continue;
		}
		const float wide_z = inward * PORTAL_WIDE_STRETCH - time * PORTAL_WIDE_SPEED;
		const float fine_z = inward * PORTAL_FINE_STRETCH - time * PORTAL_FINE_SPEED;
		const float outer_fire = inward < 0.0f ? std::exp(inward / PORTAL_EDGE_WIDTH) : 0.0f;
		const float outer_halo = inward < 0.0f
			? PORTAL_HALO * std::exp(inward / PORTAL_HALO_WIDTH) * clamp((PORTAL_OUTER - distance) / PORTAL_HALO_END, 0.0f, 1.0f) : 0.0f;

		for (int x = 0; x < width; ++x)
		{
			const float wide = perlin_noise(cosines[x] * PORTAL_WIDE_SIZE + 11.0f, sines[x] * PORTAL_WIDE_SIZE + 3.0f, wide_z);
			const float fine = perlin_noise(cosines[x] * PORTAL_FINE_SIZE + 5.0f, sines[x] * PORTAL_FINE_SIZE + 19.0f, fine_z);
			const float blob = clamp(0.5f + (wide * PORTAL_WIDE_PART + fine * PORTAL_FINE_PART) * PORTAL_NOISE_GAIN, 0.0f, 1.0f);

			float fire = outer_fire;
			float halo = 0.0f;
			if (inward >= 0.0f)
			{
				const float left = 1.0f - inward / (PORTAL_TONGUE_SHORT + (PORTAL_TONGUE_LONG - PORTAL_TONGUE_SHORT) * blob);
				fire = left > 0.0f ? std::pow(left, PORTAL_TONGUE_POWER) : 0.0f;
			} else
				halo = outer_halo * (0.4f + 0.6f * blob);

			const float hot = fire > 0.0f ? std::pow(fire, PORTAL_HOT_POWER) * PORTAL_HOT : 0.0f;
			const float burning = fire > 0.0f ? std::pow(fire, PORTAL_ALPHA_POWER) : 0.0f;
			const float opacity = inward >= 0.0f ? PORTAL_VEIL + (1.0f - PORTAL_VEIL) * burning : burning + halo;
			image->set2D(x, y, Image::Pixel(to_byte(color.x + (1.0f - color.x) * hot), to_byte(color.y + (1.0f - color.y) * hot),
				to_byte(color.z + (1.0f - color.z) * hot), to_byte(opacity)));
		}
	}
}

void drawArrow(const Vec3 &from, const Vec3 &to, const vec4 &color, float size, bool both_ends)
{
	Vec3 camera;
	vec3 direction;
	const float arrow_length = float(length(to - from));
	if (arrow_length < 1e-4f || !viewportCameraView(camera, direction))
		return;

	const vec3 along = vec3(to - from) * (1.0f / arrow_length);
	vec3 side = cross(along, vec3(camera - to));
	// Looked at along its length there is nothing to see of its sides anyway.
	if (length(side) < 1e-5f)
		return;
	side = normalize(side);

	const float view_height = viewportSizeAt(to, 1.0f) * size;
	const float shaft_width = view_height * ARROW_SHAFT_WIDTH;
	const float head_width = view_height * ARROW_HEAD_WIDTH;
	const float head_length = std::min(view_height * ARROW_HEAD_LENGTH, arrow_length * (both_ends ? 0.33f : 0.5f));
	const Vec3 head_base = to - Vec3(along * head_length);
	const Vec3 tail = both_ends ? from + Vec3(along * head_length) : from;
	const float soft_edge = viewportPixelSizeAt(to) * ARROW_SOFT_EDGE;

	// From the widest and faintest to the arrow itself. The colors are the same, so the order they get
	// sorted in by the visualizer does not change the result. With these strengths the opacity goes up
	// evenly: 1/5, then 2/5 where the next one is on top of it, and so on.
	for (int step = ARROW_SOFT_STEPS; step >= 0; --step)
	{
		const float out = soft_edge * float(step) / float(ARROW_SOFT_STEPS);
		const vec4 layer = with_strength(color, 1.0f / float(step + 1));
		const Vec3 shaft_side = Vec3(side * (shaft_width + out));
		const Vec3 shaft_end = both_ends ? tail : tail - Vec3(along * out);

		// Counterclockwise as seen by the camera.
		Visualizer::renderQuad3D(head_base - shaft_side, shaft_end - shaft_side, shaft_end + shaft_side, head_base + shaft_side, layer, 0.0f, false);
		draw_arrow_head(to, along, side, head_length, head_width, out, layer);
		if (both_ends)
			draw_arrow_head(from, -along, -side, head_length, head_width, out, layer);
	}
}

} // namespace ArtistTool
