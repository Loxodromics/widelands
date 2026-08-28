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

#include "graphic/gl/seabed.h"

#include "logic/player.h"

Widelands::DescriptionIndex triangle_terrain(const FieldsToDraw& fields_to_draw,
                                             const Widelands::Map* map,
                                             const Widelands::Player* player,
                                             const int index,
                                             const bool down_triangle) {
	const Widelands::FCoords& fcoords = fields_to_draw.at(index).fcoords;
	if (map != nullptr) {
		const auto terrains = player->fields()[map->get_index(fcoords)].terrains.load();
		return down_triangle ? terrains.d : terrains.r;
	}
	return down_triangle ? fcoords.field->terrain_d() : fcoords.field->terrain_r();
}

void resolve_seabed_terrains(
   const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
   SeabedTables* out) {
	const Widelands::DescriptionIndex size = terrains.size();
	out->submerged.resize(size);
	out->is_water.resize(size);
	for (Widelands::DescriptionIndex i = 0; i < size; ++i) {
		const Widelands::TerrainDescription& terrain = terrains.get(i);
		out->is_water[i] =
		   (terrain.get_is() & Widelands::TerrainDescription::Is::kWater) != 0 ? 1 : 0;

		// Resolve a seabed key on any terrain that declares one, not only water terrains (WP-11):
		// a land terrain names what its own submerged texture should be, drawn only when it is
		// the nearest land to a water triangle.
		Widelands::DescriptionIndex resolved = i;
		const std::string& seabed_name = terrain.seabed();
		if (!seabed_name.empty()) {
			const Widelands::DescriptionIndex seabed_index = terrains.get_index(seabed_name);
			if (seabed_index != Widelands::INVALID_INDEX) {
				resolved = seabed_index;
			}
		}
		out->submerged[i] = resolved;
	}
}

Widelands::DescriptionIndex draw_terrain_for_triangle(
   const FieldsToDraw& fields_to_draw,
   const SeabedTables& tables,
   const ShoreDistanceField* shore_distance_field,
   const Widelands::Map* map,
   const Widelands::Player* player,
   const int index,
   const bool down_triangle) {
	const Widelands::DescriptionIndex terrain =
	   triangle_terrain(fields_to_draw, map, player, index, down_triangle);

	// Land triangle: its own terrain, untouched. The substitution is underwater-only.
	if (terrain >= tables.is_water.size() || tables.is_water[terrain] == 0) {
		return terrain;
	}

	// Water triangle: the seabed of the nearest land, from the payload of the cell this triangle
	// seeded (its own 'd' or 'r' cell -- to_water_ there is 0, to_land_ is the offshore distance).
	if (shore_distance_field != nullptr) {
		int cx = 0;
		int cy = 0;
		ShoreDistanceField::triangle_cell(
		   fields_to_draw.at(index).geometric_coords, down_triangle, &cx, &cy);
		ShoreDistanceField::LandSeed seed;
		if (shore_distance_field->land_seed_at(cx, cy, &seed) &&
		    seed.terrain < tables.submerged.size()) {
			return tables.submerged[seed.terrain];
		}
	}

	// No trustworthy land seed: WP-6's per-world beach, the water terrain's own seabed key.
	return tables.submerged[terrain];
}
