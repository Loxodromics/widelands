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

#include <cmath>

#include "graphic/gl/coordinate_conversion.h"
#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/terrain_lighting.h"
#include "graphic/gl/terrain_noise.h"
#include "graphic/gl/utils.h"
#include "graphic/rhi/device.h"
#include "logic/player.h"

DitherProgram::DitherProgram() {
	if (Rhi::has_device()) {
		Rhi::PipelineDescriptor desc;
		desc.program_name = "dither";
		desc.vertex_layout.stride = sizeof(PerVertexData);
		desc.vertex_layout.attributes = {
		   {"attr_brightness", Rhi::VertexFormat::kFloat, offsetof(PerVertexData, brightness)},
		   {"attr_dither_params", Rhi::VertexFormat::kVec2,
		    offsetof(PerVertexData, dither_amplitude)},
		   {"attr_dither_ramp", Rhi::VertexFormat::kFloat, offsetof(PerVertexData, dither_ramp)},
		   {"attr_normal", Rhi::VertexFormat::kVec3, offsetof(PerVertexData, normal_x)},
		   {"attr_position", Rhi::VertexFormat::kVec2, offsetof(PerVertexData, gl_x)},
		   {"attr_texture_offset", Rhi::VertexFormat::kVec2,
		    offsetof(PerVertexData, texture_offset_x)},
		   {"attr_texture_position", Rhi::VertexFormat::kVec2, offsetof(PerVertexData, texture_x)},
		};
		desc.topology = Rhi::PrimitiveTopology::kTriangleList;
		desc.blend = Rhi::kBlendAlpha;
		desc.depth = {true, true, Rhi::CompareOp::kLessOrEqual};
		desc.samplers = {{0, "u_terrain_texture"}};
		desc.uniform_block =
		   Rhi::UniformBlockBinding{0, "per_program_state", sizeof(Gl::PerProgramState)};
		pipeline_ = Rhi::device().create_pipeline(desc);
		descriptor_set_ = Rhi::device().create_descriptor_set(*pipeline_);
		vertex_buffer_ = Rhi::device().create_buffer(0, Rhi::BufferUsage::kVertex);
		uniform_rhi_buffer_ =
		   Rhi::device().create_buffer(sizeof(Gl::PerProgramState), Rhi::BufferUsage::kUniform);
		return;
	}

	gl_program_.build("dither");

	u_terrain_texture_ = glGetUniformLocation(gl_program_.object(), "u_terrain_texture");
	u_texture_dimensions_ = glGetUniformLocation(gl_program_.object(), "u_texture_dimensions");
	u_z_value_ = glGetUniformLocation(gl_program_.object(), "u_z_value");
	u_bump_amplitude_ = glGetUniformLocation(gl_program_.object(), "u_bump_amplitude");
	u_tint_amplitude_ = glGetUniformLocation(gl_program_.object(), "u_tint_amplitude");
	u_warp_amplitude_ = glGetUniformLocation(gl_program_.object(), "u_warp_amplitude");
	u_time_ = glGetUniformLocation(gl_program_.object(), "u_time");
	u_cloud_amplitude_ = glGetUniformLocation(gl_program_.object(), "u_cloud_amplitude");
	u_sun_direction_ = glGetUniformLocation(gl_program_.object(), "u_sun_direction");
	u_sun_color_ = glGetUniformLocation(gl_program_.object(), "u_sun_color");
	u_ambient_color_ = glGetUniformLocation(gl_program_.object(), "u_ambient_color");

	gl_array_buffer_.bind();
	vao_.define_attributes({
	   {gl_program_.attribute_location("attr_brightness"), 1, sizeof(PerVertexData),
	    offsetof(PerVertexData, brightness)},
	   {gl_program_.attribute_location("attr_dither_params"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, dither_amplitude)},
	   {gl_program_.attribute_location("attr_dither_ramp"), 1, sizeof(PerVertexData),
	    offsetof(PerVertexData, dither_ramp)},
	   {gl_program_.attribute_location("attr_normal"), 3, sizeof(PerVertexData),
	    offsetof(PerVertexData, normal_x)},
	   {gl_program_.attribute_location("attr_position"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, gl_x)},
	   {gl_program_.attribute_location("attr_texture_offset"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, texture_offset_x)},
	   {gl_program_.attribute_location("attr_texture_position"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, texture_x)},
	});
}

namespace {

/* The terrain of a field's 'd' or 'r' triangle, as the player should see it:
 * under fog of war that is what they saw last, not what is there now. 'map' is
 * null exactly when the true terrain applies (no player, or seeing all).
 */
Widelands::DescriptionIndex triangle_terrain(const FieldsToDraw& fields_to_draw,
                                             const Widelands::Map* map,
                                             const Widelands::Player* player,
                                             const int index,
                                             const bool down_triangle) {
	const Widelands::FCoords& fcoords = fields_to_draw.at(index).fcoords;
	if (map != nullptr) {
		const auto terrains = player->fields()[map->get_index(fcoords)].terrains.load();
		return down_triangle ? terrains.d : terrains.r;
	}
	return down_triangle ? fcoords.field->terrain_d() : fcoords.field->terrain_r();
}

}  // namespace

