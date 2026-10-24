#include "BakeCore.h"
#include "BakeGpu.h"

#include <UnigineDecals.h>
#include <UnigineEngine.h>
#include <UnigineFileSystem.h>
#include <UnigineImage.h>
#include <UnigineLog.h>
#include <UnigineMaterial.h>
#include <UnigineMaterials.h>
#include <UnigineMathLib.h>
#include <UnigineMesh.h>
#include <UnigineNodes.h>
#include <UnigineWorld.h>
#include <UnigineXml.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <string>
#include <thread>
#include <vector>

using namespace Unigine;
using namespace Unigine::Math;

namespace BakeCore
{

namespace
{

//------------------------------------------------------------------------------
// Path helpers (virtual, forward-slash paths).
//------------------------------------------------------------------------------

// Resolves any engine path (including "guid:/..." runtime references) to a
// human-readable virtual asset path like "models/rock.fbx".
String resolveAssetPath(const char *path)
{
	if (String::isEmpty(path))
		return String();
	UGUID guid = FileSystem::getGUID(path);
	if (guid.isValid())
	{
		UGUID asset = FileSystemAssets::resolveAsset(guid);
		String vp = FileSystem::getVirtualPath(asset.isValid() ? asset : guid);
		if (!vp.empty())
			return vp;
	}
	return String(path);
}

bool isGuidPath(const String &path)
{
	return path.size() >= 6 && !strncmp(path.get(), "guid:/", 6);
}

String pathDir(const String &path)
{
	int pos = -1;
	for (int i = 0; i < path.size(); i++)
		if (path[i] == '/' || path[i] == '\\')
			pos = i;
	if (pos < 0)
		return String("");
	return path.substr(0, pos + 1);
}

String pathBaseName(const String &path)
{
	int slash = -1;
	for (int i = 0; i < path.size(); i++)
		if (path[i] == '/' || path[i] == '\\')
			slash = i;
	String name = path.substr(slash + 1);
	int dot = -1;
	for (int i = 0; i < name.size(); i++)
		if (name[i] == '.')
			dot = i;
	if (dot > 0)
		name = name.substr(0, dot);
	return name;
}

//------------------------------------------------------------------------------
// Source (high-poly) data.
//------------------------------------------------------------------------------

struct SourceSurface
{
	ImagePtr albedo;   // may be null
	ImagePtr shading;  // may be null
	ImagePtr normal;   // may be null
	ImagePtr emission; // may be null

	// per-tvertex attributes: world-space normal/tangent decoded from the mesh
	// quaternion basis per vertex, then interpolated linearly across triangles —
	// exactly like the engine passes them as varyings (see mesh vertex.h)
	std::vector<vec2> uv;
	std::vector<vec2> uv1; // second UV set (unique unwrap) — GPU captures unwrap in it
	std::vector<vec3> n, t;
	std::vector<float> sw; // sign of the quat w = binormal orientation
	bool hasUV = false;
	bool hasUV1 = false;
	bool normalInvertG = false;          // import setting of the source normal map
	vec4 uvTransform{1.0f, 1.0f, 0.0f, 0.0f}; // material base UV tiling: uv * xy + zw
	// material constants (used as fallback when textures are missing)
	vec4 albedoColor{1.0f, 1.0f, 1.0f, 1.0f};
	float metalness = 0.0f;
	float roughness = 1.0f;
	bool emissionOn = false; // material "emission" state
	vec4 emissionColor{1.0f, 1.0f, 1.0f, 1.0f};
	float emissionScale = 1.0f;

	// GPU "as rendered" capture of this surface (unwrap gbuffer render)
	BakeGpu::SurfaceCapture gpu;
	bool useGpu = false;
};

struct SourceTri
{
	vec3 p0, e1, e2; // world-space vertex + edges (Moller-Trumbore)
	int surface;
	int t0, t1, t2; // tvertex indices
	int corner;     // base corner slot (k*3) in the surface's index order — GPU atlasUV lookup
};

//------------------------------------------------------------------------------
// BVH.
//------------------------------------------------------------------------------

struct BVHNode
{
	vec3 bmin, bmax;
	int left = -1;  // inner: child index; leaf: first triangle
	int count = 0;  // leaf: triangle count; inner: 0
	int right = -1; // inner: child index
};

class BVH
{
public:
	void build(const std::vector<SourceTri> &tris)
	{
		int n = int(tris.size());
		order.resize(n);
		centroids.resize(n);
		boundsMin.resize(n);
		boundsMax.resize(n);
		for (int i = 0; i < n; i++)
		{
			order[i] = i;
			const SourceTri &tr = tris[i];
			vec3 p1 = tr.p0 + tr.e1;
			vec3 p2 = tr.p0 + tr.e2;
			vec3 bmin = min(tr.p0, min(p1, p2));
			vec3 bmax = max(tr.p0, max(p1, p2));
			boundsMin[i] = bmin;
			boundsMax[i] = bmax;
			centroids[i] = (bmin + bmax) * 0.5f;
		}
		nodes.clear();
		nodes.reserve(size_t(n) * 2 + 1);
		if (n > 0)
			buildNode(0, n);
		centroids.clear();
		centroids.shrink_to_fit();
	}

	// Collects the hit with the lowest score; the scorer gets (triangle index, t, u, v)
	// and returns the hit's score (lower is better). Returns triangle index or -1.
	template <typename Scorer>
	int trace(const std::vector<SourceTri> &tris, const vec3 &orig, const vec3 &dir,
		float tmax, const Scorer &scorer, float &outT, float &outU, float &outV) const
	{
		if (nodes.empty())
			return -1;
		vec3 invDir;
		for (int i = 0; i < 3; i++)
			invDir[i] = (Math::abs(dir[i]) > 1e-20f) ? 1.0f / dir[i] : 1e20f;

		int best = -1;
		float bestScore = 1e30f;
		int stack[64];
		int sp = 0;
		stack[sp++] = 0;
		while (sp > 0)
		{
			const BVHNode &node = nodes[stack[--sp]];
			if (!hitAABB(orig, invDir, node.bmin, node.bmax, tmax))
				continue;
			if (node.count > 0)
			{
				for (int i = 0; i < node.count; i++)
				{
					int tri = order[node.left + i];
					float t, u, v;
					if (intersectTri(orig, dir, tris[tri], tmax, t, u, v))
					{
						float score = scorer(tri, t, u, v);
						if (score < bestScore)
						{
							bestScore = score;
							best = tri;
							outT = t;
							outU = u;
							outV = v;
						}
					}
				}
			}
			else
			{
				if (sp < 62)
				{
					stack[sp++] = node.left;
					stack[sp++] = node.right;
				}
			}
		}
		return best;
	}

private:
	static bool hitAABB(const vec3 &o, const vec3 &invD, const vec3 &bmin, const vec3 &bmax, float tmax)
	{
		float t0 = 0.0f, t1 = tmax;
		for (int i = 0; i < 3; i++)
		{
			float tn = (bmin[i] - o[i]) * invD[i];
			float tf = (bmax[i] - o[i]) * invD[i];
			if (tn > tf)
			{
				float tmp = tn;
				tn = tf;
				tf = tmp;
			}
			t0 = Math::max(t0, tn);
			t1 = Math::min(t1, tf);
			if (t0 > t1)
				return false;
		}
		return true;
	}

	static bool intersectTri(const vec3 &orig, const vec3 &dir, const SourceTri &tr,
		float tmax, float &t, float &u, float &v)
	{
		vec3 pvec = cross(dir, tr.e2);
		float det = dot(tr.e1, pvec);
		if (Math::abs(det) < 1e-12f)
			return false;
		float inv = 1.0f / det;
		vec3 tvec = orig - tr.p0;
		u = dot(tvec, pvec) * inv;
		if (u < -1e-5f || u > 1.0f + 1e-5f)
			return false;
		vec3 qvec = cross(tvec, tr.e1);
		v = dot(dir, qvec) * inv;
		if (v < -1e-5f || u + v > 1.0f + 1e-5f)
			return false;
		t = dot(tr.e2, qvec) * inv;
		return t >= 0.0f && t <= tmax;
	}

	int buildNode(int start, int count)
	{
		int nodeIndex = int(nodes.size());
		nodes.push_back(BVHNode());

		vec3 bmin(1e30f), bmax(-1e30f);
		for (int i = start; i < start + count; i++)
		{
			bmin = min(bmin, boundsMin[order[i]]);
			bmax = max(bmax, boundsMax[order[i]]);
		}
		nodes[nodeIndex].bmin = bmin;
		nodes[nodeIndex].bmax = bmax;

		if (count <= 4)
		{
			nodes[nodeIndex].left = start;
			nodes[nodeIndex].count = count;
			return nodeIndex;
		}

		vec3 extent = bmax - bmin;
		int axis = 0;
		if (extent.y > extent.x)
			axis = 1;
		if (extent.z > extent[axis])
			axis = 2;

		int mid = start + count / 2;
		std::nth_element(order.begin() + start, order.begin() + mid, order.begin() + start + count,
			[this, axis](int a, int b) { return centroids[a][axis] < centroids[b][axis]; });

		int left = buildNode(start, mid - start);
		int right = buildNode(mid, start + count - mid);
		nodes[nodeIndex].left = left;
		nodes[nodeIndex].right = right;
		nodes[nodeIndex].count = 0;
		return nodeIndex;
	}

	std::vector<BVHNode> nodes;
	std::vector<int> order;
	std::vector<vec3> centroids;
	std::vector<vec3> boundsMin, boundsMax;
};

//------------------------------------------------------------------------------
// Target (low-poly) data.
//------------------------------------------------------------------------------

struct TargetTri
{
	vec3 p0, p1, p2;    // world-space positions
	vec3 n0, n1, n2;    // world-space shading normals
	vec3 t0, t1, t2;    // world-space tangents
	float signW;        // binormal orientation (sign of quat w)
	bool mirrored = false; // the part is mirrored (negative scale): its winding is reversed
	vec3 r0, r1, r2;    // world-space smoothed cage normals (ray directions)
	float s0 = 1.0f, s1 = 1.0f, s2 = 1.0f; // how many frontal distances the cage is off the corner (CageNormals::shell)
	vec3 faceN;         // world-space geometric normal (skew-mask ray direction)
	vec2 uv0, uv1, uv2; // UV0
	int group = 0;      // rays trace only into this bake group's high-poly set
	// cage of the low-poly part this triangle belongs to (meters): how far above
	// the surface the ray starts and how far below it keeps searching
	float frontal = 0.05f;
	float rear = 0.05f;
};

struct TBN
{
	vec3 t, b, n;
};

// Reproduces the engine's per-pixel normalizationTBN() (core/materials/shaders/api/matrix.h):
// normalize interpolated T and N, Gram-Schmidt T against N, B = cross(N, T) * sign.
TBN buildTBN(const vec3 &nInterp, const vec3 &tInterp, float signBinormal)
{
	TBN r;
	r.n = normalize(nInterp);
	vec3 t = normalize(tInterp);
	r.t = normalize(t - r.n * dot(t, r.n));
	r.b = normalize(cross(r.n, r.t)) * signBinormal;
	return r;
}

// Key for merging vertices by position (hard edges and surface borders share
// one smoothed cage normal, so rays do not split apart there).
struct QuantizedPos
{
	long long x, y, z;
	bool operator<(const QuantizedPos &o) const
	{
		if (x != o.x)
			return x < o.x;
		if (y != o.y)
			return y < o.y;
		return z < o.z;
	}
};

// The emission is gathered linear (as the renderer adds it up: the color of the map
// decoded, times the scale) and encoded as sRGB when the map is written.
float srgbToLinear(float c)
{
	return c <= 0.04045f ? c / 12.92f : Math::pow((c + 0.055f) / 1.055f, 2.4f);
}

float linearToSrgb(float c)
{
	return c <= 0.0031308f ? c * 12.92f : 1.055f * Math::pow(c, 1.0f / 2.4f) - 0.055f;
}

vec3 srgbToLinear(const vec3 &c)
{
	return vec3(srgbToLinear(c.x), srgbToLinear(c.y), srgbToLinear(c.z));
}

QuantizedPos quantize(const vec3 &p)
{
	const double q = 1.0 / 1e-5; // 0.01 mm cells
	return {std::llround(double(p.x) * q), std::llround(double(p.y) * q), std::llround(double(p.z) * q)};
}

// The cage, as Shrink/Fatten (Alt+S) with Offset Even makes it in Blender: the vertices,
// merged by position so that the cage does not split at hard edges and UV seams, go along
// the face normals around them weighted by the angles of the corners there (a narrow face
// next to large ones - the edge of a roof - counts as much as they do; weighted by the
// area it would be pushed along them, not off), and as far as it takes for every face
// around to be moved off by the whole distance (1 / cos of the angle between the vertex
// normal and the face normal, weighted by the angles too).
class CageNormals
{
public:
	void addTriangle(const vec3 &p0, const vec3 &p1, const vec3 &p2)
	{
		const vec3 fn = cross(p1 - p0, p2 - p0);
		if (length2(fn) < 1e-20f)
			return;
		const vec3 unit = normalize(fn);
		const vec3 p[3] = {p0, p1, p2};
		for (int k = 0; k < 3; k++)
		{
			const vec3 a = p[(k + 1) % 3] - p[k];
			const vec3 b = p[(k + 2) % 3] - p[k];
			if (length2(a) < 1e-20f || length2(b) < 1e-20f)
				continue;
			const float angle = Math::acos(Math::clamp(dot(normalize(a), normalize(b)), -1.0f, 1.0f));
			if (angle <= 0.0f)
				continue;
			Corner &corner = corners_[quantize(p[k])];
			corner.sum += unit * angle;
			corner.faces.push_back({unit, angle});
		}
	}

