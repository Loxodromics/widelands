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
#include <vector>

#include "base/vector.h"
#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/seabed.h"
#include "graphic/gl/terrain_noise.h"
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

	// Sets the cloud-shadow amplitude multiplier (0 disables). Driven by the
	// cloud_shadows config option (wlapplication.cc); see terrain_noise.h.
	void set_cloud_amplitude(float amplitude) {
		cloud_amplitude_ = amplitude;
	}

private:
	struct PerVertexData {
		float gl_x;
		float gl_y;
		float brightness;
		float normal_x;
		float normal_y;
		float normal_z;
		float texture_x;
		float texture_y;
		float texture_offset_x;
		float texture_offset_y;
	};
	static_assert(sizeof(PerVertexData) == 40, "Wrong padding.");

	void
	gl_draw(const BlitData& blit_data, float texture_w, float texture_h, float z_value, float time);

	// Adds a vertex to the end of vertices with data from 'field' and 'texture_coordinates'.
	void add_vertex(const FieldsToDraw::Field& field, const Vector2f& texture_offset);

	// RHI resources. terrain reads one texture (u_terrain_texture) and the
	// full per_program_state block.
	std::unique_ptr<Rhi::Pipeline> pipeline_;
	std::unique_ptr<Rhi::DescriptorSet> descriptor_set_;
	std::unique_ptr<Rhi::Buffer> vertex_buffer_;
	std::unique_ptr<Rhi::Buffer> uniform_rhi_buffer_;

	// The terrain-noise strength multiplier, see set_noise_strength().
	float noise_strength_ = 1.0f;

	// The cloud-shadow amplitude multiplier, see set_cloud_amplitude().
	float cloud_amplitude_ = kCloudShadowAmplitude;

	// Objects below are kept around to avoid memory allocations on each frame.
	// They could theoretically also be recreated.
	std::vector<PerVertexData> vertices_;

	// The seabed substitution table (Claude/WATER.md WP-6), rebuilt once per draw() by
	// resolve_seabed_terrains() (graphic/gl/seabed.h).
	std::vector<Widelands::DescriptionIndex> seabed_terrains_;

	DISALLOW_COPY_AND_ASSIGN(TerrainProgram);
};

#endif  // end of include guard: WL_GRAPHIC_GL_TERRAIN_PROGRAM_H
