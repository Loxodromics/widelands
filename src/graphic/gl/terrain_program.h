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

#ifndef WL_GRAPHIC_GL_TERRAIN_PROGRAM_H
#define WL_GRAPHIC_GL_TERRAIN_PROGRAM_H

#include <memory>

#include "base/vector.h"
#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/utils.h"
#include "graphic/rhi/rhi.h"
#include "logic/map_objects/description_maintainer.h"
#include "logic/map_objects/world/terrain_description.h"

class TerrainProgram {
public:
	// Compiles the program. Throws on errors.
	TerrainProgram();

	// Draws the terrain.
	void draw(uint32_t gametime,
	          const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
	          const FieldsToDraw& fields_to_draw,
	          float z_value,
	          const Widelands::Player*);

	// Sets the terrain-noise strength multiplier (0 disables, 1 is the default
	// look). Backlog item 2 / WP-8 of the renderer modernization plan.
	void set_noise_strength(float strength) {
		noise_strength_ = strength;
	}

private:
	struct PerVertexData {
		float gl_x;
		float gl_y;
		float brightness;
		float texture_x;
		float texture_y;
		float texture_offset_x;
		float texture_offset_y;
	};
	static_assert(sizeof(PerVertexData) == 28, "Wrong padding.");

	void gl_draw(const BlitData& blit_data, float texture_w, float texture_h, float z_value);

	// Adds a vertex to the end of vertices with data from 'field' and 'texture_coordinates'.
	void add_vertex(const FieldsToDraw::Field& field, const Vector2f& texture_offset);

	// The program used for drawing the terrain.
	Gl::Program gl_program_;

	// The buffer that will contain 'vertices_' for rendering.
	Gl::Buffer<PerVertexData> gl_array_buffer_;

	// The vertex array object capturing the attribute layout of this program.
	Gl::VertexArray vao_;

	// The uniform buffer carrying the per-program scalars (z-value, texture
	// dimensions, noise amplitudes) on the core backend.
	Gl::UniformBuffer uniform_buffer_;

	// Uniforms (the legacy 2.1 path keeps loose glUniform* calls; the core path
	// reads these from the uniform block instead).
	GLint u_terrain_texture_;
	GLint u_texture_dimensions_;
	GLint u_z_value_;
	GLint u_value_amplitude_;
	GLint u_tint_amplitude_;
	GLint u_warp_amplitude_;

	// RHI resources for the core path (the legacy members above are unused
	// there). terrain reads one texture (u_terrain_texture) and the full
	// per_program_state block.
	std::unique_ptr<Rhi::Pipeline> pipeline_;
	std::unique_ptr<Rhi::DescriptorSet> descriptor_set_;
	std::unique_ptr<Rhi::Buffer> vertex_buffer_;
	std::unique_ptr<Rhi::Buffer> uniform_rhi_buffer_;

	// The terrain-noise strength multiplier, see set_noise_strength().
	float noise_strength_ = 1.0f;

	// Objects below are kept around to avoid memory allocations on each frame.
	// They could theoretically also be recreated.
	std::vector<PerVertexData> vertices_;

	DISALLOW_COPY_AND_ASSIGN(TerrainProgram);
};

#endif  // end of include guard: WL_GRAPHIC_GL_TERRAIN_PROGRAM_H