	// The direction the cage goes off the vertex at 'p'.
	vec3 at(const vec3 &p, const vec3 &fallback) const
	{
		const auto it = corners_.find(quantize(p));
		if (it == corners_.end() || length2(it->second.sum) < 1e-20f)
			return fallback;
		return normalize(it->second.sum);
	}

	// How many times the distance the vertex at 'p' goes along at(): 1 on a flat surface,
	// about 1.41 on a right-angled edge. Kept within 3, where the faces fold back on
	// each other.
	float shell(const vec3 &p) const
	{
		const auto it = corners_.find(quantize(p));
		if (it == corners_.end() || length2(it->second.sum) < 1e-20f)
			return 1.0f;
		const vec3 n = normalize(it->second.sum);
		float sum = 0.0f;
		float weight = 0.0f;
		for (const Face &face : it->second.faces)
		{
			sum += face.angle / Math::max(dot(n, face.normal), 1.0f / 3.0f);
			weight += face.angle;
		}
		return weight > 0.0f ? Math::clamp(sum / weight, 1.0f, 3.0f) : 1.0f;
	}

private:
	struct Face
	{
		vec3 normal;
		float angle;
	};
	struct Corner
	{
		vec3 sum{0.0f};
		std::vector<Face> faces;
	};
	std::map<QuantizedPos, Corner> corners_;
};

//------------------------------------------------------------------------------
// Texture loading.
//------------------------------------------------------------------------------

ImagePtr loadTexture(const char *path, std::vector<std::pair<String, ImagePtr>> &cache)
{
	if (!path || !*path)
		return ImagePtr();
	for (const auto &entry : cache)
		if (entry.first == path)
			return entry.second;

	ImagePtr img = Image::create();
	if (!img->load(path))
	{
		Log::warning("Baker: failed to load texture \"%s\"\n", path);
		img = ImagePtr();
	}
	else
	{
		if (img->isCompressedFormat())
			img->decompress();
		Log::message("Baker: loaded texture \"%s\" (%dx%d, %d channels)\n",
			path, img->getWidth(), img->getHeight(), img->getNumChannels());
	}
	cache.push_back({String(path), img});
	return img;
}

// Reads the "invert_g" import parameter from the source asset's .meta file.
// Runtime ("guid://...") textures already have all import operations applied,
// so the flip must be respected only when the raw source file was loaded.
bool sourceNormalInvertG(const char *path)
{
	if (String::isEmpty(path) || !strncmp(path, "guid://", 7))
		return false;

	String metaPath = String(Engine::get()->getDataPath());
	if (!metaPath.empty() && metaPath[metaPath.size() - 1] != '/' && metaPath[metaPath.size() - 1] != '\\')
		metaPath += "/";
	metaPath += resolveAssetPath(path) + ".meta";

	XmlPtr xml = Xml::create();
	if (!xml->load(metaPath.get()))
		return false;
	XmlPtr params = xml->getChild("parameters");
	if (!params)
		return false;
	for (int i = 0; i < params->getNumChildren(); i++)
	{
		XmlPtr p = params->getChild(i);
		const char *argName = p ? p->getArg("name") : nullptr;
		if (argName && !strcmp(argName, "invert_g"))
			return p->getIntData() != 0;
	}
	return false;
}

//------------------------------------------------------------------------------
// Decals. Decals are separate projector nodes (DecalOrtho/Proj/Mesh) that the
// engine blends into the gbuffer of whatever geometry they cover. To bake them
// we sample each covering decal at the world hit point of every texel's ray and
// blend it over the base material — the same result the deferred decal pass
// produces, evaluated on the CPU. All engine access happens here (main thread,
// bake start); the workers only read the plain POD below.
//------------------------------------------------------------------------------
// A mesh decal's footprint: its triangles in the decal's object-space XY plane
// (the projection plane) with their authored UVs, plus a uniform grid for fast
// point location. A world hit point projected to object XY is looked up here to
// get the mesh UV to sample the decal material with.
struct DTri
{
	vec2 p[3];
	vec2 uv[3];
	float z[3]; // object-space Z of each corner (the decal mesh surface height)
};
struct MeshFootprint
{
	std::vector<DTri> tris;
	vec2 bbMin{1e30f, 1e30f}, bbMax{-1e30f, -1e30f};
	float zMin = 1e30f, zMax = -1e30f; // mesh extent along the projection axis
	int gw = 1, gh = 1;
	std::vector<std::vector<int>> cells;

	void build()
	{
		int n = int(tris.size());
		int g = Math::clamp(int(Math::sqrt(float(n))), 1, 64);
		gw = gh = g;
		cells.assign(size_t(gw) * gh, {});
		vec2 span = bbMax - bbMin;
		span.x = Math::max(span.x, 1e-6f);
		span.y = Math::max(span.y, 1e-6f);
		for (int i = 0; i < n; i++)
		{
			const DTri &t = tris[i];
			vec2 tmn = min(t.p[0], min(t.p[1], t.p[2]));
			vec2 tmx = max(t.p[0], max(t.p[1], t.p[2]));
			int x0 = Math::clamp(int((tmn.x - bbMin.x) / span.x * gw), 0, gw - 1);
			int x1 = Math::clamp(int((tmx.x - bbMin.x) / span.x * gw), 0, gw - 1);
			int y0 = Math::clamp(int((tmn.y - bbMin.y) / span.y * gh), 0, gh - 1);
			int y1 = Math::clamp(int((tmx.y - bbMin.y) / span.y * gh), 0, gh - 1);
			for (int y = y0; y <= y1; y++)
				for (int x = x0; x <= x1; x++)
					cells[y * gw + x].push_back(i);
		}
	}

