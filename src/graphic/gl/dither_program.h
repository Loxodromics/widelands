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

#ifndef WL_GRAPHIC_GL_DITHER_PROGRAM_H
#define WL_GRAPHIC_GL_DITHER_PROGRAM_H

#include <memory>

#include "base/vector.h"
#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/utils.h"
#include "graphic/rhi/rhi.h"
#include "logic/map_objects/description_maintainer.h"
#include "logic/map_objects/world/terrain_description.h"

class DitherProgram {
public:
	DitherProgram();
	~DitherProgram() = default;

	// Draws the terrain.
	void draw(uint32_t gametime,
	          const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
	          const FieldsToDraw& fields_to_draw,
	          float z_value,
	          const Widelands::Player*);

	void set_dither_mask(const std::string& filepath);

	// Sets the terrain-noise strength multiplier (0 disables, 1 is the default
	// look). Backlog item 2 / WP-8 of the renderer modernization plan.
	void set_noise_strength(float strength) {
		noise_strength_ = strength;
	}

private:
	enum class TrianglePoint {
		kTopLeft,
		kTopRight,
		kBottomMiddle,
	};

	// Adds the triangle between the indexes (which index 'fields_to_draw') to
	// vertices_ if the my_terrain != other_terrain and the dither_layer()
	// agree.
	void maybe_add_dithering_triangle(
	   uint32_t gametime,
	   const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
	   const FieldsToDraw& fields_to_draw,
	   int idx1,
	   int idx2,
	   int idx3,
	   int my_terrain,
	   int other_terrain);

	// Adds the 'field' as an vertex to the 'vertices_'. The 'order_index'
	// defines which texture position in the dithering texture will be used for
	// this vertex.
	void add_vertex(const FieldsToDraw::Field& field,
	                TrianglePoint triangle_point,
	                const Vector2f& texture_offset);

	struct PerVertexData {
		float gl_x;
		float gl_y;
		float texture_x;
		float texture_y;
		float brightness;
		float dither_texture_x;
		float dither_texture_y;
		float texture_offset_x;
		float texture_offset_y;
	};

	// Call through to GL.
	void gl_draw(const BlitData& blit_data, float texture_w, float texture_h, float z_value);

	// The program used for drawing the terrain.
	Gl::Program gl_program_;

	// The buffer that contains the data to be rendered.
	Gl::Buffer<PerVertexData> gl_array_buffer_;

	// The vertex array object capturing the attribute layout of this program.
	Gl::VertexArray vao_;

	// The uniform buffer carrying the per-program scalars (z-value, texture
	// dimensions, noise amplitudes) on the core backend.
	Gl::UniformBuffer uniform_buffer_;

	// Uniforms (the legacy 2.1 path keeps loose glUniform* calls; the core path
	// reads these from the uniform block instead).
	GLint u_dither_texture_;
	GLint u_terrain_texture_;
	GLint u_texture_dimensions_;
	GLint u_z_value_;
	GLint u_value_amplitude_;
	GLint u_tint_amplitude_;
	GLint u_warp_amplitude_;

	// RHI resources for the core path (the legacy members above are unused
	// there). dither reads two textures (u_dither_texture, u_terrain_texture)
	// and the full per_program_state block.
	std::unique_ptr<Rhi::Pipeline> pipeline_;
	std::unique_ptr<Rhi::DescriptorSet> descriptor_set_;
	std::unique_ptr<Rhi::Buffer> vertex_buffer_;
	std::unique_ptr<Rhi::Buffer> uniform_rhi_buffer_;

	// The texture mask for the dithering step.
	std::unique_ptr<Texture> dither_mask_;

	// The terrain-noise strength multiplier, see set_noise_strength().
	float noise_strength_ = 1.0f;

	// Objects below are here to avoid memory allocations on each frame, they
	// could theoretically also always be recreated.
	std::vector<PerVertexData> vertices_;
};

#endif  // end of include guard: WL_GRAPHIC_GL_DITHER_PROGRAM_H