void DitherProgram::TerrainSet::add(const Widelands::DescriptionIndex terrain) {
	if (contains(terrain)) {
		return;
	}
	if (count < kCapacity) {
		terrains[count++] = terrain;
	}
}

bool DitherProgram::TerrainSet::contains(const Widelands::DescriptionIndex terrain) const {
	for (uint8_t i = 0; i < count; ++i) {
		if (terrains[i] == terrain) {
			return true;
		}
	}
	return false;
}

void DitherProgram::add_vertex(const FieldsToDraw::Field& field,
                               const float dither_ramp,
                               const Widelands::TerrainDescription& terrain,
                               const Vector2f& texture_offset) {
	vertices_.emplace_back();
	PerVertexData& back = vertices_.back();

	back.gl_x = field.gl_position.x;
	back.gl_y = field.gl_position.y;
	back.texture_x = field.texture_coords.x;
	back.texture_y = field.texture_coords.y;
	back.brightness = field.brightness;
	back.normal_x = field.normal.x;
	back.normal_y = field.normal.y;
	back.normal_z = field.normal.z;
	back.dither_ramp = dither_ramp;
	back.dither_amplitude = terrain.dither_amplitude();
	back.dither_softness = terrain.dither_softness();
	back.texture_offset_x = texture_offset.x;
	back.texture_offset_y = texture_offset.y;
}

void DitherProgram::collect_vertex_terrains(const FieldsToDraw& fields_to_draw,
                                            const Widelands::Map* map,
                                            const Widelands::Player* player) {
	vertex_terrains_.assign(fields_to_draw.size(), TerrainSet());

	for (size_t i = 0; i < fields_to_draw.size(); ++i) {
		const FieldsToDraw::Field& field = fields_to_draw.at(i);
		const int index = static_cast<int>(i);

		if (field.bln_index != FieldsToDraw::kInvalidIndex &&
		    field.brn_index != FieldsToDraw::kInvalidIndex) {
			const Widelands::DescriptionIndex terrain_d =
			   triangle_terrain(fields_to_draw, map, player, index, true);
			vertex_terrains_[i].add(terrain_d);
			vertex_terrains_[field.bln_index].add(terrain_d);
			vertex_terrains_[field.brn_index].add(terrain_d);
		}

		if (field.rn_index != FieldsToDraw::kInvalidIndex &&
		    field.brn_index != FieldsToDraw::kInvalidIndex) {
			const Widelands::DescriptionIndex terrain_r =
			   triangle_terrain(fields_to_draw, map, player, index, false);
			vertex_terrains_[i].add(terrain_r);
			vertex_terrains_[field.rn_index].add(terrain_r);
			vertex_terrains_[field.brn_index].add(terrain_r);
		}
	}
}

void DitherProgram::add_dithering_triangles(
   const uint32_t gametime,
   const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
   const FieldsToDraw& fields_to_draw,
   const BaseTriangle& base) {
	const FieldsToDraw::Field* vertices[3];
	for (int i = 0; i < 3; ++i) {
		vertices[i] = &fields_to_draw.at(base.vertex[i]);
		if (vertices[i]->obscured_by_slope) {
			return;
		}
	}

	const int32_t my_layer = terrains.get(base.terrain).dither_layer();

	TerrainSet candidates;
	for (const int vertex : base.vertex) {
		const TerrainSet& incident = vertex_terrains_[vertex];
		for (uint8_t k = 0; k < incident.count; ++k) {
			if (terrains.get(incident.terrains[k]).dither_layer() > my_layer) {
				candidates.add(incident.terrains[k]);
			}
		}
	}

	/* Sort by dither layer so the higher one ends up on top. With edge-only
	 * emission a base triangle rarely carried more than one overlay and the
	 * order was incidental; vertex incidence makes overlaps routine, so the
	 * order has to be explicit. Insertion sort: 'candidates' holds a handful of
	 * entries at most.
	 */
	for (uint8_t i = 1; i < candidates.count; ++i) {
		const Widelands::DescriptionIndex terrain = candidates.terrains[i];
		const int32_t layer = terrains.get(terrain).dither_layer();
		uint8_t j = i;
		while (j > 0 && terrains.get(candidates.terrains[j - 1]).dither_layer() > layer) {
			candidates.terrains[j] = candidates.terrains[j - 1];
			--j;
		}
		candidates.terrains[j] = terrain;
	}

	for (uint8_t c = 0; c < candidates.count; ++c) {
		const Widelands::DescriptionIndex overlay_index = candidates.terrains[c];
		const Widelands::TerrainDescription& overlay = terrains.get(overlay_index);
		const Vector2f texture_offset =
		   to_gl_texture(overlay.get_texture(gametime).blit_data()).origin();

		for (int i = 0; i < 3; ++i) {
			const float ramp = vertex_terrains_[base.vertex[i]].contains(overlay_index) ? 1.f : 0.f;
			add_vertex(*vertices[i], ramp, overlay, texture_offset);
		}
	}
}