	// Locates the point in the footprint. Returns the interpolated UV and the
	// mesh surface Z there (the decal's height at that XY). Among overlapping
	// triangles picks the one whose Z is closest to refZ (the ray hit) so the
	// decal binds to the nearest surface, not a foreign one behind it.
	bool sample(const vec2 &p, float refZ, vec2 &outUV, float &outZ) const
	{
		if (p.x < bbMin.x || p.x > bbMax.x || p.y < bbMin.y || p.y > bbMax.y)
			return false;
		vec2 span = bbMax - bbMin;
		span.x = Math::max(span.x, 1e-6f);
		span.y = Math::max(span.y, 1e-6f);
		int cx = Math::clamp(int((p.x - bbMin.x) / span.x * gw), 0, gw - 1);
		int cy = Math::clamp(int((p.y - bbMin.y) / span.y * gh), 0, gh - 1);
		bool found = false;
		float best = 1e30f;
		for (int idx : cells[cy * gw + cx])
		{
			const DTri &t = tris[idx];
			vec2 v0 = t.p[1] - t.p[0], v1 = t.p[2] - t.p[0], v2 = p - t.p[0];
			float d00 = dot(v0, v0), d01 = dot(v0, v1), d11 = dot(v1, v1);
			float d20 = dot(v2, v0), d21 = dot(v2, v1);
			float den = d00 * d11 - d01 * d01;
			if (Math::abs(den) < 1e-12f)
				continue;
			float v = (d11 * d20 - d01 * d21) / den;
			float w = (d00 * d21 - d01 * d20) / den;
			float u = 1.0f - v - w;
			if (u < -1e-4f || v < -1e-4f || w < -1e-4f)
				continue;
			float z = t.z[0] * u + t.z[1] * v + t.z[2] * w;
			float dz = Math::abs(z - refZ);
			if (dz < best)
			{
				best = dz;
				outZ = z;
				outUV = t.uv[0] * u + t.uv[1] * v + t.uv[2] * w;
				found = true;
			}
		}
		return found;
	}
};

struct DecalSample
{
	mat4 iworld;      // world -> decal object space
	mat4 proj;        // object space -> clip (ortho/proj)
	bool perspective; // DecalProj divides by w; ortho does not
	vec3 axisX, axisY, axisZ; // decal world axes (Z = projector up = out of surface)
	vec3 bbMin, bbMax;        // world bbox for the per-texel cull
	ImagePtr albedo, normal, shading;
	vec4 albedoColor{1.0f, 1.0f, 1.0f, 1.0f};
	float opacity = 1.0f;
	bool normalInvertG = false;
	int order = 0;
	// scalar shading parameters (multiply the shading texture, like mesh_base)
	float metalnessParam = 0.0f;
	float roughnessParam = 1.0f;
	bool hasShadingParams = false;
	// mesh decals: object-space footprint + half thickness along Z (null = ortho/proj)
	std::shared_ptr<MeshFootprint> footprint;
	float halfZ = 1e30f;
	String name;
};

// A compact bake report mirrored to %TEMP%/baker_last_bake.txt (overwritten each
// bake) so the decal discovery + hit stats can be inspected without the editor
// console. Best-effort; failures are silent.
std::ofstream g_report;
void openReport()
{
	std::error_code ec;
	auto path = std::filesystem::temp_directory_path(ec) / "baker_last_bake.txt";
	g_report.open(path, std::ios::out | std::ios::trunc);
}
template <typename... A> void report(const char *fmt, A... a)
{
	Log::message(fmt, a...);
	if (g_report.is_open())
	{
		char buf[1024];
		std::snprintf(buf, sizeof(buf), fmt, a...);
		g_report << buf;
		g_report.flush();
	}
}

// A world transform as a float one about 'origin': the geometry of the bake is kept
// near the origin of its own, so that a model far from the origin of the world (a
// million units away, where a float steps by 6 cm) loses no precision.
mat4 relativeTransform(const Mat4 &world, const Vec3 &origin)
{
	return mat4(Math::translate(-origin) * world);
}

std::vector<DecalSample> collectDecals(const Vec3 &origin, const vec3 &hiMin, const vec3 &hiMax,
	const std::vector<int> &explicitIds, const std::vector<NodePtr> &highNodes, float decalDistance,
	std::vector<std::pair<String, ImagePtr>> &textureCache)
{
	std::vector<DecalSample> out;
	int totalDecals = 0;
	const bool autoMode = explicitIds.empty();

	// Gather candidate decal nodes.
	// Auto: the decals live in the SAME instance hierarchy as the high-poly
	// (that's how the artist placed them), so walk the subtrees of the high
	// nodes' top ancestors — these are correctly-placed instance nodes, unlike a
	// node reference's shared template (which sits at the prefab origin).
	// Explicit: whatever the user picked, resolved by id.
	std::vector<NodePtr> candidates;
	std::set<int> visited;
	std::vector<NodePtr> stack;
	if (!autoMode)
	{
		for (int id : explicitIds)
			if (NodePtr n = World::getNodeByID(id))
				stack.push_back(n);
	}
	else
	{
		for (const NodePtr &h : highNodes)
		{
			NodePtr top = h;
			while (top && top->getParent())
				top = top->getParent();
			if (top)
				stack.push_back(top);
		}
	}
	while (!stack.empty())
	{
		NodePtr n = stack.back();
		stack.pop_back();
		if (!n || !visited.insert(n->getID()).second)
			continue;
		for (int i = 0; i < n->getNumChildren(); i++)
			stack.push_back(n->getChild(i));
		// descend into node reference contents too (decals nested in a prefab —
		// e.g. r5_interior_digital_clock — are otherwise unreachable)
		if (n->getType() == Node::NODE_REFERENCE)
			if (Ptr<NodeReference> nr = checked_ptr_cast<NodeReference>(n))
				stack.push_back(nr->getReference());
		candidates.push_back(n);
	}
	report("Baker: decal search scanned %d nodes (%s)\n", int(candidates.size()),
		autoMode ? "auto: high-poly hierarchy" : "explicit selection");

	for (const NodePtr &n : candidates)
	{
		// hiding is viewport-only (like meshes) — bake decals even when disabled
		if (!n || !n->isDecal())
			continue;
		totalDecals++;
		Ptr<Decal> d = checked_ptr_cast<Decal>(n);
		if (!d)
			continue;

		// world bbox: cheap reject of far decals in auto mode; explicit picks
		// are always kept (the user chose them)
		WorldBoundBox wbb = n->getWorldBoundBox();
		// about the origin of the bake, as the high-poly bounds are
		auto toV3 = [&origin](const Vec3 &v) { return vec3(v - origin); };
		Vec3 wpos = n->getWorldTransform().getColumn3(3);
		vec3 dmin = toV3(wbb.minimum), dmax = toV3(wbb.maximum);
		report("Baker: [decal] \"%s\" (%s) enabled=%d valid=%d pos[%.2f %.2f %.2f] bbox[%.2f %.2f "
			   "%.2f]..[%.2f %.2f %.2f]\n",
			n->getName(), n->getTypeName(), n->isEnabled() ? 1 : 0, wbb.isValid() ? 1 : 0,
			float(wpos.x), float(wpos.y), float(wpos.z), dmin.x, dmin.y, dmin.z, dmax.x, dmax.y,
			dmax.z);
		if (!wbb.isValid())
		{
			// disabled decals can report an invalid/empty world bbox; fall back to
			// the transform position so the overlap cull still has something
			if (autoMode)
				continue;
			dmin = dmax = toV3(wpos);
		}
		if (autoMode
			&& (dmax.x < hiMin.x || dmin.x > hiMax.x || dmax.y < hiMin.y || dmin.y > hiMax.y
				|| dmax.z < hiMin.z || dmin.z > hiMax.z))
			continue;

		DecalSample s;
		Mat4 world = Math::translate(-origin) * d->getWorldTransform();
		s.iworld = mat4(inverse(world));

		// getProjection() lives on the concrete decal subclass, not on Decal.
		// DecalMesh has no projection matrix — its footprint is the mesh itself,
		// looked up in object-space XY.
		int dtype = n->getType();
		s.perspective = false;
		if (dtype == Node::DECAL_ORTHO)
			s.proj = checked_ptr_cast<DecalOrtho>(n)->getProjection();
		else if (dtype == Node::DECAL_PROJ)
		{
			s.proj = checked_ptr_cast<DecalProj>(n)->getProjection();
			s.perspective = true;
		}
		else if (dtype == Node::DECAL_MESH)
		{
			Ptr<DecalMesh> dm = checked_ptr_cast<DecalMesh>(n);
			Ptr<ConstMesh> dmesh = dm ? dm->getMeshForceRAM() : Ptr<ConstMesh>();
			if (!dmesh)
			{
				report("Baker: mesh decal \"%s\" has no mesh, skipped\n", n->getName());
				continue;
			}
			auto fp = std::make_shared<MeshFootprint>();
			vec3 lmin(1e30f, 1e30f, 1e30f), lmax(-1e30f, -1e30f, -1e30f);
			for (int su = 0; su < dmesh->getNumSurfaces(); su++)
			{
				if (dmesh->getNumTexCoords0(su) <= 0)
					continue;
				const Vector<int> &ci = dmesh->getCIndices(su);
				const Vector<int> &ti = dmesh->getTIndices(su);
				int nt = ci.size() / 3;
				for (int k = 0; k < nt; k++)
				{
					DTri t;
					for (int j = 0; j < 3; j++)
					{
						vec3 v = dmesh->getVertex(ci[k * 3 + j], su);
						t.p[j] = vec2(v.x, v.y); // decal projects along local Z
						t.z[j] = v.z;
						t.uv[j] = dmesh->getTexCoord0(ti[k * 3 + j], su);
						fp->bbMin = min(fp->bbMin, t.p[j]);
						fp->bbMax = max(fp->bbMax, t.p[j]);
						lmin = min(lmin, v);
						lmax = max(lmax, v);
					}
					fp->tris.push_back(t);
				}
			}
			if (fp->tris.empty())
			{
				report("Baker: mesh decal \"%s\" has no UV triangles, skipped\n", n->getName());
				continue;
			}
			fp->zMin = lmin.z;
			fp->zMax = lmax.z;
			fp->build();
			s.footprint = fp;
			float r = d->getRadius();
			// how far a target surface may sit from the decal mesh surface along
			// the projection axis and still receive the decal (user setting).
			// Screen-projection decals hug the mesh, so a small band binds only to
			// the nearest geometry; a wider one bleeds onto foreign parts.
			s.halfZ = Math::max(decalDistance, 1e-4f);
			report("Baker:   mesh footprint local bbox [%.3f %.3f %.3f]..[%.3f %.3f %.3f] "
				   "tris=%d radius=%.3f halfZ=%.3f\n",
				lmin.x, lmin.y, lmin.z, lmax.x, lmax.y, lmax.z, int(fp->tris.size()), r, s.halfZ);
		}
		else
			continue;

		mat3 rot = mat3(mat4(world));
		s.axisX = normalize(rot * vec3(1.0f, 0.0f, 0.0f));
		s.axisY = normalize(rot * vec3(0.0f, 1.0f, 0.0f));
		s.axisZ = normalize(rot * vec3(0.0f, 0.0f, 1.0f));
		s.bbMin = dmin;
		s.bbMax = dmax;
		s.opacity = d->getOpacity();
		s.name = n->getName();

		if (MaterialPtr m = d->getMaterial())
		{
			if (m->findParameter("albedo_color") >= 0)
				s.albedoColor = m->getParameterFloat4("albedo_color");
			s.albedo = loadTexture(m->getTexturePath("albedo"), textureCache);
			s.shading = loadTexture(m->getTexturePath("shading"), textureCache);
			s.normal = loadTexture(m->getTexturePath("normal"), textureCache);
			if (s.normal)
				s.normalInvertG = sourceNormalInvertG(m->getTexturePath("normal"));
			if (m->findParameter("render_order") >= 0)
				s.order = int(m->getParameterFloat("render_order"));
			// scalar metalness/roughness multipliers (mesh_base convention)
			if (m->findParameter("metalness") >= 0)
			{
				s.metalnessParam = m->getParameterFloat("metalness");
				s.hasShadingParams = true;
			}
			if (m->findParameter("roughness") >= 0)
			{
				s.roughnessParam = m->getParameterFloat("roughness");
				s.hasShadingParams = true;
			}
		}

		report("Baker: decal \"%s\" (%s) opacity=%.2f%s%s%s bbox[%.2f %.2f %.2f]..[%.2f %.2f %.2f]\n",
			n->getName(), n->getTypeName(), s.opacity, s.albedo ? " alb" : "",
			s.normal ? " nrm" : "", s.shading ? " sh" : "", dmin.x, dmin.y, dmin.z, dmax.x, dmax.y,
			dmax.z);
		out.push_back(std::move(s));
	}

	// render order: lower first, later decals paint on top (stable = discovery order)
	std::stable_sort(out.begin(), out.end(),
		[](const DecalSample &a, const DecalSample &b) { return a.order < b.order; });

	report("Baker: high-poly bbox [%.2f %.2f %.2f]..[%.2f %.2f %.2f]\n", hiMin.x, hiMin.y, hiMin.z,
		hiMax.x, hiMax.y, hiMax.z);
	report("Baker: %d enabled decal(s) in world, %d overlap the high-poly\n", totalDecals,
		int(out.size()));
	return out;
}

// Blends every covering decal over the base albedo/shading/world-normal at one
// world hit point, in render order. Mutates the three in place. Returns the
// number of decals that actually contributed (for diagnostics).
int applyDecals(const std::vector<DecalSample> &decals, const vec3 &worldHit, vec4 &albedo,
	vec4 &shading, vec3 &worldN, std::vector<std::atomic<long long>> *hits)
{
	int applied = 0;
	for (size_t di = 0; di < decals.size(); di++)
	{
		const DecalSample &d = decals[di];
		if (worldHit.x < d.bbMin.x || worldHit.x > d.bbMax.x || worldHit.y < d.bbMin.y
			|| worldHit.y > d.bbMax.y || worldHit.z < d.bbMin.z || worldHit.z > d.bbMax.z)
			continue;

		vec4 po = d.iworld * vec4(worldHit, 1.0f);
		vec2 uv;
		if (d.footprint)
		{
			// mesh decal: locate the object-space XY point in the footprint, then
			// bind only to surfaces within the projection depth of the decal mesh
			// surface there (prevents projecting onto foreign geometry that merely
			// shares the same XY column further along Z — the "ghost" projections)
			float meshZ;
			if (!d.footprint->sample(vec2(po.x, po.y), po.z, uv, meshZ))
				continue;
			if (Math::abs(po.z - meshZ) > d.halfZ)
				continue;
		}
		else
		{
			vec4 clip = d.proj * po;
			vec3 ndc;
			if (d.perspective)
			{
				if (clip.w <= 1e-6f)
					continue;
				ndc = vec3(clip.x, clip.y, clip.z) / clip.w;
			}
			else
				ndc = vec3(clip.x, clip.y, clip.z);
			// inside the projection box? clip x/y in [-1,1]; depth kept lenient
			// ([-1,1] covers both D3D [0,1] and GL [-1,1] conventions — the world
			// bbox cull already bounds the range, decals are thin)
			if (ndc.x < -1.0f || ndc.x > 1.0f || ndc.y < -1.0f || ndc.y > 1.0f || ndc.z < -1.0f
				|| ndc.z > 1.0f)
				continue;
			// texture UV in the decal (V flipped for Image::get2D row order)
			uv = vec2(ndc.x * 0.5f + 0.5f, 1.0f - (ndc.y * 0.5f + 0.5f));
		}

		// surface must face the projector (projector shoots along -axisZ)
		float cosA = dot(worldN, d.axisZ);
		if (cosA <= 0.0f)
			continue;

		vec4 dalb = d.albedoColor;
		if (d.albedo)
			dalb = d.albedo->toVec4(d.albedo->get2D(uv)) * d.albedoColor;
		float a = dalb.w * d.opacity * saturate(cosA);
		if (a <= 0.001f)
			continue;
		applied++;
		if (hits)
			(*hits)[di].fetch_add(1, std::memory_order_relaxed);

		albedo = vec4(albedo.x + (dalb.x - albedo.x) * a, albedo.y + (dalb.y - albedo.y) * a,
			albedo.z + (dalb.z - albedo.z) * a, albedo.w);

		// shading (_sh: R=metalness, G=roughness, B=specular, A=microfiber). The
		// scalar metalness/roughness parameters multiply the texture channels
		// (mesh_base convention); with no texture they are used directly.
		if (d.shading || d.hasShadingParams)
		{
			vec4 ds(d.metalnessParam, d.roughnessParam, 0.5f, 0.0f);
			if (d.shading)
			{
				vec4 t = d.shading->toVec4(d.shading->get2D(uv));
				ds = vec4(t.x * d.metalnessParam, t.y * d.roughnessParam, t.z, t.w);
			}
			shading = vec4(shading.x + (ds.x - shading.x) * a, shading.y + (ds.y - shading.y) * a,
				shading.z + (ds.z - shading.z) * a, shading.w + (ds.w - shading.w) * a);
		}

		if (d.normal)
		{
			vec4 nm = d.normal->toVec4(d.normal->get2D(uv));
			float nx = nm.x * 2.0f - 1.0f;
			float ny = nm.y * 2.0f - 1.0f;
			// decal normal maps use the opposite green-channel convention here —
			// flip Y so the relief is not inverted. Not by Invert G: that is of the map
			// written (flipping here too would cancel it out).
			if (!d.normalInvertG)
				ny = -ny;
			float nz = Math::sqrt(Math::max(1.0f - nx * nx - ny * ny, 0.0f));
			// decal tangent frame: X/Y in the projection plane, Z out of the surface
			vec3 dWN = normalize(d.axisX * nx + d.axisY * ny + d.axisZ * nz);
			worldN = normalize(worldN + (dWN - worldN) * a);
		}
	}
	return applied;
}

} // namespace

//------------------------------------------------------------------------------
// Baking.
//------------------------------------------------------------------------------

Result bake(const std::vector<BakeGroup> &groups,
	const Settings &settings, const ProgressFn &progress)
{
	Result result;
	// Everything is baked about the first low-poly part (see relativeTransform()).
	const Vec3 origin = !groups.empty() && !groups[0].lows.empty() && groups[0].lows[0]
		? groups[0].lows[0]->getWorldPosition() : Vec3_zero;

	auto fail = [&result](const char *msg) -> Result & {
		result.success = false;
		result.error = msg;
		Log::error("Baker: %s\n", msg);
		return result;
	};

	if (groups.empty())
		return fail("Models are not set.");
	for (const BakeGroup &g : groups)
		if (g.highs.empty() || g.lows.empty())
			return fail("A bake group has no high-poly or no low-poly set.");

	// diagnostics report (mirrors key log lines to %TEMP%/baker_last_bake.txt)
	openReport();

	const Ptr<ObjectMeshStatic> &firstHigh = groups[0].highs[0];
	const Ptr<ObjectMeshStatic> &firstLow = groups[0].lows[0];

	Log::message("Baker: bake start: resolution=%d frontal=%f rear=%f supersamples=%d groups=%d high=\"%s\" low=\"%s\"\n",
		settings.resolution, settings.frontalDistance, settings.rearDistance, settings.supersamples,
		int(groups.size()), firstHigh->getName(), firstLow->getName());

	if (!progress(0, "Reading geometry..."))
	{
		result.cancelled = true;
		return result;
	}

	//--------------------------------------------------------------------------
	// Extract high-poly geometry, materials and textures. Surfaces of all
	// groups and objects share one consecutive numbering (matches the capture
	// indexing). Triangles are kept per group: each group traces only its own.
	//--------------------------------------------------------------------------
	std::vector<SourceSurface> srcSurfaces;
	std::vector<std::vector<SourceTri>> groupTris(groups.size());
	std::vector<std::pair<String, ImagePtr>> textureCache;
	bool anyAlbedo = false, anyShading = false, anyNormalMap = false;

	for (size_t gi = 0; gi < groups.size(); gi++)
	{
	std::vector<SourceTri> &srcTris = groupTris[gi];
	for (const Ptr<ObjectMeshStatic> &high : groups[gi].highs)
	{
	Ptr<ConstMesh> highMesh = high->getMeshForceRAM();
	if (!highMesh)
	{
		Log::warning("Baker: cannot get geometry of \"%s\", skipping the object\n", high->getName());
		continue;
	}
	mat4 highTm = relativeTransform(high->getWorldTransform(), origin);
	mat3 highNm = transpose(inverse(mat3(highTm)));
	const int surfBase = int(srcSurfaces.size());
	srcSurfaces.resize(surfBase + highMesh->getNumSurfaces());

	for (int ls = 0; ls < highMesh->getNumSurfaces(); ls++)
	{
		const int s = surfBase + ls; // flat surface index across all objects
		SourceSurface &surf = srcSurfaces[s];

		Log::message("Baker: high-poly \"%s\" surface %d \"%s\": %d triangles, enabled=%d, viewport_mask=0x%08x, lod=[%g..%g], material=\"%s\"\n",
			high->getName(), s, highMesh->getSurfaceName(ls), highMesh->getNumCIndices(ls) / 3, high->isEnabled(ls) ? 1 : 0,
			high->getViewportMask(ls), high->getMinVisibleDistance(ls), high->getMaxVisibleDistance(ls),
			high->getMaterial(ls) ? high->getMaterial(ls)->getFilePath().get() : "<none>");
		if (g_report.is_open())
			g_report << "Baker: surface " << s << " material=\""
					 << (high->getMaterial(ls) ? high->getMaterial(ls)->getFilePath().get() : "<none>")
					 << "\"\n";

		// bake only what is actually visible up close:
		// skip disabled surfaces, surfaces hidden by viewport mask,
		// and LOD shells that are not visible at zero distance
		if (!isSurfaceBakeable(high, ls))
		{
			Log::message("Baker: surface %d is not visible up close (disabled/mask/LOD), skipping\n", s);
			continue;
		}

		MaterialPtr mat = high->getMaterial(ls);
		if (mat)
		{
			if (mat->findParameter("albedo_color") >= 0)
				surf.albedoColor = mat->getParameterFloat4("albedo_color");
			if (mat->findParameter("metalness") >= 0)
				surf.metalness = mat->getParameterFloat("metalness");
			if (mat->findParameter("roughness") >= 0)
				surf.roughness = mat->getParameterFloat("roughness");
			if (mat->findParameter("uv_transform") >= 0)
			{
				surf.uvTransform = mat->getParameterFloat4("uv_transform");
				if (surf.uvTransform.x != 1.0f || surf.uvTransform.y != 1.0f
					|| surf.uvTransform.z != 0.0f || surf.uvTransform.w != 0.0f)
					Log::message("Baker: surface %d uses uv_transform %g %g %g %g\n", s,
						surf.uvTransform.x, surf.uvTransform.y, surf.uvTransform.z, surf.uvTransform.w);
			}
			if (mat->findTexture("albedo") >= 0)
				surf.albedo = loadTexture(mat->getTexturePath("albedo"), textureCache);
			if (mat->findTexture("shading") >= 0)
				surf.shading = loadTexture(mat->getTexturePath("shading"), textureCache);
			if (mat->findTexture("normal") >= 0)
			{
				surf.normal = loadTexture(mat->getTexturePath("normal"), textureCache);
				if (surf.normal)
				{
					surf.normalInvertG = sourceNormalInvertG(mat->getTexturePath("normal"));
					if (surf.normalInvertG)
						Log::message("Baker: surface %d normal map is imported with invert_g, compensating\n", s);
				}
			}
			if (settings.bakeEmission && mat->findState("emission") >= 0 && mat->getState("emission"))
			{
				surf.emissionOn = true;
				if (mat->findParameter("emission_color") >= 0)
					surf.emissionColor = mat->getParameterFloat4("emission_color");
				if (mat->findParameter("emission_scale") >= 0)
					surf.emissionScale = mat->getParameterFloat("emission_scale");
				if (mat->findTexture("emission") >= 0)
					surf.emission = loadTexture(mat->getTexturePath("emission"), textureCache);
			}
		}
		anyAlbedo |= (bool)surf.albedo;
		anyShading |= (bool)surf.shading;
		anyNormalMap |= (bool)surf.normal;

		const Vector<int> &cind = highMesh->getCIndices(ls);
		const Vector<int> &tind = highMesh->getTIndices(ls);
		int numTris = cind.size() / 3;
		if (numTris <= 0)
			continue;

		int numT = highMesh->getNumTangents(ls);
		surf.hasUV = highMesh->getNumTexCoords0(ls) > 0;
		surf.uv.resize(surf.hasUV ? highMesh->getNumTexCoords0(ls) : 0);
		for (int i = 0; i < int(surf.uv.size()); i++)
			surf.uv[i] = highMesh->getTexCoord0(i, ls);
		surf.hasUV1 = surf.hasUV && highMesh->getNumTexCoords1(ls) >= int(surf.uv.size());
		surf.uv1.resize(surf.hasUV1 ? surf.uv.size() : 0);
		for (int i = 0; i < int(surf.uv1.size()); i++)
			surf.uv1[i] = highMesh->getTexCoord1(i, ls);
		surf.n.resize(numT);
		surf.t.resize(numT);
		surf.sw.resize(numT);
		for (int i = 0; i < numT; i++)
		{
			quat q = highMesh->getTangent(i, ls);
			surf.n[i] = normalize(highNm * q.getNormal());
			surf.t[i] = normalize(highNm * q.getTangent());
			surf.sw[i] = (q.w < 0.0f) ? -1.0f : 1.0f;
		}

		// GPU "as rendered" mode: use captures prepared by the UI (readbacks
		// complete only between editor frames, so they cannot be made here)
		if (settings.gpuMode && surf.hasUV && settings.gpuCaptures
			&& s < int(settings.gpuCaptures->size()) && (*settings.gpuCaptures)[s].valid())
		{
			surf.gpu = (*settings.gpuCaptures)[s];
			if (!surf.gpu.atlasUV.empty())
			{
				// chart-repacked capture: per-corner atlas coordinates. SourceTri
				// ::corner indexes them by cindex triangle, so both index arrays
				// must agree in length or the per-hit lookup would read past the
				// end (the mesh API does not guarantee they match).
				if (int(surf.gpu.atlasUV.size()) == int(tind.size())
					&& int(cind.size()) == int(tind.size()))
					surf.useGpu = true;
				else
					Log::warning("Baker: surface %d capture atlas size mismatch "
						"(atlas %d, tindices %d, cindices %d), falling back to texture sampling\n",
						s, int(surf.gpu.atlasUV.size()), int(tind.size()), int(cind.size()));
			}
			// legacy path: a UV1-space capture needs the surface's UV1 to sample it back
			else if (surf.gpu.uvChannel == 1 && !surf.hasUV1)
				Log::warning("Baker: surface %d capture is in UV1 but the mesh copy has no UV1, "
					"falling back to texture sampling\n", s);
			else
				surf.useGpu = true;
		}
		else if (settings.gpuMode)
		{
			Log::warning("Baker: no GPU capture for surface %d, falling back to texture sampling\n", s);
		}
			// Report how much of this capture actually holds chart data. Real
			// assets exist whose UV set collapses part of the mesh to zero area
			// (surface 27 of the R5 interior: 400 of 800 triangles, and it has no
			// UV1 to fall back to) — those faces get no pixels at all, and each
			// hit landing there is routed to the material textures per TEXEL via
			// the coverage mask below.
			if (surf.useGpu && !surf.gpu.coverage.empty())
			{
				size_t covered = 0;
				for (unsigned char c : surf.gpu.coverage)
					covered += c ? 1u : 0u;
				const double frac = double(covered) / double(surf.gpu.coverage.size());
				if (frac < 0.02)
					Log::warning("Baker: surface %d capture is almost empty (%.0f%% of the "
								 "atlas covered) - it bakes from the material textures\n",
						s, frac * 100.0);
			}


		srcTris.reserve(srcTris.size() + numTris);
		for (int k = 0; k < numTris; k++)
		{
			SourceTri tr;
			vec3 p0 = highTm * highMesh->getVertex(cind[k * 3 + 0], ls);
			vec3 p1 = highTm * highMesh->getVertex(cind[k * 3 + 1], ls);
			vec3 p2 = highTm * highMesh->getVertex(cind[k * 3 + 2], ls);
			tr.p0 = p0;
			tr.e1 = p1 - p0;
			tr.e2 = p2 - p0;
			tr.surface = s;
			tr.t0 = tind[k * 3 + 0];
			tr.t1 = tind[k * 3 + 1];
			tr.t2 = tind[k * 3 + 2];
			tr.corner = k * 3;
			srcTris.push_back(tr);
		}
	}
	} // objects
	} // groups

	for (size_t gi = 0; gi < groups.size(); gi++)
		if (groupTris[gi].empty())
			return fail("A bake group's high-poly has no triangles.");

	//--------------------------------------------------------------------------
	// Extract low-poly geometry (all groups; the parts share one UV layout).
	//--------------------------------------------------------------------------
	std::vector<TargetTri> tgtTris;
	for (size_t gi = 0; gi < groups.size(); gi++)
	for (const Ptr<ObjectMeshStatic> &low : groups[gi].lows)
	{
		Ptr<ConstMesh> lowMesh = low->getMeshForceRAM();
		if (!lowMesh)
			return fail("Cannot get the low-poly model geometry.");
		mat4 lowTm = relativeTransform(low->getWorldTransform(), origin);
		// A mirrored part (negative scale) has its triangles wound the other way: the
		// face normals of the cage are flipped for it, or the cage would go inward.
		const bool lowMirrored = mat3(lowTm).determinant() < 0.0f;
		mat3 lowNm = transpose(inverse(mat3(lowTm)));

		// this part's cage: its own override if it has one, else the global pair
		vec2 partCage(Math::max(settings.frontalDistance, 1e-5f),
			Math::max(settings.rearDistance, 0.0f));
		{
			auto it = settings.partCage.find(low->getID());
			if (it != settings.partCage.end())
				partCage = vec2(Math::max(it->second.x, 1e-5f), Math::max(it->second.y, 0.0f));
		}

		for (int s = 0; s < lowMesh->getNumSurfaces(); s++)
		{
			// as for the high-poly: a disabled surface, one the camera does not see, a far
			// LOD is not a part of what is baked
			if (!isSurfaceBakeable(low, s))
				continue;
			if (lowMesh->getNumTexCoords0(s) <= 0)
				return fail("The low-poly model has no UV map (UV0).");

			const Vector<int> &cind = lowMesh->getCIndices(s);
			const Vector<int> &tind = lowMesh->getTIndices(s);
			int numTris = cind.size() / 3;

			tgtTris.reserve(tgtTris.size() + numTris);
			for (int k = 0; k < numTris; k++)
			{
				TargetTri tr;
				tr.p0 = lowTm * lowMesh->getVertex(cind[k * 3 + 0], s);
				tr.p1 = lowTm * lowMesh->getVertex(cind[k * 3 + 1], s);
				tr.p2 = lowTm * lowMesh->getVertex(cind[k * 3 + 2], s);
				int i0 = tind[k * 3 + 0], i1 = tind[k * 3 + 1], i2 = tind[k * 3 + 2];
				quat q0 = lowMesh->getTangent(i0, s);
				quat q1 = lowMesh->getTangent(i1, s);
				quat q2 = lowMesh->getTangent(i2, s);
				tr.n0 = normalize(lowNm * q0.getNormal());
				tr.n1 = normalize(lowNm * q1.getNormal());
				tr.n2 = normalize(lowNm * q2.getNormal());
				tr.t0 = normalize(lowNm * q0.getTangent());
				tr.t1 = normalize(lowNm * q1.getTangent());
				tr.t2 = normalize(lowNm * q2.getTangent());
				tr.signW = (q0.w < 0.0f) ? -1.0f : 1.0f;
				tr.uv0 = lowMesh->getTexCoord0(i0, s);
				tr.uv1 = lowMesh->getTexCoord0(i1, s);
				tr.uv2 = lowMesh->getTexCoord0(i2, s);
				tr.group = int(gi);
				tr.frontal = partCage.x;
				tr.rear = partCage.y;
				tr.mirrored = lowMirrored;
				tgtTris.push_back(tr);
			}
		}
	}

	if (tgtTris.empty())
		return fail("The low-poly model has no triangles.");

	// Smoothed cage normals (see CageNormals).
	{
		CageNormals smoothed;
		for (const TargetTri &tr : tgtTris)
			tr.mirrored ? smoothed.addTriangle(tr.p0, tr.p2, tr.p1) : smoothed.addTriangle(tr.p0, tr.p1, tr.p2);
		for (TargetTri &tr : tgtTris)
		{
			const vec3 fn = cross(tr.p1 - tr.p0, tr.p2 - tr.p0) * (tr.mirrored ? -1.0f : 1.0f);
			tr.r0 = smoothed.at(tr.p0, tr.n0);
			tr.r1 = smoothed.at(tr.p1, tr.n1);
			tr.r2 = smoothed.at(tr.p2, tr.n2);
			tr.s0 = smoothed.shell(tr.p0);
			tr.s1 = smoothed.shell(tr.p1);
			tr.s2 = smoothed.shell(tr.p2);
			tr.faceN = length2(fn) > 1e-20f ? normalize(fn) : tr.n0;
		}
	}

	//--------------------------------------------------------------------------
	// Skew mask: painted on the low-poly into its surface custom texture slot
	// (white = rays along the geometric normal, black = smoothed cage normal).
	// One mask in the shared UV layout — the first assigned one found is used.
	//--------------------------------------------------------------------------
	ImagePtr skewMask;
	if (settings.useSkewMask)
	{
		for (const BakeGroup &g : groups)
		{
			for (const Ptr<ObjectMeshStatic> &low : g.lows)
			{
				for (int s = 0; s < low->getNumSurfaces() && !skewMask; s++)
				{
					if (!low->isSurfaceCustomTextureEnabled(s))
						continue;
					skewMask = loadTexture(low->getSurfaceCustomTexturePath(s), textureCache);
					if (skewMask)
					{
						// coverage stats: catches "painted but not saved" and orientation issues
						long long painted = 0;
						const int mw = skewMask->getWidth(), mh = skewMask->getHeight();
						for (int my = 0; my < mh; my++)
							for (int mx = 0; mx < mw; mx++)
								if (skewMask->toVec4(skewMask->get2D(ivec2(mx, my))).x > 0.1f)
									painted++;
						Log::message("Baker: skew mask \"%s\" (%dx%d, painted %.1f%%)\n",
							resolveAssetPath(low->getSurfaceCustomTexturePath(s)).get(),
							mw, mh, 100.0 * double(painted) / double((long long)mw * mh));
						if (painted == 0)
						{
							Log::warning("Baker: skew mask is fully black — ignored\n");
							skewMask = ImagePtr();
						}
					}
				}
				if (skewMask)
					break;
			}
			if (skewMask)
				break;
		}
	}

	if (!progress(10, "Building BVH..."))
	{
		result.cancelled = true;
		return result;
	}

	std::vector<BVH> bvhs(groups.size());
	for (size_t gi = 0; gi < groups.size(); gi++)
		bvhs[gi].build(groupTris[gi]);

	//--------------------------------------------------------------------------
	// World decals overlapping the high-poly (projected onto the bake per texel).
	//--------------------------------------------------------------------------
	std::vector<DecalSample> decals;
	if (settings.bakeDecals)
	{
		vec3 hiMin(1e30f, 1e30f, 1e30f), hiMax(-1e30f, -1e30f, -1e30f);
		for (const auto &gt : groupTris)
			for (const SourceTri &t : gt)
			{
				vec3 v[3] = {t.p0, t.p0 + t.e1, t.p0 + t.e2};
				for (const vec3 &p : v)
				{
					hiMin = min(hiMin, p);
					hiMax = max(hiMax, p);
				}
			}

		// node world positions (to compare against the decal positions)
		for (const BakeGroup &g : groups)
		{
			for (const Ptr<ObjectMeshStatic> &h : g.highs)
			{
				Vec3 p = h->getWorldTransform().getTranslate();
				report("Baker: [high node] \"%s\" worldpos[%.2f %.2f %.2f]\n", h->getName(),
					float(p.x), float(p.y), float(p.z));
			}
			for (const Ptr<ObjectMeshStatic> &l : g.lows)
			{
				Vec3 p = l->getWorldTransform().getTranslate();
				report("Baker: [low node] \"%s\" worldpos[%.2f %.2f %.2f]\n", l->getName(),
					float(p.x), float(p.y), float(p.z));
			}
		}

		std::vector<NodePtr> highNodes;
		for (const BakeGroup &g : groups)
			for (const Ptr<ObjectMeshStatic> &h : g.highs)
				highNodes.push_back(h);
		decals = collectDecals(origin, hiMin, hiMax, settings.decalNodeIds, highNodes,
			settings.decalDistance, textureCache);
	}
	// per-decal texel-sample hit counters (diagnostics)
	std::vector<std::atomic<long long>> decalHits(decals.size());

	//--------------------------------------------------------------------------
	// Rasterize + trace (worker threads own disjoint row bands).
	//--------------------------------------------------------------------------
	const int res = settings.resolution;
	// the cage is per low-poly part now: each TargetTri carries its own
	// frontal/rear (see Settings::partCage), resolved when the triangle was built
	const size_t pixelCount = size_t(res) * size_t(res);

	// accumulators: albedo rgba, shading rgba, normal xyz (target tangent space), weight
	std::vector<float> accAlbedo(pixelCount * 4, 0.0f);
	std::vector<float> accShading(pixelCount * 4, 0.0f);
	std::vector<float> accNormal(pixelCount * 3, 0.0f);
	std::vector<float> accEmission(settings.bakeEmission ? pixelCount * 4 : 0, 0.0f);
	std::vector<float> accWeight(pixelCount, 0.0f);

	// subsample offsets
	std::vector<vec2> offsets;
	int grid = 1;
	if (settings.supersamples >= 64)
		grid = 8;
	else if (settings.supersamples >= 16)
		grid = 4;
	else if (settings.supersamples >= 4)
		grid = 2;
	for (int sy = 0; sy < grid; sy++)
		for (int sx = 0; sx < grid; sx++)
			offsets.push_back(vec2((sx + 0.5f) / grid, (sy + 0.5f) / grid));

	int numWorkers = int(std::thread::hardware_concurrency());
	if (numWorkers < 1)
		numWorkers = 4;
	numWorkers = Math::min(numWorkers, res);

	std::atomic<long long> trisDone(0);
	std::atomic<bool> cancelFlag(false);

	// hit statistics (for diagnostics)
	std::atomic<long long> statFront(0), statRear(0), statBackface(0), statMiss(0);
	// per-source-surface diagnostics, merged from the workers at the end
	std::mutex surfStatMutex;
	std::vector<long long> surfHit(srcSurfaces.size(), 0);
	std::vector<long long> surfGpu(srcSurfaces.size(), 0);
	std::vector<double> surfAlb(srcSurfaces.size(), 0.0);
	std::atomic<long long> statDecal(0); // texel-samples that received a decal

	auto worker = [&](int bandY0, int bandY1) {
		long long locFront = 0, locRear = 0, locBackface = 0, locMiss = 0, locDecal = 0;
		std::vector<long long> locSurfHit(srcSurfaces.size(), 0);
		std::vector<long long> locSurfGpu(srcSurfaces.size(), 0);
		std::vector<double> locSurfAlb(srcSurfaces.size(), 0.0);
		for (const TargetTri &tr : tgtTris)
		{
			if (cancelFlag.load(std::memory_order_relaxed))
				return;
			trisDone.fetch_add(1, std::memory_order_relaxed);

			// rays of this triangle see only its own bake group's high-poly
			const std::vector<SourceTri> &srcTris = groupTris[tr.group];
			const BVH &bvh = bvhs[tr.group];

			// cage of the low-poly part this triangle belongs to
			const float frontal = tr.frontal;
			const float rear = tr.rear;

			// UV triangle in pixel space
			vec2 a = tr.uv0 * float(res);
			vec2 b = tr.uv1 * float(res);
			vec2 c = tr.uv2 * float(res);
			float area = (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
			if (Math::abs(area) < 1e-9f)
				continue;
			float invArea = 1.0f / area;

			int minX = Math::max(int(Math::floor(Math::min(a.x, Math::min(b.x, c.x)))), 0);
			int maxX = Math::min(int(Math::ceil(Math::max(a.x, Math::max(b.x, c.x)))), res - 1);
			int minY = Math::max(int(Math::floor(Math::min(a.y, Math::min(b.y, c.y)))), bandY0);
			int maxY = Math::min(int(Math::ceil(Math::max(a.y, Math::max(b.y, c.y)))), bandY1 - 1);
			if (minX > maxX || minY > maxY)
				continue;

			for (int y = minY; y <= maxY; y++)
			{
				for (int x = minX; x <= maxX; x++)
				{
					size_t pix = size_t(y) * res + x;
					for (const vec2 &off : offsets)
					{
						vec2 p(x + off.x, y + off.y);
						// barycentrics in UV space
						float l1 = ((p.x - a.x) * (c.y - a.y) - (p.y - a.y) * (c.x - a.x)) * invArea;
						float l2 = ((b.x - a.x) * (p.y - a.y) - (b.y - a.y) * (p.x - a.x)) * invArea;
						float l0 = 1.0f - l1 - l2;
						const float eps = -1e-4f;
						if (l0 < eps || l1 < eps || l2 < eps)
							continue;

						vec3 pos = tr.p0 * l0 + tr.p1 * l1 + tr.p2 * l2;
						// interpolate normal/tangent linearly and rebuild the basis exactly
						// like the engine fragment shader (normalizationTBN)
						TBN lowTBN = buildTBN(tr.n0 * l0 + tr.n1 * l1 + tr.n2 * l2,
							tr.t0 * l0 + tr.t1 * l1 + tr.t2 * l2, tr.signW);
						const vec3 &nrm = lowTBN.n;
						const vec3 &tan = lowTBN.t;
						const vec3 &bin = lowTBN.b;
						vec3 rayN = settings.raysAlongShading
							? nrm
							: normalize(tr.r0 * l0 + tr.r1 * l1 + tr.r2 * l2);
						float skew = 0.0f;
						if (skewMask)
						{
							// painted skew mask lives in the same UV space as the output
							skew = skewMask->toVec4(
								skewMask->get2D(vec2(p.x / float(res), p.y / float(res)))).x;
							if (skew > 0.001f)
								rayN = normalize(rayN * (1.0f - skew) + tr.faceN * skew);
						}

						// trace from the cage surface inward along the smoothed normal and
						// take the FIRST front-facing hit (the outermost high-poly detail),
						// like conventional bakers do. Cage distance therefore controls how
						// far above the low-poly surface details are still captured. On an
						// edge the cage is further off the corner (CageNormals::shell).
						const float lift = frontal * (tr.s0 * l0 + tr.s1 * l1 + tr.s2 * l2);
						const float rayLength = lift + rear;
						vec3 orig = pos + rayN * lift;
						vec3 dir = -rayN;
						auto scorer = [&](int tri, float t, float u, float v) -> float {
							const SourceTri &str = srcTris[tri];
							const SourceSurface &ssurf = srcSurfaces[str.surface];
							float sw0 = 1.0f - u - v;
							vec3 sn = ssurf.n[str.t0] * sw0 + ssurf.n[str.t1] * u + ssurf.n[str.t2] * v;
							float score = t; // smaller t = closer to the cage = more outward
							if (dot(sn, dir) > 0.0f)
								score += rayLength * 100.0f; // back-facing: use only if nothing better
							return score;
						};
						float hitT, hitU, hitV;
						int hitTri = bvh.trace(srcTris, orig, dir, rayLength, scorer, hitT, hitU, hitV);
						if (hitTri < 0)
						{
							locMiss++;
							continue;
						}

						const SourceTri &st = srcTris[hitTri];
						const SourceSurface &surf = srcSurfaces[st.surface];
						float w0 = 1.0f - hitU - hitV;

						TBN srcTBN = buildTBN(
							surf.n[st.t0] * w0 + surf.n[st.t1] * hitU + surf.n[st.t2] * hitV,
							surf.t[st.t0] * w0 + surf.t[st.t1] * hitU + surf.t[st.t2] * hitV,
							surf.sw[st.t0]);
						const vec3 &srcN = srcTBN.n;

						vec4 debugColor(0.0f, 0.0f, 0.0f, 1.0f);
						{
							vec3 hn = srcN;
							if (dot(hn, dir) > 0.0f)
							{
								locBackface++;
								debugColor = vec4(1.0f, 0.0f, 0.0f, 1.0f); // red
							}
							else if (hitT <= lift)
							{
								locFront++;
								debugColor = vec4(0.0f, 1.0f, 0.0f, 1.0f); // green
							}
							else
							{
								locRear++;
								debugColor = vec4(0.0f, 0.0f, 1.0f, 1.0f); // blue
							}
						}

						// fallbacks when the material has no textures: engine constants
						vec4 albedo = surf.albedoColor;
						vec4 shading(surf.metalness, surf.roughness, 0.5f, 0.0f);
						// The emission ALPHA carries a mask: opaque where the hit
						// surface has its emission state ON, transparent where it
						// does not. The glow itself can be legitimately black in
						// places, so colour alone cannot tell "not emissive" from
						// "emissive but dark" — the mask can, which is what makes
						// the map usable for compositing elsewhere.
						vec4 emission(0.0f, 0.0f, 0.0f, 0.0f);
						if (surf.emissionOn)
							emission = vec4(srgbToLinear(surf.emissionColor.xyz) * surf.emissionScale, 1.0f);
						vec3 worldN = srcN;
						// diagnostics: did this sample read the GPU capture?
						bool sampledGpu = false;
						if (surf.hasUV)
						{
							vec2 suvRaw = surf.uv[st.t0] * w0 + surf.uv[st.t1] * hitU + surf.uv[st.t2] * hitV;

							// sample the "as rendered" unwrap gbuffer captures through the
							// chart-repacked atlas (unique by construction); the pack
							// transform is affine, so barycentric interpolation of the
							// per-tvertex atlas coords is exact. Triangles of charts too
							// small to capture are marked with negative coords — those
							// take the CPU texture path below.
							bool gpuHit = surf.useGpu;
							vec2 cuv(0.0f, 0.0f);
							if (gpuHit)
							{
								if (!surf.gpu.atlasUV.empty())
								{
									// per-corner atlas coords: entries [corner .. corner+2]
									cuv = surf.gpu.atlasUV[st.corner] * w0
										+ surf.gpu.atlasUV[st.corner + 1] * hitU
										+ surf.gpu.atlasUV[st.corner + 2] * hitV;
									if (cuv.x < 0.0f || cuv.y < 0.0f)
										gpuHit = false; // uncapturable chart
								}
								else
								{
									// legacy path: capture rendered in a raw UV channel
									vec2 gpuUV = suvRaw;
									if (surf.gpu.uvChannel == 1)
										gpuUV = surf.uv1[st.t0] * w0 + surf.uv1[st.t1] * hitU + surf.uv1[st.t2] * hitV;
									cuv = vec2((gpuUV.x - surf.gpu.uvMin.x) / surf.gpu.uvScale.x,
										(gpuUV.y - surf.gpu.uvMin.y) / surf.gpu.uvScale.y);
								}
							}

							if (gpuHit)
							{
								cuv.x = saturate(cuv.x);
								cuv.y = saturate(cuv.y);
							}

							// Does the capture actually hold data at this texel?
							// The render background is a BLACK quad, so brightness
							// cannot tell background from a legitimately black
							// material — only the coverage mask can. Uncovered means
							// this piece of the mesh has no unwrap area (collapsed
							// UVs), so bake it from the material textures. Gutter
							// texels count as covered, so chart edges still sample
							// the dilated capture. Per texel, not per triangle: a
							// triangle straddling a chart edge must not flip whole.
							if (gpuHit && !surf.gpu.coverage.empty())
							{
								const int cvw = surf.gpu.coverageWidth;
								const int cvh = surf.gpu.coverageHeight;
								const int tx = Math::clamp(int(cuv.x * cvw), 0, cvw - 1);
								const int ty = Math::clamp(int(cuv.y * cvh), 0, cvh - 1);
								if (!surf.gpu.coverage[size_t(ty) * size_t(cvw) + size_t(tx)])
									gpuHit = false;
							}

							if (gpuHit)
							{
								vec4 a4 = surf.gpu.albedo->toVec4(surf.gpu.albedo->get2D(cuv));
								vec4 s4 = surf.gpu.shading->toVec4(surf.gpu.shading->get2D(cuv));
								// octahedral-packed normal: bilinear filtering would break the
								// bit packing, sample nearest texel
								int gw = surf.gpu.normal->getWidth();
								int gh = surf.gpu.normal->getHeight();
								ivec2 ncoord(Math::clamp(int(cuv.x * gw), 0, gw - 1),
									Math::clamp(int(cuv.y * gh), 0, gh - 1));
								vec4 n4 = surf.gpu.normal->toVec4(surf.gpu.normal->get2D(ncoord));

								sampledGpu = true;
								albedo = a4; // the gbuffer render target already stores sRGB-encoded albedo
								// _sh layout: R=metalness, G=roughness, B=specular(f0), A=microfiber
								shading = vec4(s4.x, n4.w, s4.y, s4.w);
								if (surf.gpu.emission)
								{
									vec4 e4 = surf.gpu.emission->toVec4(surf.gpu.emission->get2D(cuv));
									// the capture exists for every surface, so the
									// material's emission state decides the mask
									emission = vec4(srgbToLinear(e4.xyz), surf.emissionOn ? 1.0f : 0.0f);
								}

								vec3 ts = BakeGpu::unpackGBufferNormal(n4);
								worldN = normalize(srcTBN.t * ts.x + srcTBN.b * ts.y + srcTBN.n * ts.z);
							}
							else
							{
								vec2 suv = suvRaw;
								// apply the material's base UV tiling, like the engine shader does,
								// and wrap into [0..1) — get2D behavior outside is undocumented
								suv = vec2(suv.x * surf.uvTransform.x + surf.uvTransform.z,
									suv.y * surf.uvTransform.y + surf.uvTransform.w);
								suv.x -= Math::floor(suv.x);
								suv.y -= Math::floor(suv.y);
								if (surf.albedo)
								{
									// the engine multiplies the albedo texture by albedo_color
									albedo = surf.albedo->toVec4(surf.albedo->get2D(suv)) * surf.albedoColor;
								}
								if (surf.shading)
								{
									// the scalar parameters multiply the texture, as the
									// engine shader does (and as the decals do above)
									const vec4 t = surf.shading->toVec4(surf.shading->get2D(suv));
									shading = vec4(t.x * surf.metalness, t.y * surf.roughness, t.z, t.w);
								}
								if (surf.emissionOn && surf.emission)
								{
									vec4 e4 = surf.emission->toVec4(surf.emission->get2D(suv));
									emission = vec4(srgbToLinear(e4.xyz * surf.emissionColor.xyz) * surf.emissionScale, 1.0f);
								}

								if (surf.normal)
								{
									const vec3 &stn = srcTBN.t;
									const vec3 &sbn = srcTBN.b;
									vec4 nm = surf.normal->toVec4(surf.normal->get2D(suv));
									float nx = nm.x * 2.0f - 1.0f;
									float ny = nm.y * 2.0f - 1.0f;
									// Invert G is of the map written, not of the maps read:
									// flipping here too would cancel it out.
									if (surf.normalInvertG)
										ny = -ny; // renderer sees the imported (G-inverted) copy
									float nz = Math::sqrt(Math::max(1.0f - nx * nx - ny * ny, 0.0f));
									worldN = normalize(stn * nx + sbn * ny + srcN * nz);
								}
							}

						}

						// per-source-surface diagnostics (before decals): which high-poly
						// surface this texel-sample read, whether it came from the GPU
						// capture, and how bright it was. A surface that bakes wrong
						// shows up here as "gpu=0" (capture never used) or as a mean
						// far from its material's albedo.
						locSurfHit[st.surface]++;
						if (sampledGpu)
							locSurfGpu[st.surface]++;
						locSurfAlb[st.surface] += double(albedo.x + albedo.y + albedo.z) / 3.0;

						// world decals blended over the base result at the world hit
						// point (in render order), before encoding into tangent space
						if (!decals.empty() && !settings.debugZones)
						{
							vec3 worldHit = st.p0 + st.e1 * hitU + st.e2 * hitV;
							if (applyDecals(decals, worldHit, albedo, shading, worldN, &decalHits)
								> 0)
								locDecal++;
						}

						// normal into target tangent space (single path for hasUV/no-UV)
						accNormal[pix * 3 + 0] += dot(tan, worldN);
						accNormal[pix * 3 + 1] += dot(bin, worldN);
						accNormal[pix * 3 + 2] += dot(nrm, worldN);

						if (settings.debugZones)
						{
							albedo = debugColor;
							// overlay the skew mask in yellow so its coverage is visible
							if (skew > 0.001f)
								albedo = albedo * (1.0f - skew * 0.7f)
									+ vec4(1.0f, 1.0f, 0.0f, 1.0f) * (skew * 0.7f);
						}
						accAlbedo[pix * 4 + 0] += albedo.x;
						accAlbedo[pix * 4 + 1] += albedo.y;
						accAlbedo[pix * 4 + 2] += albedo.z;
						accAlbedo[pix * 4 + 3] += albedo.w;
						accShading[pix * 4 + 0] += shading.x;
						accShading[pix * 4 + 1] += shading.y;
						accShading[pix * 4 + 2] += shading.z;
						accShading[pix * 4 + 3] += shading.w;
						if (settings.bakeEmission)
						{
							accEmission[pix * 4 + 0] += emission.x;
							accEmission[pix * 4 + 1] += emission.y;
							accEmission[pix * 4 + 2] += emission.z;
							accEmission[pix * 4 + 3] += emission.w;
						}
						accWeight[pix] += 1.0f;
					}
				}
			}
		}
		statFront += locFront;
		statRear += locRear;
		statBackface += locBackface;
		statMiss += locMiss;
		statDecal += locDecal;
		{
			std::lock_guard<std::mutex> lock(surfStatMutex);
			for (size_t i = 0; i < surfHit.size(); i++)
			{
				surfHit[i] += locSurfHit[i];
				surfGpu[i] += locSurfGpu[i];
				surfAlb[i] += locSurfAlb[i];
			}
		}
	};

	{
		std::vector<std::thread> threads;
		// The bands can be fewer than the workers (512 rows on 48 workers: 47 bands of
		// 11): the wait is for the threads that were started, not for numWorkers.
		std::atomic<int> finished(0);
		int rowsPerBand = (res + numWorkers - 1) / numWorkers;
		for (int i = 0; i < numWorkers; i++)
		{
			int y0 = i * rowsPerBand;
			int y1 = Math::min(y0 + rowsPerBand, res);
			if (y0 >= y1)
				break;
			threads.emplace_back([&worker, &finished, y0, y1]() {
				worker(y0, y1);
				finished.fetch_add(1);
			});
		}
		const long long started = (long long)tgtTris.size() * (long long)threads.size();

		bool keepGoing = true;
		while (finished.load() < int(threads.size()) && !cancelFlag.load())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(50));
			int percent = 15 + int(70.0 * double(trisDone.load()) / double(started > 0 ? started : 1));
			keepGoing = progress(Math::min(percent, 85), "Baking...");
			if (!keepGoing)
				cancelFlag.store(true);
		}
		for (auto &th : threads)
			th.join();

		if (cancelFlag.load())
		{
			result.cancelled = true;
			return result;
		}
	}

