#pragma once

#include "BakeGpu.h"

#include <UnigineObjects.h>
#include <UnigineString.h>

#include <functional>
#include <map>
#include <vector>

namespace BakeCore
{

struct Settings
{
	int resolution = 2048;        // output textures are resolution x resolution
	float frontalDistance = 0.05f; // meters; how far ABOVE the low-poly surface to search (cage offset)
	float rearDistance = 0.05f;    // meters; how far BELOW the low-poly surface to search
	// Per-part cage overrides, keyed by LOW-POLY node id, {frontal, rear} in
	// meters. The cage offsets the ray ORIGIN from the low-poly surface, so it
	// belongs to the low-poly part, not to the high-poly one. Parts without an
	// entry use frontalDistance/rearDistance above: one part that sits far from
	// its high-poly no longer forces a big cage on the whole model (a global
	// cage large enough for it makes neighbouring parts catch foreign detail).
	std::map<int, Unigine::Math::vec2> partCage;
	int supersamples = 4;          // 1, 4, 16 or 64 rays per texel (grid)
	bool flipNormalY = false;      // extra G-channel flip; engine shaders sample maps as-is,
	                               // so with basis matching normalizationTBN no flip is needed
	bool debugZones = false;       // paint hit zones into albedo: green=front, blue=rear, red=backface
	bool gpuMode = false;          // "as rendered": sample materials from GPU unwrap renders (layers/masks)
	// captures indexed by high-poly surface number (prepared by the UI before bake,
	// because GPU readbacks complete only between editor frames)
	const std::vector<BakeGpu::SurfaceCapture> *gpuCaptures = nullptr;
	bool raysAlongShading = false; // cast rays along shading normals instead of smoothed cage normals
	// skew mask: a texture painted on the low-poly (surface custom texture slot).
	// White = cast the ray along the triangle's geometric normal (no skew, like
	// Marmoset's paint skew), black = smoothed cage normal, gray = blend.
	bool useSkewMask = true;
	// Which maps of the set to write. A map left out is not saved and its path
	// comes back empty, so the material keeps the texture it already has: that
	// is what makes a single-map re-bake possible (emission came out wrong ->
	// bake _e alone, without touching the albedo the user already accepted).
	// Ray tracing still runs in full — the maps share one traversal.
	bool bakeAlbedo = true;    // _alb
	bool bakeShading = true;   // _sh
	bool bakeNormal = true;    // _n
	bool bakeEmission = false; // _e; also enables the Emission state on the material
	// project the scene's world decals (DecalOrtho/Proj/Mesh) onto the bake, so
	// stickers/labels/dirt that live as separate decal nodes end up in the
	// low-poly texture set.
	bool bakeDecals = false;
	// explicit decal node IDs to project (user-picked in the editor). Empty =
	// auto: walk the scene hierarchy and take every decal overlapping the
	// high-poly. Explicit picking is more reliable for decals nested inside
	// node references (correct instance world transforms).
	std::vector<int> decalNodeIds;
	// max distance (meters) a mesh-decal may project onto a target surface: bind
	// only to geometry within this band of the decal mesh (kills ghost
	// projections onto foreign parts sharing the same projection column).
	float decalDistance = 0.01f;
	int dilationPixels = 16;    // edge padding size
	// base name for the output textures/material; empty = derived from the
	// first high-poly object (see suggestBaseName)
	Unigine::String outputName;
};

struct Result
{
	bool success = false;
	bool cancelled = false;
	Unigine::String error;

	// virtual (data-relative) paths of the saved textures; empty if not baked
	Unigine::String albedoPath;
	Unigine::String shadingPath;
	Unigine::String normalPath;
	Unigine::String emissionPath;

	// material that received the baked textures (created or reused)
	Unigine::String materialPath;
};

// Called from the baking loop with progress 0..100 and a status text.
// Return false to cancel baking.
using ProgressFn = std::function<bool(int, const char *)>;

// A bake group: its low-poly parts trace rays only into its own high-poly
// parts, so neighboring pieces do not imprint on each other. All groups write
// into the same texture set (the low-poly parts share one UV layout).
struct BakeGroup
{
	std::vector<Unigine::Ptr<Unigine::ObjectMeshStatic>> highs;
	std::vector<Unigine::Ptr<Unigine::ObjectMeshStatic>> lows;
};

// Bakes all groups into one texture set. High-poly surfaces of all groups are
// numbered consecutively (group order, object order, surface order) —
// gpuCaptures must be indexed the same way. Output names come from the first
// high-poly object; the material is assigned to every low-poly part.
// Note: bake() only writes the texture files. Import them as assets, then call
// assignMaterial() — a material referencing a not-yet-imported texture binds
// the raw file, bypassing the import pipeline (normal maps render broken).
Result bake(const std::vector<BakeGroup> &groups,
	const Settings &settings, const ProgressFn &progress);

// Creates (or reuses) the low-poly material, assigns the baked textures and
// neutralizes the multipliers. Fills result.materialPath.
void assignMaterial(const std::vector<BakeGroup> &groups, Result &result);

// Suggests a cage per low-poly part: probes each part with sparse rays along
// its shading normals and returns the smallest {frontal, rear} (meters) that
// still catches almost all of the high-poly detail in front of / behind it,
// plus a safety margin. Keyed by low-poly node id, ready for Settings::partCage.
// maxProbe caps how far to look (meters). Parts whose probes never hit are left
// out of the result, so the caller keeps its current value for them.
std::map<int, Unigine::Math::vec2> suggestPartCage(const std::vector<BakeGroup> &groups,
	float maxProbe, const ProgressFn &progress);

// Collects the node itself (if it is a Static Mesh) and all Static Mesh
// descendants. Hidden (disabled) nodes are included: hiding is a viewport
// convenience and must not affect baking.
std::vector<Unigine::Ptr<Unigine::ObjectMeshStatic>> collectMeshes(const Unigine::NodePtr &root);

// True if the surface takes part in baking: enabled, not hidden by viewport
// mask, and visible at zero distance (i.e. not a distant LOD shell).
bool isSurfaceBakeable(const Unigine::Ptr<Unigine::ObjectMeshStatic> &obj, int surface);

// Default base name for the output textures/material: the high-poly mesh
// asset name, else the node name, else "baked".
Unigine::String suggestBaseName(const Unigine::Ptr<Unigine::ObjectMeshStatic> &high);

// Creates (or reuses) a black paintable skew-mask texture "<lowmesh>_skew.png"
// next to the low-poly mesh asset. Returns the virtual asset path, or an empty
// string with 'error' filled. Import the asset, then call assignSkewMask().
Unigine::String createSkewMask(const Unigine::Ptr<Unigine::ObjectMeshStatic> &low,
	int size, Unigine::String &error);

// Assigns the mask to the surface custom texture slot of every low-poly
// surface (the slot the editor's Texture Editor paints). The asset must
// already be imported.
void assignSkewMask(const Unigine::Ptr<Unigine::ObjectMeshStatic> &low, const char *virtualPath);

} // namespace BakeCore
