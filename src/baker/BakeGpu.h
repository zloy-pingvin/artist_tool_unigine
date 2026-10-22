#pragma once

#include <UnigineImage.h>
#include <UnigineMesh.h>
#include <UnigineObjects.h>
#include <UnigineTextures.h>

#include <atomic>
#include <memory>
#include <vector>

namespace BakeGpu
{

// GBuffer contents of one surface rendered "unwrapped into its UV space" with
// its real material (all layers/masks/tiling evaluated by the engine shaders).
struct SurfaceCapture
{
	Unigine::ImagePtr albedo;  // rgb = albedo, a = occlusion
	Unigine::ImagePtr shading; // r = metalness, g = f0(specular), b = translucent, a = microfiber
	Unigine::ImagePtr normal;  // rgb = octahedral-packed tangent-space normal, a = roughness
	// final frame of the isolated render (black environment, no lights) =
	// the emission pass output alone; null when emission capture was not requested
	Unigine::ImagePtr emission;

	// capture texture coordinate per triangle CORNER of the surface (three
	// consecutive entries per triangle, same order as the index arrays): the UV
	// charts are REPACKED into a unique atlas (tiled/overlapping unwraps are
	// ambiguous — several mesh locations would share one capture texel), and
	// per-corner storage separates even welded stacked charts. Sample the
	// capture at the barycentric interpolation of entries [3k .. 3k+2] of
	// triangle k; (-1,-1) marks uncapturable charts (CPU fallback).
	std::vector<Unigine::Math::vec2> atlasUV;

	// Post-dilation coverage of the capture atlas: 1 = the texel holds real or
	// gutter-filled chart data, 0 = untouched background.
	// The background quad is BLACK, so pixel brightness cannot tell "this part
	// of the mesh was never rasterized" from "the material is legitimately
	// black" — only this mask can. Checked per hit: an uncovered texel means
	// that piece of the mesh has no unwrap area at all (UVs collapsed to zero
	// area), so it bakes from the material textures instead of reading
	// background. Gutter-filled texels count as covered, so bilinear sampling
	// at chart edges keeps working.
	std::vector<unsigned char> coverage;
	int coverageWidth = 0;
	int coverageHeight = 0;

	// legacy mapping (used only when atlasUV is empty): (uv - uvMin) / uvScale
	Unigine::Math::vec2 uvMin{0.0f, 0.0f};
	Unigine::Math::vec2 uvScale{1.0f, 1.0f};
	// which UV channel the unwrap/repacking was built from
	int uvChannel = 0;

	bool valid() const { return albedo && shading && normal; }
};

// A capture in flight: the render is done, but the GPU->CPU transfer completes
// only at the swap stage of an engine frame, i.e. after control returns to the
// editor loop. Poll ready() from a timer.
struct PendingCapture
{
	int surface = -1;
	Unigine::Math::vec2 uvMin{0.0f, 0.0f};
	Unigine::Math::vec2 uvScale{1.0f, 1.0f};
	int uvChannel = 0;
	std::vector<Unigine::Math::vec2> atlasUV; // packed atlas coordinate per triangle corner

	Unigine::ImagePtr albedo, shading, normal, emission; // filled by engine callbacks (async thread)
	Unigine::TexturePtr keepA, keepS, keepN, keepE;      // keep GPU copies alive
	std::atomic<int> arrived{0};
	int expected = 3; // 4 when the emission readback is requested too

	bool ready() const
	{
		return arrived.load(std::memory_order_acquire) >= expected && albedo && shading && normal
			&& (expected < 4 || emission);
	}
};
using PendingCapturePtr = std::shared_ptr<PendingCapture>;

// Renders the surface unwrapped into its UV space and requests the gbuffer
// readback. Returns nullptr on failure. Result images arrive after the editor
// renders the next frame.
// uvChannelMode: -1 = auto (pick the channel whose charts overlap less),
// 0 / 1 = force UV0 / UV1 (still falls back to UV0 if UV1 is missing/degenerate).
// captureEmission additionally reads back the final render image (emission).
PendingCapturePtr requestSurfaceCapture(const Unigine::Ptr<Unigine::ObjectMeshStatic> &obj,
	const Unigine::Ptr<Unigine::ConstMesh> &mesh, int surface, int size, int uvChannelMode = -1,
	bool captureEmission = false);

// Converts a completed pending capture into a usable SurfaceCapture
// (orientation fix, decompression).
SurfaceCapture finishCapture(const PendingCapturePtr &pending);

// Performs a minimal dummy render whose swap stage delivers the readbacks
// queued during the previous renders. Call after the last requestSurfaceCapture.
void flushTransfers();

// How many readbacks are requested and not delivered yet. Their callbacks are code of
// the plugin: it must not be unloaded while any is left (see BakerWindow::update_busy()).
int transfersInFlight();

// Decodes the octahedral-packed gbuffer normal
// (CPU port of core/materials/shaders/api/pack_utils.h + gbuffer.h).
Unigine::Math::vec3 unpackGBufferNormal(const Unigine::Math::vec4 &rgba);

} // namespace BakeGpu
