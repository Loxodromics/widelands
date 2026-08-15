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

#include "graphic/gl/dither_program.h"

#include "base/wexception.h"
#include "graphic/gl/coordinate_conversion.h"
#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/terrain_noise.h"
#include "graphic/gl/utils.h"
#include "graphic/image_io.h"
#include "graphic/rhi/device.h"
#include "graphic/texture.h"
#include "io/filesystem/layered_filesystem.h"
#include "logic/player.h"

DitherProgram::DitherProgram() {
	if (Rhi::has_device()) {
		Rhi::PipelineDescriptor desc;
		desc.program_name = "dither";
		desc.vertex_layout.stride = sizeof(PerVertexData);
		desc.vertex_layout.attributes = {
		   {"attr_brightness", Rhi::VertexFormat::kFloat, offsetof(PerVertexData, brightness)},
		   {"attr_dither_texture_position", Rhi::VertexFormat::kVec2,
		    offsetof(PerVertexData, dither_texture_x)},
		   {"attr_position", Rhi::VertexFormat::kVec2, offsetof(PerVertexData, gl_x)},
		   {"attr_texture_offset", Rhi::VertexFormat::kVec2,
		    offsetof(PerVertexData, texture_offset_x)},
		   {"attr_texture_position", Rhi::VertexFormat::kVec2,
		    offsetof(PerVertexData, texture_x)},
		};
		desc.topology = Rhi::PrimitiveTopology::kTriangleList;
		desc.blend = Rhi::kBlendAlpha;
		desc.depth = {true, true, Rhi::CompareOp::kLessOrEqual};
		desc.samplers = {{0, "u_dither_texture"}, {1, "u_terrain_texture"}};
		desc.uniform_block = Rhi::UniformBlockBinding{
		   0, "per_program_state", sizeof(Gl::PerProgramState)};
		pipeline_ = Rhi::device().create_pipeline(desc);
		descriptor_set_ = Rhi::device().create_descriptor_set(*pipeline_);
		vertex_buffer_ = Rhi::device().create_buffer(0, Rhi::BufferUsage::kVertex);
		uniform_rhi_buffer_ =
		   Rhi::device().create_buffer(sizeof(Gl::PerProgramState), Rhi::BufferUsage::kUniform);
		return;
	}

	gl_program_.build("dither");

	u_dither_texture_ = glGetUniformLocation(gl_program_.object(), "u_dither_texture");
	u_terrain_texture_ = glGetUniformLocation(gl_program_.object(), "u_terrain_texture");
	u_texture_dimensions_ = glGetUniformLocation(gl_program_.object(), "u_texture_dimensions");
	u_z_value_ = glGetUniformLocation(gl_program_.object(), "u_z_value");
	u_value_amplitude_ = glGetUniformLocation(gl_program_.object(), "u_value_amplitude");
	u_tint_amplitude_ = glGetUniformLocation(gl_program_.object(), "u_tint_amplitude");
	u_warp_amplitude_ = glGetUniformLocation(gl_program_.object(), "u_warp_amplitude");

	gl_array_buffer_.bind();
	vao_.define_attributes({
	   {gl_program_.attribute_location("attr_brightness"), 1, sizeof(PerVertexData),
	    offsetof(PerVertexData, brightness)},
	   {gl_program_.attribute_location("attr_dither_texture_position"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, dither_texture_x)},
	   {gl_program_.attribute_location("attr_position"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, gl_x)},
	   {gl_program_.attribute_location("attr_texture_offset"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, texture_offset_x)},
	   {gl_program_.attribute_location("attr_texture_position"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, texture_x)},
	});
}

void DitherProgram::set_dither_mask(const std::string& filepath) {
	dither_mask_.reset(new Texture(load_image_as_sdl_surface(filepath, g_fs), true));

	// The glTexParameteri block below is skipped on the core path because the
	// RHI descriptor-set binding handles the texture as-is. It is redundant on
	// *both* paths: Texture::init (texture.cc) already sets wrap=clamp-to-edge
	// and filter=linear for every texture it creates, including this mask. It
	// stays on the legacy path because decision 4 freezes that path; do not
	// delete it (Phase C review, C12).
	if (!Rhi::has_device()) {
		Gl::State::instance().bind(GL_TEXTURE0, dither_mask_->blit_data().texture_id);
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
		glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
	}
}

