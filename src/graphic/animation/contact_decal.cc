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

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace {

// 3x3 binary erosion, mirroring footprint_gate.py:erode: a pixel survives
// only with all eight neighbours set. The false border keeps the eroded
// shadow strictly inside its original extent.
std::vector<uint8_t> erode3x3(const std::vector<uint8_t>& mask, const int width, const int height) {
	std::vector<uint8_t> out(mask.size(), 0);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			bool all_set = true;
			for (int dy = -1; dy <= 1 && all_set; ++dy) {
				for (int dx = -1; dx <= 1; ++dx) {
					const int nx = x + dx;
					const int ny = y + dy;
					if (nx < 0 || ny < 0 || nx >= width || ny >= height ||
					    !mask[ny * width + nx]) {
						all_set = false;
						break;
					}
				}
			}
			out[y * width + x] = all_set;
		}
	}
	return out;
}

}  // namespace

std::optional<ContactDecal> derive_contact_decal(const uint8_t* rgba, const int width,
                                                 const int height, const Vector2i& hotspot) {
	// 1. Body mask: opaque pixels minus the baked shadow, whose interior is
	// eroded away so a single stray dark pixel cannot knock out a column.
	std::vector<uint8_t> silhouette(static_cast<size_t>(width) * height);
	std::vector<uint8_t> shadow_raw(static_cast<size_t>(width) * height);
	for (int y = 0; y < height; ++y) {
		for (int x = 0; x < width; ++x) {
			const uint8_t* const pixel = rgba + (y * width + x) * 4;
			silhouette[y * width + x] = pixel[3] >= kAlphaThreshold;
			shadow_raw[y * width + x] = pixel[0] < kShadowRgbMax && pixel[1] < kShadowRgbMax &&
			                            pixel[2] < kShadowRgbMax && pixel[3] > kShadowAlphaLo &&
			                            pixel[3] < kShadowAlphaHi;
		}
	}
	const std::vector<uint8_t> shadow = erode3x3(shadow_raw, width, height);
	std::vector<uint8_t> body(silhouette.size());
	for (size_t i = 0; i < body.size(); ++i) {
		body[i] = silhouette[i] && !shadow[i];
	}

	// 2-3. Bottom contour per column and the contact band around the hotspot
	// line. The band is fixed, never derived from the sprite width: the first
	// gate rule measured the canopy and produced exactly the wide smear this
	// design exists to avoid.
	std::vector<int> contour(width, -1);
	std::vector<std::pair<int, int>> contact_points;
	int non_empty = 0;
	int below_band = 0;
	for (int x = 0; x < width; ++x) {
		for (int y = height - 1; y >= 0; --y) {
			if (!body[y * width + x]) {
				continue;
			}
			contour[x] = y;
			++non_empty;
			if (y > hotspot.y + kBandDown) {
				++below_band;
			}
			if (y >= hotspot.y - kBandUp && y <= hotspot.y + kBandDown) {
				contact_points.emplace_back(x, y);
			}
			break;
		}
	}

	// 4. Rejection: most of the silhouette hanging below the band means the
	// object is floating (ships), and without a single contact column there is
	// nothing to anchor a decal on.
	if (static_cast<float>(below_band) / std::max(non_empty, 1) > kRejectBelowFraction ||
	    contact_points.empty()) {
		return std::nullopt;
	}

	// 5-6. Distance field and alpha. The metric is anisotropic because the
	// ground plane is squashed 2:1 on screen (kTriangleWidth 64 / kTriangleHeight
	// 32, mapviewpixelconstants.h): one screen-x pixel is one ground-x pixel,
	// but one screen-y pixel is half a ground-y pixel. Brute force is fine at
	// these sizes (frames are at most ~160 px wide); a chamfer pass would be
	// an optimisation, not a requirement.
	//
	// The decal is cropped to the band plus the falloff padding: height is
	// independent of the sprite height, which keeps tall sprites (trees) cheap.
	const int pad = (kFalloff + 1) / 2;  // ceil(kFalloff / 2), half because of the 2x y metric
	ContactDecal decal;
	decal.width = width + 2 * kFalloff;
	decal.height = kBandUp + kBandDown + 2 * pad;
	decal.hotspot = Vector2i(hotspot.x + kFalloff, kBandUp + pad);
	decal.alpha.assign(static_cast<size_t>(decal.width) * decal.height, 0);
	for (int dy = 0; dy < decal.height; ++dy) {
		const int sy = hotspot.y - kBandUp - pad + dy;
		for (int dx = 0; dx < decal.width; ++dx) {
			const int sx = dx - kFalloff;
			float best = std::numeric_limits<float>::infinity();
			for (const auto& contact : contact_points) {
				const float ddx = static_cast<float>(sx - contact.first);
				const float ddy = 2.f * (sy - contact.second);
				best = std::min(best, std::sqrt(ddx * ddx + ddy * ddy));
			}
			const float t = 1.f - best / kFalloff;
			const float alpha = t > 0.f ? t * t : 0.f;  // clamp(1 - d/kFalloff, 0, 1)^2
			decal.alpha[dy * decal.width + dx] = static_cast<uint8_t>(std::lround(255.f * alpha));
		}
	}
	return decal;
}
