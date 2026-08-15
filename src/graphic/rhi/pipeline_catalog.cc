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

#include "graphic/rhi/pipeline_catalog.h"

#include "base/wexception.h"

namespace Rhi {

namespace {

// All twelve pipelines share the renderer's depth state (RHI_INTERFACE.md
// §5.1: test on, write on, GL_LEQUAL - the blended pass writes depth too, and
// that must be reproduced rather than "cleaned up").
constexpr DepthState kCatalogDepth{true, true, CompareOp::kLessOrEqual};

PipelineDescriptor make_blit(const BlendState& blend) {
	PipelineDescriptor desc;
	desc.program_name = "blit";
	desc.vertex_layout.stride = 60;
	desc.vertex_layout.attributes = {
	   {"attr_mask_texture_position", VertexFormat::kVec2, 20},
	   {"attr_texture_position", VertexFormat::kVec2, 12},
	   {"attr_position", VertexFormat::kVec3, 0},
	   {"attr_blend", VertexFormat::kVec4, 28},
	   {"attr_program_flavor", VertexFormat::kFloat, 44},
	   {"attr_light", VertexFormat::kVec3, 48},
	};
	desc.topology = PrimitiveTopology::kTriangleList;
	desc.blend = blend;
	desc.depth = kCatalogDepth;
	return desc;
}

PipelineDescriptor make_terrain() {
	PipelineDescriptor desc;
	desc.program_name = "terrain";
	desc.vertex_layout.stride = 40;
	desc.vertex_layout.attributes = {
	   {"attr_brightness", VertexFormat::kFloat, 8},
	   {"attr_normal", VertexFormat::kVec3, 12},
	   {"attr_position", VertexFormat::kVec2, 0},
	   {"attr_texture_offset", VertexFormat::kVec2, 32},
	   {"attr_texture_position", VertexFormat::kVec2, 24},
	};
	desc.topology = PrimitiveTopology::kTriangleList;
	desc.blend = kBlendOpaque;
	desc.depth = kCatalogDepth;
	return desc;
}

PipelineDescriptor make_dither() {
	PipelineDescriptor desc;
	desc.program_name = "dither";
	desc.vertex_layout.stride = 52;
	desc.vertex_layout.attributes = {
	   {"attr_brightness", VertexFormat::kFloat, 16},
	   {"attr_dither_params", VertexFormat::kVec2, 36},
	   {"attr_dither_ramp", VertexFormat::kFloat, 32},
	   {"attr_normal", VertexFormat::kVec3, 20},
	   {"attr_position", VertexFormat::kVec2, 0},
	   {"attr_texture_offset", VertexFormat::kVec2, 44},
	   {"attr_texture_position", VertexFormat::kVec2, 8},
	};
	desc.topology = PrimitiveTopology::kTriangleList;
	desc.blend = kBlendAlpha;
	desc.depth = kCatalogDepth;
	return desc;
}

PipelineDescriptor make_road() {
	PipelineDescriptor desc;
	desc.program_name = "road";
	desc.vertex_layout.stride = 20;
	desc.vertex_layout.attributes = {
	   {"attr_position", VertexFormat::kVec2, 0},
	   {"attr_texture_position", VertexFormat::kVec2, 8},
	   {"attr_brightness", VertexFormat::kFloat, 16},
	};
	desc.topology = PrimitiveTopology::kTriangleList;
	desc.blend = kBlendAlpha;
	desc.depth = kCatalogDepth;
	return desc;
}

PipelineDescriptor make_grid() {
	PipelineDescriptor desc;
	desc.program_name = "grid";
	desc.vertex_layout.stride = 20;
	desc.vertex_layout.attributes = {
	   {"attr_position", VertexFormat::kVec2, 0},
	   {"attr_color", VertexFormat::kVec3, 8},
	};
	desc.topology = PrimitiveTopology::kLineList;
	desc.blend = kBlendAlpha;
	desc.depth = kCatalogDepth;
	return desc;
}

PipelineDescriptor make_workarea() {
	PipelineDescriptor desc;
	desc.program_name = "workarea";
	desc.vertex_layout.stride = 24;
	desc.vertex_layout.attributes = {
	   {"attr_position", VertexFormat::kVec2, 0},
	   {"attr_overlay", VertexFormat::kVec4, 8},
	};
	desc.topology = PrimitiveTopology::kTriangleList;
	desc.blend = kBlendAlpha;
	desc.depth = kCatalogDepth;
	return desc;
}

PipelineDescriptor make_fill_rect(const BlendState& blend) {
	PipelineDescriptor desc;
	desc.program_name = "fill_rect";
	desc.vertex_layout.stride = 28;
	desc.vertex_layout.attributes = {
	   {"attr_position", VertexFormat::kVec3, 0},
	   {"attr_color", VertexFormat::kVec4, 12},
	};
	desc.topology = PrimitiveTopology::kTriangleList;
	desc.blend = blend;
	desc.depth = kCatalogDepth;
	return desc;
}

PipelineDescriptor make_draw_line() {
	PipelineDescriptor desc;
	desc.program_name = "draw_line";
	desc.vertex_layout.stride = 28;
	desc.vertex_layout.attributes = {
	   {"attr_position", VertexFormat::kVec3, 0},
	   {"attr_color", VertexFormat::kVec4, 12},
	};
	desc.topology = PrimitiveTopology::kTriangleList;
	desc.blend = kBlendAlpha;
	desc.depth = kCatalogDepth;
	return desc;
}

}  // namespace

const std::vector<PipelineDescriptor>& pipeline_catalog() {
	static const std::vector<PipelineDescriptor> kCatalog = {
	   // blit batches by (texture, mask, blend) and picks alpha/opaque per
	   // batch (blit_program.cc); fill_rect draws with all four blend states.
	   make_blit(kBlendAlpha),
	   make_blit(kBlendOpaque),
	   make_terrain(),
	   make_dither(),
	   make_road(),
	   make_workarea(),
	   make_grid(),
	   make_fill_rect(kBlendAlpha),
	   make_fill_rect(kBlendAdditive),
	   make_fill_rect(kBlendReverseSubtract),
	   make_fill_rect(kBlendOpaque),
	   make_draw_line(),
	};
	return kCatalog;
}

const PipelineDescriptor&
pipeline_catalog_entry(const std::string& program_name, const BlendState& blend) {
	for (const PipelineDescriptor& entry : pipeline_catalog()) {
		if (entry.program_name == program_name && entry.blend.src_factor == blend.src_factor &&
		    entry.blend.dst_factor == blend.dst_factor && entry.blend.op == blend.op) {
			return entry;
		}
	}
	throw wexception("Rhi::pipeline_catalog_entry: no catalog entry for program '%s' with the "
	                 "requested blend state",
	                 program_name.c_str());
}

void verify_vertex_layout(const std::string& program_name,
                          const VertexLayout& catalog_layout,
                          const uint32_t actual_stride,
                          std::initializer_list<VertexAttribute> actual_attributes) {
	if (catalog_layout.stride != actual_stride) {
		throw wexception(
		   "Vertex layout drift in program '%s': the pipeline catalog says stride %u but the "
		   "vertex struct is %u bytes (update rhi/pipeline_catalog.cc or the struct)",
		   program_name.c_str(), catalog_layout.stride, actual_stride);
	}
	if (catalog_layout.attributes.size() != actual_attributes.size()) {
		throw wexception("Vertex layout drift in program '%s': the pipeline catalog declares "
		                 "%" PRIuS " attributes but the vertex struct check passes %" PRIuS,
		                 program_name.c_str(), catalog_layout.attributes.size(),
		                 actual_attributes.size());
	}
	for (const VertexAttribute& catalog_attribute : catalog_layout.attributes) {
		bool found = false;
		for (const VertexAttribute& actual_attribute : actual_attributes) {
			if (catalog_attribute.name == actual_attribute.name &&
			    catalog_attribute.format == actual_attribute.format &&
			    catalog_attribute.offset == actual_attribute.offset) {
				found = true;
				break;
			}
		}
		if (!found) {
			throw wexception("Vertex layout drift in program '%s': attribute '%s' (format %d, "
			                 "offset %u) does not match the vertex struct (update "
			                 "rhi/pipeline_catalog.cc or the struct)",
			                 program_name.c_str(), catalog_attribute.name.c_str(),
			                 static_cast<int>(catalog_attribute.format), catalog_attribute.offset);
		}
	}
}

}  // namespace Rhi
