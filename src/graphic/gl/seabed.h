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

#include <vector>

#include "graphic/gl/fields_to_draw.h"
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

/* For every terrain index, the index to actually draw in its place: the resolved seabed() for a
 * Is::kWater terrain, the terrain itself otherwise (also the fallback when the name is unset or
 * unresolvable -- an add-on water terrain with no seabed key, say -- so nothing breaks silently
 * in a hard way).
 *
 * Claude/WATER.md WP-6: substituting here, upstream of both the terrain and dither passes, is
 * what keeps a water/land pair out of the dither program without teaching it anything about
 * water -- no water terrain ever reaches collect_vertex_terrains() or add_dithering_triangles(),
 * so the water/land case the dither program would otherwise need special-cased for it simply
 * never arises. Where the seabed equals the neighbouring land, this also leaves that boundary
 * nothing to dither.
 *
 * Rebuilt every frame into '*out': one DescriptionMaintainer::get_index() lookup per *terrain*,
 * not per triangle -- on the order of 100 terrains against ~13k chamfer cells (WATER.md §4.2), so
 * cheap enough that a cache with its own invalidation rules is not worth the bug surface.
 */
void resolve_seabed_terrains(
   const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
   std::vector<Widelands::DescriptionIndex>* out);

#endif  // end of include guard: WL_GRAPHIC_GL_SEABED_H
