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

#ifndef WL_GRAPHIC_GL_BLIT_PROGRAM_H
#define WL_GRAPHIC_GL_BLIT_PROGRAM_H

#include <memory>

#include "base/macros.h"
#include "base/rect.h"
#include "base/vector.h"
#include "graphic/blend_mode.h"
#include "graphic/blit_mode.h"
#include "graphic/color.h"
#include "graphic/gl/blit_data.h"
#include "graphic/gl/system_headers.h"
#include "graphic/gl/utils.h"
#include "graphic/rhi/rhi.h"

// Blits images. Can blend them with player color or make them monochrome.
class BlitProgram {
public:
	struct Arguments {
		Rectf destination_rect;
		float z_value;
		BlitData texture;
		BlitData mask;
		RGBAColor blend;
		BlendMode blend_mode;
		BlitMode blit_mode;
		// Field lighting (V3, Claude/VISUAL_FIDELITY_RANKED.md §4.3), white
		// unless the caller is drawing a map-object sprite under a
		// RenderTarget::LightScope.
		Vector3f light = Vector3f(1.f, 1.f, 1.f);
	};

	// Returns the (singleton) instance of this class.
	static BlitProgram& instance();
	~BlitProgram() = default;

	// Draws the rectangle 'gl_src_rect' from the texture with the name
	// 'gl_texture_image' to 'gl_dest_rect' in the currently bound framebuffer. All
	// coordinates are in the OpenGL frame. The 'texture_mask' is used to selectively apply
	// the 'blend'. This is used for blitting player colored images.
	void draw(const Rectf& gl_dest_rect,
	          float z_value,
	          const BlitData& texture,
	          const BlitData& mask,
	          const RGBAColor& blend,
	          const BlendMode& blend_mode,
	          const Vector3f& light = Vector3f(1.f, 1.f, 1.f));

	// Draws the rectangle 'gl_src_rect' from the texture with the name
	// 'texture' to 'gl_dest_rect' in the currently bound framebuffer. All
	// coordinates are in the OpenGL frame. The image is first converted to
	// luminance, then all values are multiplied with blend.
	void draw_monochrome(const Rectf& gl_dest_rect,
	                     float z_value,
	                     const BlitData& texture,
	                     const RGBAColor& blend);

	// Draws a bunch of items at once.
	void draw(const std::vector<Arguments>& arguments);

private:
	BlitProgram();

	struct PerVertexData {
		PerVertexData(float init_gl_x,
		              float init_gl_y,
		              float init_gl_z,
		              float init_texture_x,
		              float init_texture_y,
		              float init_mask_texture_x,
		              float init_mask_texture_y,
		              float init_blend_r,
		              float init_blend_g,
		              float init_blend_b,
		              float init_blend_a,
		              float init_program_flavor,
		              float init_light_r,
		              float init_light_g,
		              float init_light_b)
		   : gl_x(init_gl_x),
		     gl_y(init_gl_y),
		     gl_z(init_gl_z),
		     texture_x(init_texture_x),
		     texture_y(init_texture_y),
		     mask_texture_x(init_mask_texture_x),
		     mask_texture_y(init_mask_texture_y),
		     blend_r(init_blend_r),
		     blend_g(init_blend_g),
		     blend_b(init_blend_b),
		     blend_a(init_blend_a),
		     program_flavor(init_program_flavor),
		     light_r(init_light_r),
		     light_g(init_light_g),
		     light_b(init_light_b) {
		}

		float gl_x, gl_y, gl_z;
		float texture_x, texture_y;
		float mask_texture_x, mask_texture_y;
		float blend_r, blend_g, blend_b, blend_a;
		float program_flavor;
		float light_r, light_g, light_b;
	};
	static_assert(sizeof(PerVertexData) == 60, "Wrong padding.");

	// The buffer that will contain the quad for rendering.
	Gl::Buffer<PerVertexData> gl_array_buffer_;

	// The vertex array object capturing the attribute layout of this program.
	Gl::VertexArray vao_;

	// The program.
	Gl::Program gl_program_;

	// Uniforms.
	GLint u_texture_;
	GLint u_mask_;

	// RHI resources for the core path (the legacy members above are unused
	// there). blit needs two pipelines (alpha and opaque) for its two textures
	// (u_texture, u_mask); there is no uniform block. A descriptor set is
	// created for a specific pipeline (C7), so each pipeline is paired with its
	// own set in one object: selecting a pipeline and selecting a set are then
	// a single decision that cannot drift apart.
	struct Variant {
		std::unique_ptr<Rhi::Pipeline> pipeline;
		std::unique_ptr<Rhi::DescriptorSet> descriptor_set;
	};
	Variant alpha_;
	Variant opaque_;
	std::unique_ptr<Rhi::Buffer> vertex_buffer_;

	// The pipeline and its descriptor set to use for 'blend_mode' on the core
	// path.
	const Variant& variant_for(BlendMode blend_mode) const;

	// Cached for efficiency.
	std::vector<PerVertexData> vertices_;

	DISALLOW_COPY_AND_ASSIGN(BlitProgram);
};

#endif  // end of include guard: WL_GRAPHIC_GL_BLIT_PROGRAM_H
