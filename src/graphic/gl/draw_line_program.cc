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

#include "graphic/gl/draw_line_program.h"

#include <cassert>
#include <iterator>

// static
DrawLineProgram& DrawLineProgram::instance() {
	static DrawLineProgram draw_line_program;
	return draw_line_program;
}

DrawLineProgram::DrawLineProgram() {
	gl_program_.build("draw_line");

	gl_array_buffer_.bind();
	vao_.define_attributes({
	   {kAttrPosition, 3, sizeof(PerVertexData), offsetof(PerVertexData, gl_x)},
	   {kAttrColor, 4, sizeof(PerVertexData), offsetof(PerVertexData, color_r)},
	});
}

void DrawLineProgram::draw(std::vector<Arguments> arguments) {
	glUseProgram(gl_program_.object());

	gl_array_buffer_.bind();
	vao_.bind();

	vertices_.clear();

	for (Arguments& current_args : arguments) {
		// We do not support anything else for drawing lines, really.
		assert(current_args.blend_mode == BlendMode::UseAlpha);

		for (auto& vertice : current_args.vertices) {
			vertice.gl_z = current_args.z_value;
		}
		std::move(
		   current_args.vertices.begin(), current_args.vertices.end(), std::back_inserter(vertices_));
	}
	gl_array_buffer_.update(vertices_);
	glDrawArrays(GL_TRIANGLES, 0, vertices_.size());
}
