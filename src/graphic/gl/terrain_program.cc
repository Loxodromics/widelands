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

#include "graphic/gl/terrain_program.h"

#include <atomic>

#include "graphic/gl/coordinate_conversion.h"
#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/initialize.h"
#include "graphic/gl/terrain_noise.h"
#include "graphic/gl/utils.h"
#include "graphic/texture.h"
#include "logic/player.h"

// The shader is authored in GLSL 330 and emitted to the 120/330/300 es
// dialects by Gl::emit_dialect (see data/shaders/terrain.vp and .fp).
TerrainProgram::TerrainProgram() {
	gl_program_.build("terrain");

	u_terrain_texture_ = glGetUniformLocation(gl_program_.object(), "u_terrain_texture");
	if (Gl::backend() == Gl::Backend::kOpenGLCore) {
		gl_program_.bind_uniform_block(
		   "per_program_state", Gl::kPerProgramStateBindingPoint, sizeof(Gl::PerProgramState));
	} else {
		u_texture_dimensions_ = glGetUniformLocation(gl_program_.object(), "u_texture_dimensions");
		u_z_value_ = glGetUniformLocation(gl_program_.object(), "u_z_value");
		u_value_amplitude_ = glGetUniformLocation(gl_program_.object(), "u_value_amplitude");
		u_tint_amplitude_ = glGetUniformLocation(gl_program_.object(), "u_tint_amplitude");
		u_warp_amplitude_ = glGetUniformLocation(gl_program_.object(), "u_warp_amplitude");
	}

	gl_array_buffer_.bind();
	vao_.define_attributes({
	   {gl_program_.attribute_location("attr_brightness"), 1, sizeof(PerVertexData),
	    offsetof(PerVertexData, brightness)},
	   {gl_program_.attribute_location("attr_position"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, gl_x)},
	   {gl_program_.attribute_location("attr_texture_offset"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, texture_offset_x)},
	   {gl_program_.attribute_location("attr_texture_position"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, texture_x)},
	});
}

void TerrainProgram::gl_draw(int gl_texture, float texture_w, float texture_h, float z_value) {
	glUseProgram(gl_program_.object());

	auto& gl_state = Gl::State::instance();

	gl_array_buffer_.bind();
	gl_array_buffer_.update(vertices_);
	vao_.bind();

	gl_state.bind(GL_TEXTURE0, gl_texture);

	glUniform1i(u_terrain_texture_, 0);

	if (Gl::backend() == Gl::Backend::kOpenGLCore) {
		Gl::PerProgramState state{};
		state.z_value = z_value;
		state.value_amplitude = kValueAmplitude * noise_strength_;
		state.tint_amplitude = kTintAmplitude * noise_strength_;
		state.warp_amplitude = kWarpAmplitude * noise_strength_;
		state.texture_w = texture_w;
		state.texture_h = texture_h;
		uniform_buffer_.update(&state, sizeof(state));
		uniform_buffer_.bind_base(Gl::kPerProgramStateBindingPoint);
	} else {
		glUniform1f(u_z_value_, z_value);
		glUniform2f(u_texture_dimensions_, texture_w, texture_h);
		glUniform1f(u_value_amplitude_, kValueAmplitude * noise_strength_);
		glUniform1f(u_tint_amplitude_, kTintAmplitude * noise_strength_);
		glUniform1f(u_warp_amplitude_, kWarpAmplitude * noise_strength_);
	}

	glDrawArrays(GL_TRIANGLES, 0, vertices_.size());
}

void TerrainProgram::add_vertex(const FieldsToDraw::Field& field, const Vector2f& texture_offset) {
	vertices_.emplace_back();
	PerVertexData& back = vertices_.back();

	back.gl_x = field.gl_position.x;
	back.gl_y = field.gl_position.y;
	back.brightness = field.brightness;
	back.texture_x = field.texture_coords.x;
	back.texture_y = field.texture_coords.y;
	back.texture_offset_x = texture_offset.x;
	back.texture_offset_y = texture_offset.y;
}

void TerrainProgram::draw(
   uint32_t gametime,
   const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
   const FieldsToDraw& fields_to_draw,
   float z_value,
   const Widelands::Player* player) {
	// This method expects that all terrains have the same dimensions and that
	// all are packed into the same texture atlas, i.e. all are in the same GL
	// texture. It does not check for this invariance for speeds sake.

	vertices_.clear();
	vertices_.reserve(fields_to_draw.size() * 3);

	for (size_t current_index = 0; current_index < fields_to_draw.size(); ++current_index) {
		const FieldsToDraw::Field& field = fields_to_draw.at(current_index);

		// The bottom right neighbor fields_to_draw is needed for both triangles
		// associated with this field. If it is not in fields_to_draw, there is no need to
		// draw any triangles.
		if (field.brn_index == FieldsToDraw::kInvalidIndex) {
			continue;
		}

		// Right triangle.
		if (field.rn_index != FieldsToDraw::kInvalidIndex) {
			const Widelands::DescriptionIndex terrain =
			   (player != nullptr) && !player->see_all() ?
			      player->fields()[player->egbase().map().get_index(field.fcoords)].terrains.load().r :
			      field.fcoords.field->terrain_r();
			const Vector2f texture_offset =
			   to_gl_texture(terrains.get(terrain).get_texture(gametime).blit_data()).origin();
			add_vertex(fields_to_draw.at(current_index), texture_offset);
			add_vertex(fields_to_draw.at(field.brn_index), texture_offset);
			add_vertex(fields_to_draw.at(field.rn_index), texture_offset);
		}

		// Down triangle.
		if (field.bln_index != FieldsToDraw::kInvalidIndex) {
			const Widelands::DescriptionIndex terrain =
			   (player != nullptr) && !player->see_all() ?
			      player->fields()[player->egbase().map().get_index(field.fcoords)].terrains.load().d :
			      field.fcoords.field->terrain_d();
			const Vector2f texture_offset =
			   to_gl_texture(terrains.get(terrain).get_texture(gametime).blit_data()).origin();
			add_vertex(fields_to_draw.at(current_index), texture_offset);
			add_vertex(fields_to_draw.at(field.bln_index), texture_offset);
			add_vertex(fields_to_draw.at(field.brn_index), texture_offset);
		}
	}

	const BlitData& blit_data = terrains.get(0).get_texture(0).blit_data();
	const Rectf texture_coordinates = to_gl_texture(blit_data);
	gl_draw(blit_data.texture_id, texture_coordinates.w, texture_coordinates.h, z_value);
}
