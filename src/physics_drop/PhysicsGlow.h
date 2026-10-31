#pragma once

#include <UnigineImage.h>
#include <UnigineMathLib.h>
#include <UnigineMesh.h>

namespace ArtistTool
{

// See-through walls that show where something ends: the brightest at the bottom, melting away
// upward, and seen only from the inside, so that only their far side shows. The ground and the
// things in front of them hide them. Drawn for one frame.

// On a sphere, from the height 'base' up to 'fade' above it. 'bands' - the steps of the gradient.
void drawGlowSphere(const Unigine::Math::Vec3 &center, float radius, double base, float fade, const Unigine::Math::vec4 &color, int bands);
// The glow of a portal, per pixel, so without any steps. A level ring of radius 1 with the picture
// wrapped around it: across the image once around, along it from inside out.
Unigine::MeshPtr makePortalMesh();
// The picture: a hot rim, tongues of fire inward that move with 'time' in seconds, a faint halo
// outward, a light veil all over the inside. The color is in RGB, the opacity in A.
void fillPortalImage(const Unigine::ImagePtr &image, const Unigine::Math::vec3 &color, float time);
// An arrow of about the same size on the screen, facing the camera, seen through everything. 'size'
// makes it thicker, 'both_ends' gives it a head at each end.
void drawArrow(const Unigine::Math::Vec3 &from, const Unigine::Math::Vec3 &to, const Unigine::Math::vec4 &color, float size = 1.0f, bool both_ends = false);

} // namespace ArtistTool