void DitherProgram::add_vertex(const FieldsToDraw::Field& field,
                               const TrianglePoint triangle_point,
                               const Vector2f& texture_offset) {
	vertices_.emplace_back();
	PerVertexData& back = vertices_.back();

	back.gl_x = field.gl_position.x;
	back.gl_y = field.gl_position.y;
	back.texture_x = field.texture_coords.x;
	back.texture_y = field.texture_coords.y;
	back.brightness = field.brightness;
	back.texture_offset_x = texture_offset.x;
	back.texture_offset_y = texture_offset.y;

	switch (triangle_point) {
	case TrianglePoint::kTopRight:
		back.dither_texture_x = 1.;
		back.dither_texture_y = 1.;
		break;
	case TrianglePoint::kTopLeft:
		back.dither_texture_x = 0.;
		back.dither_texture_y = 1.;
		break;
	case TrianglePoint::kBottomMiddle:
		back.dither_texture_x = 0.5;
		back.dither_texture_y = 0.;
		break;
	default:
		NEVER_HERE();
	}
}

void DitherProgram::maybe_add_dithering_triangle(
   const uint32_t gametime,
   const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
   const FieldsToDraw& fields_to_draw,
   const int idx1,
   const int idx2,
   const int idx3,
   const int my_terrain,
   const int other_terrain) {
	if (my_terrain == other_terrain) {
		return;
	}
	const Widelands::TerrainDescription& other_terrain_description = terrains.get(other_terrain);
	if (terrains.get(my_terrain).dither_layer() < other_terrain_description.dither_layer()) {
		const FieldsToDraw::Field& f1 = fields_to_draw.at(idx1);
		if (f1.obscured_by_slope) {
			return;
		}
		const FieldsToDraw::Field& f2 = fields_to_draw.at(idx2);
		if (f2.obscured_by_slope) {
			return;
		}
		const FieldsToDraw::Field& f3 = fields_to_draw.at(idx3);
		if (f3.obscured_by_slope) {
			return;
		}

		const Vector2f texture_offset =
		   to_gl_texture(other_terrain_description.get_texture(gametime).blit_data()).origin();
		add_vertex(f1, TrianglePoint::kTopRight, texture_offset);
		add_vertex(f2, TrianglePoint::kTopLeft, texture_offset);
		add_vertex(f3, TrianglePoint::kBottomMiddle, texture_offset);
	}
}