	report("Baker: ray stats: front(above surface)=%lld rear(below surface)=%lld backface=%lld miss=%lld\n",
		statFront.load(), statRear.load(), statBackface.load(), statMiss.load());
	// per-source-surface table: pinpoints a surface that bakes wrong. "gpu%" is
	// the share of its texel-samples that came from the GPU capture (0% = the
	// capture was never used, so the CPU texture path produced the result) and
	// "mean" is the average baked brightness before decals.
	for (size_t si = 0; si < surfHit.size(); si++)
	{
		if (surfHit[si] <= 0)
			continue;
		// measure the capture image itself and the atlasUV range used to sample
		// it: separates "the capture came back black" from "the bake sampled the
		// wrong place in a good capture"
		double capMean = -1.0, capCov = -1.0;
		const SourceSurface &ss = srcSurfaces[si];
		if (ss.gpu.albedo)
		{
			const int cw = ss.gpu.albedo->getWidth();
			const int ch = ss.gpu.albedo->getHeight();
			double sum = 0.0;
			long long lit = 0, total = 0;
			for (int yy = 0; yy < ch; yy += 4)
				for (int xx = 0; xx < cw; xx += 4)
				{
					vec4 c = ss.gpu.albedo->toVec4(ss.gpu.albedo->get2D(ivec2(xx, yy)));
					double l = double(c.x + c.y + c.z) / 3.0;
					total++;
					if (l > 0.004)
					{
						sum += l;
						lit++;
					}
				}
			capMean = lit > 0 ? sum / double(lit) : 0.0;
			capCov = total > 0 ? double(lit) / double(total) : 0.0;
		}
		vec2 uvMin(2.0f, 2.0f), uvMax(-2.0f, -2.0f);
		for (const vec2 &p : ss.gpu.atlasUV)
		{
			uvMin = min(uvMin, p);
			uvMax = max(uvMax, p);
		}
		report("Baker: surf-bake %d hits=%lld gpu=%lld (%.0f%%) mean=%.4f useGpu=%d atlas=%d "
			   "uvch=%d cap[mean=%.4f cov=%.2f %dx%d] auv[%.3f %.3f]..[%.3f %.3f]\n",
			int(si), surfHit[si], surfGpu[si],
			100.0 * double(surfGpu[si]) / double(surfHit[si]),
			surfAlb[si] / double(surfHit[si]),
			ss.useGpu ? 1 : 0, int(ss.gpu.atlasUV.size()), ss.gpu.uvChannel,
			capMean, capCov,
			ss.gpu.albedo ? ss.gpu.albedo->getWidth() : 0,
			ss.gpu.albedo ? ss.gpu.albedo->getHeight() : 0,
			uvMin.x, uvMin.y, uvMax.x, uvMax.y);
	}
	if (settings.bakeDecals)
	{
		report("Baker: decal-covered texel-samples=%lld\n", statDecal.load());
		for (size_t di = 0; di < decals.size(); di++)
			report("Baker: decal-hits \"%s\" (%s) = %lld\n", decals[di].name.get(),
				decals[di].footprint ? "mesh" : "proj", decalHits[di].load());
	}
	if (g_report.is_open())
		g_report.close();

