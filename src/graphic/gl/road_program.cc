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

#include "graphic/gl/road_program.h"

#include <algorithm>
#include <cassert>

#include "graphic/gl/coordinate_conversion.h"
#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/terrain_lighting.h"
#include "graphic/gl/utils.h"
#include "graphic/rhi/device.h"
#include "graphic/texture.h"
#include "logic/player.h"

namespace {

/* Roads read a single scalar brightness, not the two-tone terrain_light()
 * used by terrain.fp/dither.fp (V2, Claude/VISUAL_FIDELITY_RANKED.md §4.2), so
 * this collapses field_light() (terrain_lighting.h) to Rec.709 luma rather
 * than carrying the colour split, folded onto the field's visibility factor.
 * Roads not carrying the terrain's colour tint is a knowing simplification --
 * see the standing "roads do not carry the terrain variation" item,
 * VISUAL_FIDELITY_RANKED.md §5.
 */
float road_brightness(const FieldsToDraw::Field& field) {
	const Vector3f lit = field_light(field.normal);
	constexpr float kLumaR = 0.2126f;
	constexpr float kLumaG = 0.7152f;
	constexpr float kLumaB = 0.0722f;
	const float luma = kLumaR * lit.x + kLumaG * lit.y + kLumaB * lit.z;
	return field.brightness * luma;
}

}  // namespace

RoadProgram::RoadProgram() {
	Rhi::PipelineDescriptor desc;
	desc.program_name = "road";
	desc.vertex_layout.stride = sizeof(PerVertexData);
	desc.vertex_layout.attributes = {
	   {"attr_position", Rhi::VertexFormat::kVec2, offsetof(PerVertexData, gl_x)},
	   {"attr_texture_position", Rhi::VertexFormat::kVec2, offsetof(PerVertexData, texture_x)},
	   {"attr_brightness", Rhi::VertexFormat::kFloat, offsetof(PerVertexData, brightness)},
	};
	desc.topology = Rhi::PrimitiveTopology::kTriangleList;
	desc.blend = Rhi::kBlendAlpha;
	desc.depth = {true, true, Rhi::CompareOp::kLessOrEqual};
	desc.samplers = {{0, "u_texture"}};
	desc.uniform_block = Rhi::UniformBlockBinding{0, "per_program_state", Gl::kZValueOnlyBlockSize};
	pipeline_ = Rhi::device().create_pipeline(desc);
	descriptor_set_ = Rhi::device().create_descriptor_set(*pipeline_);
	vertex_buffer_ = Rhi::device().create_buffer(0, Rhi::BufferUsage::kVertex);
	uniform_rhi_buffer_ =
	   Rhi::device().create_buffer(sizeof(Gl::PerProgramState), Rhi::BufferUsage::kUniform);
}

void RoadProgram::add_road(const int renderbuffer_width,
                           const int renderbuffer_height,
                           const FieldsToDraw::Field& start,
                           const FieldsToDraw::Field& end,
                           const float scale,
                           const Widelands::RoadSegment road_type,
                           const Direction direction,
                           BlitData* road_texture) {
	// The thickness of the road in pixels on screen.
	static constexpr float kRoadThicknessInPixels = 5.f;

	// The overshot of the road in either direction in percent.
	static constexpr float kRoadElongationInPercent = .1f;

	const float delta_x = end.surface_pixel.x - start.surface_pixel.x;
	const float delta_y = end.surface_pixel.y - start.surface_pixel.y;
	const float vector_length = std::hypot(delta_x, delta_y);

	const float road_overshoot_x = delta_x * kRoadElongationInPercent;
	const float road_overshoot_y = delta_y * kRoadElongationInPercent;

	// Find the reciprocal unit vector, so that we can calculate start and end
	// points for the quad that will make the road.
	const float road_thickness_x = (-delta_y / vector_length) * kRoadThicknessInPixels * scale;
	const float road_thickness_y = (delta_x / vector_length) * kRoadThicknessInPixels * scale;

	assert(start.owner != nullptr || end.owner != nullptr);

	Widelands::Player* visible_owner = start.owner;
	if (start.owner == nullptr) {
		visible_owner = end.owner;
	}

	assert(road_type == Widelands::RoadSegment::kNormal ||
	       road_type == Widelands::RoadSegment::kBusy ||
	       road_type == Widelands::RoadSegment::kWaterway);
	const Image& texture =
	   road_type == Widelands::RoadSegment::kNormal ?
	      visible_owner->tribe().road_textures().get_normal_texture(start.fcoords, direction) :
	   road_type == Widelands::RoadSegment::kWaterway ?
	      visible_owner->tribe().road_textures().get_waterway_texture(start.fcoords, direction) :
	      visible_owner->tribe().road_textures().get_busy_texture(start.fcoords, direction);
	if (!has_texture(*road_texture)) {
		*road_texture = texture.blit_data();
	}
	// We assume that all road textures are in the same OpenGL texture, i.e. in
	// one texture atlas.
	assert(batch_id(*road_texture) == batch_id(texture.blit_data()));

	const Rectf texture_rect = to_gl_texture(texture.blit_data());

	const float start_brightness = road_brightness(start);
	const float end_brightness = road_brightness(end);

	vertices_.emplace_back(PerVertexData{
	   start.surface_pixel.x - road_overshoot_x + road_thickness_x,
	   start.surface_pixel.y - road_overshoot_y + road_thickness_y,
	   texture_rect.x,
	   texture_rect.y,
	   start_brightness,
	});
	pixel_to_gl_renderbuffer(
	   renderbuffer_width, renderbuffer_height, &vertices_.back().gl_x, &vertices_.back().gl_y);

	vertices_.emplace_back(PerVertexData{
	   start.surface_pixel.x - road_overshoot_x - road_thickness_x,
	   start.surface_pixel.y - road_overshoot_y - road_thickness_y,
	   texture_rect.x,
	   texture_rect.y + texture_rect.h,
	   start_brightness,
	});
	pixel_to_gl_renderbuffer(
	   renderbuffer_width, renderbuffer_height, &vertices_.back().gl_x, &vertices_.back().gl_y);

	vertices_.emplace_back(PerVertexData{
	   end.surface_pixel.x + road_overshoot_x + road_thickness_x,
	   end.surface_pixel.y + road_overshoot_y + road_thickness_y,
	   texture_rect.x + texture_rect.w,
	   texture_rect.y,
	   end_brightness,
	});
	pixel_to_gl_renderbuffer(
	   renderbuffer_width, renderbuffer_height, &vertices_.back().gl_x, &vertices_.back().gl_y);

	// As OpenGl does not support drawing quads in modern days and we have a
	// bunch of roads that might not be neighbored, we need to add two triangles
	// for each road. :(. Another alternative would be to use primitive restart,
	// but that is a fairly recent OpenGL feature.
	vertices_.emplace_back(vertices_.at(vertices_.size() - 2));
	vertices_.emplace_back(vertices_.at(vertices_.size() - 2));

	vertices_.emplace_back(PerVertexData{
	   end.surface_pixel.x + road_overshoot_x - road_thickness_x,
	   end.surface_pixel.y + road_overshoot_y - road_thickness_y,
	   texture_rect.x + texture_rect.w,
	   texture_rect.y + texture_rect.h,
	   end_brightness,
	});
	pixel_to_gl_renderbuffer(
	   renderbuffer_width, renderbuffer_height, &vertices_.back().gl_x, &vertices_.back().gl_y);
}

