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

#ifndef WL_GRAPHIC_GL_FILL_RECT_PROGRAM_H
#define WL_GRAPHIC_GL_FILL_RECT_PROGRAM_H

#include <memory>

#include "base/rect.h"
#include "graphic/blend_mode.h"
#include "graphic/color.h"
#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/utils.h"
#include "graphic/rhi/rhi.h"

class FillRectProgram {
public:
	struct Arguments {
		struct Vertex {
			Vector2f point{Vector2f::zero()};
			float color_r{0.f};
			float color_g{0.f};
			float color_b{0.f};
			float color_a{0.f};
		};

		Vertex triangle[3];
		float z_value{0.f};
		BlendMode blend_mode{BlendMode::UseAlpha};
	};

	// Returns the (singleton) instance of this class.
	static FillRectProgram& instance();

	// Fills a solid rect in 'color'. If blend_mode is BlendMode::UseAlpha, this
	// will brighten the rect, if it is BlendMode::Subtract it darkens it.
	void
	draw(const Rectf& destination_rect, float z_value, const RGBAColor& color, BlendMode blend_mode);

	void draw_height_heat_map_overlays(const FieldsToDraw& fields_to_draw, float z_value);

	void draw(const std::vector<Arguments>& arguments);

	static std::vector<Arguments> make_arguments_for_rect(const Rectf& destination_rect,
	                                                      float z_value,
	                                                      const RGBAColor& color,
	                                                      BlendMode blend_mode);

private:
	FillRectProgram();

	struct PerVertexData {
		PerVertexData(float init_gl_x,
		              float init_gl_y,
		              float init_gl_z,
		              float init_r,
		              float init_g,
		              float init_b,
		              float init_a)
		   : gl_x(init_gl_x),
		     gl_y(init_gl_y),
		     gl_z(init_gl_z),
		     r(init_r),
		     g(init_g),
		     b(init_b),
		     a(init_a) {
		}

		float gl_x, gl_y, gl_z;
		float r, g, b, a;
	};
	static_assert(sizeof(PerVertexData) == 28, "Wrong padding.");

	// This is only kept around so that we do not constantly allocate memory for
	// it.
	std::vector<PerVertexData> vertices_;

	// The buffer that will contain the quad for rendering.
	Gl::Buffer<PerVertexData> gl_array_buffer_;

	// The vertex array object capturing the attribute layout of this program.
	Gl::VertexArray vao_;

	// The program.
	Gl::Program gl_program_;

	// RHI resources for the core path (the legacy members above are unused
	// there). fill_rect interprets each BlendMode differently (see
	// Claude/RHI_INTERFACE.md §4), so it owns one pipeline per blend state.
	std::unique_ptr<Rhi::Pipeline> pipeline_alpha_;
	std::unique_ptr<Rhi::Pipeline> pipeline_additive_;
	std::unique_ptr<Rhi::Pipeline> pipeline_reverse_subtract_;
	std::unique_ptr<Rhi::Pipeline> pipeline_opaque_;
	std::unique_ptr<Rhi::Buffer> vertex_buffer_;

	// The pipeline to use for 'blend_mode' on the core path.
	Rhi::Pipeline* pipeline_for(BlendMode blend_mode) const;

	DISALLOW_COPY_AND_ASSIGN(FillRectProgram);
};

#endif  // end of include guard: WL_GRAPHIC_GL_FILL_RECT_PROGRAM_H