	if (!progress(86, "Processing edges..."))
	{
		result.cancelled = true;
		return result;
	}

	//--------------------------------------------------------------------------
	// Resolve + dilation.
	//--------------------------------------------------------------------------
	std::vector<unsigned char> covered(pixelCount, 0);
	for (size_t i = 0; i < pixelCount; i++)
	{
		float w = accWeight[i];
		if (w <= 0.0f)
			continue;
		covered[i] = 1;
		float inv = 1.0f / w;
		for (int c = 0; c < 4; c++)
		{
			accAlbedo[i * 4 + c] *= inv;
			accShading[i * 4 + c] *= inv;
			if (settings.bakeEmission)
				accEmission[i * 4 + c] *= inv;
		}
		vec3 n(accNormal[i * 3 + 0], accNormal[i * 3 + 1], accNormal[i * 3 + 2]);
		n = normalize(n);
		accNormal[i * 3 + 0] = n.x;
		accNormal[i * 3 + 1] = n.y;
		accNormal[i * 3 + 2] = n.z;
	}

	// wavefront dilation
	{
		std::vector<int> frontier;
		std::vector<int> next;
		auto isCovered = [&](int x, int y) -> bool {
			return x >= 0 && y >= 0 && x < res && y < res && covered[size_t(y) * res + x] != 0;
		};
		for (int pass = 0; pass < settings.dilationPixels; pass++)
		{
			next.clear();
			for (int y = 0; y < res; y++)
				for (int x = 0; x < res; x++)
				{
					size_t i = size_t(y) * res + x;
					if (covered[i])
						continue;
					// average covered neighbors
					float alb[4] = {0, 0, 0, 0}, sh[4] = {0, 0, 0, 0}, em[4] = {0, 0, 0, 0}, nr[3] = {0, 0, 0};
					int cnt = 0;
					for (int dy = -1; dy <= 1; dy++)
						for (int dx = -1; dx <= 1; dx++)
						{
							if (!dx && !dy)
								continue;
							if (!isCovered(x + dx, y + dy))
								continue;
							size_t j = size_t(y + dy) * res + (x + dx);
							for (int cch = 0; cch < 4; cch++)
							{
								alb[cch] += accAlbedo[j * 4 + cch];
								sh[cch] += accShading[j * 4 + cch];
								if (settings.bakeEmission)
									em[cch] += accEmission[j * 4 + cch];
							}
							for (int cch = 0; cch < 3; cch++)
								nr[cch] += accNormal[j * 3 + cch];
							cnt++;
						}
					if (!cnt)
						continue;
					float inv = 1.0f / cnt;
					for (int cch = 0; cch < 4; cch++)
					{
						accAlbedo[i * 4 + cch] = alb[cch] * inv;
						accShading[i * 4 + cch] = sh[cch] * inv;
						if (settings.bakeEmission)
							accEmission[i * 4 + cch] = em[cch] * inv;
					}
					vec3 n(nr[0] * inv, nr[1] * inv, nr[2] * inv);
					n = normalize(n);
					accNormal[i * 3 + 0] = n.x;
					accNormal[i * 3 + 1] = n.y;
					accNormal[i * 3 + 2] = n.z;
					next.push_back(int(i));
				}
			if (next.empty())
				break;
			for (int i : next)
				covered[i] = 1;
		}
	}

