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
 *
 */

#ifndef WL_GRAPHIC_GL_SHORE_DISTANCE_FIELD_H
#define WL_GRAPHIC_GL_SHORE_DISTANCE_FIELD_H

#include <cstdint>
#include <vector>

#include "graphic/gl/fields_to_draw.h"
#include "logic/map_objects/description_maintainer.h"
#include "logic/map_objects/world/terrain_description.h"
#include "logic/widelands.h"
#include "logic/widelands_geometry.h"

namespace Widelands {
class Map;
class Player;
}  // namespace Widelands

/* The signed distance-to-shore field (Claude/WATER.md D10, §4.2 and WP-3).
 *
 * The field lives on its own map-anchored rectilinear grid of kCellSize map
 * pixels per cell, *not* on the field lattice: the lattice staggers every
 * other row by half a field, so a texture indexed by (fx, fy) would have
 * hardware bilinear interpolate between texels that are not vertically
 * aligned in the map. On this grid the map-pixel-to-cell mapping is plain
 * affine, and each of a field's two terrain triangles gets its own cell, which
 * is the resolution the source data actually has.
 *
 * Cell (cx, cy) is centred on map pixel (kCellSize*cx, kCellSize*cy); node
 * (fx, fy) therefore lands on cell (2*fx + (fy&1), fy), whose right neighbour
 * is the same field's 'r' triangle.
 */
class ShoreDistanceField {
public:
	/// Cell size in map pixels. Square: kTriangleWidth / 2 by kTriangleHeight.
	static constexpr int kCellSize = 32;

	/* The clamp on |distance|, in field widths (64 map pixels). §4.8 wants a
	 * depth ramp roughly 10 fields wide, so 12 leaves headroom for WP-7 to tune
	 * without re-deriving the margin below.
	 *
	 * The clamp is what makes the field translation-invariant, and therefore
	 * what makes panning an index shift rather than a resample. A chamfer value
	 * is only correct if the nearest seed lies inside the computed grid; for a
	 * cell mid-ocean it does not, so an *unclamped* value would depend on where
	 * the grid boundary happens to fall, i.e. on the view. With the clamp, a
	 * cell either has its true nearest seed inside the grid (and reads its
	 * exact distance) or saturates at the clamp regardless of grid extent.
	 */
	static constexpr float kMaxShoreDistance = 12.0f;

	/* The grid's margin beyond the drawn fields, in cells. One cell is half a
	 * field width, so the clamp above is 24 cells; the margin must exceed that
	 * for the invariance argument to hold, because a seed outside the grid is
	 * then always further away than the clamp and can never have been the
	 * nearest one. Two of the spare cells absorb the chamfer metric's ~8 %
	 * *over*estimate of Euclidean distance in near-diagonal directions -- which
	 * errs on the safe side here, but costs nothing to cover.
	 *
	 * WP-5 (water.fp's water_shore_warp()) spends two more: it moves the
	 * sampled position by up to kWaterWarpAmplitude field widths (2*A cells)
	 * before the lookup, and that displacement needs its own headroom past the
	 * clamp -- it is not covered by the two chamfer-error cells above, which
	 * were already fully committed. This was found empirically: at the
	 * previous margin of 26, panning by one field width produced a handful of
	 * scattered non-identical pixels (not a uniform border band, because only
	 * the rare cell where a near-maximal warp excursion coincides with a
	 * near-maximal chamfer error actually exceeds the old margin) rather than
	 * the exact match the invariance argument promises. 28 covers
	 * kWaterWarpAmplitude = 0.8 (1.6 cells, rounded up to 2) with no headroom
	 * to spare; raise this further before raising that amplitude. See
	 * Claude/WATER.md §4.3.
	 */
	static constexpr int kGridMarginCells = 28;

	/* Recomputes the whole field for the currently drawn fields. Unconditional:
	 * at §4.2's cell counts this is a fraction of a millisecond, and recomputing
	 * removes a whole class of staleness bugs (editor terrain edits, fog reveal,
	 * ownership changes) before they can exist. Add caching only when a
	 * measurement asks for it.
	 */
	void rebuild(const FieldsToDraw& fields_to_draw,
	             const Widelands::Map& map,
	             const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
	             const Widelands::Player* player);

	/// Global cell index of the grid's first column/row.
	[[nodiscard]] int cx0() const {
		return cx0_;
	}
	[[nodiscard]] int cy0() const {
		return cy0_;
	}

	[[nodiscard]] int width() const {
		return width_;
	}
	[[nodiscard]] int height() const {
		return height_;
	}

