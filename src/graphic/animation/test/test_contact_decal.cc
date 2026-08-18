/*
 * Copyright (C) 2026 by the Widelands Development Team
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * as published by the Free Software Foundation; either version 2
 * of the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <https://www.gnu.org/licenses/>.
 */

#include "graphic/animation/contact_decal.h"

#include <vector>

#include "base/test.h"
#include "base/vector.h"

namespace {

struct RgbaBuffer {
	int w;
	int h;
	std::vector<uint8_t> data;

	explicit RgbaBuffer(const int width, const int height)
	   : w(width), h(height), data(static_cast<size_t>(width) * height * 4, 0) {
	}

	void fill(int x0, int y0, int x1, int y1, const uint8_t r, const uint8_t g, const uint8_t b,
	          const uint8_t a) {
		for (int y = y0; y < y1; ++y) {
			for (int x = x0; x < x1; ++x) {
				uint8_t* const p = data.data() + (y * w + x) * 4;
				p[0] = r;
				p[1] = g;
				p[2] = b;
				p[3] = a;
			}
		}
	}
};

// The decal's alpha at the given sprite coordinates (0 outside the decal).
uint8_t decal_alpha_at(const ContactDecal& decal, const Vector2i& hotspot, const int sprite_x,
                       const int sprite_y) {
	const int pad = (kFalloff + 1) / 2;
	const int row = sprite_y - (hotspot.y - kBandUp - pad);
	const int col = sprite_x + kFalloff;
	if (row < 0 || row >= decal.height || col < 0 || col >= decal.width) {
		return 0;
	}
	return decal.alpha[row * decal.width + col];
}

// The bottom contour an implementation that ignores the baked-shadow
// exclusion would compute: the gate's naive silhouette threshold of 10,
// applied to the raw alpha. Used to prove the exclusion changes the result.
std::vector<int> naive_contour(const RgbaBuffer& buffer) {
	std::vector<int> contour(buffer.w, -1);
	for (int x = 0; x < buffer.w; ++x) {
		for (int y = buffer.h - 1; y >= 0; --y) {
			if (buffer.data[(y * buffer.w + x) * 4 + 3] >= 10) {
				contour[x] = y;
				break;
			}
		}
	}
	return contour;
}

bool naive_has_contact(const std::vector<int>& contour, const Vector2i& hotspot, const int x) {
	return contour[x] >= 0 && contour[x] >= hotspot.y - kBandUp &&
	       contour[x] <= hotspot.y + kBandDown;
}

}  // namespace

TESTSUITE_START(contact_decal)

// Narrow opaque trunk at the hotspot line, wide opaque canopy floating above:
// the decal must come from the trunk columns, never the canopy width. This is
// the regression the whole design exists for -- the first gate rule derived
// the band from the sprite width and swallowed the canopy skirt.
TESTCASE(tree_like) {
	RgbaBuffer buffer(40, 24);
	buffer.fill(0, 0, 40, 8, 255, 255, 255, 255);     // canopy
	buffer.fill(18, 8, 22, 21, 255, 255, 255, 255);   // trunk, base on the ground line
	const Vector2i hotspot(20, 20);
	const std::optional<ContactDecal> decal =
	   derive_contact_decal(buffer.data.data(), buffer.w, buffer.h, hotspot);
	check_equal(decal.has_value(), true);

	check_equal(decal->width, buffer.w + 2 * kFalloff);
	check_equal(decal->height, kBandUp + kBandDown + 2 * ((kFalloff + 1) / 2));
	check_equal(decal->hotspot.x, hotspot.x + kFalloff);
	check_equal(decal->hotspot.y, kBandUp + (kFalloff + 1) / 2);

	// At the ground line the alpha is anchored on the trunk and nowhere else:
	// the canopy column is at least 13 px away from the trunk contact, so its
	// decal alpha stays negligible for any sane falloff.
	check_equal(decal_alpha_at(*decal, hotspot, 20, 20), 255);
	check_equal(decal_alpha_at(*decal, hotspot, 5, 20) < 10, true);

	// Every row above the band is empty: had the canopy counted as a contact,
	// its bottom row (y 8) would have left a falloff trail up here.
	for (int row = 0; row < kBandUp; ++row) {
		for (int col = 0; col < decal->width; ++col) {
			check_equal(decal->alpha[row * decal->width + col], 0);
		}
	}
}