void DitherProgram::gl_draw(const BlitData& blit_data,
                            const float texture_w,
                            const float texture_h,
                            const float z_value) {
	assert(dither_mask_ != nullptr);

	if (Rhi::has_device()) {
		vertex_buffer_->update(vertices_.data(), vertices_.size() * sizeof(PerVertexData));

		Gl::PerProgramState state{};
		state.z_value = z_value;
		state.value_amplitude = kValueAmplitude * noise_strength_;
		state.tint_amplitude = kTintAmplitude * noise_strength_;
		state.warp_amplitude = kWarpAmplitude * noise_strength_;
		state.texture_w = texture_w;
		state.texture_h = texture_h;
		uniform_rhi_buffer_->update(&state, sizeof(state));

		descriptor_set_->set_texture(0, dither_mask_->blit_data().texture);
		descriptor_set_->set_texture(1, blit_data.texture);
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

	gl_state.bind(GL_TEXTURE0, dither_mask_->blit_data().texture_id);
	gl_state.bind(GL_TEXTURE1, blit_data.texture_id);

	glUniform1i(u_dither_texture_, 0);
	glUniform1i(u_terrain_texture_, 1);

	glUniform1f(u_z_value_, z_value);
	glUniform2f(u_texture_dimensions_, texture_w, texture_h);
	glUniform1f(u_value_amplitude_, kValueAmplitude * noise_strength_);
	glUniform1f(u_tint_amplitude_, kTintAmplitude * noise_strength_);
	glUniform1f(u_warp_amplitude_, kWarpAmplitude * noise_strength_);

	glDrawArrays(GL_TRIANGLES, 0, vertices_.size());
}

void DitherProgram::draw(
   const uint32_t gametime,
   const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
   const FieldsToDraw& fields_to_draw,
   const float z_value,
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

		const Widelands::Map* map =
		   (player != nullptr) && !player->see_all() ? &player->egbase().map() : nullptr;
		// Dithering triangles for Down triangle.
		if (field.bln_index != FieldsToDraw::kInvalidIndex) {
			const Widelands::DescriptionIndex terrain_d =
			   map != nullptr ? player->fields()[map->get_index(field.fcoords)].terrains.load().d :
			                    field.fcoords.field->terrain_d();
			const Widelands::DescriptionIndex terrain_r =
			   map != nullptr ? player->fields()[map->get_index(field.fcoords)].terrains.load().r :
			                    field.fcoords.field->terrain_r();
			maybe_add_dithering_triangle(gametime, terrains, fields_to_draw, field.brn_index,
			                             current_index, field.bln_index, terrain_d, terrain_r);

			const Widelands::DescriptionIndex terrain_dd =
			   map != nullptr ?
			      player->fields()[map->get_index(map->bl_n(field.fcoords))].terrains.load().r :
			      fields_to_draw.at(field.bln_index).fcoords.field->terrain_r();
			maybe_add_dithering_triangle(gametime, terrains, fields_to_draw, field.bln_index,
			                             field.brn_index, current_index, terrain_d, terrain_dd);

			if (field.ln_index != FieldsToDraw::kInvalidIndex) {
				const Widelands::DescriptionIndex terrain_l =
				   map != nullptr ?
				      player->fields()[map->get_index(map->l_n(field.fcoords))].terrains.load().r :
				      fields_to_draw.at(field.ln_index).fcoords.field->terrain_r();
				maybe_add_dithering_triangle(gametime, terrains, fields_to_draw, current_index,
				                             field.bln_index, field.brn_index, terrain_d, terrain_l);
			}
		}

		// Dithering for right triangle.
		if (field.rn_index != FieldsToDraw::kInvalidIndex) {
			const Widelands::DescriptionIndex terrain_r =
			   map != nullptr ? player->fields()[map->get_index(field.fcoords)].terrains.load().r :
			                    field.fcoords.field->terrain_r();
			const Widelands::DescriptionIndex terrain_d =
			   map != nullptr ? player->fields()[map->get_index(field.fcoords)].terrains.load().d :
			                    field.fcoords.field->terrain_d();

			maybe_add_dithering_triangle(gametime, terrains, fields_to_draw, current_index,
			                             field.brn_index, field.rn_index, terrain_r, terrain_d);
			const Widelands::DescriptionIndex terrain_rr =
			   map != nullptr ?
			      player->fields()[map->get_index(map->r_n(field.fcoords))].terrains.load().d :
			      fields_to_draw.at(field.rn_index).fcoords.field->terrain_d();
			maybe_add_dithering_triangle(gametime, terrains, fields_to_draw, field.brn_index,
			                             field.rn_index, current_index, terrain_r, terrain_rr);

			if (field.trn_index != FieldsToDraw::kInvalidIndex) {
				const Widelands::DescriptionIndex terrain_u =
				   map != nullptr ?
				      player->fields()[map->get_index(map->tr_n(field.fcoords))].terrains.load().d :
				      fields_to_draw.at(field.trn_index).fcoords.field->terrain_d();
				maybe_add_dithering_triangle(gametime, terrains, fields_to_draw, field.rn_index,
				                             current_index, field.brn_index, terrain_r, terrain_u);
			}
		}
	}

	const BlitData& blit_data = terrains.get(0).get_texture(0).blit_data();
	const Rectf texture_coordinates = to_gl_texture(blit_data);
	gl_draw(blit_data, texture_coordinates.w, texture_coordinates.h, z_value);
}
