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

#include "graphic/gl/water_program.h"

#include <algorithm>
#include <cmath>

#include "base/log.h"
#include "graphic/gl/terrain_noise.h"
#include "graphic/rhi/device.h"

namespace {

/* How many rebuilds one timing report covers. kMaxFPS caps the frame rate and
 * the default build is Debug + ASan, so the frame time can say nothing useful
 * about the chamfer; the rebuild is timed directly instead (WATER.md WP-3).
 * Reported over a window rather than per frame, so the log stays readable.
 */
constexpr int kTimingWindow = 60;

/// One contour line per field width of |distance|, so the contour count is a
/// direct read-out of the ramp in the unit WP-7 will tune it in.
constexpr float kContourSpacing = 1.0f;

/// Half-width of the yellow zero-crossing band, in field widths. One cell is
/// 0.5, so this marks the waterline without swallowing the first contour.
constexpr float kZeroBand = 0.1f;

}  // namespace

WaterProgram::WaterProgram() {
	Rhi::PipelineDescriptor desc;
	desc.program_name = "water";
	desc.vertex_layout.stride = sizeof(PerVertexData);
	desc.vertex_layout.attributes = {
	   {"attr_position", Rhi::VertexFormat::kVec2, offsetof(PerVertexData, gl_x)},
	   {"attr_texture_position", Rhi::VertexFormat::kVec2, offsetof(PerVertexData, texture_x)},
	   {"attr_brightness", Rhi::VertexFormat::kFloat, offsetof(PerVertexData, brightness)},
	};
	desc.topology = Rhi::PrimitiveTopology::kTriangleList;
	desc.blend = Rhi::kBlendAlpha;
	desc.depth = {true, true, Rhi::CompareOp::kLessOrEqual};
	desc.samplers = {{0, "u_shore_distance"}};
	desc.uniform_block = Rhi::UniformBlockBinding{0, "per_program_state", sizeof(WaterProgramState)};
	pipeline_ = Rhi::device().create_pipeline(desc);
	descriptor_set_ = Rhi::device().create_descriptor_set(*pipeline_);
	vertex_buffer_ = Rhi::device().create_buffer(0, Rhi::BufferUsage::kVertex);
	uniform_rhi_buffer_ =
	   Rhi::device().create_buffer(sizeof(WaterProgramState), Rhi::BufferUsage::kUniform);
}

void WaterProgram::add_vertex(const FieldsToDraw::Field& field) {
	vertices_.emplace_back();
	PerVertexData& back = vertices_.back();
	back.gl_x = field.gl_position.x;
	back.gl_y = field.gl_position.y;
	back.texture_x = field.texture_coords.x;
	back.texture_y = field.texture_coords.y;
	back.brightness = field.brightness;
}

void WaterProgram::report_rebuild_cost(const ShoreDistanceField& field, const bool debug) {
	/* Only reachable at all under --water-debug: the pass itself runs
	 * unconditionally since WP-6, but this instrument is a one-off measurement
	 * against WP-3's baseline (WATER.md WP-11), not shipping telemetry -- left
	 * running it would log every kTimingWindow rebuilds in normal gameplay.
	 */
	if (!debug) {
		return;
	}

	/* A grid size change means the zoom or the window changed, which is exactly
	 * the axis the cost measurement varies, so each new size reports its first
	 * rebuild on its own before settling into windowed means. Without that, a
	 * capture that renders fewer frames than one window says nothing about the
	 * view it was actually asked for.
	 */
	if (field.width() != timing_width_ || field.height() != timing_height_) {
		timing_width_ = field.width();
		timing_height_ = field.height();
		timing_sum_us_ = 0;
		timing_max_us_ = 0;
		timing_count_ = 0;
		timing_window_ = 1;
	}

	timing_sum_us_ += field.last_rebuild_us();
	timing_max_us_ = std::max(timing_max_us_, field.last_rebuild_us());
	++timing_count_;
	if (timing_count_ >= timing_window_) {
		log_info("shore distance field: %dx%d cells, %d rebuild(s), mean %lld us, max %lld us\n",
		         timing_width_, timing_height_, timing_count_,
		         static_cast<long long>(timing_sum_us_ / timing_count_),
		         static_cast<long long>(timing_max_us_));
		timing_sum_us_ = 0;
		timing_max_us_ = 0;
		timing_count_ = 0;
		timing_window_ = kTimingWindow;
	}
}

