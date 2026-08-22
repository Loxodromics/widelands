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

#include "graphic/gl/blit_program.h"

#include "graphic/blit_mode.h"
#include "graphic/gl/blit_data.h"
#include "graphic/gl/coordinate_conversion.h"
#include "graphic/gl/utils.h"
#include "graphic/rhi/device.h"

namespace {

// While drawing we put all draw calls into a buffer, so that we have to
// transfer the buffer to the GPU only once, even though we might need to do
// many glDraw* calls. This structure represents the parameters for one glDraw*
// call. It carries the BlitData so each backend can read the field it needs
// (the RHI texture on the core path, the GL name on the legacy path).
struct DrawBatch {
	int offset;
	int count;
	BlitData texture;
	BlitData mask;
	BlendMode blend_mode;
};

}  // namespace

BlitProgram::BlitProgram() {
	Rhi::PipelineDescriptor desc;
	desc.program_name = "blit";
	desc.vertex_layout.stride = sizeof(PerVertexData);
	desc.vertex_layout.attributes = {
	   {"attr_blend", Rhi::VertexFormat::kVec4, offsetof(PerVertexData, blend_r)},
	   {"attr_mask_texture_position", Rhi::VertexFormat::kVec2,
	    offsetof(PerVertexData, mask_texture_x)},
	   {"attr_position", Rhi::VertexFormat::kVec3, offsetof(PerVertexData, gl_x)},
	   {"attr_texture_position", Rhi::VertexFormat::kVec2, offsetof(PerVertexData, texture_x)},
	   {"attr_program_flavor", Rhi::VertexFormat::kFloat, offsetof(PerVertexData, program_flavor)},
	   {"attr_light", Rhi::VertexFormat::kVec3, offsetof(PerVertexData, light_r)},
	};
	desc.topology = Rhi::PrimitiveTopology::kTriangleList;
	desc.depth = {true, true, Rhi::CompareOp::kLessOrEqual};
	desc.samplers = {{0, "u_texture"}, {1, "u_mask"}};

	// One descriptor set per pipeline (C7): the layouts happen to be
	// compatible, but a descriptor set belongs to a pipeline, and Vulkan
	// enforces that. Each set is created from the pipeline it is paired
	// with, right next to it, so the pairing is visible in one place.
	desc.blend = Rhi::kBlendAlpha;
	alpha_.pipeline = Rhi::device().create_pipeline(desc);
	alpha_.descriptor_set = Rhi::device().create_descriptor_set(*alpha_.pipeline);

	desc.blend = Rhi::kBlendOpaque;
	opaque_.pipeline = Rhi::device().create_pipeline(desc);
	opaque_.descriptor_set = Rhi::device().create_descriptor_set(*opaque_.pipeline);

	vertex_buffer_ = Rhi::device().create_buffer(0, Rhi::BufferUsage::kVertex);
}

const BlitProgram::Variant& BlitProgram::variant_for(const BlendMode blend_mode) const {
	// blit uses ordinary alpha blending for Default/UseAlpha and overwrites the
	// destination for Copy (Claude/RHI_INTERFACE.md §4). The pipeline and its
	// descriptor set are chosen together, in one decision — picking them with
	// two independent conditionals is how a set ends up bound under a pipeline
	// it was not created for, which is what C7 was about.
	return blend_mode == BlendMode::Copy ? opaque_ : alpha_;
}

