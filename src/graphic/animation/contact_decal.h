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

#ifndef WL_GRAPHIC_ANIMATION_CONTACT_DECAL_H
#define WL_GRAPHIC_ANIMATION_CONTACT_DECAL_H

#include <cstdint>
#include <optional>
#include <vector>

#include "base/vector.h"

// The contact shadow under map-object sprites (V6, Claude/VISUAL_FIDELITY_RANKED.md
// §4.6). All constants below are mirrored in Claude/footprint_gate.py, which
// measured them against the real sprite set; the two implementations are
// independent, so a divergence between them means one drifted.
//
// A decal is derived from the animation's own alpha channel rather than drawn
// as a generic ellipse: blits are axis-aligned only, and sizing from the
// animation width would measure a tree's canopy where the contact is at its
// trunk. The derivation anchors on the animation hotspot, which already marks
// the node's ground point.
//
// The load-bearing step is masking out the baked shadow first (the §2.1
// discriminator below): without it, the bottom contour of a part of the asset
// set follows the shadow instead of the object (6.13 px mean / 31 px max on
// the oak, up to 40 px on the tower -- footprint_gate.py, run 2026-08-18).

/// The decal's alpha channel, laid out as width * height bytes.
struct ContactDecal {
	std::vector<uint8_t> alpha;
	int width{0};
	int height{0};
	/// Where the node sits inside the decal; the decal is drawn so that this
	/// point lands on the node's ground point.
	Vector2i hotspot{0, 0};
};

/// Derives the contact-shadow decal for one RGBA8888 frame. Returns nullopt
/// when the object is not standing on this node's ground line (ships float;
/// the hotspot is mid-hull), or when nothing of the frame is opaque.
std::optional<ContactDecal> derive_contact_decal(const uint8_t* rgba, int width, int height,
                                                 const Vector2i& hotspot);

// Opaque enough to count as object body. Deliberately high: soft shadow edges
// and grass fringes read as background, not as part of the object.
constexpr uint8_t kAlphaThreshold = 128;

// The baked-shadow discriminator from VISUAL_FIDELITY_RANKED.md §2.1, reused
// verbatim: pixels this dark and semi-transparent are the soft shadow baked
// into the sprite by the Blender rig, not object body.
constexpr uint8_t kShadowRgbMax = 60;
constexpr uint8_t kShadowAlphaLo = 10;
constexpr uint8_t kShadowAlphaHi = 250;

// The ground band around the hotspot line, in sprite pixels at scale 1.0.
// Down is half of kTriangleHeight (64/32, mapviewpixelconstants.h): a
// one-field footprint reaches half a field in front of its node, and that
// front edge is what the silhouette shows -- the lumberjack's hut needs it
// (its base runs to +24 px below a hotspot at y 45). Up is deliberately much
// tighter: the back of the footprint is occluded by the object anyway, so the
// only thing a generous upward band buys is floating canopy. The anisotropic
// falloff spreads the decal backwards instead.
constexpr int kBandUp = 6;
constexpr int kBandDown = 16;

// An object most of whose silhouette hangs below the ground band is not
// standing on this node's ground line (ship 0.72 against 0.00-0.09 for
// everything grounded, per footprint_gate.py).
constexpr float kRejectBelowFraction = 0.60f;

// The falloff radius in screen-x pixels at scale 1.0. The one free aesthetic
// knob; the vertical padding is halved because the distance metric is
// anisotropic (see derive_contact_decal).
constexpr int kFalloff = 10;

// Global alpha multiplier for the decal, 0.0 switches the feature off
// entirely. Tuned on the strength ladder in VISUAL_FIDELITY_RANKED.md §4.6.
constexpr float kContactShadowStrength = 0.25f;

#endif  // end of include guard: WL_GRAPHIC_ANIMATION_CONTACT_DECAL_H
