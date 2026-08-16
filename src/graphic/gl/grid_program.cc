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

#include "graphic/gl/grid_program.h"

#include <array>
#include <cstdlib>

#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/utils.h"
#include "graphic/rhi/device.h"
#include "graphic/rhi/pipeline_catalog.h"

GridProgram::GridProgram() {
	// Pin the vertex struct against the pipeline catalog (WP-14); see
	// blit_program.cc for the rationale.
	const Rhi::PipelineDescriptor base =
	   Rhi::pipeline_catalog_entry("grid", Rhi::kBlendAlpha);
	Rhi::verify_vertex_layout(
	   "grid", base.vertex_layout, sizeof(PerVertexData),
	   {{"attr_position", Rhi::VertexFormat::kVec2, offsetof(PerVertexData, gl_x)},
	    {"attr_color", Rhi::VertexFormat::kVec3, offsetof(PerVertexData, col_r)}});

	if (Rhi::has_device()) {
		Rhi::PipelineDescriptor desc = base;
		desc.uniform_block =
		   Rhi::UniformBlockBinding{0, "per_program_state", Gl::kZValueOnlyBlockSize};
		pipeline_ = Rhi::device().create_pipeline(desc);
		descriptor_set_ = Rhi::device().create_descriptor_set(*pipeline_);
		vertex_buffer_ = Rhi::device().create_buffer(0, Rhi::BufferUsage::kVertex);
		uniform_rhi_buffer_ =
		   Rhi::device().create_buffer(sizeof(Gl::PerProgramState), Rhi::BufferUsage::kUniform);
		return;
	}

	gl_program_.build("grid");

	u_z_value_ = glGetUniformLocation(gl_program_.object(), "u_z_value");

	gl_array_buffer_.bind();
	vao_.define_attributes({
	   {gl_program_.attribute_location("attr_position"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, gl_x)},
	   {gl_program_.attribute_location("attr_color"), 3, sizeof(PerVertexData),
	    offsetof(PerVertexData, col_r)},
	});
}

void GridProgram::gl_draw(const BlitData& texture, const float z_value) {
	if (Rhi::has_device()) {
		vertex_buffer_->update(vertices_.data(), vertices_.size() * sizeof(PerVertexData));

		Gl::PerProgramState state{};
		state.z_value = z_value;
		uniform_rhi_buffer_->update(&state, sizeof(state));

		// grid.fp never samples a texture; the legacy path's texture binding
		// is inert and deliberately not reproduced here (the Vulkan
		// descriptor layout declares no sampler, so binding 0 is the UBO).
		descriptor_set_->set_uniform_buffer(0, uniform_rhi_buffer_.get(), 0, sizeof(state));

		auto& command_buffer = Rhi::command_buffer();
		command_buffer.bind_pipeline(pipeline_.get());
		command_buffer.bind_descriptor_set(descriptor_set_.get());
		command_buffer.bind_vertex_buffer(vertex_buffer_.get());
		command_buffer.draw(0, vertices_.size());
		return;
	}

	glUseProgram(gl_program_.object());

	auto& gl_state = Gl::State::instance();

	gl_array_buffer_.bind();
	gl_array_buffer_.update(vertices_);
	vao_.bind();

	gl_state.bind(GL_TEXTURE0, texture.texture_id);

	glUniform1f(u_z_value_, z_value);

	glDrawArrays(GL_LINES, 0, vertices_.size());
}

void GridProgram::add_vertex(const FieldsToDraw::Field& field, float r, float g, float b) {
	vertices_.emplace_back();
	PerVertexData& back = vertices_.back();
	back.gl_x = field.gl_position.x;
	back.gl_y = field.gl_position.y;
	back.col_r = r;
	back.col_g = g;
	back.col_b = b;
}

void GridProgram::draw(const BlitData& texture,
                       const FieldsToDraw& fields_to_draw,
                       float z_value,
                       bool height_heat_map) {
	vertices_.clear();
	vertices_.reserve(fields_to_draw.size() * 2);

	// This defines the default RGB grid color
	float r = 0.f;
	float g = 0.f;
	float b = 0.f;
	auto calc_rgb = [&fields_to_draw, &r, &g, &b](const FieldsToDraw::Field& field, int neighbour) {
		constexpr std::array<std::array<float, 3>, Widelands::kDefaultMaxFieldHeightDiff + 1>
		   kColors = {{
		      {0.f, 0.64f, 0.f},
		      {0.48f, 0.68f, 0.f},
		      {0.95f, 0.72f, 0.f},
		      {0.87f, 0.36f, 0.f},
		      {0.79f, 0.f, 0.f},
		      {0.59f, 0.f, 0.f},
		   }};

		const int h1 = field.fcoords.field->get_height();
		const int h2 = fields_to_draw.at(neighbour).fcoords.field->get_height();
		const int index = std::min(abs(h1 - h2), Widelands::kDefaultMaxFieldHeightDiff);
		r = kColors[index][0];
		g = kColors[index][1];
		b = kColors[index][2];
	};

	for (size_t current_index = 0; current_index < fields_to_draw.size(); ++current_index) {
		const FieldsToDraw::Field& field = fields_to_draw.at(current_index);

		// Southwestern edge
		if (field.bln_index != FieldsToDraw::kInvalidIndex &&
		    !(field.obscured_by_slope && fields_to_draw.at(field.bln_index).obscured_by_slope)) {
			if (height_heat_map) {
				calc_rgb(field, field.bln_index);
			}
			add_vertex(fields_to_draw.at(current_index), r, g, b);
			add_vertex(fields_to_draw.at(field.bln_index), r, g, b);
		}

		// Southeastern edge
		if (field.brn_index != FieldsToDraw::kInvalidIndex &&
		    !(field.obscured_by_slope && fields_to_draw.at(field.brn_index).obscured_by_slope)) {
			if (height_heat_map) {
				calc_rgb(field, field.brn_index);
			}
			add_vertex(fields_to_draw.at(current_index), r, g, b);
			add_vertex(fields_to_draw.at(field.brn_index), r, g, b);
		}

		// Eastern edge
		if (field.rn_index != FieldsToDraw::kInvalidIndex &&
		    !(field.obscured_by_slope && fields_to_draw.at(field.rn_index).obscured_by_slope)) {
			if (height_heat_map) {
				calc_rgb(field, field.rn_index);
			}
			add_vertex(fields_to_draw.at(current_index), r, g, b);
			add_vertex(fields_to_draw.at(field.rn_index), r, g, b);
		}
	}

	gl_draw(texture, z_value);
}
