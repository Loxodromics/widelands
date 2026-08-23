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
	values_.resize(cells);

	seed(map, terrains, player);
	chamfer(&to_land_, width_, height_);
	chamfer(&to_water_, width_, height_);

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

		const auto seed_cell = [&](const int cx, const CellKind kind) {
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
			if (from_player_view) {
				const Widelands::Player::Field& pf = player->fields()[map.get_index(fcoords)];
				if (pf.vision.state() != Widelands::VisibleState::kUnexplored) {
					const Widelands::Field::Terrains remembered = pf.terrains.load();
					kind_d = is_water(remembered.d) ? CellKind::kWater : CellKind::kLand;
					kind_r = is_water(remembered.r) ? CellKind::kWater : CellKind::kLand;
				}
			} else {
				kind_d = is_water(fcoords.field->terrain_d()) ? CellKind::kWater : CellKind::kLand;
				kind_r = is_water(fcoords.field->terrain_r()) ? CellKind::kWater : CellKind::kLand;
			}

			const int cx_d = 2 * fx + parity;
			seed_cell(cx_d, kind_d);
			seed_cell(cx_d + 1, kind_r);
		}
	}
}

// static
void ShoreDistanceField::chamfer(std::vector<float>* distance, const int width, const int height) {
	float* d = distance->data();

	// Forward pass: row-major, over the four already-visited neighbours.
	for (int y = 0; y < height; ++y) {
		float* row = d + static_cast<size_t>(y) * width;
		const float* above = y > 0 ? row - width : nullptr;
		for (int x = 0; x < width; ++x) {
			float best = row[x];
			if (above != nullptr) {
				if (x > 0) {
					best = std::min(best, above[x - 1] + kDiagonalStep);
				}
				best = std::min(best, above[x] + kOrthoStep);
				if (x + 1 < width) {
					best = std::min(best, above[x + 1] + kDiagonalStep);
				}
			}
			if (x > 0) {
				best = std::min(best, row[x - 1] + kOrthoStep);
			}
			row[x] = std::min(best, kMaxShoreDistance);
		}
	}

	// Backward pass: over the other four.
	for (int y = height - 1; y >= 0; --y) {
		float* row = d + static_cast<size_t>(y) * width;
		const float* below = y + 1 < height ? row + width : nullptr;
		for (int x = width - 1; x >= 0; --x) {
			float best = row[x];
			if (below != nullptr) {
				if (x > 0) {
					best = std::min(best, below[x - 1] + kDiagonalStep);
				}
				best = std::min(best, below[x] + kOrthoStep);
				if (x + 1 < width) {
					best = std::min(best, below[x + 1] + kDiagonalStep);
				}
			}
			if (x + 1 < width) {
				best = std::min(best, row[x + 1] + kOrthoStep);
			}
			row[x] = std::min(best, kMaxShoreDistance);
		}
	}
}
