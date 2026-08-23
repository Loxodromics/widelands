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
   std::vector<Widelands::DescriptionIndex>* out) {
	const Widelands::DescriptionIndex size = terrains.size();
	out->resize(size);
	for (Widelands::DescriptionIndex i = 0; i < size; ++i) {
		const Widelands::TerrainDescription& terrain = terrains.get(i);
		Widelands::DescriptionIndex resolved = i;
		if ((terrain.get_is() & Widelands::TerrainDescription::Is::kWater) != 0) {
			const std::string& seabed_name = terrain.seabed();
			if (!seabed_name.empty()) {
				const Widelands::DescriptionIndex seabed_index = terrains.get_index(seabed_name);
				if (seabed_index != Widelands::INVALID_INDEX) {
					resolved = seabed_index;
				}
			}
		}
		(*out)[i] = resolved;
	}
}