void WaterProgram::upload_distance_texture(const ShoreDistanceField& field) {
	const uint32_t width = static_cast<uint32_t>(field.width());
	const uint32_t height = static_cast<uint32_t>(field.height());
	if (distance_texture_ == nullptr || distance_texture_->width() != width ||
	    distance_texture_->height() != height) {
		Rhi::TextureDescriptor desc;
		desc.width = width;
		desc.height = height;
		desc.format = Rhi::TextureFormat::kR16F;
		desc.wrap = Rhi::TextureWrap::kClampToEdge;
		desc.filter = Rhi::TextureFilter::kLinear;
		distance_texture_ = Rhi::device().create_texture(desc);
	}
	// Plain floats: the backend hands GL_FLOAT to a GL_R16F texture, so the
	// driver does the half conversion and we never pack halves ourselves.
	distance_texture_->upload(field.values().data());
}

void WaterProgram::draw(const FieldsToDraw& fields_to_draw,
                        const ShoreDistanceField& shore_distance_field,
                        const float z_value,
                        const uint32_t gametime,
                        const bool debug) {
	report_rebuild_cost(shore_distance_field, debug);
	upload_distance_texture(shore_distance_field);

	/* The same triangle stream and winding as TerrainProgram::draw. Both signs
	 * of the field are drawn, water and land alike, which is the whole point of
	 * the chosen visualisation: the land side is where the sign and the wet-sand
	 * band of WP-12 have to be judged.
	 */
	vertices_.clear();
	vertices_.reserve(fields_to_draw.size() * 3);
	for (size_t current_index = 0; current_index < fields_to_draw.size(); ++current_index) {
		const FieldsToDraw::Field& field = fields_to_draw.at(current_index);
		if (field.brn_index == FieldsToDraw::kInvalidIndex) {
			continue;
		}
		if (field.rn_index != FieldsToDraw::kInvalidIndex) {
			add_vertex(fields_to_draw.at(current_index));
			add_vertex(fields_to_draw.at(field.brn_index));
			add_vertex(fields_to_draw.at(field.rn_index));
		}
		if (field.bln_index != FieldsToDraw::kInvalidIndex) {
			add_vertex(fields_to_draw.at(current_index));
			add_vertex(fields_to_draw.at(field.bln_index));
			add_vertex(fields_to_draw.at(field.brn_index));
		}
	}
	vertex_buffer_->update(vertices_.data(), vertices_.size() * sizeof(PerVertexData));

	// Cloud shadows animate on the deterministic simulation clock; the wrap bounds the value's
	// magnitude on long-running games, same as terrain_program.cc/dither_program.cc.
	const float time = std::fmod(static_cast<float>(gametime) / 1000.0f, kCloudTimeWrapPeriod);

	WaterProgramState state{};
	state.z_value = z_value;
	state.max_distance = ShoreDistanceField::kMaxShoreDistance;
	state.contour_spacing = kContourSpacing;
	state.zero_band = kZeroBand;
	state.time = time;
	state.cloud_amplitude = cloud_amplitude_;
	state.debug = debug ? 1.f : 0.f;
	state.grid_x = static_cast<float>(shore_distance_field.cx0());
	state.grid_y = static_cast<float>(shore_distance_field.cy0());
	state.inv_grid_width = 1.f / static_cast<float>(shore_distance_field.width());
	state.inv_grid_height = 1.f / static_cast<float>(shore_distance_field.height());
	uniform_rhi_buffer_->update(&state, sizeof(state));

	descriptor_set_->set_texture(0, distance_texture_.get());
	descriptor_set_->set_uniform_buffer(0, uniform_rhi_buffer_.get(), 0, sizeof(state));

	auto& command_buffer = Rhi::command_buffer();
	// A no-op on the GL backend today, but it is the contract the Vulkan
	// backend will need: the texture was just written and is about to be read.
	command_buffer.transition(distance_texture_.get(), Rhi::TextureLayout::kShaderReadOnly);
	command_buffer.bind_pipeline(pipeline_.get());
	command_buffer.bind_descriptor_set(descriptor_set_.get());
	command_buffer.bind_vertex_buffer(vertex_buffer_.get());
	command_buffer.draw(0, vertices_.size());
}
