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
#include "graphic/gl/initialize.h"
#include "graphic/gl/utils.h"
#include "graphic/rhi/gl/gl_device.h"

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
	if (Gl::backend() == Gl::Backend::kOpenGLCore) {
		Rhi::PipelineDescriptor desc;
		desc.program_name = "blit";
		desc.vertex_layout.stride = sizeof(PerVertexData);
		desc.vertex_layout.attributes = {
		   {"attr_blend", Rhi::VertexFormat::kVec4, offsetof(PerVertexData, blend_r)},
		   {"attr_mask_texture_position", Rhi::VertexFormat::kVec2,
		    offsetof(PerVertexData, mask_texture_x)},
		   {"attr_position", Rhi::VertexFormat::kVec3, offsetof(PerVertexData, gl_x)},
		   {"attr_texture_position", Rhi::VertexFormat::kVec2,
		    offsetof(PerVertexData, texture_x)},
		   {"attr_program_flavor", Rhi::VertexFormat::kFloat, offsetof(PerVertexData, program_flavor)},
		};
		desc.topology = Rhi::PrimitiveTopology::kTriangleList;
		desc.depth = {true, true, Rhi::CompareOp::kLessOrEqual};
		desc.samplers = {{0, "u_texture"}, {1, "u_mask"}};

		desc.blend = Rhi::kBlendAlpha;
		pipeline_alpha_ = Rhi::device().create_pipeline(desc);
		desc.blend = Rhi::kBlendOpaque;
		pipeline_opaque_ = Rhi::device().create_pipeline(desc);

		descriptor_set_ = Rhi::device().create_descriptor_set(*pipeline_alpha_);
		vertex_buffer_ = Rhi::device().create_buffer(sizeof(PerVertexData), Rhi::BufferUsage::kVertex);
		return;
	}

	gl_program_.build("blit");

	u_texture_ = glGetUniformLocation(gl_program_.object(), "u_texture");
	u_mask_ = glGetUniformLocation(gl_program_.object(), "u_mask");

	gl_array_buffer_.bind();
	vao_.define_attributes({
	   {gl_program_.attribute_location("attr_blend"), 4, sizeof(PerVertexData),
	    offsetof(PerVertexData, blend_r)},
	   {gl_program_.attribute_location("attr_mask_texture_position"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, mask_texture_x)},
	   {gl_program_.attribute_location("attr_position"), 3, sizeof(PerVertexData),
	    offsetof(PerVertexData, gl_x)},
	   {gl_program_.attribute_location("attr_texture_position"), 2, sizeof(PerVertexData),
	    offsetof(PerVertexData, texture_x)},
	   {gl_program_.attribute_location("attr_program_flavor"), 1, sizeof(PerVertexData),
	    offsetof(PerVertexData, program_flavor)},
	});
}

Rhi::Pipeline* BlitProgram::pipeline_for(const BlendMode blend_mode) const {
	// blit uses ordinary alpha blending for Default/UseAlpha and overwrites the
	// destination for Copy (Claude/RHI_INTERFACE.md §4).
	return blend_mode == BlendMode::Copy ? pipeline_opaque_.get() : pipeline_alpha_.get();
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
			    current_args.texture.texture_id != template_args.texture.texture_id ||
			    (current_args.mask.texture_id != 0 &&
			     current_args.mask.texture_id != template_args.mask.texture_id)) {
				break;
			}

			const float blend_r = current_args.blend.r / 255.;
			const float blend_g = current_args.blend.g / 255.;
			const float blend_b = current_args.blend.b / 255.;
			const float blend_a = current_args.blend.a / 255.;

			const Rectf texture_rect = to_gl_texture(current_args.texture);
			const Rectf mask_rect = to_gl_texture(current_args.mask);
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
			                       mask_rect.y, blend_r, blend_g, blend_b, blend_a, program_flavor);

			vertices_.emplace_back(current_args.destination_rect.x + current_args.destination_rect.w,
			                       current_args.destination_rect.y, current_args.z_value,
			                       texture_rect.x + texture_rect.w, texture_rect.y,
			                       mask_rect.x + mask_rect.w, mask_rect.y, blend_r, blend_g, blend_b,
			                       blend_a, program_flavor);

			vertices_.emplace_back(
			   current_args.destination_rect.x,
			   current_args.destination_rect.y + current_args.destination_rect.h, current_args.z_value,
			   texture_rect.x, texture_rect.y + texture_rect.h, mask_rect.x, mask_rect.y + mask_rect.h,
			   blend_r, blend_g, blend_b, blend_a, program_flavor);

			vertices_.emplace_back(vertices_.at(vertices_.size() - 2));
			vertices_.emplace_back(vertices_.at(vertices_.size() - 2));

			vertices_.emplace_back(current_args.destination_rect.x + current_args.destination_rect.w,
			                       current_args.destination_rect.y + current_args.destination_rect.h,
			                       current_args.z_value, texture_rect.x + texture_rect.w,
			                       texture_rect.y + texture_rect.h, mask_rect.x + mask_rect.w,
			                       mask_rect.y + mask_rect.h, blend_r, blend_g, blend_b, blend_a,
			                       program_flavor);
			++i;
		}

		draw_batches.emplace_back(DrawBatch{offset, static_cast<int>(vertices_.size() - offset),
		                                    template_args.texture, template_args.mask,
		                                    template_args.blend_mode});
		offset = vertices_.size();
	}

	if (Gl::backend() == Gl::Backend::kOpenGLCore) {
		vertex_buffer_->update(vertices_.data(), vertices_.size() * sizeof(PerVertexData));

		auto& command_buffer = Rhi::command_buffer();
		command_buffer.bind_vertex_buffer(vertex_buffer_.get());
		for (const auto& draw_arg : draw_batches) {
			descriptor_set_->set_texture(0, draw_arg.texture.texture);
			descriptor_set_->set_texture(1, draw_arg.mask.texture);
			command_buffer.bind_pipeline(pipeline_for(draw_arg.blend_mode));
			command_buffer.bind_descriptor_set(descriptor_set_.get());
			command_buffer.draw(draw_arg.offset, draw_arg.count);
		}
		return;
	}

	glUseProgram(gl_program_.object());

	auto& gl_state = Gl::State::instance();

	gl_array_buffer_.bind();
	vao_.bind();

	glUniform1i(u_texture_, 0);
	glUniform1i(u_mask_, 1);

	gl_array_buffer_.update(vertices_);

	// Now do the draw calls.
	for (const auto& draw_arg : draw_batches) {
		gl_state.bind(GL_TEXTURE0, draw_arg.texture.texture_id);
		gl_state.bind(GL_TEXTURE1, draw_arg.mask.texture_id);

		if (draw_arg.blend_mode == BlendMode::Copy) {
			glBlendFunc(GL_ONE, GL_ZERO);
		}
		glDrawArrays(GL_TRIANGLES, draw_arg.offset, draw_arg.count);

		if (draw_arg.blend_mode == BlendMode::Copy) {
			glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
		}
	}
}

void BlitProgram::draw(const Rectf& gl_dest_rect,
                       const float z_value,
                       const BlitData& texture,
                       const BlitData& mask,
                       const RGBAColor& blend,
                       const BlendMode& blend_mode) {
	draw({Arguments{gl_dest_rect, z_value, texture, mask, blend, blend_mode,
	                mask.texture_id != 0 ? BlitMode::kBlendedWithMask : BlitMode::kDirect}});
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
