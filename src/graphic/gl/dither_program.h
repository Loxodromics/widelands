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

	// Sets the terrain-noise strength multiplier (0 disables, 1 is the default
	// look). Backlog item 2 / WP-8 of the renderer modernization plan.
	void set_noise_strength(float strength) {
		noise_strength_ = strength;
	}

private:
	/* A small set of terrains, used both for the terrains incident to one field
	 * vertex (at most 6, the triangles that meet there) and for the overlay
	 * candidates of one base triangle. The latter bounds the capacity: the
	 * triangles touching any of a triangle's three vertices number 12 in the
	 * lattice, so 12 distinct terrains is a hard ceiling, not a guess. Inline,
	 * so collecting these per frame allocates nothing.
	 */
	struct TerrainSet {
		static constexpr uint8_t kCapacity = 12;

		void add(Widelands::DescriptionIndex terrain);
		[[nodiscard]] bool contains(Widelands::DescriptionIndex terrain) const;

		Widelands::DescriptionIndex terrains[kCapacity];
		uint8_t count = 0;
	};

	// One of the two terrain triangles of a field: the indices of its three
	// field vertices in 'fields_to_draw', and the terrain it is painted with.
	struct BaseTriangle {
		int vertex[3];
		Widelands::DescriptionIndex terrain;
	};

	/* Fills vertex_terrains_ with, for every field, the set of terrains meeting
	 * at it. Scatters rather than gathers -- each triangle adds its terrain to
	 * its own three vertices -- so every triangle is visited once and no
	 * top-left neighbour index is needed.
	 */
	void collect_vertex_terrains(const FieldsToDraw& fields_to_draw,
	                             const Widelands::Map* map,
	                             const Widelands::Player* player);

	/* Emits one overlay triangle over 'base' for every terrain that meets any
	 * of its three vertices and dithers over it. The ramp is 1 at a vertex
	 * incident to that terrain and 0 elsewhere; because incidence is a property
	 * of the vertex and not of the triangle, two triangles sharing an edge give
	 * their shared vertices the same value, so the coverage field is continuous
	 * across every edge in the mesh and never terminates on triangle geometry.
	 */
	void add_dithering_triangles(
	   uint32_t gametime,
	   const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
	   const FieldsToDraw& fields_to_draw,
	   const BaseTriangle& base);

	// Adds the 'field' as a vertex to the 'vertices_'. 'terrain' is the overlay
	// terrain, supplying the per-terrain dither amplitude and softness, and
	// 'texture_offset' is its origin in the texture atlas.
	void add_vertex(const FieldsToDraw::Field& field,
	                float dither_ramp,
	                const Widelands::TerrainDescription& terrain,
	                const Vector2f& texture_offset);

	struct PerVertexData {
		float gl_x;
		float gl_y;
		float texture_x;
		float texture_y;
		float brightness;
		float dither_ramp;
		float dither_amplitude;
		float dither_softness;
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
	GLint u_terrain_texture_;
	GLint u_texture_dimensions_;
	GLint u_z_value_;
	GLint u_value_amplitude_;
	GLint u_tint_amplitude_;
	GLint u_warp_amplitude_;

	// RHI resources for the core path (the legacy members above are unused
	// there). dither reads the terrain atlas and the full per_program_state
	// block.
	std::unique_ptr<Rhi::Pipeline> pipeline_;
	std::unique_ptr<Rhi::DescriptorSet> descriptor_set_;
	std::unique_ptr<Rhi::Buffer> vertex_buffer_;
	std::unique_ptr<Rhi::Buffer> uniform_rhi_buffer_;

	// The terrain-noise strength multiplier, see set_noise_strength().
	float noise_strength_ = 1.0f;

	// Objects below are here to avoid memory allocations on each frame, they
	// could theoretically also always be recreated.
	std::vector<PerVertexData> vertices_;

	// Parallel to 'fields_to_draw': the terrains meeting at each field vertex,
	// rebuilt once per draw by collect_vertex_terrains().
	std::vector<TerrainSet> vertex_terrains_;
};

#endif  // end of include guard: WL_GRAPHIC_GL_DITHER_PROGRAM_H
