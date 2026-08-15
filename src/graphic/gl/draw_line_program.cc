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

#include "graphic/gl/initialize.h"
#include "graphic/rhi/gl/gl_device.h"

// static
DrawLineProgram& DrawLineProgram::instance() {
	static DrawLineProgram draw_line_program;
	return draw_line_program;
}

DrawLineProgram::DrawLineProgram() {
	if (Gl::backend() == Gl::Backend::kOpenGLCore) {
		Rhi::PipelineDescriptor desc;
		desc.program_name = "draw_line";
		desc.vertex_layout.stride = sizeof(PerVertexData);
		desc.vertex_layout.attributes = {
		   {"attr_position", Rhi::VertexFormat::kVec3, offsetof(PerVertexData, gl_x)},
		   {"attr_color", Rhi::VertexFormat::kVec4, offsetof(PerVertexData, color_r)},
		};
		desc.topology = Rhi::PrimitiveTopology::kTriangleList;
		desc.blend = Rhi::kBlendAlpha;
		desc.depth = {true, true, Rhi::CompareOp::kLessOrEqual};
		pipeline_ = Rhi::device().create_pipeline(desc);
		vertex_buffer_ = Rhi::device().create_buffer(sizeof(PerVertexData), Rhi::BufferUsage::kVertex);
		return;
	}

	gl_program_.build("draw_line");

	gl_array_buffer_.bind();
	vao_.define_attributes({
	   {gl_program_.attribute_location("attr_position"), 3, sizeof(PerVertexData),
	    offsetof(PerVertexData, gl_x)},
	   {gl_program_.attribute_location("attr_color"), 4, sizeof(PerVertexData),
	    offsetof(PerVertexData, color_r)},
	});
}

void DrawLineProgram::draw(std::vector<Arguments> arguments) {
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

	if (Gl::backend() == Gl::Backend::kOpenGLCore) {
		vertex_buffer_->update(vertices_.data(), vertices_.size() * sizeof(PerVertexData));
		auto& command_buffer = Rhi::command_buffer();
		command_buffer.bind_pipeline(pipeline_.get());
		command_buffer.bind_vertex_buffer(vertex_buffer_.get());
		command_buffer.draw(0, vertices_.size());
		return;
	}

	glUseProgram(gl_program_.object());
	gl_array_buffer_.bind();
	vao_.bind();
	gl_array_buffer_.update(vertices_);
	glDrawArrays(GL_TRIANGLES, 0, vertices_.size());
}