void DitherProgram::gl_draw(const BlitData& blit_data,
                            const float texture_w,
                            const float texture_h,
                            const float z_value,
                            const float time) {
	if (Rhi::has_device()) {
		vertex_buffer_->update(vertices_.data(), vertices_.size() * sizeof(PerVertexData));

		Gl::PerProgramState state{};
		state.z_value = z_value;
		state.bump_amplitude = kBumpAmplitude * noise_strength_;
		state.tint_amplitude = kTintAmplitude * noise_strength_;
		state.warp_amplitude = kWarpAmplitude * noise_strength_;
		state.texture_w = texture_w;
		state.texture_h = texture_h;
		state.time = time;
		state.cloud_amplitude = cloud_amplitude_;
		state.sun_x = kSunDirection.x;
		state.sun_y = kSunDirection.y;
		state.sun_z = kSunDirection.z;
		state.sun_color_r = kSunColor.x;
		state.sun_color_g = kSunColor.y;
		state.sun_color_b = kSunColor.z;
		state.ambient_color_r = kAmbientColor.x;
		state.ambient_color_g = kAmbientColor.y;
		state.ambient_color_b = kAmbientColor.z;
		uniform_rhi_buffer_->update(&state, sizeof(state));

		descriptor_set_->set_texture(0, blit_data.texture);
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

	gl_state.bind(GL_TEXTURE0, blit_data.texture_id);

	glUniform1i(u_terrain_texture_, 0);

	glUniform1f(u_z_value_, z_value);
	glUniform2f(u_texture_dimensions_, texture_w, texture_h);
	glUniform1f(u_bump_amplitude_, kBumpAmplitude * noise_strength_);
	glUniform1f(u_tint_amplitude_, kTintAmplitude * noise_strength_);
	glUniform1f(u_warp_amplitude_, kWarpAmplitude * noise_strength_);
	glUniform1f(u_time_, time);
	glUniform1f(u_cloud_amplitude_, cloud_amplitude_);
	glUniform3f(u_sun_direction_, kSunDirection.x, kSunDirection.y, kSunDirection.z);
	glUniform3f(u_sun_color_, kSunColor.x, kSunColor.y, kSunColor.z);
	glUniform3f(u_ambient_color_, kAmbientColor.x, kAmbientColor.y, kAmbientColor.z);

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

	const Widelands::Map* map =
	   (player != nullptr) && !player->see_all() ? &player->egbase().map() : nullptr;

	collect_vertex_terrains(fields_to_draw, map, player);

	for (size_t current_index = 0; current_index < fields_to_draw.size(); ++current_index) {
		const FieldsToDraw::Field& field = fields_to_draw.at(current_index);

		// The bottom right neighbor is needed for both triangles associated with
		// this field. If it is not in fields_to_draw, there is nothing to draw.
		if (field.brn_index == FieldsToDraw::kInvalidIndex) {
			continue;
		}
		const int index = static_cast<int>(current_index);

		// Vertex order matches what the edge-only emission used, so the winding
		// of the emitted triangles is unchanged.
		if (field.bln_index != FieldsToDraw::kInvalidIndex) {
			add_dithering_triangles(
			   gametime, terrains, fields_to_draw,
			   BaseTriangle{{field.brn_index, index, field.bln_index},
			                triangle_terrain(fields_to_draw, map, player, index, true)});
		}

		if (field.rn_index != FieldsToDraw::kInvalidIndex) {
			add_dithering_triangles(
			   gametime, terrains, fields_to_draw,
			   BaseTriangle{{index, field.brn_index, field.rn_index},
			                triangle_terrain(fields_to_draw, map, player, index, false)});
		}
	}

	const BlitData& blit_data = terrains.get(0).get_texture(0).blit_data();
	const Rectf texture_coordinates = to_gl_texture(blit_data);
	// Cloud shadows animate on the deterministic simulation clock; the wrap
	// bounds u_time's magnitude on long-running games (kCloudTimeWrapPeriod,
	// terrain_noise.h).
	const float time = std::fmod(static_cast<float>(gametime) / 1000.0f, kCloudTimeWrapPeriod);
	gl_draw(blit_data, texture_coordinates.w, texture_coordinates.h, z_value, time);
}
