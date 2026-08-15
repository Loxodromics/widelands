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

#ifndef WL_GRAPHIC_RHI_PIPELINE_CATALOG_H
#define WL_GRAPHIC_RHI_PIPELINE_CATALOG_H

#include <initializer_list>

#include "graphic/rhi/rhi.h"

// The pipeline catalog (renderer modernization plan, WP-14): the single source
// of truth for the twelve pipelines the renderer needs - the eight shader
// programs times the blend states each program actually draws with. Both the
// eight program constructors (which attach their GL-specific sampler and
// uniform-block knowledge on top) and the Vulkan backend (which pre-builds its
// pipeline cache from it) consume this table, so a vertex layout cannot drift
// between the two - which is exactly what WP-14's acceptance criterion
// "pipeline layouts match the RHI's vertex layouts" is about.
//
// The catalog deliberately carries no samplers or uniform blocks: those are
// GL-330-specific (the authored shaders have no layout(binding=N)), and the
// Vulkan backend derives its descriptor layouts from the bindings.json
// manifest instead (WP-13). Strides and offsets are explicit numbers here;
// verify_vertex_layout pins them against the PerVertexData structs the
// programs actually fill.
namespace Rhi {

// All twelve entries (program name x blend state), in a stable order.
const std::vector<PipelineDescriptor>& pipeline_catalog();

// The catalog entry for 'program_name' drawn with 'blend'. Throws wexception
// on an unknown combination.
const PipelineDescriptor&
pipeline_catalog_entry(const std::string& program_name, const BlendState& blend);

// Loud drift check: the catalog layout must match the vertex struct the
// program really fills ('actual_stride' / 'actual_attributes' are
// sizeof/offsetof of that program's PerVertexData). A struct edit that drifts
// from the catalog throws here at startup instead of corrupting the rendered
// geometry silently - under Vulkan such a drift produces no validation error
// (the offsets are pure CPU-side knowledge).
void verify_vertex_layout(const std::string& program_name,
                          const VertexLayout& catalog_layout,
                          uint32_t actual_stride,
                          std::initializer_list<VertexAttribute> actual_attributes);

}  // namespace Rhi

#endif  // end of include guard: WL_GRAPHIC_RHI_PIPELINE_CATALOG_H
