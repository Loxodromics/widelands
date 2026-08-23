/*
 * Copyright (C) 2010-2026 by the Widelands Development Team
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

#ifndef WL_GRAPHIC_GAME_RENDERER_H
#define WL_GRAPHIC_GAME_RENDERER_H

#include "graphic/gl/fields_to_draw.h"
#include "logic/map_objects/descriptions.h"

// Draw the terrain only. 'map' is needed by the water pass, whose distance
// field is seeded from terrain outside the drawn window (WATER.md WP-3).
void draw_terrain(uint32_t gametime,
                  const Widelands::Descriptions& descriptions,
                  const Widelands::Map& map,
                  const FieldsToDraw& fields_to_draw,
                  float scale,
                  const Workareas& workarea,
                  bool height_heat_map,
                  bool grid,
                  const Widelands::Player*,
                  RenderTarget* dst);

// Draw the border stones for 'field' if it is a border and 'visibility' is
// correct.
void draw_border_markers(const FieldsToDraw::Field& field,
                         float scale,
                         const FieldsToDraw& fields_to_draw,
                         RenderTarget* dst);

// Draw the contact shadow for 'descr' on 'field', before the object's own
// sprite (V6, Claude/VISUAL_FIDELITY_RANKED.md §4.6). The caller must have
// already skipped 'field.obscured_by_slope'.
void draw_contact_shadow(const Widelands::MapObjectDescr& descr,
                         const FieldsToDraw::Field& field,
                         float scale,
                         RenderTarget* dst);

#endif  // end of include guard: WL_GRAPHIC_GAME_RENDERER_H
