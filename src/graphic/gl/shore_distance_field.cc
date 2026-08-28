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

#include "graphic/gl/shore_distance_field.h"

#include <algorithm>
#include <chrono>
#include <cmath>

#include "logic/map.h"
#include "logic/player.h"

namespace {

/* One orthogonal cell step, in field widths. A cell is half a field wide, and
 * the grid is square in map pixels, so the same number holds in both axes --
 * which is the reason the field is stored in field widths at all: the unit is
 * isotropic here and it is what WP-7's ramp and WP-9's foam band are specified
 * in.
 */
constexpr float kOrthoStep = 0.5f;
const float kDiagonalStep = 0.5f * std::sqrt(2.f);

/// Floor division, which the plain / rounds towards zero for. Cell and field
/// coordinates are freely negative (the view may sit left of the map origin).
int floor_div(const int numerator, const int denominator) {
	const int quotient = numerator / denominator;
	return (numerator % denominator != 0 && ((numerator < 0) != (denominator < 0))) ? quotient - 1 :
	                                                                                  quotient;
}

}  // namespace

void ShoreDistanceField::rebuild(
   const FieldsToDraw& fields_to_draw,
   const Widelands::Map& map,
   const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
   const Widelands::Player* player) {
	const auto started = std::chrono::steady_clock::now();

	/* The window, in *global* integer cell indices. Because cx0_/cy0_ are
	 * integers on the global cell lattice by construction, panning by whole
	 * cells is an index shift and never a resample -- §4.2's "the grid origin
	 * snaps to integer cell coordinates" falls out of the geometry rather than
	 * needing code of its own.
	 *
	 * The upper x bound is 2*max_fx + 2 because a field's rightmost cell is
	 * 2*fx + (fy&1) + 1.
	 */
	cx0_ = 2 * fields_to_draw.min_fx() - kGridMarginCells;
	cy0_ = fields_to_draw.min_fy() - kGridMarginCells;
	width_ = (2 * fields_to_draw.max_fx() + 2 + kGridMarginCells) - cx0_ + 1;
	height_ = (fields_to_draw.max_fy() + kGridMarginCells) - cy0_ + 1;

	const size_t cells = static_cast<size_t>(width_) * static_cast<size_t>(height_);
	to_land_.assign(cells, kMaxShoreDistance);
	to_water_.assign(cells, kMaxShoreDistance);
	land_seed_.assign(cells, kNoLandSeed);
	values_.resize(cells);

	seed(map, terrains, player);
	chamfer(&to_land_, &land_seed_, width_, height_);
	chamfer(&to_water_, nullptr, width_, height_);

	for (size_t i = 0; i < cells; ++i) {
		/* Sign by whichever seed is nearer, rather than by the cell's own
		 * classification. For a seeded cell the two agree exactly -- a water
		 * cell has to_water_ == 0, a land cell has to_land_ == 0 -- so this
		 * costs nothing there, and it is the only part that decides what an
		 * *unknown* cell reads as.
		 *
		 * That case is what makes the rule worth stating. An unknown cell has no
		 * side of its own, and any fixed choice invents a shoreline: read as
		 * water it puts a waterline along every fog boundary that touches known
		 * land, read as land it puts one along every boundary that touches known
		 * water. Extending the nearer known side instead means an unknown region
		 * behind land reads as far inland and one behind water reads as deep
		 * water -- no zero crossing either way -- while a fog pocket genuinely
		 * between known land and known water puts the crossing halfway between
		 * them, which is where a coastline plausibly is.
		 */
		values_[i] = to_land_[i] <= to_water_[i] ? -to_water_[i] : to_land_[i];
	}

	last_rebuild_us_ = std::chrono::duration_cast<std::chrono::microseconds>(
	                      std::chrono::steady_clock::now() - started)
	                      .count();
}

void ShoreDistanceField::seed(
   const Widelands::Map& map,
   const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
   const Widelands::Player* player) {
	const bool from_player_view = (player != nullptr) && !player->see_all();

	const auto is_water = [&terrains](const Widelands::DescriptionIndex terrain) {
		return (terrains.get(terrain).get_is() & Widelands::TerrainDescription::Is::kWater) != 0;
	};

	for (int cy = cy0_; cy < cy0_ + height_; ++cy) {
		// Row parity: node (fx, cy) owns the cells 2*fx + parity (its 'd'
		// triangle) and one to the right of that (its 'r' triangle).
		const int parity = cy & 1;
		const int first_fx = floor_div(cx0_ - parity, 2);
		const int last_fx = floor_div(cx0_ + width_ - 1 - parity, 2);
		const size_t row_start = static_cast<size_t>(cy - cy0_) * width_;
		float* row_to_land = &to_land_[row_start];
		float* row_to_water = &to_water_[row_start];
		uint32_t* row_land_seed = &land_seed_[row_start];

		const auto seed_cell = [&](const int cx, const CellKind kind, const uint32_t payload) {
			const int x = cx - cx0_;
			if (x < 0 || x >= width_) {
				return;
			}
			switch (kind) {
			case CellKind::kWater:
				row_to_water[x] = 0.f;
				break;
			case CellKind::kLand:
				row_to_land[x] = 0.f;
				row_land_seed[x] = payload;
				break;
			case CellKind::kUnknown:
				// Seeds neither side; see CellKind.
				break;
			}
		};

		for (int fx = first_fx; fx <= last_fx; ++fx) {
			/* Field coordinates stay geometric (possibly out of bounds) and are
			 * normalised only for the map lookup, exactly as FieldsToDraw::reset
			 * does, so a grid window straddling the map seam wraps correctly.
			 */
			Widelands::Coords normalized(fx, cy);
			map.normalize_coords(normalized);
			const Widelands::FCoords fcoords = map.get_fcoords(normalized);

			CellKind kind_d = CellKind::kUnknown;
			CellKind kind_r = CellKind::kUnknown;
			Widelands::DescriptionIndex terrain_d = 0;
			Widelands::DescriptionIndex terrain_r = 0;
			if (from_player_view) {
				const Widelands::Player::Field& pf = player->fields()[map.get_index(fcoords)];
				if (pf.vision.state() != Widelands::VisibleState::kUnexplored) {
					const Widelands::Field::Terrains remembered = pf.terrains.load();
					terrain_d = remembered.d;
					terrain_r = remembered.r;
					kind_d = is_water(terrain_d) ? CellKind::kWater : CellKind::kLand;
					kind_r = is_water(terrain_r) ? CellKind::kWater : CellKind::kLand;
				}
			} else {
				terrain_d = fcoords.field->terrain_d();
				terrain_r = fcoords.field->terrain_r();
				kind_d = is_water(terrain_d) ? CellKind::kWater : CellKind::kLand;
				kind_r = is_water(terrain_r) ? CellKind::kWater : CellKind::kLand;
			}

			/* The payload carries the seeding triangle's own terrain and the field's true height
			 * (WATER.md WP-11). Height is packed in the low byte, terrain above it -- see
			 * land_seed_at() for the inverse. Only the kLand branch of seed_cell() reads it.
			 */
			const uint32_t height_bits = fcoords.field->get_height();
			const int cx_d = 2 * fx + parity;
			seed_cell(cx_d, kind_d, (static_cast<uint32_t>(terrain_d) << 8) | height_bits);
			seed_cell(cx_d + 1, kind_r, (static_cast<uint32_t>(terrain_r) << 8) | height_bits);
		}
	}
}

