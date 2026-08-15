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

#include <map>
#include <set>
#include <string>

#include <base/test.h>

#include "graphic/rhi/pipeline_catalog.h"

TESTSUITE_START(pipeline_catalog)

// The catalog is the single source of truth for the renderer's twelve
// pipelines (renderer modernization plan, WP-14): the eight programs times
// their blend states. These invariants are what the Vulkan backend's eager
// pipeline build depends on, so a broken catalog must fail the unit tests
// rather than a vulkan boot.
TESTCASE(catalog_shape) {
	const std::vector<Rhi::PipelineDescriptor>& catalog = Rhi::pipeline_catalog();
	check_equal(catalog.size(), size_t(12));

	// Eight unique program names.
	std::set<std::string> program_names;
	for (const Rhi::PipelineDescriptor& entry : catalog) {
		program_names.insert(entry.program_name);
	}
	check_equal(program_names.size(), size_t(8));

	// Unique (program, blend) keys.
	std::set<std::string> keys;
	for (const Rhi::PipelineDescriptor& entry : catalog) {
		const std::string key = entry.program_name + "/" +
		                        std::to_string(static_cast<int>(entry.blend.src_factor)) + "/" +
		                        std::to_string(static_cast<int>(entry.blend.dst_factor)) + "/" +
		                        std::to_string(static_cast<int>(entry.blend.op));
		check_equal(true, keys.insert(key).second);
	}

	// Every attribute lies inside the stride, no duplicate attribute names,
	// and every program has at least one attribute.
	for (const Rhi::PipelineDescriptor& entry : catalog) {
		check_equal(true, entry.vertex_layout.stride > 0u);
		check_equal(true, !entry.vertex_layout.attributes.empty());
		std::set<std::string> attribute_names;
		for (const Rhi::VertexAttribute& attribute : entry.vertex_layout.attributes) {
			check_equal(true, attribute.offset < entry.vertex_layout.stride);
			check_equal(true, attribute_names.insert(attribute.name).second);
		}
	}

	// Every entry carries the renderer's single depth state.
	for (const Rhi::PipelineDescriptor& entry : catalog) {
		check_equal(true, entry.depth.test_enabled);
		check_equal(true, entry.depth.write_enabled);
		check_equal(static_cast<int>(entry.depth.compare_op),
		            static_cast<int>(Rhi::CompareOp::kLessOrEqual));
	}

	// fill_rect is the one program with all four blend states; every other
	// program has exactly one, except blit with its alpha/opaque pair.
	std::map<std::string, int> variants_per_program;
	for (const Rhi::PipelineDescriptor& entry : catalog) {
		++variants_per_program[entry.program_name];
	}
	check_equal(variants_per_program.at("fill_rect"), 4);
	check_equal(variants_per_program.at("blit"), 2);
	for (const auto& pair : variants_per_program) {
		if (pair.first != "fill_rect" && pair.first != "blit") {
			check_equal(pair.second, 1);
		}
	}
}

TESTCASE(catalog_lookup) {
	// Lookup by (program, blend) works for every entry and throws for an
	// unknown combination.
	for (const Rhi::PipelineDescriptor& entry : Rhi::pipeline_catalog()) {
		const Rhi::PipelineDescriptor& found =
		   Rhi::pipeline_catalog_entry(entry.program_name, entry.blend);
		check_equal(found.program_name, entry.program_name);
		check_equal(found.vertex_layout.stride, entry.vertex_layout.stride);
	}
	check_error(WException, "no catalog entry",
	            []() { Rhi::pipeline_catalog_entry("terrain", Rhi::kBlendAlpha); });
}

TESTCASE(verify_vertex_layout) {
	const Rhi::PipelineDescriptor terrain =
	   Rhi::pipeline_catalog_entry("terrain", Rhi::kBlendOpaque);

	// A matching layout passes.
	Rhi::verify_vertex_layout(
	   "terrain", terrain.vertex_layout, 28,
	   {{"attr_brightness", Rhi::VertexFormat::kFloat, 8},
	    {"attr_position", Rhi::VertexFormat::kVec2, 0},
	    {"attr_texture_offset", Rhi::VertexFormat::kVec2, 20},
	    {"attr_texture_position", Rhi::VertexFormat::kVec2, 12}});

	// A drifted stride throws.
	check_error(WException, "Vertex layout drift", [&terrain]() {
		Rhi::verify_vertex_layout("terrain", terrain.vertex_layout, 32,
		                          {{"attr_brightness", Rhi::VertexFormat::kFloat, 8},
		                           {"attr_position", Rhi::VertexFormat::kVec2, 0},
		                           {"attr_texture_offset", Rhi::VertexFormat::kVec2, 20},
		                           {"attr_texture_position", Rhi::VertexFormat::kVec2, 12}});
	});

	// A drifted offset throws.
	check_error(WException, "Vertex layout drift", [&terrain]() {
		Rhi::verify_vertex_layout("terrain", terrain.vertex_layout, 28,
		                          {{"attr_brightness", Rhi::VertexFormat::kFloat, 8},
		                           {"attr_position", Rhi::VertexFormat::kVec2, 0},
		                           {"attr_texture_offset", Rhi::VertexFormat::kVec2, 20},
		                           {"attr_texture_position", Rhi::VertexFormat::kVec2, 16}});
	});

	// A dropped attribute throws.
	check_error(WException, "Vertex layout drift", [&terrain]() {
		Rhi::verify_vertex_layout("terrain", terrain.vertex_layout, 28,
		                          {{"attr_brightness", Rhi::VertexFormat::kFloat, 8},
		                           {"attr_position", Rhi::VertexFormat::kVec2, 0},
		                           {"attr_texture_offset", Rhi::VertexFormat::kVec2, 20}});
	});
}

TESTSUITE_END()
