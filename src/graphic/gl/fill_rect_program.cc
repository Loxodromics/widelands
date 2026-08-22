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

#include "graphic/gl/fill_rect_program.h"

#include "base/macros.h"
#include "base/math.h"
#include "base/wexception.h"
#include "graphic/rhi/device.h"

// static
FillRectProgram& FillRectProgram::instance() {
	static FillRectProgram fill_rect_program;
	return fill_rect_program;
}

FillRectProgram::FillRectProgram() {
	Rhi::PipelineDescriptor desc;
	desc.program_name = "fill_rect";
	desc.vertex_layout.stride = sizeof(PerVertexData);
	desc.vertex_layout.attributes = {
	   {"attr_position", Rhi::VertexFormat::kVec3, offsetof(PerVertexData, gl_x)},
	   {"attr_color", Rhi::VertexFormat::kVec4, offsetof(PerVertexData, r)},
	};
	desc.topology = Rhi::PrimitiveTopology::kTriangleList;
	desc.depth = {true, true, Rhi::CompareOp::kLessOrEqual};

	desc.blend = Rhi::kBlendAlpha;
	pipeline_alpha_ = Rhi::device().create_pipeline(desc);
	desc.blend = Rhi::kBlendAdditive;
	pipeline_additive_ = Rhi::device().create_pipeline(desc);
	desc.blend = Rhi::kBlendReverseSubtract;
	pipeline_reverse_subtract_ = Rhi::device().create_pipeline(desc);
	desc.blend = Rhi::kBlendOpaque;
	pipeline_opaque_ = Rhi::device().create_pipeline(desc);

	vertex_buffer_ = Rhi::device().create_buffer(0, Rhi::BufferUsage::kVertex);
}

Rhi::Pipeline* FillRectProgram::pipeline_for(const BlendMode blend_mode) const {
	switch (blend_mode) {
	case BlendMode::Copy:
		return pipeline_opaque_.get();
	case BlendMode::Default:
		return pipeline_alpha_.get();
	case BlendMode::UseAlpha:
		return pipeline_additive_.get();
	case BlendMode::Subtract:
		return pipeline_reverse_subtract_.get();
	}
	NEVER_HERE();
}

std::vector<FillRectProgram::Arguments>
FillRectProgram::make_arguments_for_rect(const Rectf& destination_rect,
                                         const float z_value,
                                         const RGBAColor& color,
                                         const BlendMode blend_mode) {
	const float r = color.r / 255.f;
	const float g = color.g / 255.f;
	const float b = color.b / 255.f;
	const float a = color.a / 255.f;

	Arguments::Vertex vbr = {
	   Vector2f(destination_rect.x + destination_rect.w, destination_rect.y + destination_rect.h), r,
	   g, b, a};
	Arguments::Vertex vtr = {
	   Vector2f(destination_rect.x + destination_rect.w, destination_rect.y), r, g, b, a};
	Arguments::Vertex vbl = {
	   Vector2f(destination_rect.x, destination_rect.y + destination_rect.h), r, g, b, a};
	Arguments::Vertex vtl = {Vector2f(destination_rect.x, destination_rect.y), r, g, b, a};

	return {
	   Arguments{{vbr, vtl, vtr}, z_value, blend_mode},
	   Arguments{{vbr, vtl, vbl}, z_value, blend_mode},
	};
}

void FillRectProgram::draw(const Rectf& destination_rect,
                           const float z_value,
                           const RGBAColor& color,
                           const BlendMode blend_mode) {
	draw(make_arguments_for_rect(destination_rect, z_value, color, blend_mode));
}

static inline void assign_color_to_vertex(FillRectProgram::Arguments::Vertex& vertex,
                                          const float val) {
	vertex.color_a = 0.9f;
	vertex.color_g = 0.f;

	// Progression from black via blue and purple to red.
	vertex.color_r = math::clamp(val * 3.f - 1.f, 0.f, 1.f);
	vertex.color_b = math::clamp(3.f * (val < 0.5f ? val : (1.f - val)), 0.f, 1.f);
}

void FillRectProgram::draw_height_heat_map_overlays(const FieldsToDraw& fields_to_draw,
                                                    const float z_value) {
	std::vector<Arguments> arguments;

	for (size_t current_index = 0; current_index < fields_to_draw.size(); ++current_index) {
		const FieldsToDraw::Field& field = fields_to_draw.at(current_index);
		if (field.brn_index == FieldsToDraw::kInvalidIndex) {
			continue;
		}

		const FieldsToDraw::Field& field_brn = fields_to_draw.at(field.brn_index);

		float val1 = field.fcoords.field->get_height();
		float val2 = field_brn.fcoords.field->get_height();
		val1 /= MAX_FIELD_HEIGHT;
		val2 /= MAX_FIELD_HEIGHT;
		assert(val1 >= 0.f && val1 <= 1.f);
		assert(val2 >= 0.f && val2 <= 1.f);

		Arguments arg;
		arg.z_value = z_value;
		arg.blend_mode = BlendMode::Default;
		arg.triangle[0].point = field.gl_position;
		arg.triangle[1].point = field_brn.gl_position;
		assign_color_to_vertex(arg.triangle[0], val1);
		assign_color_to_vertex(arg.triangle[1], val2);

		if (field.rn_index != FieldsToDraw::kInvalidIndex) {
			const FieldsToDraw::Field& field_rn = fields_to_draw.at(field.rn_index);
			float val3 = field_rn.fcoords.field->get_height();
			val3 /= MAX_FIELD_HEIGHT;
			assert(val3 >= 0.f && val3 <= 1.f);

			arg.triangle[2].point = field_rn.gl_position;
			assign_color_to_vertex(arg.triangle[2], val3);

			arguments.push_back(arg);
		}

		if (field.bln_index != FieldsToDraw::kInvalidIndex) {
			const FieldsToDraw::Field& field_bln = fields_to_draw.at(field.bln_index);
			float val3 = field_bln.fcoords.field->get_height();
			val3 /= MAX_FIELD_HEIGHT;
			assert(val3 >= 0.f && val3 <= 1.f);

			arg.triangle[2].point = field_bln.gl_position;
			assign_color_to_vertex(arg.triangle[2], val3);

			arguments.push_back(arg);
		}
	}

	draw(arguments);
}

void FillRectProgram::draw(const std::vector<Arguments>& arguments) {
	size_t i = 0;

	while (i < arguments.size()) {
		vertices_.clear();
		const Arguments& template_args = arguments[i];

		// The RHI pipeline carries the blend state, so selecting the pipeline
		// is all this needs; no glBlend* setup/restore around the draw call.
		while (i < arguments.size()) {
			const Arguments& current_args = arguments[i];
			if (current_args.blend_mode != template_args.blend_mode) {
				break;
			}
			for (const Arguments::Vertex& vertex : current_args.triangle) {
				vertices_.emplace_back(vertex.point.x, vertex.point.y, current_args.z_value,
				                       vertex.color_r, vertex.color_g, vertex.color_b, vertex.color_a);
			}
			++i;
		}
		vertex_buffer_->update(vertices_.data(), vertices_.size() * sizeof(PerVertexData));
		auto& command_buffer = Rhi::command_buffer();
		command_buffer.bind_pipeline(pipeline_for(template_args.blend_mode));
		command_buffer.bind_vertex_buffer(vertex_buffer_.get());
		command_buffer.draw(0, vertices_.size());
	}
}
