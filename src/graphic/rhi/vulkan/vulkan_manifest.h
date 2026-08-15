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

#ifndef WL_GRAPHIC_RHI_VULKAN_VULKAN_MANIFEST_H
#define WL_GRAPHIC_RHI_VULKAN_VULKAN_MANIFEST_H

#include <cstdint>
#include <string>
#include <vector>

// The parsed bindings manifest (data/shaders/vulkan/bindings.json): the
// machine-readable descriptor set / vertex attribute assignments committed
// alongside the SPIR-V (renderer modernization plan, WP-13). It is the single
// source of truth for the Vulkan backend's descriptor layouts and vertex
// input locations, so nothing has to reflect over SPIR-V. The parser is
// hand-rolled for this one fixed, machine-generated format - a general JSON
// library is deliberately not vendored for it.
namespace Rhi {

// One program's uniform block and the shader stages that declare it. The
// stages decide the descriptor set layout's stage flags: terrain/dither
// declare per_program_state in both stages, road/grid/workarea only in the
// vertex stage.
struct ManifestUniformBlock {
	std::string name;
	uint32_t binding = 0;
	bool vertex_stage = false;
	bool fragment_stage = false;
};

// One program's assignments. Samplers are always fragment-stage only (the
// authored shaders declare them nowhere else), so no stage flags are carried
// for them.
struct ManifestProgram {
	// (sampler name, descriptor binding), in declaration order.
	std::vector<std::pair<std::string, uint32_t>> samplers;
	std::vector<ManifestUniformBlock> uniform_blocks;
	// (vertex attribute name, shader location) from the authored
	// layout(location=N) declarations.
	std::vector<std::pair<std::string, uint32_t>> attributes;
};

struct VulkanManifest {
	std::vector<std::pair<std::string, ManifestProgram>> programs;

	// The program with 'name', or nullptr when the manifest has no entry.
	const ManifestProgram* find_program(const std::string& name) const;
};

// Parses the manifest text. Throws wexception on malformed input.
VulkanManifest parse_manifest(const std::string& text);

// Reads and parses data/shaders/vulkan/bindings.json through the layered
// filesystem (g_fs). Throws wexception when the file is missing or malformed.
VulkanManifest load_manifest();

}  // namespace Rhi

#endif  // end of include guard: WL_GRAPHIC_RHI_VULKAN_VULKAN_MANIFEST_H