	if (!progress(92, "Saving textures..."))
	{
		result.cancelled = true;
		return result;
	}

	//--------------------------------------------------------------------------
	// Output paths.
	//--------------------------------------------------------------------------
	String highAssetPath = resolveAssetPath(firstHigh->getMeshPath());
	String lowAssetPath = resolveAssetPath(firstLow->getMeshPath());

	// user-provided output name wins; sanitize filesystem-reserved characters
	String baseName;
	{
		std::string clean;
		for (int i = 0; i < settings.outputName.size(); i++)
		{
			const char c = settings.outputName[i];
			clean += (c && strchr("\\/:*?\"<>|", c)) ? '_' : c;
		}
		baseName = String(clean.c_str()).trim();
	}
	if (baseName.empty())
		baseName = suggestBaseName(firstHigh);

	String anchorPath = lowAssetPath;
	if (anchorPath.empty() || isGuidPath(anchorPath))
		anchorPath = highAssetPath;
	if (anchorPath.empty())
		return fail("Neither model has a mesh file — save the models as assets.");
	if (isGuidPath(anchorPath))
		return fail("Cannot determine the models' asset folder (paths are GUID references).");

	String virtualDir = pathDir(anchorPath);
	// Build the on-disk path from the data root: FileSystem::getAbsolutePath resolves
	// asset paths into .runtimes, which is not where new asset files belong.
	String dataPath = Engine::get()->getDataPath();
	if (dataPath.empty())
		return fail("Cannot determine the project data folder.");
	if (dataPath[dataPath.size() - 1] != '/' && dataPath[dataPath.size() - 1] != '\\')
		dataPath += "/";
	String absDir = dataPath + virtualDir;
	Log::message("Baker: data path \"%s\"\n", dataPath.get());
	Log::message("Baker: output dir \"%s\"\n", absDir.get());

