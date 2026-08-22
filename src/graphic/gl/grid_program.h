/*
 * Copyright (C) 2006-2026 by the Widelands Development Team
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

#ifndef WL_GRAPHIC_GL_GRID_PROGRAM_H
#define WL_GRAPHIC_GL_GRID_PROGRAM_H

#include <memory>

#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/utils.h"
#include "graphic/rhi/rhi.h"

class GridProgram {
public:
	// Compiles the program. Throws on errors.
	GridProgram();

	// Draws the grid layer
	void draw(const BlitData& texture,
	          const FieldsToDraw& fields_to_draw,
	          float z_value,
	          bool height_heat_map);

private:
	struct PerVertexData {
		float gl_x;
		float gl_y;
		float col_r;
		float col_g;
		float col_b;
	};
	static_assert(sizeof(PerVertexData) == 20, "Wrong padding.");

	void gl_draw(const BlitData& texture, float z_value);

	// Adds a vertex to the end of vertices with data from 'field' and the given RGB color.
	void add_vertex(const FieldsToDraw::Field& field, float r, float g, float b);

	// RHI resources. grid binds a texture its shader never samples (see
	// Claude/RHI_INTERFACE.md §6.1) and the z-only block.
	std::unique_ptr<Rhi::Pipeline> pipeline_;
	std::unique_ptr<Rhi::DescriptorSet> descriptor_set_;
	std::unique_ptr<Rhi::Buffer> vertex_buffer_;
	std::unique_ptr<Rhi::Buffer> uniform_rhi_buffer_;

	// Objects below are kept around to avoid memory allocations on each frame.
	// They could theoretically also be recreated.
	std::vector<PerVertexData> vertices_;

	DISALLOW_COPY_AND_ASSIGN(GridProgram);
};

#endif  // end of include guard: WL_GRAPHIC_GL_GRID_PROGRAM_H
