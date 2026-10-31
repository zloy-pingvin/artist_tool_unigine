#pragma once

#include <UnigineMathLib.h>

#include <vector>

namespace ArtistTool
{

// What of a pile can be seen from outside of it. The objects are drawn into a picture of their numbers
// from many directions of the upper half of a sphere, each far away and looking at the pile, and every one
// gets the share of itself that was seen. A loose pile is full of gaps: nearly everything shows by a few
// pixels from somewhere, so what counts is how much. Only the pile itself hides anything here, so what is
// behind a wall of the world counts as seen.
struct VisibilitySolid
{
	// Its convex hull, in a space common to all of them, not far from the pile: floats are enough.
	std::vector<Unigine::Math::vec3> points;
	// The points of its triangles, three numbers for each. May be shared by many solids.
	const std::vector<int> *triangles{nullptr};
};

struct VisibilitySettings
{
	int directions{128};
	// How many pixels the smallest object is across.
	float pixels_across{8.0f};
	// The objects are this much smaller when they hide each other, so the gaps between them are wider
	// than they are.
	float shrink{1.0f};
};

// For every solid, the share of its pixels over all the directions that were not behind something else:
// from 0, nowhere seen, to 1, nothing is ever in its way.
std::vector<float> measureSeenShares(const std::vector<VisibilitySolid> &solids, const VisibilitySettings &settings);

// The picture itself, from one direction (it goes from the pile to who looks): 'size' times 'size'
// numbers of solids, -1 where there is none. Of use for checking the result.
void drawSolidNumbers(const std::vector<VisibilitySolid> &solids, const std::vector<char> *only, float shrink,
	const Unigine::Math::vec3 &direction, float pixel, std::vector<int> &numbers, int &size);

// The middle and the radius of what is to be drawn, and the size of a pixel for the smallest of them.
void measureSolids(const std::vector<VisibilitySolid> &solids, Unigine::Math::vec3 &middle, float &radius, float &smallest);

} // namespace ArtistTool