	// The maps are written over whatever has their names: never over a texture the
	// high-poly itself uses (its own statue_n.png next to it, when the name is "statue").
	{
		std::set<std::string> highTextures;
		for (const BakeGroup &g : groups)
			for (const Ptr<ObjectMeshStatic> &high : g.highs)
				for (int s = 0; high && s < high->getNumSurfaces(); s++)
				{
					const MaterialPtr m = high->getMaterial(s);
					for (int t = 0; m && t < m->getNumTextures(); t++)
					{
						const String path = resolveAssetPath(m->getTexturePath(t));
						if (!path.empty())
							highTextures.insert(path.get());
					}
				}
		const std::pair<bool, const char *> outputs[] = {{settings.bakeAlbedo, "_alb"}, {settings.bakeShading, "_sh"},
			{settings.bakeNormal, "_n"}, {settings.bakeEmission, "_e"}};
		for (const auto &output : outputs)
		{
			const String target = virtualDir + baseName + output.second + ".png";
			if (output.first && highTextures.count(target.get()))
				return fail("A baked map would overwrite a texture of the high-poly: give the bake another name.");
		}
	}

	//--------------------------------------------------------------------------
	// Write images.
	//--------------------------------------------------------------------------
	auto writeImage = [&](const char *postfix, int channels, const std::vector<float> &data,
						  int stride, String &outVirtual, bool sixteenBit = false) -> bool {
		ImagePtr img = Image::create();
		int format = channels == 3 ? (sixteenBit ? Image::FORMAT_RGB16 : Image::FORMAT_RGB8)
								   : Image::FORMAT_RGBA8;
		img->create2D(res, res, format);
		for (int y = 0; y < res; y++)
			for (int x = 0; x < res; x++)
			{
				size_t i = size_t(y) * res + x;
				vec4 color(0.0f, 0.0f, 0.0f, 1.0f);
				for (int c = 0; c < Math::min(stride, 4); c++)
					color[c] = data[i * stride + c];
				img->set2D(x, y, img->toPixel(color));
			}
		String fileName = baseName + postfix + ".png";
		String absPath = absDir + fileName;

		// Image::save() on a path of an already imported asset is intercepted by the
		// engine virtual file system and written into the .runtimes copy instead of
		// the source file. Save to an OS temp file and copy over via the OS instead.
		namespace fs = std::filesystem;
		std::error_code ec;
		fs::path tmpPath = fs::temp_directory_path(ec) / (String(baseName + postfix).get() + std::string("_baker_tmp.png"));
		if (ec)
		{
			Log::error("Baker: temp dir unavailable: %s\n", ec.message().c_str());
			return false;
		}
		if (!img->save(tmpPath.string().c_str()))
		{
			Log::error("Baker: failed to save \"%s\"\n", tmpPath.string().c_str());
			return false;
		}
		fs::copy_file(tmpPath, fs::u8path(absPath.get()), fs::copy_options::overwrite_existing, ec);
		std::error_code ec2;
		fs::remove(tmpPath, ec2);
		if (ec)
		{
			Log::error("Baker: failed to copy texture to \"%s\": %s\n", absPath.get(), ec.message().c_str());
			return false;
		}
		Log::message("Baker: saved \"%s\"\n", absPath.get());
		outVirtual = virtualDir + fileName;
		return true;
	};

	// Written when picked, also without textures in the high-poly: the color of its
	// material and the GPU capture are what is baked then.
	(void)anyAlbedo;
	(void)anyShading;
	if (settings.bakeAlbedo)
	{
		if (!writeImage("_alb", 4, accAlbedo, 4, result.albedoPath))
			return fail("Failed to save the albedo texture.");
	}
	if (settings.bakeShading)
	{
		if (!writeImage("_sh", 4, accShading, 4, result.shadingPath))
			return fail("Failed to save the shading texture (_sh).");
	}
	if (settings.bakeEmission)
	{
		// The emission is linear here. An 8-bit map holds up to 1: an emission brighter than
		// that (a scale of 5) is written divided by its brightest value - linear, so that the
		// material multiplying it back gives every texel its brightness - then encoded.
		float brightest = 0.0f;
		for (size_t i = 0; i + 3 < accEmission.size(); i += 4)
			brightest = Math::max(brightest, Math::max(accEmission[i], Math::max(accEmission[i + 1], accEmission[i + 2])));
		const float divide = brightest > 1.0f ? brightest : 1.0f;
		result.emissionScale = divide;
		for (size_t i = 0; i + 3 < accEmission.size(); i += 4)
			for (int c = 0; c < 3; c++)
				accEmission[i + c] = linearToSrgb(Math::clamp(accEmission[i + c] / divide, 0.0f, 1.0f));
		if (!writeImage("_e", 4, accEmission, 4, result.emissionPath))
			return fail("Failed to save the emission texture (_e).");
	}

	// normal: encode from [-1..1] to [0..1]
	if (settings.bakeNormal)
	{
		std::vector<float> encoded(pixelCount * 3);
		for (size_t i = 0; i < pixelCount; i++)
		{
			// default: flat normal for uncovered pixels
			vec3 n(0.0f, 0.0f, 1.0f);
			if (covered[i])
				n = vec3(accNormal[i * 3 + 0], accNormal[i * 3 + 1], accNormal[i * 3 + 2]);
			if (settings.flipNormalY)
				n.y = -n.y;
			encoded[i * 3 + 0] = n.x * 0.5f + 0.5f;
			encoded[i * 3 + 1] = n.y * 0.5f + 0.5f;
			encoded[i * 3 + 2] = n.z * 0.5f + 0.5f;
		}
		if (!writeImage("_n", 3, encoded, 3, result.normalPath, true))
			return fail("Failed to save the normal map.");
	}

	progress(100, "Done");
	result.success = true;
	Log::message("Baker: done. albedo=\"%s\" shading=\"%s\" normal=\"%s\" emission=\"%s\"\n",
		result.albedoPath.get(), result.shadingPath.get(), result.normalPath.get(),
		result.emissionPath.get());
	return result;
}

// Runs AFTER the baked textures are imported as assets: assigning a texture
// path that is not an asset yet binds the raw file, bypassing the import
// pipeline (a raw normal map renders broken until the next reimport).
void assignMaterial(const std::vector<BakeGroup> &groups, Result &result)
{
	if (groups.empty() || groups[0].lows.empty())
		return;
	// Any written map identifies the set — a single-map re-bake leaves the other
	// paths empty, and keying off the normal alone would silently skip the whole
	// material assignment whenever _n was not one of the selected maps.
	struct NamedPath
	{
		const String &path;
		const char *suffix;
	};
	const NamedPath candidates[] = {{result.normalPath, "_n"}, {result.albedoPath, "_alb"},
		{result.shadingPath, "_sh"}, {result.emissionPath, "_e"}};
	const NamedPath *source = nullptr;
	for (const NamedPath &c : candidates)
		if (!c.path.empty())
		{
			source = &c;
			break;
		}
	if (!source)
		return;
	const Ptr<ObjectMeshStatic> &firstLow = groups[0].lows[0];

	String virtualDir = pathDir(source->path);
	String baseName = pathBaseName(source->path);
	const size_t suffixLen = strlen(source->suffix);
	if (baseName.size() > int(suffixLen)
		&& !strcmp(baseName.get() + baseName.size() - suffixLen, source->suffix))
		baseName = baseName.substr(0, baseName.size() - int(suffixLen));

	MaterialPtr mat = firstLow->getMaterial(0);
	// A material the high-poly uses too (one imported material for both, a mesh baked onto
	// itself) is not baked into: the high-poly would show the baked maps, and a re-bake
	// would read its own output. The low-poly gets a material of its own then.
	bool sharedWithHigh = false;
	if (mat && !mat->isBase())
	{
		for (const BakeGroup &g : groups)
			for (const Ptr<ObjectMeshStatic> &high : g.highs)
				for (int s = 0; high && s < high->getNumSurfaces() && !sharedWithHigh; s++)
				{
					const MaterialPtr hm = high->getMaterial(s);
					sharedWithHigh = hm && hm->getGUID() == mat->getGUID();
				}
	}
	if (!mat || mat->isBase() || sharedWithHigh)
	{
		MaterialPtr base;
		if (mat && mat->isBase())
			base = mat;
		if (!base)
			base = Materials::findManualMaterial("mesh_base");
		if (!base)
			base = Materials::findManualMaterial("Unigine::mesh_base");
		if (!base)
		{
			Log::error("Baker: the mesh_base base material was not found\n");
			return;
		}

		String matPath = virtualDir + baseName + "_baked.mat";
		// A material of a previous bake under this name is used again: a second material
		// with the file of the first could not be registered, and would overwrite it.
		MaterialPtr existing = Materials::findMaterialByPath(matPath.get());
		if (!existing && FileSystem::isFileExist(matPath.get()))
			existing = Materials::loadMaterial(matPath.get());
		if (existing && !existing->isBase())
			mat = existing;
		else
		{
			mat = base->inherit();
			if (!mat->createMaterialFile(matPath.get()))
				Log::warning("Baker: failed to create material file \"%s\"\n", matPath.get());
		}
		for (const BakeGroup &g : groups)
			for (const Ptr<ObjectMeshStatic> &low : g.lows)
				low->setMaterial(mat, "*");
		result.materialPath = matPath;
	}
	else
	{
		// the first part already has an inherited material: bake into it and
		// share it with all the surfaces of all the parts (the other surfaces of the
		// first part too: they may have other materials)
		for (const BakeGroup &g : groups)
			for (const Ptr<ObjectMeshStatic> &low : g.lows)
				low->setMaterial(mat, "*");
		result.materialPath = mat->getFilePath();
	}

	// The maps are baked to the UV0 of the low-poly as it is: the tiling and the offset
	// the material had would repeat or shift them. But it is one for all the maps of the
	// material: while a map that stays is not one of a bake (the low-poly's own tiled
	// texture, when only some maps are baked), it keeps its tiling - and the new maps are
	// off; the caller tells it (uvTransformKept).
	const auto baked_or_none = [&](const char *slot, const String &baked, const char *suffix) {
		if (!baked.empty() || mat->findTexture(slot) < 0)
			return true;
		const String current = resolveAssetPath(mat->getTexturePath(slot));
		if (current.empty() || !strncmp(current.get(), "core/", 5))
			return true; // the default texture of the base material: nothing to tile
		const String name = pathBaseName(current);
		const String bakedName = baseName + suffix;
		return name == bakedName;
	};
	if (mat->findParameter("uv_transform") >= 0)
	{
		const vec4 identity(1.0f, 1.0f, 0.0f, 0.0f);
		if (baked_or_none("albedo", result.albedoPath, "_alb") && baked_or_none("shading", result.shadingPath, "_sh")
			&& baked_or_none("normal", result.normalPath, "_n"))
			mat->setParameterFloat4("uv_transform", identity);
		else if (mat->getParameterFloat4("uv_transform") != identity)
			result.uvTransformKept = true;
	}
	if (!result.albedoPath.empty() && mat->findTexture("albedo") >= 0)
	{
		mat->setTexturePath("albedo", result.albedoPath.get());
		// the baked texture already contains the final color: the material
		// must not tint it again
		if (mat->findParameter("albedo_color") >= 0)
			mat->setParameterFloat4("albedo_color", vec4(1.0f, 1.0f, 1.0f, 1.0f));
	}
	if (!result.shadingPath.empty() && mat->findTexture("shading") >= 0)
	{
		mat->setTexturePath("shading", result.shadingPath.get());
		// the baked _sh stores the final values; the material multipliers must
		// pass them through (mesh_base defaults metalness to 0, killing the R channel)
		if (mat->findParameter("metalness") >= 0)
			mat->setParameterFloat("metalness", 1.0f);
		if (mat->findParameter("roughness") >= 0)
			mat->setParameterFloat("roughness", 1.0f);
	}
	if (!result.normalPath.empty() && mat->findTexture("normal") >= 0)
		mat->setTexturePath("normal", result.normalPath.get());
	if (!result.emissionPath.empty() && mat->findState("emission") >= 0)
	{
		// the baked texture stores the final emission color (divided by emissionScale when
		// brighter than a map holds): the color neutral, the scale brings the brightness back
		mat->setState("emission", 1);
		if (mat->findTexture("emission") >= 0)
			mat->setTexturePath("emission", result.emissionPath.get());
		if (mat->findParameter("emission_color") >= 0)
			mat->setParameterFloat4("emission_color", vec4(1.0f, 1.0f, 1.0f, 1.0f));
		if (mat->findParameter("emission_scale") >= 0)
			mat->setParameterFloat("emission_scale", result.emissionScale);
	}
	mat->save();
	Log::message("Baker: material \"%s\" updated\n", result.materialPath.get());
}

std::vector<Ptr<ObjectMeshStatic>> collectMeshes(const NodePtr &root)
{
	// disabled (hidden) nodes are included on purpose: the UI hides models
	// for convenient mask painting, and hiding must not affect baking
	std::vector<Ptr<ObjectMeshStatic>> result;
	if (!root)
		return result;
	Ptr<ObjectMeshStatic> mesh = checked_ptr_cast<ObjectMeshStatic>(root);
	if (mesh)
		result.push_back(mesh);
	for (int i = 0; i < root->getNumChildren(); i++)
	{
		NodePtr child = root->getChild(i);
		if (!child)
			continue;
		std::vector<Ptr<ObjectMeshStatic>> sub = collectMeshes(child);
		result.insert(result.end(), sub.begin(), sub.end());
	}
	return result;
}