void RoadProgram::draw(const int renderbuffer_width,
                       const int renderbuffer_height,
                       const FieldsToDraw& fields_to_draw,
                       const float scale,
                       const float z_value) {
	vertices_.clear();

	BlitData road_texture{};
	for (size_t current_index = 0; current_index < fields_to_draw.size(); ++current_index) {
		const FieldsToDraw::Field& field = fields_to_draw.at(current_index);

		// Road to right neighbor.
		if (field.rn_index != FieldsToDraw::kInvalidIndex &&
		    !(field.obscured_by_slope && fields_to_draw.at(field.rn_index).obscured_by_slope)) {
			if (field.road_e != Widelands::RoadSegment::kNone &&
			    field.road_e != Widelands::RoadSegment::kBridgeNormal &&
			    field.road_e != Widelands::RoadSegment::kBridgeBusy) {
				add_road(renderbuffer_width, renderbuffer_height, field,
				         fields_to_draw.at(field.rn_index), scale, field.road_e, kEast, &road_texture);
			}
		}

		// Road to bottom right neighbor.
		if (field.brn_index != FieldsToDraw::kInvalidIndex &&
		    !(field.obscured_by_slope && fields_to_draw.at(field.brn_index).obscured_by_slope)) {
			if (field.road_se != Widelands::RoadSegment::kNone &&
			    field.road_se != Widelands::RoadSegment::kBridgeNormal &&
			    field.road_se != Widelands::RoadSegment::kBridgeBusy) {
				add_road(renderbuffer_width, renderbuffer_height, field,
				         fields_to_draw.at(field.brn_index), scale, field.road_se, kSouthEast,
				         &road_texture);
			}
		}

		// Road to bottom left neighbor.
		if (field.bln_index != FieldsToDraw::kInvalidIndex &&
		    !(field.obscured_by_slope && fields_to_draw.at(field.bln_index).obscured_by_slope)) {
			if (field.road_sw != Widelands::RoadSegment::kNone &&
			    field.road_sw != Widelands::RoadSegment::kBridgeNormal &&
			    field.road_sw != Widelands::RoadSegment::kBridgeBusy) {
				add_road(renderbuffer_width, renderbuffer_height, field,
				         fields_to_draw.at(field.bln_index), scale, field.road_sw, kSouthWest,
				         &road_texture);
			}
		}
	}

	vertex_buffer_->update(vertices_.data(), vertices_.size() * sizeof(PerVertexData));

	Gl::PerProgramState state{};
	state.z_value = z_value;
	uniform_rhi_buffer_->update(&state, sizeof(state));

	descriptor_set_->set_texture(0, road_texture.texture);
	descriptor_set_->set_uniform_buffer(0, uniform_rhi_buffer_.get(), 0, sizeof(state));

	auto& command_buffer = Rhi::command_buffer();
	command_buffer.bind_pipeline(pipeline_.get());
	command_buffer.bind_descriptor_set(descriptor_set_.get());
	command_buffer.bind_vertex_buffer(vertex_buffer_.get());
	command_buffer.draw(0, vertices_.size());
}
