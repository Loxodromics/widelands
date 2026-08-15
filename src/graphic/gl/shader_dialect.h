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
 */

#ifndef WL_GRAPHIC_GL_SHADER_DIALECT_H
#define WL_GRAPHIC_GL_SHADER_DIALECT_H

#include <cstdint>
#include <functional>
#include <string>
#include <unordered_map>
#include <vector>

namespace Gl {

// Which stage a shader source file feeds. The dialect emitter needs to know
// this because vertex and fragment shaders share the `in`/`out` keywords but
// mean different things by them.
enum class ShaderStage {
	kVertex,
	kFragment,
};

// The GLSL dialects the in-tree preprocessor emits from one authored source.
// WP-6 of the renderer modernization plan (Claude/RENDERER_MODERNIZATION_PLAN.md):
// 120 for the frozen legacy 2.1 path, 330 for the core profile, 300 es for a
// future GLES backend (emitted but not yet consumed, decision 9). The core
// dialect is 330 rather than the plan's "150": `layout(location=N)` (decision 5)
// was introduced in GLSL 3.30, so 150 would need GL_ARB_explicit_attrib_location.
// kVulkan (WP-13) emits GLSL 450 with Vulkan semantics for the build-time
// SPIR-V toolchain (src/graphic/rhi/vulkan/spirv_compiler_main.cc): explicit
// set/binding decorations, explicit locations on every input/output, and the
// clip-space compensation from RHI_INTERFACE.md §2.4.
enum class ShaderDialect {
	kGLSL120,
	kGLSL330,
	kGLSL300es,
	kVulkan,
};

// A (location, name) pair recorded from a `layout(location=N) in T name;`
// declaration. The legacy 120 path feeds these to glBindAttribLocation before
// linking, since GLSL 1.20 has no layout qualifier (decision 5).
struct AttributeBinding {
	int32_t location;
	std::string name;
};

// The result of emitting one shader for a target dialect: the transformed
// source, plus the attribute bindings parsed from the vertex stage (empty for
// fragment shaders).
struct EmittedShader {
	std::string source;
	std::vector<AttributeBinding> attributes;
};

// The name-based binding and location assignments the Vulkan dialect needs
// (renderer modernization plan, WP-13). Built program-wide across both stages,
// because vertex and fragment share one descriptor set and one interface:
// 'samplers' and 'uniform_blocks' map uniform names to their descriptor
// bindings (all in set 0), and 'varyings' maps varying names to interface
// locations — the vertex stage assigns them by declaration order, the fragment
// stage looks them up by name, so a differing declaration order between the
// stages cannot silently swap two varyings (dither.fp vs dither.vp differ).
// Only ShaderDialect::kVulkan consults the maps; emit_dialect throws when a
// declaration it must decorate is missing from them, so a stale map is a loud
// error rather than an un-decorated declaration.
struct VulkanBindings {
	std::unordered_map<std::string, uint32_t> samplers;
	std::unordered_map<std::string, uint32_t> uniform_blocks;
	std::unordered_map<std::string, uint32_t> varyings;
};

// Reads an included shader file by name and returns its content; throws on
// failure. Decoupled from the filesystem so the game (which reads through the
// layered filesystem, g_fs) and the build-time SPIR-V tool (which reads from
// the source tree) share one include expansion.
using IncludeReader = std::function<std::string(const std::string&)>;

// Expands `#include "name"` lines in shader source. GLSL 1.20 has no #include
// of its own, and terrain.fp and dither.fp must compute identical values from
// the same shared code or every terrain border grows a seam. Included files
// are read through 'read_include' and are not rescanned, so a nested include
// is reported rather than silently ignored.
std::string expand_includes(const std::string& source,
                            const std::string& program_name,
                            const IncludeReader& read_include);

// Rewrites an already include-expanded shader source into the given dialect.
// 'source' must be authored in GLSL 330 style: `layout(location=N) in` for
// vertex inputs, `in`/`out` for varyings, `out vec4` for the fragment output,
// and `texture(...)` for sampling. Throws on malformed declarations. The
// 'vulkan_bindings' parameter is consulted only by ShaderDialect::kVulkan.
EmittedShader emit_dialect(const std::string& expanded_source,
                           ShaderStage stage,
                           ShaderDialect dialect,
                           const std::string& program_name,
                           const VulkanBindings& vulkan_bindings = {});

}  // namespace Gl

#endif  // end of include guard: WL_GRAPHIC_GL_SHADER_DIALECT_H