// static
void ShoreDistanceField::chamfer(std::vector<float>* distance,
                                 std::vector<uint32_t>* payload,
                                 const int width,
                                 const int height) {
	float* d = distance->data();
	uint32_t* p = payload != nullptr ? payload->data() : nullptr;

	// Forward pass: row-major, over the four already-visited neighbours.
	for (int y = 0; y < height; ++y) {
		const size_t row = static_cast<size_t>(y) * width;
		for (int x = 0; x < width; ++x) {
			const size_t i = row + x;
			float best = d[i];
			size_t best_src = i;
			/* Compare-and-assign rather than a std::min chain so the winning
			 * neighbour is known and its payload can follow. std::min(best, cand)
			 * is exactly (cand < best ? cand : best), so the distances stay
			 * byte-for-byte what WP-3 produced; a tie keeps the earlier
			 * candidate in this fixed neighbour order.
			 */
			const auto consider = [&](const size_t src, const float step) {
				const float cand = d[src] + step;
				if (cand < best) {
					best = cand;
					best_src = src;
				}
			};
			if (y > 0) {
				if (x > 0) {
					consider(i - width - 1, kDiagonalStep);
				}
				consider(i - width, kOrthoStep);
				if (x + 1 < width) {
					consider(i - width + 1, kDiagonalStep);
				}
			}
			if (x > 0) {
				consider(i - 1, kOrthoStep);
			}
			if (best > kMaxShoreDistance) {
				best = kMaxShoreDistance;
			}
			d[i] = best;
			if (p != nullptr && best_src != i) {
				p[i] = p[best_src];
			}
		}
	}

	// Backward pass: over the other four.
	for (int y = height - 1; y >= 0; --y) {
		const size_t row = static_cast<size_t>(y) * width;
		for (int x = width - 1; x >= 0; --x) {
			const size_t i = row + x;
			float best = d[i];
			size_t best_src = i;
			const auto consider = [&](const size_t src, const float step) {
				const float cand = d[src] + step;
				if (cand < best) {
					best = cand;
					best_src = src;
				}
			};
			if (y + 1 < height) {
				if (x > 0) {
					consider(i + width - 1, kDiagonalStep);
				}
				consider(i + width, kOrthoStep);
				if (x + 1 < width) {
					consider(i + width + 1, kDiagonalStep);
				}
			}
			if (x + 1 < width) {
				consider(i + 1, kOrthoStep);
			}
			if (best > kMaxShoreDistance) {
				best = kMaxShoreDistance;
			}
			d[i] = best;
			if (p != nullptr && best_src != i) {
				p[i] = p[best_src];
			}
		}
	}
}

// static
void ShoreDistanceField::triangle_cell(const Widelands::Coords& geometric,
                                       const bool down_triangle,
                                       int* cx,
                                       int* cy) {
	// The inverse of seed()'s node-to-cell mapping: node (fx, fy) owns cell
	// 2*fx + (fy&1) (its 'd' triangle) and the one to the right of it ('r').
	const int parity = geometric.y & 1;
	*cx = 2 * geometric.x + parity + (down_triangle ? 0 : 1);
	*cy = geometric.y;
}

bool ShoreDistanceField::land_seed_at(const int cx, const int cy, LandSeed* out) const {
	const int x = cx - cx0_;
	const int y = cy - cy0_;
	if (x < 0 || x >= width_ || y < 0 || y >= height_) {
		return false;
	}
	const size_t i = static_cast<size_t>(y) * width_ + x;
	// The trust gate: a cell still at (or above) the clamp had no seed inside
	// the grid, so neither its distance nor its winning seed is view-independent.
	if (to_land_[i] >= kMaxShoreDistance) {
		return false;
	}
	const uint32_t word = land_seed_[i];
	if (word == kNoLandSeed) {
		return false;
	}
	out->terrain = static_cast<Widelands::DescriptionIndex>(word >> 8);
	out->height = static_cast<uint8_t>(word & 0xFFu);
	return true;
}