// A silhouette hanging far below the hotspot line is not standing on it
// (ships float; the hotspot is mid-hull).
TESTCASE(ship_like) {
	RgbaBuffer buffer(40, 80);
	buffer.fill(10, 30, 30, 70, 255, 255, 255, 255);
	const std::optional<ContactDecal> decal =
	   derive_contact_decal(buffer.data.data(), buffer.w, buffer.h, Vector2i(20, 20));
	check_equal(decal.has_value(), false);
}

// A base straddling the ground line is accepted, and the contact spans the
// whole base, not just the columns nearest the hotspot.
TESTCASE(building_like) {
	RgbaBuffer buffer(40, 48);
	buffer.fill(8, 16, 33, 33, 255, 255, 255, 255);  // base, bottom row on the ground line
	const Vector2i hotspot(20, 20);
	const std::optional<ContactDecal> decal =
	   derive_contact_decal(buffer.data.data(), buffer.w, buffer.h, hotspot);
	check_equal(decal.has_value(), true);

	// The base's bottom row is a contact point everywhere along its width.
	for (int x : {8, 14, 20, 26, 32}) {
		check_equal(decal_alpha_at(*decal, hotspot, x, 32), 255);
	}
}

// A baked shadow offset down-right must not drag the contour. The gate found
// this shadow alpha range (10..250, RGB below 60) in a part of the asset set;
// its typical alpha is below the silhouette threshold, so it is excluded from
// the body entirely. An implementation without the exclusion (the gate's
// threshold of 10) would anchor a contact on the shadow's bottom.
TESTCASE(baked_shadow) {
	RgbaBuffer buffer(40, 48);
	buffer.fill(14, 16, 26, 24, 255, 255, 255, 255);  // body
	buffer.fill(28, 24, 36, 32, 0, 0, 0, 127);        // baked shadow
	const Vector2i hotspot(20, 20);
	const std::optional<ContactDecal> decal =
	   derive_contact_decal(buffer.data.data(), buffer.w, buffer.h, hotspot);
	check_equal(decal.has_value(), true);

	// The contour follows the body: the shadow's columns contribute no contact.
	check_equal(decal_alpha_at(*decal, hotspot, 30, 32), 0);

	// ...whereas the naive reference would put one exactly there.
	const std::vector<int> naive = naive_contour(buffer);
	check_equal(naive_has_contact(naive, hotspot, 30), true);
	check_equal(naive_has_contact(naive, hotspot, 20), true);
}

// A shadow opaque enough for the silhouette threshold (alpha >= 128) is only
// thinned, not removed: the erosion strips its interior, but the 1px ring --
// including the bottom row -- survives, and the contour follows the ring.
// This is the gate's algorithm as measured; the exclusion cannot fix a fully
// opaque shadow's ring.
TESTCASE(shadow_ring_survives) {
	RgbaBuffer buffer(40, 48);
	buffer.fill(14, 16, 26, 24, 255, 255, 255, 255);  // body
	buffer.fill(28, 24, 36, 32, 0, 0, 0, 180);        // opaque-enough baked shadow
	const Vector2i hotspot(20, 20);
	const std::optional<ContactDecal> decal =
	   derive_contact_decal(buffer.data.data(), buffer.w, buffer.h, hotspot);
	check_equal(decal.has_value(), true);

	// The ring's bottom row is a contact point; the row below it (the blob's
	// extent in the naive silhouette) is not, whatever the falloff.
	check_equal(decal_alpha_at(*decal, hotspot, 30, 31), 255);
	check_equal(decal_alpha_at(*decal, hotspot, 30, 32) < 255, true);
}

// Empty and fully transparent frames are rejected without crashing, and so is
// an object with no pixel inside the ground band at all.
TESTCASE(no_contact) {
	const RgbaBuffer empty(40, 48);
	check_equal(derive_contact_decal(empty.data.data(), empty.w, empty.h, Vector2i(20, 20)).has_value(),
	            false);

	RgbaBuffer floating(40, 24);
	floating.fill(0, 0, 40, 8, 255, 255, 255, 255);  // high above the ground line
	check_equal(derive_contact_decal(floating.data.data(), floating.w, floating.h, Vector2i(20, 20))
	               .has_value(),
	            false);
}

TESTSUITE_END()
