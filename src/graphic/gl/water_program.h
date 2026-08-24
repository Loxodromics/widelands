/*
 * Copyright (C) 2026 by the Widelands Development Team
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

#ifndef WL_GRAPHIC_GL_WATER_PROGRAM_H
#define WL_GRAPHIC_GL_WATER_PROGRAM_H

#include <cstdint>
#include <memory>
#include <vector>

#include "graphic/gl/fields_to_draw.h"
#include "graphic/gl/shore_distance_field.h"
#include "graphic/gl/terrain_noise.h"
#include "graphic/rhi/rhi.h"
#include "logic/map_objects/description_maintainer.h"
#include "logic/map_objects/world/terrain_description.h"

namespace Widelands {
class Map;
class Player;
}  // namespace Widelands

/* The water pass. At WP-3 it drew the shore distance field only as a false-colour debug overlay
 * over every terrain triangle; WP-6 turns the same pass into the real wash over the seabed the
 * terrain pass now draws for water triangles (graphic/gl/seabed.h), which is why it was built as
 * the water pass rather than as scaffolding to be thrown away. --water-debug
 * (RenderQueue::set_water_debug()) now selects between the two as a uniform, not an enqueue
 * gate: the pass runs unconditionally, since water is an ordinary terrain layer with no on/off
 * switch (D2, Claude/WATER.md).
 *
 * It owns the distance field. TerrainArguments already carries everything the
 * rebuild needs, so program ownership needs no plumbing and costs nothing while
 * the debug flag is off. (WATER.md's WP-3 text put the field on MapView; that
 * moves back at WP-11, when the terrain pass wants the seed payload too.)
 */
class WaterProgram {
public:
	WaterProgram();

	void draw(const Widelands::DescriptionMaintainer<Widelands::TerrainDescription>& terrains,
	          const FieldsToDraw& fields_to_draw,
	          const Widelands::Map& map,
	          const Widelands::Player* player,
	          float z_value,
	          uint32_t gametime,
	          bool debug);

	// Sets the cloud-shadow amplitude multiplier (0 disables). Driven by the
	// cloud_shadows config option (wlapplication.cc); see terrain_noise.h.
	void set_cloud_amplitude(float amplitude) {
		cloud_amplitude_ = amplitude;
	}

private:
	struct PerVertexData {
		float gl_x;
		float gl_y;
		float texture_x;
		float texture_y;
		float brightness;
	};
	static_assert(sizeof(PerVertexData) == 20, "Wrong padding.");

	/* The program's own std140 uniform block. Gl::PerProgramState has no free
	 * slots, and the 16-byte z-value-only block the road/grid/workarea programs
	 * share is too small. Binding point 0 is still fine -- only one program
	 * draws at a time -- and Program::bind_uniform_block asserts this size
	 * against GL_UNIFORM_BLOCK_DATA_SIZE, so a drifted struct fails at startup.
	 *
	 * time/cloud_amplitude/debug were added at WP-6, after max_distance/u_grid (kept: WP-7 needs
	 * max_distance for its ramp, and the debug view still uses it too). They round the scalar
	 * block up from 16 to 32 bytes (7 floats plus one explicit pad) so grid_x stays 16-byte
	 * aligned, the same alignment std140 already gave u_grid for free at the smaller size.
	 */
	struct WaterProgramState {
		float z_value;          // offset 0
		float max_distance;     // offset 4
		float contour_spacing;  // offset 8
		float zero_band;        // offset 12
		float time;             // offset 16
		float cloud_amplitude;  // offset 20
		float debug;            // offset 24
		float padding;          // offset 28
		float grid_x;           // offset 32 (vec4 u_grid)
		float grid_y;           // offset 36
		float inv_grid_width;   // offset 40
		float inv_grid_height;  // offset 44
	};
	static_assert(sizeof(WaterProgramState) == 48, "std140 layout of per_program_state");

	void add_vertex(const FieldsToDraw::Field& field);

	/* Creates or recreates the distance texture when the grid dimensions
	 * change, then uploads the current values. Rhi::Texture::upload() is a
	 * whole-image upload at the size given at creation, so a texture cannot be
	 * resized by uploading to it. The dimensions only change on zoom or window
	 * resize, never on panning.
	 */
	void upload_distance_texture();

	/// Logs the chamfer's own cost, averaged over a window of rebuilds. A no-op unless 'debug'
	/// (--water-debug): shipping gameplay must not carry this instrument's logging cost.
	void report_rebuild_cost(bool debug);

	std::unique_ptr<Rhi::Pipeline> pipeline_;
	std::unique_ptr<Rhi::DescriptorSet> descriptor_set_;
	std::unique_ptr<Rhi::Buffer> vertex_buffer_;
	std::unique_ptr<Rhi::Buffer> uniform_rhi_buffer_;
	std::unique_ptr<Rhi::Texture> distance_texture_;

	ShoreDistanceField field_;

	// The cloud-shadow amplitude multiplier, see set_cloud_amplitude().
	float cloud_amplitude_ = kCloudShadowAmplitude;

	int64_t timing_sum_us_ = 0;
	int64_t timing_max_us_ = 0;
	int timing_count_ = 0;
	int timing_width_ = 0;
	int timing_height_ = 0;
	// The first rebuild at any grid size is reported on its own, so that a
	// capture drawing only a handful of frames still yields a number for the
	// view it was asked for; every window after that covers kTimingWindow.
	int timing_window_ = 1;

	// Kept around to avoid a per-frame allocation.
	std::vector<PerVertexData> vertices_;

	DISALLOW_COPY_AND_ASSIGN(WaterProgram);
};

#endif  // end of include guard: WL_GRAPHIC_GL_WATER_PROGRAM_H
