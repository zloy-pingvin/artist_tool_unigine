#include "BakeGpu.h"

#include <UnigineCallback.h>
#include <UnigineLog.h>
#include <UnigineMaterials.h>
#include <UnigineMathLib.h>
#include <UnigineRender.h>
#include <UnigineTextures.h>
#include <UnigineViewport.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <memory>
#include <unordered_map>
#include <vector>

using namespace Unigine;
using namespace Unigine::Math;

namespace BakeGpu
{

// The readbacks requested and not delivered yet: their callbacks are code of the plugin
// the engine is still to call (see transfersInFlight()).
static std::atomic<int> g_transfers_in_flight{0};

int transfersInFlight()
{
	return g_transfers_in_flight.load(std::memory_order_acquire);
}

namespace
{

// CPU port of pack888To1212 (core/materials/shaders/api/pack_utils.h)
vec2 pack888To1212(const vec3 &value)
{
	vec3 x888 = vec3(Math::floor(value.x * 255.0f), Math::floor(value.y * 255.0f), Math::floor(value.z * 255.0f));
	float high = Math::floor(x888.z / 16.0f);
	float low = x888.z - high * 16.0f;
	vec2 x1212 = vec2(x888.x + low * 256.0f, x888.y + high * 256.0f);
	return vec2(saturate(x1212.x / 4095.0f), saturate(x1212.y / 4095.0f));
}

// CPU port of unpackOctahedronToUnitVector
vec3 unpackOctahedronToUnitVector(const vec2 &octahedron)
{
	vec3 n(octahedron.x, octahedron.y, 1.0f - Math::abs(octahedron.x) - Math::abs(octahedron.y));
	if (n.z <= 0.0f)
	{
		float sx = n.x > 0.0f ? 1.0f : -1.0f;
		float sy = n.y > 0.0f ? 1.0f : -1.0f;
		float ox = (1.0f - Math::abs(n.y)) * sx;
		float oy = (1.0f - Math::abs(n.x)) * sy;
		n.x = ox;
		n.y = oy;
	}
	return normalize(n);
}

} // namespace

vec3 unpackGBufferNormal(const vec4 &rgba)
{
	vec2 oct = pack888To1212(vec3(rgba.x, rgba.y, rgba.z)) * 2.0f - vec2(1.0f, 1.0f);
	return unpackOctahedronToUnitVector(oct);
}

// Rasterizes the surface triangles (positions per triangle CORNER, three
// consecutive entries per triangle, in the unit square) into a per-texel claim
// counter (saturating at 255).
// minArea (in texels²) skips triangles too small for the GPU rasterizer to
// produce any pixel. It matters when the mask is used as CAPTURE COVERAGE: the
// inside test below is non-strict, so a triangle collapsed to zero area (a UV
// set that simply has no unwrap for those faces) passes with all weights 0 and
// would be reported as covered while the render drew nothing there. Left at 0
// for the overlap metric, whose meaning does not change.
static void rasterizeCounts(const std::vector<vec2> &pt,
	int width, int height, std::vector<unsigned char> &cnt, float minArea = 0.0f)
{
	cnt.assign(size_t(width) * size_t(height), 0);
	for (int t = 0; t + 2 < int(pt.size()); t += 3)
	{
		// skip uncapturable charts (marked with negative atlas coordinates)
		if (pt[t].x < 0.0f || pt[t + 1].x < 0.0f || pt[t + 2].x < 0.0f)
			continue;
		vec2 p[3];
		for (int k = 0; k < 3; k++)
			p[k] = vec2(pt[t + k].x * float(width), pt[t + k].y * float(height));
		if (minArea > 0.0f)
		{
			const float area2 = Math::abs((p[1].x - p[0].x) * (p[2].y - p[0].y)
				- (p[1].y - p[0].y) * (p[2].x - p[0].x));
			if (area2 * 0.5f < minArea)
				continue;
		}
		int x0 = Math::clamp(int(Math::floor(Math::min(p[0].x, Math::min(p[1].x, p[2].x)))), 0, width - 1);
		int x1 = Math::clamp(int(Math::ceil(Math::max(p[0].x, Math::max(p[1].x, p[2].x)))), 0, width - 1);
		int y0 = Math::clamp(int(Math::floor(Math::min(p[0].y, Math::min(p[1].y, p[2].y)))), 0, height - 1);
		int y1 = Math::clamp(int(Math::ceil(Math::max(p[0].y, Math::max(p[1].y, p[2].y)))), 0, height - 1);
		vec2 e0 = p[1] - p[0], e1 = p[2] - p[1], e2 = p[0] - p[2];
		float sign = e0.x * (p[2].y - p[0].y) - e0.y * (p[2].x - p[0].x) >= 0.0f ? 1.0f : -1.0f;
		for (int y = y0; y <= y1; y++)
			for (int x = x0; x <= x1; x++)
			{
				vec2 c(x + 0.5f, y + 0.5f);
				float w0 = (e0.x * (c.y - p[0].y) - e0.y * (c.x - p[0].x)) * sign;
				float w1 = (e1.x * (c.y - p[1].y) - e1.y * (c.x - p[1].x)) * sign;
				float w2 = (e2.x * (c.y - p[2].y) - e2.y * (c.x - p[2].x)) * sign;
				if (w0 >= 0.0f && w1 >= 0.0f && w2 >= 0.0f)
				{
					unsigned char &v = cnt[y * width + x];
					if (v < 255)
						v++;
				}
			}
	}
}

//------------------------------------------------------------------------------
// Chart repacking. Tiled/overlapping unwraps make the flat-clone render
// ambiguous: several mesh locations land on the same capture texel, so one of
// them wins and its neighbors get foreign data (visibly wrong UV1-driven dirt
// layers). The channel is split into charts (UV-connected triangle islands)
// whose bboxes are packed into the unit atlas without overlap; the capture is
// rendered and sampled back through this packed parameterization. Repacking
// also normalizes heavily tiled unwraps (a 17x17-tile bbox squeezed into the
// capture leaves ~100 texels per tile — mush).
//------------------------------------------------------------------------------
struct ChartPack
{
	bool valid = false;
	// per triangle CORNER (three consecutive entries per triangle), in the unit
	// atlas; (-1,-1) = uncapturable chart. Per-corner storage lets stacked
	// instanced charts be separated even when their tvertices are welded.
	std::vector<vec2> atlasUV;
	int numCharts = 0;
	int invalidTris = 0;  // triangles of degenerate/sub-texel charts (CPU fallback)
	float overlap = 1.0f; // post-pack overlap: only chart-internal folds remain
	// fraction of triangles that receive at least ~one texel of atlas area.
	// A tiling/detail UV set can carry most of its triangles collapsed to
	// (near) zero area: the capture then looks fine (a few big triangles cover
	// the atlas) while every collapsed triangle has no pixels of its own and
	// samples back as black background.
	float usableFrac = 0.0f;
	float scale = 0.0f;   // channel UV units -> atlas units
};

static ChartPack buildChartPack(const Ptr<ConstMesh> &mesh, int surface, int channel,
	const Vector<int> &tind, const Vector<int> &cind, int numT, float margin, int size)
{
	ChartPack pack;
	(void)numT;

	const int numC = tind.size(); // corner slots, 3 per triangle
	std::vector<vec2> uv(numC);
	for (int i = 0; i < numC; i++)
		uv[i] = channel ? mesh->getTexCoord1(tind[i], surface) : mesh->getTexCoord0(tind[i], surface);

	vec2 uvMin(1e9f, 1e9f), uvMax(-1e9f, -1e9f);
	for (int i = 0; i < numC; i++)
	{
		uvMin = min(uvMin, uv[i]);
		uvMax = max(uvMax, uv[i]);
	}
	// a collapsed bbox means a dummy channel (e.g. all zeros)
	if (uvMax.x - uvMin.x < 1e-3f || uvMax.y - uvMin.y < 1e-3f)
		return pack;

	// charts = islands of triangle corners connected through mesh geometry:
	// the 3 corners of a triangle, plus triangles sharing a mesh edge (same
	// position indices) with matching UV values on both ends. Deliberately NOT
	// united by shared tvertex index alone — stacked instanced charts are often
	// welded to identical tvertices, and only position-aware connectivity can
	// pull them apart into their own atlas rects.
	std::vector<int> parent(numC);
	for (int i = 0; i < numC; i++)
		parent[i] = i;
	auto findRoot = [&parent](int i) {
		while (parent[i] != i)
		{
			parent[i] = parent[parent[i]];
			i = parent[i];
		}
		return i;
	};
	auto unite = [&parent, &findRoot](int a, int b) {
		a = findRoot(a);
		b = findRoot(b);
		if (a != b)
			parent[b] = a;
	};
	for (int t = 0; t + 2 < numC; t += 3)
	{
		unite(t, t + 1);
		unite(t, t + 2);
	}

	if (cind.size() == tind.size())
	{
		const float weldEps = 2e-4f;
		auto sameUV = [&uv, weldEps](int a, int b) {
			return Math::abs(uv[a].x - uv[b].x) < weldEps && Math::abs(uv[a].y - uv[b].y) < weldEps;
		};
		std::unordered_map<unsigned long long, std::pair<int, int>> edges; // position edge -> corner slots
		edges.reserve(numC);
		for (int t = 0; t + 2 < numC; t += 3)
			for (int k = 0; k < 3; k++)
			{
				int i0 = t + k, i1 = t + (k + 1) % 3;
				unsigned int a = (unsigned int)cind[i0], b = (unsigned int)cind[i1];
				unsigned long long key = a < b ? ((unsigned long long)a << 32) | b
											   : ((unsigned long long)b << 32) | a;
				auto it = edges.find(key);
				if (it == edges.end())
				{
					edges[key] = {i0, i1};
					continue;
				}
				// match corners by position index (the neighbor lists the edge reversed)
				int j0 = it->second.first, j1 = it->second.second;
				if (cind[j0] != cind[i0])
					std::swap(j0, j1);
				if (cind[j0] == cind[i0] && cind[j1] == cind[i1]
					&& sameUV(i0, j0) && sameUV(i1, j1))
				{
					unite(i0, j0);
					unite(i1, j1);
				}
			}
	}

	struct Rect
	{
		vec2 mn{1e9f, 1e9f};
		vec2 mx{-1e9f, -1e9f};
		vec2 pos{0.0f, 0.0f};
		bool rot = false; // packed rotated 90° (tall thin charts pack denser)
	};
	std::vector<Rect> charts;
	std::vector<int> chartOf(numC, -1);
	for (int i = 0; i < numC; i++)
	{
		int r = findRoot(i);
		if (chartOf[r] < 0)
		{
			chartOf[r] = int(charts.size());
			charts.push_back(Rect());
		}
		Rect &c = charts[chartOf[r]];
		c.mn = min(c.mn, uv[i]);
		c.mx = max(c.mx, uv[i]);
	}
	if (charts.empty())
		return pack;

	// texel density equalization: a chart's atlas RECT gets the area of the
	// chart on the MESH (in m²), not its UV size — unwrap density varies wildly
	// (big roof slopes are usually compressed in UV and would bake into mush
	// while tiny pieces stay sharp). Rect area == mesh area also means the
	// total packed area equals the model's area: a thin diagonal sliver with a
	// huge near-empty bbox pays for its own waste with lower internal density
	// instead of blowing up the whole atlas and starving every other chart.
	std::vector<float> meshArea(charts.size(), 0.0f);
	bool haveAreas = cind.size() == tind.size();
	if (haveAreas)
		for (int t = 0; t + 2 < numC; t += 3)
		{
			int c = chartOf[findRoot(t)];
			if (c < 0)
				continue;
			vec3 p0 = mesh->getVertex(cind[t], surface);
			vec3 p1 = mesh->getVertex(cind[t + 1], surface);
			vec3 p2 = mesh->getVertex(cind[t + 2], surface);
			meshArea[c] += 0.5f * length(cross(p1 - p0, p2 - p0));
		}
	std::vector<float> chartK(charts.size(), 1.0f);
	if (haveAreas)
		for (int c = 0; c < int(charts.size()); c++)
		{
			vec2 bb = charts[c].mx - charts[c].mn;
			float bboxArea = Math::max(bb.x, 1e-6f) * Math::max(bb.y, 1e-6f);
			// rect (bw*k) x (bh*k) == meshArea
			chartK[c] = sqrtf(Math::max(meshArea[c], 1e-9f) / bboxArea);
		}

	// tall thin charts lie down (true 90° rotation, orientation preserved) so
	// the shelves stay low and pack denser
	for (int c = 0; c < int(charts.size()); c++)
	{
		vec2 bb = charts[c].mx - charts[c].mn;
		charts[c].rot = bb.y > bb.x * 1.5f;
	}

	// shelf packing, charts sorted by height; binary search for the best scale
	const float eps = 1e-4f;
	auto sizeOf = [&charts, &chartK, eps](int c) {
		vec2 s = (charts[c].mx - charts[c].mn) * chartK[c];
		if (charts[c].rot)
			s = vec2(s.y, s.x);
		return vec2(Math::max(s.x, eps), Math::max(s.y, eps));
	};
	std::vector<int> order(charts.size());
	for (int i = 0; i < int(order.size()); i++)
		order[i] = i;
	std::sort(order.begin(), order.end(),
		[&sizeOf](int a, int b) { return sizeOf(a).y > sizeOf(b).y; });

	float usedW = 1.0f, usedH = 1.0f; // filled by the storing tryPack call
	auto tryPack = [&](float s, bool store) -> bool {
		float x = margin, y = margin, shelf = 0.0f;
		float maxX = 0.0f, maxY = 0.0f;
		for (int ci : order)
		{
			vec2 wh = sizeOf(ci) * s;
			if (wh.x > 1.0f - 2.0f * margin)
				return false;
			if (x + wh.x + margin > 1.0f)
			{
				x = margin;
				y += shelf + margin;
				shelf = 0.0f;
			}
			if (y + wh.y + margin > 1.0f)
				return false;
			if (store)
				charts[ci].pos = vec2(x, y);
			maxX = Math::max(maxX, x + wh.x);
			maxY = Math::max(maxY, y + wh.y);
			x += wh.x + margin;
			shelf = Math::max(shelf, wh.y);
		}
		if (store)
		{
			usedW = maxX + margin;
			usedH = maxY + margin;
		}
		return true;
	};

	float maxW = eps, maxH = eps;
	for (int c = 0; c < int(charts.size()); c++)
	{
		vec2 s = sizeOf(c);
		maxW = Math::max(maxW, s.x);
		maxH = Math::max(maxH, s.y);
	}
	float lo = 0.0f;
	float hi = Math::min((1.0f - 2.0f * margin) / maxW, (1.0f - 2.0f * margin) / maxH);
	if (tryPack(hi, false))
		lo = hi;
	else
		for (int it = 0; it < 24; it++)
		{
			float mid = 0.5f * (lo + hi);
			if (tryPack(mid, false))
				lo = mid;
			else
				hi = mid;
		}

	pack.atlasUV.assign(numC, vec2(0.0f, 0.0f));
	if (lo > 0.0f && tryPack(lo, true))
	{
		pack.scale = lo;
		pack.numCharts = int(charts.size());

		// stretch the packed layout to fill the whole unit atlas: shelf packing
		// of few/elongated charts leaves unused space, and unlike the mesh UVs
		// the capture parameterization is free to be anisotropic — stretching
		// costs nothing and recovers texel density (margins only grow)
		const float fx = 1.0f / Math::max(usedW, 1e-3f);
		const float fy = 1.0f / Math::max(usedH, 1e-3f);

		// charts that end up smaller than ~a texel cannot be rendered into the
		// capture at all (degenerate or micro UVs) — sampling them would return
		// gutter fill from foreign charts. Mark them for the CPU fallback.
		std::vector<char> chartInvalid(charts.size(), 0);
		for (int c = 0; c < int(charts.size()); c++)
		{
			vec2 wh = sizeOf(c) * lo;
			if (wh.x * fx * float(size) < 1.5f || wh.y * fy * float(size) < 1.5f)
				chartInvalid[c] = 1;
		}

		for (int i = 0; i < numC; i++)
		{
			int c = chartOf[findRoot(i)];
			if (chartInvalid[c])
			{
				pack.atlasUV[i] = vec2(-1.0f, -1.0f);
				continue;
			}
			const Rect &r = charts[c];
			vec2 local = (uv[i] - r.mn) * (chartK[c] * lo);
			if (r.rot)
			{
				// true rotation (x,y)->(y, W-x): keeps the winding orientation,
				// unlike a transpose which would mirror the chart
				float w = (r.mx.x - r.mn.x) * chartK[c] * lo;
				local = vec2(local.y, w - local.x);
			}
			vec2 p = r.pos + local;
			pack.atlasUV[i] = vec2(p.x * fx, p.y * fy);
		}

		for (int t = 0; t + 2 < numC; t += 3)
			if (pack.atlasUV[t].x < 0.0f)
				pack.invalidTris++;
	}
	else
	{
		// pathological pack (e.g. thousands of charts leave no room for the
		// margins) — plain bbox normalization, same as the pre-repack behavior
		vec2 span = uvMax - uvMin;
		span.x = Math::max(span.x, eps);
		span.y = Math::max(span.y, eps);
		pack.numCharts = 1;
		for (int i = 0; i < numC; i++)
			pack.atlasUV[i] = vec2((uv[i].x - uvMin.x) / span.x, (uv[i].y - uvMin.y) / span.y);
	}

	// how much of the surface actually gets capturable atlas area
	{
		int usable = 0, tris = 0;
		const float sz = float(size);
		for (int t = 0; t + 2 < numC; t += 3)
		{
			tris++;
			if (pack.atlasUV[t].x < 0.0f)
				continue; // uncapturable chart: CPU fallback, counted as unusable
			vec2 a = pack.atlasUV[t] * sz;
			vec2 b = pack.atlasUV[t + 1] * sz;
			vec2 c = pack.atlasUV[t + 2] * sz;
			const float area =
				0.5f * Math::abs((b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x));
			if (area >= 1.0f)
				usable++;
		}
		pack.usableFrac = tris > 0 ? float(usable) / float(tris) : 0.0f;
	}

	// residual overlap of the packed layout (chart-internal folds only)
	std::vector<unsigned char> cnt;
	rasterizeCounts(pack.atlasUV, 256, 256, cnt);
	int covered = 0, multi = 0;
	for (unsigned char v : cnt)
	{
		if (v > 0)
			covered++;
		if (v > 1)
			multi++;
	}
	if (covered == 0)
		return pack;
	pack.overlap = float(multi) / float(covered);
	pack.valid = true;
	return pack;
}

// Fills the empty gutters between the packed charts by copying border texels
// outward. Without this, bilinear sampling at a chart edge mixes in the black
// render background — dark seam lines along every chart border.
static void dilateCaptures(const std::vector<ImagePtr> &images,
	const std::vector<vec2> &atlasUV, int width, int height,
	std::vector<unsigned char> *outCoverage = nullptr)
{
	std::vector<unsigned char> mask;
	// half a texel: below that the GPU cannot cover a texel centre either, so
	// such triangles must not be reported as captured (they would read
	// background and bake black instead of falling back to the textures)
	rasterizeCounts(atlasUV, width, height, mask, 0.5f);

	static const int dx[8] = {1, -1, 0, 0, 1, 1, -1, -1};
	static const int dy[8] = {0, 0, 1, -1, 1, -1, 1, -1};
	const int passes = 6;

	struct Fill
	{
		int x, y, sx, sy;
	};
	std::vector<Fill> fills;
	for (int pass = 0; pass < passes; pass++)
	{
		fills.clear();
		for (int y = 0; y < height; y++)
			for (int x = 0; x < width; x++)
			{
				if (mask[y * width + x])
					continue;
				for (int k = 0; k < 8; k++)
				{
					int nx = x + dx[k], ny = y + dy[k];
					if (nx < 0 || ny < 0 || nx >= width || ny >= height)
						continue;
					if (mask[ny * width + nx])
					{
						fills.push_back({x, y, nx, ny});
						break;
					}
				}
			}
		if (fills.empty())
			break;
		for (const Fill &f : fills)
			for (const ImagePtr &img : images)
				if (img)
					img->set2D(f.x, f.y, img->get2D(f.sx, f.sy));
		for (const Fill &f : fills)
			mask[f.y * width + f.x] = 1;
	}

	// hand the final (chart + gutter) coverage to the caller: the bake needs it
	// to tell background apart from a legitimately black material
	if (outCoverage)
		*outCoverage = std::move(mask);
}

PendingCapturePtr requestSurfaceCapture(const Ptr<ObjectMeshStatic> &obj, const Ptr<ConstMesh> &mesh,
	int surface, int size, int uvChannelMode, bool captureEmission)
{
	PendingCapturePtr result; // null = failure

	const int numT = mesh->getNumTexCoords0(surface);
	if (numT <= 0)
		return result;
	const Vector<int> &tind = mesh->getTIndices(surface);
	if (tind.size() < 3)
		return result;

	const int numT1 = mesh->getNumTexCoords1(surface);
	const int numCol = mesh->getNumColors(surface);
	const bool hasUV1 = numT1 >= numT;
	const bool hasColors = numCol >= numT;

	//--------------------------------------------------------------------------
	// Pick the UNWRAP space. Both channels are split into charts and repacked
	// into a unique atlas (raw channels overlap/tile — ambiguous captures); the
	// channel whose packed layout keeps less residual overlap (chart-internal
	// folds) wins. Ties go to the channel with fewer charts (fewer seams).
	//--------------------------------------------------------------------------
	const int MARGIN_TEXELS = 8; // chart spacing in the atlas, filled by dilation
	const float margin = float(MARGIN_TEXELS) / float(Math::max(size, 64));
	const Vector<int> &cind = mesh->getCIndices(surface);
	ChartPack pk0 = buildChartPack(mesh, surface, 0, tind, cind, numT, margin, size);
	ChartPack pk1 = hasUV1 ? buildChartPack(mesh, surface, 1, tind, cind, numT, margin, size) : ChartPack();

	int unwrapChannel = 0;
	if (uvChannelMode == 1)
	{
		unwrapChannel = pk1.valid ? 1 : 0;
		if (!pk1.valid)
			Log::warning("Baker: surface %d: UV1 forced but missing/degenerate, using UV0\n", surface);
	}
	else if (uvChannelMode < 0 && pk1.valid)
	{
		if (!pk0.valid)
			unwrapChannel = 1;
		// a channel that collapses most of its triangles cannot be captured at
		// all — those triangles have no pixels and sample back as background.
		// This outweighs overlap and seam count.
		else if (Math::abs(pk1.usableFrac - pk0.usableFrac) > 0.05f)
			unwrapChannel = pk1.usableFrac > pk0.usableFrac ? 1 : 0;
		else if (fabsf(pk1.overlap - pk0.overlap) < 0.02f)
		{
			// tie on overlap: fewer uncapturable triangles, then fewer seams
			if (pk1.invalidTris != pk0.invalidTris)
				unwrapChannel = pk1.invalidTris < pk0.invalidTris ? 1 : 0;
			else
				unwrapChannel = pk1.numCharts <= pk0.numCharts ? 1 : 0;
		}
		else
			unwrapChannel = pk1.overlap < pk0.overlap ? 1 : 0;
	}

	ChartPack &sel = unwrapChannel ? pk1 : pk0;
	if (!sel.valid)
	{
		Log::warning("Baker: surface %d has no usable UV charts, skipping GPU capture\n", surface);
		return result;
	}

	Unigine::Log::message(
		"Baker: capture surface %d: unwrap in UV%d%s repacked (charts UV0=%d UV1=%d, "
		"packed overlap UV0=%.1f%% UV1=%.1f%%, usable UV0=%.0f%% UV1=%.0f%%, "
		"cpu-fallback tris=%d, scale=%.3f, colors=%d)\n",
		surface, unwrapChannel, uvChannelMode >= 0 ? " [forced]" : "",
		pk0.numCharts, pk1.numCharts,
		pk0.valid ? pk0.overlap * 100.0f : -1.0f, pk1.valid ? pk1.overlap * 100.0f : -1.0f,
		pk0.valid ? pk0.usableFrac * 100.0f : -1.0f,
		pk1.valid ? pk1.usableFrac * 100.0f : -1.0f,
		sel.invalidTris, sel.scale, numCol);

	//--------------------------------------------------------------------------
	// Flat "unwrapped" clone of the surface: vertex position = packed atlas UV.
	// One vertex per triangle CORNER (atlas coords are per corner — welded
	// stacked charts land in different rects); identity tangent basis
	// (T=+X, B=+Y, N=+Z), so the gbuffer normal of this quad IS the material's
	// final tangent-space normal.
	//--------------------------------------------------------------------------
	MeshPtr flatMesh = Mesh::create();
	int fs = flatMesh->addSurface("baker_unwrap");

	const quat identityBasis(0.0f, 0.0f, 0.0f, 1.0f);

	const int numC = tind.size();
	for (int i = 0; i < numC; i++)
	{
		vec2 p = sel.atlasUV[i];
		// V grows downward in texture space -> map to -Y so the image is not mirrored
		flatMesh->addVertex(vec3(p.x, -p.y, 0.0f), fs);
		flatMesh->addTexCoord0(mesh->getTexCoord0(tind[i], surface), fs);
		if (hasUV1)
			flatMesh->addTexCoord1(mesh->getTexCoord1(tind[i], surface), fs);
		if (hasColors)
			flatMesh->addColor(mesh->getColor(tind[i], surface), fs);
		flatMesh->addTangent(identityBasis, fs);
	}

	//--------------------------------------------------------------------------
	// Charts MIRRORED in the unwrap space become back-facing on the flat clone
	// and get culled — black holes in the capture. Flip their winding (indices
	// only: UV interpolation is unaffected). Front-facing here = POSITIVE 2D
	// winding in the final vertex plane (p.x, -p.y) — an absolute convention
	// proven by the background quad, which is wound positive and renders.
	// (The old per-surface anchor to the UV0 majority sign culled ENTIRE
	// surfaces whose UV0 was majority-mirrored — fully black captures.)
	//--------------------------------------------------------------------------
	auto atlasWindingSign = [&sel](int t) -> float {
		vec2 q[3];
		for (int k = 0; k < 3; k++)
		{
			const vec2 &p = sel.atlasUV[t + k];
			q[k] = vec2(p.x, -p.y); // same mapping as the vertex positions
		}
		return (q[1].x - q[0].x) * (q[2].y - q[0].y) - (q[1].y - q[0].y) * (q[2].x - q[0].x);
	};

	// sequential per-corner indices (vertex i == corner slot i)
	Vector<int> fixedInd;
	fixedInd.resize(numC);
	for (int i = 0; i < numC; i++)
		fixedInd[i] = i;
	int flipped = 0;
	for (int t = 0; t + 2 < fixedInd.size(); t += 3)
	{
		if (atlasWindingSign(t) < 0.0f)
		{
			int tmp = fixedInd[t + 1];
			fixedInd[t + 1] = fixedInd[t + 2];
			fixedInd[t + 2] = tmp;
			flipped++;
		}
	}
	if (flipped > 0)
		Log::message("Baker: capture surface %d: unflipped %d mirrored triangles\n", surface, flipped);

	flatMesh->addCIndices(fixedInd, fs);
	flatMesh->addTIndices(fixedInd, fs);

	//--------------------------------------------------------------------------
	// Opaque background quad slightly behind the charts. The gbuffer is NOT
	// cleared between manual node renders, so every texel the charts leave
	// uncovered would keep leftovers of the PREVIOUS surface's capture — thin
	// charts then sample foreign content at their edges (white specks on the
	// corner boards). The quad rasterizes the whole frame deterministically.
	//--------------------------------------------------------------------------
	int bs = flatMesh->addSurface("baker_bg");
	{
		const float bgZ = -0.5f;
		const vec3 corners[4] = {vec3(-1.0f, -2.0f, bgZ), vec3(2.0f, -2.0f, bgZ),
			vec3(2.0f, 1.0f, bgZ), vec3(-1.0f, 1.0f, bgZ)};
		for (int k = 0; k < 4; k++)
		{
			flatMesh->addVertex(corners[k], bs);
			flatMesh->addTexCoord0(vec2(0.0f, 0.0f), bs);
			flatMesh->addTangent(identityBasis, bs);
		}
		Vector<int> bi;
		bi.append(0);
		bi.append(1);
		bi.append(2);
		bi.append(0);
		bi.append(2);
		bi.append(3);
		flatMesh->addCIndices(bi, bs);
		flatMesh->addTIndices(bi, bs);
	}

	//--------------------------------------------------------------------------
	// Temporary node with the surface's real material, far away from the scene.
	//--------------------------------------------------------------------------
	Ptr<ObjectMeshStatic> flatNode = ObjectMeshStatic::create();
	flatNode->setMeshProceduralMode(ObjectMeshStatic::PROCEDURAL_MODE_DYNAMIC);
	if (!flatNode->applyMoveMeshProceduralForce(flatMesh))
	{
		Log::error("Baker GPU: failed to apply procedural unwrap mesh (surface %d)\n", surface);
		flatNode.deleteLater();
		return result;
	}
	// UNIQUE spot per surface! Clones live until the end of the frame
	// (deleteLater), so captures of later surfaces would otherwise see the
	// stacked clones of all earlier ones (z-fighting foreign charts into the
	// atlas — the corner boards bug). The camera depth range is 10 units, so a
	// 100-unit spacing fully isolates each clone.
	const float quadZ = -9000.0f - float(surface) * 100.0f;
	flatNode->setWorldTransform(Mat4(translate(vec3(0.0f, 0.0f, quadZ))));
	flatNode->setMaterial(obj->getMaterial(surface), 0);

	// plain black material for the background quad (tiny runtime-only child
	// material; a handful leak per bake — negligible)
	MaterialPtr bgBase = Materials::findManualMaterial("mesh_base");
	if (!bgBase)
		bgBase = Materials::findManualMaterial("Unigine::mesh_base");
	if (bgBase)
	{
		MaterialPtr bgMat = bgBase->inherit();
		if (bgMat->findParameter("albedo_color") >= 0)
			bgMat->setParameterFloat4("albedo_color", vec4(0.0f, 0.0f, 0.0f, 1.0f));
		flatNode->setMaterial(bgMat, 1);
	}

	//--------------------------------------------------------------------------
	// Orthographic camera looking straight at the quad (identity rotation, so
	// view space == world space up to translation).
	//--------------------------------------------------------------------------
	CameraPtr camera = Camera::create();
	camera->setProjection(ortho(-0.5f, 0.5f, -0.5f, 0.5f, 0.1f, 10.0f));
	camera->setModelview(inverse(Mat4(translate(vec3(0.5f, -0.5f, quadZ + 1.0f)))));

	TexturePtr rt = Texture::create();
	rt->create2D(size, size, Texture::FORMAT_RGBA8,
		Texture::SAMPLER_FILTER_LINEAR | Texture::FORMAT_USAGE_RENDER);

	ViewportPtr viewport = Viewport::create();
	viewport->setSkipFlags(Viewport::SKIP_TRANSPARENT | Viewport::SKIP_VELOCITY_BUFFER
		| Viewport::SKIP_POSTEFFECTS | Viewport::SKIP_VISUALIZER
		| Viewport::SKIP_AUTO_EXPOSURE | Viewport::SKIP_AUTO_WHITE_BALANCE);
	viewport->setEnvironmentTexture(Render::getBlackCubeTexture());
	// no world lights: with the black environment the final image of this render
	// then contains the emission pass output alone (the emission capture source)
	viewport->setNodeLightUsage(Viewport::USAGE_NODE_LIGHT);

	//--------------------------------------------------------------------------
	// Grab the gbuffer right after the opacity pass of our render.
	// The readback transfers must be requested WHILE a render is in progress:
	// they are delivered at the swap stage of the NEXT manual render (that is
	// how it behaves inside the editor). The pending holder lives on the heap
	// and is captured by value, so the late callbacks are always safe.
	//--------------------------------------------------------------------------
	auto pending = std::make_shared<PendingCapture>();
	pending->surface = surface;
	pending->uvChannel = unwrapChannel;
	pending->atlasUV = std::move(sel.atlasUV); // sample the capture through the packed atlas

	EventConnections conns;
	Render::getEventEndOpacityGBuffer().connect(conns, [pending, size]() {
		if (pending->keepA) // only the first fire of our own render
			return;
		TexturePtr srcAlbedo = Renderer::getTextureGBufferAlbedo();
		// ignore nested passes (probes etc.) with a different resolution
		if (!srcAlbedo || srcAlbedo->getWidth() != size || srcAlbedo->getHeight() != size)
			return;
		auto copyOf = [](const TexturePtr &src) -> TexturePtr {
			if (!src)
				return TexturePtr();
			TexturePtr dst = Texture::create();
			dst->create2D(src->getWidth(), src->getHeight(), src->getFormat(),
				Texture::SAMPLER_FILTER_LINEAR | Texture::FORMAT_USAGE_RENDER);
			if (!dst->copy(src))
				return TexturePtr();
			return dst;
		};
		pending->keepA = copyOf(srcAlbedo);
		pending->keepS = copyOf(Renderer::getTextureGBufferShading());
		pending->keepN = copyOf(Renderer::getTextureGBufferNormal());
	});

	viewport->renderNodeTexture2D(camera, flatNode, rt);
	conns.disconnectAll();
	flatNode.deleteLater();

	if (!pending->keepA || !pending->keepS || !pending->keepN)
	{
		Log::error("Baker GPU: gbuffer capture failed (surface %d)\n", surface);
		return PendingCapturePtr();
	}

	// emission: the final image of this isolated render (black environment, no
	// light sources) contains the emission pass output alone
	if (captureEmission)
	{
		pending->expected = 4;
		pending->keepE = rt;
	}

	// request the readbacks OUTSIDE the render: transfers requested here are
	// delivered at the swap stage of a later manual render.
	// IMPORTANT: the engine destroys the delivered image right after the
	// callback returns (engine-managed Ptr becomes null) — clone it
	g_transfers_in_flight.fetch_add(1, std::memory_order_acq_rel);
	Render::transferTextureToImage(
		MakeCallback([pending](ImagePtr img) {
			pending->albedo = img ? Image::create(img) : ImagePtr();
			// the readback landed: drop the render target now instead of holding
			// it until the end of the bake. Keeping one 3-buffer set per surface
			// alive (163 surfaces x 873x873 x RGBA8 ~ 1.5 GB of VRAM) starves the
			// renderer, and captures then come back holding another surface's
			// render — the atlas and its packed coords no longer match.
			pending->keepA = TexturePtr();
			pending->arrived.fetch_add(1, std::memory_order_release);
			g_transfers_in_flight.fetch_sub(1, std::memory_order_acq_rel);
		}),
		pending->keepA);
	g_transfers_in_flight.fetch_add(1, std::memory_order_acq_rel);
	Render::transferTextureToImage(
		MakeCallback([pending](ImagePtr img) {
			pending->shading = img ? Image::create(img) : ImagePtr();
			pending->keepS = TexturePtr();
			pending->arrived.fetch_add(1, std::memory_order_release);
			g_transfers_in_flight.fetch_sub(1, std::memory_order_acq_rel);
		}),
		pending->keepS);
	g_transfers_in_flight.fetch_add(1, std::memory_order_acq_rel);
	Render::transferTextureToImage(
		MakeCallback([pending](ImagePtr img) {
			pending->normal = img ? Image::create(img) : ImagePtr();
			pending->keepN = TexturePtr();
			pending->arrived.fetch_add(1, std::memory_order_release);
			g_transfers_in_flight.fetch_sub(1, std::memory_order_acq_rel);
		}),
		pending->keepN);
	if (captureEmission)
	{
		g_transfers_in_flight.fetch_add(1, std::memory_order_acq_rel);
		Render::transferTextureToImage(
			MakeCallback([pending](ImagePtr img) {
				pending->emission = img ? Image::create(img) : ImagePtr();
				pending->keepE = TexturePtr();
				pending->arrived.fetch_add(1, std::memory_order_release);
			g_transfers_in_flight.fetch_sub(1, std::memory_order_acq_rel);
			}),
			pending->keepE);
	}
	return pending;
}

// A minimal dummy render: its swap stage delivers the readbacks queued during
// the previous render (this is how transfers behave inside the editor).
void flushTransfers()
{
	MeshPtr mesh = Mesh::create();
	int s = mesh->addSurface("baker_flush");
	const quat identityBasis(0.0f, 0.0f, 0.0f, 1.0f);
	for (int i = 0; i < 3; i++)
	{
		mesh->addVertex(vec3(i == 1 ? 1.0f : 0.0f, i == 2 ? 1.0f : 0.0f, 0.0f), s);
		mesh->addTexCoord0(vec2(0.0f, 0.0f), s);
		mesh->addTangent(identityBasis, s);
	}
	Vector<int> idx;
	idx.append(0);
	idx.append(1);
	idx.append(2);
	mesh->addCIndices(idx, s);
	mesh->addTIndices(idx, s);

	// keep away from the per-surface capture spots (-9000 - surface*100)
	Ptr<ObjectMeshStatic> node = ObjectMeshStatic::create();
	node->setMeshProceduralMode(ObjectMeshStatic::PROCEDURAL_MODE_DYNAMIC);
	node->applyMoveMeshProceduralForce(mesh);
	node->setWorldTransform(Mat4(translate(vec3(0.0f, 0.0f, -8000.0f))));

	CameraPtr camera = Camera::create();
	camera->setProjection(ortho(-0.5f, 0.5f, -0.5f, 0.5f, 0.1f, 10.0f));
	camera->setModelview(inverse(Mat4(translate(vec3(0.5f, 0.5f, -7999.0f)))));

	TexturePtr rt = Texture::create();
	rt->create2D(16, 16, Texture::FORMAT_RGBA8,
		Texture::SAMPLER_FILTER_LINEAR | Texture::FORMAT_USAGE_RENDER);

	ViewportPtr viewport = Viewport::create();
	viewport->setSkipFlags(Viewport::SKIP_TRANSPARENT | Viewport::SKIP_VELOCITY_BUFFER
		| Viewport::SKIP_POSTEFFECTS | Viewport::SKIP_VISUALIZER);
	viewport->setEnvironmentTexture(Render::getBlackCubeTexture());
	viewport->renderNodeTexture2D(camera, node, rt);
	node.deleteLater();
}

SurfaceCapture finishCapture(const PendingCapturePtr &pending)
{
	SurfaceCapture result;
	if (!pending || !pending->ready())
		return result;

	ImagePtr albedo = pending->albedo;
	ImagePtr shading = pending->shading;
	ImagePtr normal = pending->normal;
	ImagePtr emission = pending->emission; // may be null

	// unify orientation with Image::get2D (row 0 = v 0)
	if (!Render::isFlipped())
	{
		albedo->flipY();
		shading->flipY();
		normal->flipY();
		if (emission)
			emission->flipY();
	}

	for (ImagePtr img : {albedo, shading, normal, emission})
		if (img && img->isCompressedFormat())
			img->decompress();

	// fill the gutters between the packed charts so bilinear sampling at chart
	// edges does not mix in the black background (dark seams otherwise)
	if (!pending->atlasUV.empty())
	{
		const int w = albedo->getWidth(), h = albedo->getHeight();
		std::vector<ImagePtr> imgs;
		for (const ImagePtr &img : {albedo, shading, normal, emission})
			if (img && img->getWidth() == w && img->getHeight() == h)
				imgs.push_back(img);
		dilateCaptures(imgs, pending->atlasUV, w, h, &result.coverage);
		result.coverageWidth = w;
		result.coverageHeight = h;
	}

	Log::message("Baker GPU: surface %d captured %dx%d (atlas points %d)\n",
		pending->surface, albedo->getWidth(), albedo->getHeight(), int(pending->atlasUV.size()));

	result.atlasUV = std::move(pending->atlasUV);
	result.uvMin = pending->uvMin;
	result.uvScale = pending->uvScale;
	result.uvChannel = pending->uvChannel;
	result.albedo = albedo;
	result.shading = shading;
	result.normal = normal;
	result.emission = emission;
	return result;
}

} // namespace BakeGpu