void BlitProgram::draw(const std::vector<Arguments>& arguments) {
	// Prepare the buffer for many draw calls.
	std::vector<DrawBatch> draw_batches;
	int offset = 0;
	vertices_.clear();

	size_t i = 0;
	while (i < arguments.size()) {
		const auto& template_args = arguments[i];

		// Batch common blit operations up.
		while (i < arguments.size()) {
			const auto& current_args = arguments[i];
			if (current_args.blend_mode != template_args.blend_mode ||
			    batch_id(current_args.texture) != batch_id(template_args.texture) ||
			    (has_texture(current_args.mask) &&
			     batch_id(current_args.mask) != batch_id(template_args.mask))) {
				break;
			}

			const float blend_r = current_args.blend.r / 255.;
			const float blend_g = current_args.blend.g / 255.;
			const float blend_b = current_args.blend.b / 255.;
			const float blend_a = current_args.blend.a / 255.;

			const Rectf texture_rect = to_gl_texture(current_args.texture);
			const Rectf mask_rect = to_gl_texture(current_args.mask);
			const float light_r = current_args.light.x;
			const float light_g = current_args.light.y;
			const float light_b = current_args.light.z;
			float program_flavor = 0;
			switch (current_args.blit_mode) {
			case BlitMode::kDirect:
				program_flavor = 0.;
				break;

			case BlitMode::kMonochrome:
				program_flavor = 1.;
				break;

			case BlitMode::kBlendedWithMask:
				program_flavor = 2.;
				break;

			default:
				NEVER_HERE();
			}

			vertices_.emplace_back(current_args.destination_rect.x, current_args.destination_rect.y,
			                       current_args.z_value, texture_rect.x, texture_rect.y, mask_rect.x,
			                       mask_rect.y, blend_r, blend_g, blend_b, blend_a, program_flavor,
			                       light_r, light_g, light_b);

			vertices_.emplace_back(current_args.destination_rect.x + current_args.destination_rect.w,
			                       current_args.destination_rect.y, current_args.z_value,
			                       texture_rect.x + texture_rect.w, texture_rect.y,
			                       mask_rect.x + mask_rect.w, mask_rect.y, blend_r, blend_g, blend_b,
			                       blend_a, program_flavor, light_r, light_g, light_b);

			vertices_.emplace_back(
			   current_args.destination_rect.x,
			   current_args.destination_rect.y + current_args.destination_rect.h, current_args.z_value,
			   texture_rect.x, texture_rect.y + texture_rect.h, mask_rect.x, mask_rect.y + mask_rect.h,
			   blend_r, blend_g, blend_b, blend_a, program_flavor, light_r, light_g, light_b);

			vertices_.emplace_back(vertices_.at(vertices_.size() - 2));
			vertices_.emplace_back(vertices_.at(vertices_.size() - 2));

			vertices_.emplace_back(current_args.destination_rect.x + current_args.destination_rect.w,
			                       current_args.destination_rect.y + current_args.destination_rect.h,
			                       current_args.z_value, texture_rect.x + texture_rect.w,
			                       texture_rect.y + texture_rect.h, mask_rect.x + mask_rect.w,
			                       mask_rect.y + mask_rect.h, blend_r, blend_g, blend_b, blend_a,
			                       program_flavor, light_r, light_g, light_b);
			++i;
		}

		draw_batches.emplace_back(DrawBatch{offset, static_cast<int>(vertices_.size() - offset),
		                                    template_args.texture, template_args.mask,
		                                    template_args.blend_mode});
		offset = vertices_.size();
	}

	vertex_buffer_->update(vertices_.data(), vertices_.size() * sizeof(PerVertexData));

	auto& command_buffer = Rhi::command_buffer();
	command_buffer.bind_vertex_buffer(vertex_buffer_.get());
	for (const auto& draw_arg : draw_batches) {
		const Variant& variant = variant_for(draw_arg.blend_mode);
		variant.descriptor_set->set_texture(0, draw_arg.texture.texture);
		variant.descriptor_set->set_texture(1, draw_arg.mask.texture);
		command_buffer.bind_pipeline(variant.pipeline.get());
		command_buffer.bind_descriptor_set(variant.descriptor_set.get());
		command_buffer.draw(draw_arg.offset, draw_arg.count);
	}
}

void BlitProgram::draw(const Rectf& gl_dest_rect,
                       const float z_value,
                       const BlitData& texture,
                       const BlitData& mask,
                       const RGBAColor& blend,
                       const BlendMode& blend_mode,
                       const Vector3f& light) {
	draw({Arguments{gl_dest_rect, z_value, texture, mask, blend, blend_mode,
	                has_texture(mask) ? BlitMode::kBlendedWithMask : BlitMode::kDirect, light}});
}

void BlitProgram::draw_monochrome(const Rectf& dest_rect,
                                  const float z_value,
                                  const BlitData& texture,
                                  const RGBAColor& blend) {
	draw({Arguments{dest_rect, z_value, texture, BlitData{nullptr, 0, 0, 0, Rectf()}, blend,
	                BlendMode::UseAlpha, BlitMode::kMonochrome}});
}

// static
BlitProgram& BlitProgram::instance() {
	static BlitProgram blit_program;
	return blit_program;
}