	/* Row-major, row 0 at cy0 (increasing map y). Positive is water and carries
	 * the distance to the nearest land, negative is land and carries the
	 * distance to the nearest water, both in field widths and both clamped to
	 * kMaxShoreDistance. Which sign an unexplored cell takes is decided in
	 * rebuild(); see the comment there.
	 */
	[[nodiscard]] const std::vector<float>& values() const {
		return values_;
	}

	/// Microseconds the last rebuild() took, for the WP-3 cost measurement.
	[[nodiscard]] int64_t last_rebuild_us() const {
		return last_rebuild_us_;
	}

	/* The nearest-land payload (WATER.md WP-11 / §4.4): the terrain and true map height of the
	 * land seed the chamfer says is closest to a cell -- a cheap approximate Voronoi of "nearest
	 * land". The terrain selects the seabed drawn under the wash; the height is for WP-13's cliff
	 * shadows. Height is the real Field height even under fog: seed() plants nothing on an
	 * unexplored field, so a payload only ever travels with land the player remembers.
	 */
	struct LandSeed {
		Widelands::DescriptionIndex terrain;
		uint8_t height;
	};

	/* Cell of a field's 'd' (down_triangle) or 'r' triangle -- the inverse of seed()'s own
	 * node-to-cell mapping, kept here so the two cannot drift. 'geometric' is the field's
	 * geometric (possibly out-of-bounds) coordinate, as FieldsToDraw::Field carries it.
	 */
	static void triangle_cell(const Widelands::Coords& geometric, bool down_triangle, int* cx,
	                          int* cy);

	/* The land seed for cell (cx, cy) in global cell coordinates. Returns false -- and leaves
	 * '*out' untouched -- where no land seed is within kMaxShoreDistance of the cell, so no answer
	 * can be trusted: that is WP-3's clamp/margin argument (shore_distance_field.h above) applied
	 * to the payload. kGridMarginCells exceeds 2 * kMaxShoreDistance, so a cell reading under the
	 * clamp had its whole winning path inside the grid, which makes both the distance and the
	 * identity of the winning seed functions of the in-grid seeds alone -- translation-invariant,
	 * the property the panning gate checks. to_land_ starts pre-assigned to the clamp, so an
	 * unimproved cell fails the gate by construction.
	 */
	[[nodiscard]] bool land_seed_at(int cx, int cy, LandSeed* out) const;

private:
	/* What a cell's triangle is made of. kUnknown is the fog-of-war case and
	 * seeds neither distance buffer, so an unexplored coastline produces no
	 * shore band. Note that Player::Field::terrains is initialised to {0, 0} --
	 * a real terrain index -- so it is the vision state that has to be tested,
	 * not the terrain value.
	 */
	enum class CellKind : uint8_t {
		kUnknown,
		kWater,
		kLand,
	};

	/// Zeroes the matching distance buffer at every cell whose terrain the
	/// player may see, leaving unexplored cells to seed neither.
	void seed(const Widelands::Map& map,
	          const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
	          const Widelands::Player* player);

	/* Sentinel for a cell no land seed has reached. terrain_index tops out at
	 * 16 bits, so a packed word (terrain_index << 8 | height) can never be
	 * 0xFFFFFFFF.
	 */
	static constexpr uint32_t kNoLandSeed = 0xFFFFFFFFu;

	/* Two-pass sequential chamfer with weights (1, sqrt(2)) scaled to field
	 * widths, clamped to kMaxShoreDistance. Clamping cannot corrupt a value
	 * below the clamp: chamfer distances increase monotonically along a
	 * shortest path, so every intermediate value on the path to a cell is at
	 * most that cell's own final value.
	 *
	 * 'payload' is null for the to_water_ pass. When set, each cell copies the
	 * packed word of whichever neighbour won its distance -- an approximate
	 * Voronoi carried along the same sweep. The min-chains are written as
	 * explicit compare-and-assign so the winner is known; ties resolve
	 * first-wins in the fixed neighbour order, which stays deterministic and
	 * translation-invariant.
	 */
	static void chamfer(std::vector<float>* distance, std::vector<uint32_t>* payload, int width,
	                    int height);

	int cx0_ = 0;
	int cy0_ = 0;
	int width_ = 0;
	int height_ = 0;

	int64_t last_rebuild_us_ = 0;

	// Kept between rebuilds to avoid a per-frame allocation.
	std::vector<float> to_land_;
	std::vector<float> to_water_;
	std::vector<float> values_;
	// Parallel to to_land_: (terrain_index << 8) | height of the nearest land
	// seed, kNoLandSeed where none is in range. See land_seed_at().
	std::vector<uint32_t> land_seed_;
};

#endif  // end of include guard: WL_GRAPHIC_GL_SHORE_DISTANCE_FIELD_H
