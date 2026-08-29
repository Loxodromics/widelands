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

#ifndef WL_GRAPHIC_GL_SEABED_H
#define WL_GRAPHIC_GL_SEABED_H

#include <cstdint>
#include <vector>

#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/shore_distance_field.h"
#include "logic/map_objects/description_maintainer.h"
#include "logic/map_objects/world/terrain_description.h"

namespace Widelands {
class Map;
class Player;
}  // namespace Widelands

/* The terrain of a field's 'd' or 'r' triangle, as the player should see it: under fog of war
 * that is what they saw last, not what is there now. 'map' is null exactly when the true terrain
 * applies (no player, or seeing all).
 *
 * Shared by the terrain and dither passes (Claude/WATER.md WP-6) -- previously duplicated inline
 * in terrain_program.cc and as an anonymous-namespace helper in dither_program.cc. Both passes
 * call this to decide what to substitute through resolve_seabed_terrains() below.
 */
Widelands::DescriptionIndex triangle_terrain(const FieldsToDraw& fields_to_draw,
                                             const Widelands::Map* map,
                                             const Widelands::Player* player,
                                             int index,
                                             bool down_triangle);

/* The per-frame seabed lookup tables (Claude/WATER.md WP-6, extended by WP-11).
 *
 * 'submerged[t]' is the terrain index to draw in place of terrain 't' when 't' is underwater: its
 * resolved seabed() if it declares one, 't' itself otherwise. WP-6 filled this only for
 * Is::kWater terrains; WP-11 fills it for any terrain declaring a seabed key, so the nearest-land
 * terrain the SDF payload names can be routed through its own optional seabed before it is drawn.
 * 't' unchanged is also the fallback when the key is unset or unresolvable, so nothing breaks
 * hard.
 *
 * 'is_water[t]' is 1 where terrain 't' is Is::kWater, filled in the same sweep so the per-triangle
 * path (draw_terrain_for_triangle) needs no DescriptionMaintainer::get().
 *
 * One get_index() lookup per *terrain*, not per triangle -- ~100 terrains against ~13k triangles
 * (WATER.md §4.2), so a cache with its own invalidation rules is not worth the bug surface.
 */
struct SeabedTables {
	std::vector<Widelands::DescriptionIndex> submerged;
	std::vector<uint8_t> is_water;
};

void resolve_seabed_terrains(
   const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
   SeabedTables* out);

/* The terrain index to actually draw for one field triangle: fog-aware (through triangle_terrain)
 * and, for water triangles, seabed-aware (Claude/WATER.md WP-11).
 *
 * A land triangle is returned unchanged -- the substitution only ever applies underwater, so a
 * table carrying land-side seabed keys does not corrupt the land pass. A water triangle returns
 * the seabed of the nearest land the shore distance field's payload names for that triangle's own
 * cell; failing that (no land seed within kMaxShoreDistance, or no field at all) it returns the
 * water terrain's own seabed -- WP-6's single per-world beach.
 *
 * 'map' is null exactly when the true terrain applies (no player, or seeing all), same contract
 * as triangle_terrain().
 *
 * Why this sits upstream of both passes rather than inside either (WP-6, still the reason): no
 * water terrain index ever reaches the terrain pass, collect_vertex_terrains() or
 * add_dithering_triangles(), so the water/land case the dither program would otherwise need
 * special-casing for simply never arises, and neither program had to be taught anything about
 * water. WP-11 only changes *which* seabed comes back, not where the substitution happens. Note
 * the consequence it does change: where the seabed now equals the neighbouring land, that
 * boundary has nothing left to dither, so WP-6's uniform sandy fringe survives only on coasts
 * whose land is actually sand.
 */
Widelands::DescriptionIndex draw_terrain_for_triangle(const FieldsToDraw& fields_to_draw,
                                                      const SeabedTables& tables,
                                                      const ShoreDistanceField* shore_distance_field,
                                                      const Widelands::Map* map,
                                                      const Widelands::Player* player,
                                                      int index,
                                                      bool down_triangle);

#endif  // end of include guard: WL_GRAPHIC_GL_SEABED_H