String suggestBaseName(const Ptr<ObjectMeshStatic> &high)
{
	String baseName;
	if (high)
	{
		String assetPath = resolveAssetPath(high->getMeshPath());
		if (!isGuidPath(assetPath))
			baseName = pathBaseName(assetPath);
		if (baseName.empty())
			baseName = high->getName();
	}
	if (baseName.empty())
		baseName = "baked";
	return baseName;
}

CageShell buildCageShell(const std::vector<Ptr<ObjectMeshStatic>> &lows)
{
	CageShell cage;
	cage.origin = !lows.empty() && lows[0] ? lows[0]->getWorldPosition() : Vec3_zero;
	for (const Ptr<ObjectMeshStatic> &low : lows)
	{
		if (!low)
			continue;
		Ptr<ConstMesh> mesh = low->getMeshForceRAM();
		if (!mesh)
			continue;
		const mat4 tm = relativeTransform(low->getWorldTransform(), cage.origin);
		// a mirrored part (negative scale): its triangles wound back the usual way, as the
		// bake does (TargetTri::mirrored)
		const bool mirrored = mat3(tm).determinant() < 0.0f;
		for (int s = 0; s < mesh->getNumSurfaces(); s++)
		{
			if (!isSurfaceBakeable(low, s))
				continue;
			const Vector<int> &cind = mesh->getCIndices(s);
			for (int k = 0; k + 2 < cind.size(); k += 3)
			{
				for (int v = 0; v < 3; v++)
					cage.points.push_back(tm * mesh->getVertex(cind[k + (mirrored && v > 0 ? 3 - v : v)], s));
				cage.part.push_back(low->getID());
			}
		}
	}

	// The cage of the bake.
	CageNormals smoothed;
	for (size_t i = 0; i + 2 < cage.points.size(); i += 3)
		smoothed.addTriangle(cage.points[i], cage.points[i + 1], cage.points[i + 2]);
	std::set<std::pair<QuantizedPos, QuantizedPos>> edges;
	for (size_t i = 0; i + 2 < cage.points.size(); i += 3)
	{
		const vec3 faceN = cross(cage.points[i + 1] - cage.points[i], cage.points[i + 2] - cage.points[i]);
		const vec3 fallback = length2(faceN) > 1e-20f ? normalize(faceN) : vec3(0.0f, 0.0f, 1.0f);
		for (int k = 0; k < 3; k++)
		{
			const vec3 &p = cage.points[i + k];
			cage.normals.push_back(smoothed.at(p, fallback));
			cage.shell.push_back(smoothed.shell(p));
			// each edge once: by its ends merged by position
			const int from = int(i) + k;
			const int to = int(i) + (k + 1) % 3;
			QuantizedPos a = quantize(cage.points[from]);
			QuantizedPos b = quantize(cage.points[to]);
			if (b < a)
				std::swap(a, b);
			if (edges.insert({a, b}).second)
			{
				cage.edges.push_back(from);
				cage.edges.push_back(to);
			}
		}
	}
	return cage;
}

bool isSurfaceBakeable(const Ptr<ObjectMeshStatic> &obj, int surface)
{
	if (!obj || surface < 0 || surface >= obj->getNumSurfaces())
		return false;
	if (!obj->isEnabled(surface))
		return false;
	if (obj->getViewportMask(surface) == 0)
		return false;
	if (obj->getMinVisibleDistance(surface) > 0.0f || obj->getMaxVisibleDistance(surface) <= 0.0f)
		return false;
	return true;
}

std::map<int, vec2> suggestPartCage(const std::vector<BakeGroup> &groups,
	float maxProbe, const ProgressFn &progress)
{
	std::map<int, vec2> out;
	const float probe = Math::max(maxProbe, 1e-4f);
	// about the first low-poly part, as the bake itself
	const Vec3 origin = !groups.empty() && !groups[0].lows.empty() && groups[0].lows[0]
		? groups[0].lows[0]->getWorldPosition() : Vec3_zero;

	int partsTotal = 0;
	for (const BakeGroup &g : groups)
		partsTotal += int(g.lows.size());
	int partsDone = 0;

	for (const BakeGroup &g : groups)
	{
		// high-poly triangles of this group, positions only: the probe just needs
		// the nearest hit, no materials, UVs or normals
		std::vector<SourceTri> tris;
		for (const Ptr<ObjectMeshStatic> &high : g.highs)
		{
			Ptr<ConstMesh> hm = high->getMeshForceRAM();
			if (!hm)
				continue;
			mat4 tm = relativeTransform(high->getWorldTransform(), origin);
			for (int s = 0; s < hm->getNumSurfaces(); s++)
			{
				if (!isSurfaceBakeable(high, s))
					continue;
				const Vector<int> &cind = hm->getCIndices(s);
				for (int k = 0; k + 2 < cind.size(); k += 3)
				{
					const vec3 a = tm * hm->getVertex(cind[k], s);
					const vec3 b = tm * hm->getVertex(cind[k + 1], s);
					const vec3 c = tm * hm->getVertex(cind[k + 2], s);
					SourceTri st;
					st.p0 = a;
					st.e1 = b - a;
					st.e2 = c - a;
					st.surface = 0;
					st.t0 = st.t1 = st.t2 = 0;
					st.corner = 0;
					tris.push_back(st);
				}
			}
		}
		if (tris.empty())
			continue;
		BVH bvh;
		bvh.build(tris);
		auto nearest = [](int, float t, float, float) { return t; };

		for (const Ptr<ObjectMeshStatic> &low : g.lows)
		{
			partsDone++;
			if (progress
				&& !progress(partsTotal > 0 ? partsDone * 100 / partsTotal : 100, "Probing the cage"))
				return out;
			Ptr<ConstMesh> lm = low->getMeshForceRAM();
			if (!lm)
				continue;
			mat4 tm = relativeTransform(low->getWorldTransform(), origin);
			mat3 nm = transpose(inverse(mat3(tm)));

			// Probe range for THIS part, from its own size. Using the whole
			// model's extent here is what made the probe useless: on a car
			// interior it came out at 0.3 m, and from that height a ray hits the
			// roof or the dashboard long before the part's own high-poly, so the
			// statistics measured the distance to NEIGHBOURS.
			float probeLen = probe;
			{
				const auto bb = low->getWorldBoundBox();
				const vec3 diag = vec3(bb.maximum) - vec3(bb.minimum);
				const float d = length(diag);
				if (d > 0.0f)
					probeLen = Math::clamp(d * 0.1f, 0.005f, probe);
			}

			// where the high-poly detail sits relative to the low-poly surface
			std::vector<float> above, below;
			for (int s = 0; s < lm->getNumSurfaces(); s++)
			{
				// as the bake: a disabled surface, a hidden one, a far LOD is not baked
				if (!isSurfaceBakeable(low, s))
					continue;
				const Vector<int> &cind = lm->getCIndices(s);
				const Vector<int> &tind = lm->getTIndices(s);
				if (cind.size() != tind.size())
					continue;
				const int numTris = cind.size() / 3;
				// a few thousand probes per part are plenty to find the extremes
				const int step = numTris > 3000 ? numTris / 3000 : 1;
				for (int k = 0; k < numTris; k += step)
				{
					const int c0 = k * 3;
					const vec3 a = tm * lm->getVertex(cind[c0], s);
					const vec3 b = tm * lm->getVertex(cind[c0 + 1], s);
					const vec3 c = tm * lm->getVertex(cind[c0 + 2], s);
					// authored shading normals: reliably outward, unlike a face
					// cross product whose sign follows the triangle winding
					vec3 n = nm * lm->getTangent(tind[c0], s).getNormal()
						+ nm * lm->getTangent(tind[c0 + 1], s).getNormal()
						+ nm * lm->getTangent(tind[c0 + 2], s).getNormal();
					if (length2(n) < 1e-12f)
						continue;
					n = normalize(n);
					const vec3 pos = (a + b + c) / 3.0f;
					// Two SHORT rays from the surface itself, outward and inward,
					// each taking the FIRST hit. Starting at the surface is what
					// keeps the measurement honest: the part's own high-poly is
					// millimetres away and always wins, while a foreign surface
					// further along the normal is simply out of range.
					const float eps = 1e-4f;
					float t = 0.0f, u = 0.0f, v = 0.0f;
					if (bvh.trace(tris, pos + n * eps, n, probeLen, nearest, t, u, v) >= 0)
						above.push_back(t + eps);
					if (bvh.trace(tris, pos - n * eps, -n, probeLen, nearest, t, u, v) >= 0)
						below.push_back(t + eps);
				}
			}
			if (above.empty() && below.empty())
				continue; // nothing hit: keep whatever the caller has for this part

			// the single farthest probe is often a stray hit on a neighbouring
			// part, so take a high percentile instead of the maximum, then add a
			// margin so the cage is not exactly on the limit
			auto percentile = [](std::vector<float> &v, float p) -> float {
				if (v.empty())
					return 0.0f;
				std::sort(v.begin(), v.end());
				const size_t i = size_t(p * float(v.size() - 1) + 0.5f);
				return v[Math::min(i, v.size() - 1)];
			};
			// 95th percentile, not the maximum: a handful of samples always land
			// on a neighbour through a gap, and the cage must not be sized by
			// them. Erring tight is the safe direction — a cage that is too big
			// makes parts catch each other's detail, while a slightly small one
			// only misses the deepest crevice (and shows up in the miss stats).
			const float fRaw = percentile(above, 0.95f);
			const float rRaw = percentile(below, 0.95f);
			const float f = fRaw * 1.1f + 0.0005f;
			const float r = rRaw * 1.1f + 0.0005f;
			out[low->getID()] =
				vec2(Math::clamp(f, 0.002f, probeLen), Math::clamp(r, 0.002f, probeLen));
			Log::message("Baker: cage probe \"%s\": frontal=%.4f rear=%.4f "
						 "(range %.3f, samples %d/%d, median %.4f/%.4f)\n",
				low->getName(), out[low->getID()].x, out[low->getID()].y, probeLen,
				int(above.size()), int(below.size()),
				percentile(above, 0.5f), percentile(below, 0.5f));
		}
	}
	return out;
}

String createSkewMask(const Ptr<ObjectMeshStatic> &low, int size, String &error)
{
	error = "";
	if (!low)
	{
		error = "The low-poly model is not set.";
		return String();
	}
	String lowAssetPath = resolveAssetPath(low->getMeshPath());
	if (lowAssetPath.empty() || isGuidPath(lowAssetPath))
	{
		error = "The low-poly model has no mesh file — save the model as an asset.";
		return String();
	}
	String dataPath = Engine::get()->getDataPath();
	if (dataPath.empty())
	{
		error = "Cannot determine the project data folder.";
		return String();
	}
	if (dataPath[dataPath.size() - 1] != '/' && dataPath[dataPath.size() - 1] != '\\')
		dataPath += "/";

	String fileName = pathBaseName(lowAssetPath) + "_skew.png";
	String virtualPath = pathDir(lowAssetPath) + fileName;
	String absPath = dataPath + virtualPath;

	namespace fs = std::filesystem;
	std::error_code ec;
	if (!fs::exists(fs::u8path(absPath.get()), ec))
	{
		ImagePtr img = Image::create();
		img->create2D(size, size, Image::FORMAT_R8);
		unsigned char *px = img->getPixels2D();
		if (px)
			memset(px, 0, img->getPixelsSize());

		// same VFS workaround as the baked textures: save to an OS temp file and
		// copy over, so the write lands in the asset file and not in .runtimes
		fs::path tmpPath = fs::temp_directory_path(ec) / "baker_skew_tmp.png";
		if (ec || !img->save(tmpPath.string().c_str()))
		{
			error = "Failed to save the mask to a temporary file.";
			return String();
		}
		fs::copy_file(tmpPath, fs::u8path(absPath.get()), fs::copy_options::overwrite_existing, ec);
		std::error_code ec2;
		fs::remove(tmpPath, ec2);
		if (ec)
		{
			error = String("Failed to write the mask: ") + absPath;
			return String();
		}
		Log::message("Baker: created skew mask \"%s\" (%dx%d)\n", absPath.get(), size, size);
	}
	else
	{
		Log::message("Baker: skew mask \"%s\" already exists, reusing\n", absPath.get());
	}

	return virtualPath;
}

void assignSkewMask(const Ptr<ObjectMeshStatic> &low, const char *virtualPath)
{
	if (!low || !virtualPath || !*virtualPath)
		return;
	// assign into the slot the editor's Texture Editor paints; must run AFTER
	// the asset is imported, otherwise the engine cannot resolve the path.
	// Use the guid:// form — that is what the editor stores on manual assignment.
	String assignPath = virtualPath;
	UGUID guid = FileSystem::getGUID(virtualPath);
	if (guid.isValid())
		assignPath = String("guid://") + String(guid.makeString().get());
	for (int s = 0; s < low->getNumSurfaces(); s++)
	{
		// enable FIRST: the path setter may be a no-op on a disabled slot
		low->setSurfaceCustomTextureEnabled(true, s);
		low->setSurfaceCustomTextureMode(ObjectMeshStatic::SURFACE_CUSTOM_TEXTURE_MODE_UNIQUE, s);
		low->setSurfaceCustomTexturePath(assignPath.get(), s);
		Log::message("Baker: surface %d custom texture set to \"%s\", read back: enabled=%d path=\"%s\"\n",
			s, assignPath.get(), int(low->isSurfaceCustomTextureEnabled(s)),
			low->getSurfaceCustomTexturePath(s));
	}
}

} // namespace BakeCore
