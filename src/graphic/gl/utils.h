/*
 * Copyright (C) 2010-2026 by the Widelands Development Team
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

#ifndef WL_GRAPHIC_GL_UTILS_H
#define WL_GRAPHIC_GL_UTILS_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include "base/macros.h"
#include "base/wexception.h"
#include "graphic/gl/system_headers.h"

namespace Gl {

class Shader;

// Returns the name of the 'error'.
const char* gl_error_to_string(GLenum error);

// Which stage a shader source file feeds. The dialect emitter needs to know
// this because vertex and fragment shaders share the `in`/`out` keywords but
// mean different things by them.
enum class ShaderStage {
	kVertex,
	kFragment,
};

// The GLSL dialects the in-tree preprocessor emits from one authored source.
// WP-6 of the renderer modernization plan (Claude/RENDERER_MODERNIZATION_PLAN.md):
// 330 for the core profile, 300 es for a future GLES backend (emitted but not
// yet consumed, decision 9). The GLSL 1.20 dialect, for the frozen legacy 2.1
// path, was removed in WP-1 of the water rendering overhaul
// (Claude/WATER.md). The core dialect is 330 rather than the plan's "150":
// `layout(location=N)` (decision 5) was introduced in GLSL 3.30, so 150
// would need GL_ARB_explicit_attrib_location.
enum class ShaderDialect {
	kGLSL330,
	kGLSL300es,
};

// A (location, name) pair recorded from a `layout(location=N) in T name;`
// declaration.
struct AttributeBinding {
	GLint location;
	std::string name;
};

// The result of emitting one shader for a target dialect: the transformed
// source, plus the attribute bindings parsed from the vertex stage (empty for
// fragment shaders).
struct EmittedShader {
	std::string source;
	std::vector<AttributeBinding> attributes;
};

// Rewrites an already include-expanded shader source into the given dialect.
// 'source' must be authored in GLSL 330 style: `layout(location=N) in` for
// vertex inputs, `in`/`out` for varyings, `out vec4` for the fragment output,
// and `texture(...)` for sampling. Throws on malformed declarations.
EmittedShader emit_dialect(const std::string& expanded_source,
                           ShaderStage stage,
                           ShaderDialect dialect,
                           const std::string& program_name);

// Thin wrapper around a OpenGL program object to ensure proper cleanup. Throws
// on all errors. The program object itself is created lazily in build(), not
// in the constructor: on the core path the programs are built through the RHI
// (GlCorePipeline), so a Program member that never sees build() must not leak a
// glCreateProgram (C9).
class Program {
public:
	Program();
	~Program();

	[[nodiscard]] GLuint object() const {
		return program_object_;
	}

	// Creates and compiles shader objects based on the corresponding files in data/shaders,
	// then links them into the program.
	void build(const std::string& program_name);

	// Binds the uniform block 'name' to 'binding_point', verifying that the
	// block's std140 data size matches 'expected_size' (the sizeof of the C++
	// struct that fills it).
	void
	bind_uniform_block(const std::string& name, GLuint binding_point, size_t expected_size) const;

	// Returns the location recorded for attribute 'name' by the shader source's
	// layout(location=N) qualifier, parsed during build(). Throws if the name
	// is absent, so a renamed or renumbered attribute becomes a startup
	// exception naming the program rather than a silently mismatched VAO.
	[[nodiscard]] GLint attribute_location(const std::string& name) const;

private:
	GLuint program_object_{0U};
	std::unique_ptr<Shader> vertex_shader_;
	std::unique_ptr<Shader> fragment_shader_;
	std::vector<AttributeBinding> attributes_;

	DISALLOW_COPY_AND_ASSIGN(Program);
};

// The std140 layout of the "per_program_state" uniform block shared by the
// terrain, dither, road, grid and workarea programs (renderer modernization
// plan WP-8, Claude/RENDERER_MODERNIZATION_PLAN.md). Floats come first so the
// vec2 lands on its 8-byte alignment without internal padding. std140 rounds a
// uniform block up to a multiple of 16 bytes, so the 24 bytes of scalar data
// are followed by 8 bytes of explicit padding before the three vec3s (each
// std140-aligned to 16 bytes, so each needs one padding float of its own) --
// binding a shorter buffer to a longer block leaves shader results undefined
// per spec, so Program::bind_uniform_block() asserts sizeof(*this) against the
// block's reported GL_UNIFORM_BLOCK_DATA_SIZE. The terrain/dither programs
// repurpose those two padding floats as u_time/u_cloud_amplitude (cloud
// shadows, Claude/VISUAL_FIDELITY_RANKED.md §4.8), so the block size is
// unchanged and the road/grid/workarea programs' z_value-only blocks
// (kZValueOnlyBlockSize) still bind correctly.
//
// sun_direction/sun_color/ambient_color feed the render-side terrain lighting
// (V2, Claude/VISUAL_FIDELITY_RANKED.md §4.2); their values are derived and
// documented in graphic/gl/terrain_lighting.h.
struct PerProgramState {
	float z_value;          // offset 0
	float bump_amplitude;   // offset 4  (terrain noise; terrain/dither only)
	float tint_amplitude;   // offset 8  (terrain noise; terrain/dither only)
	float warp_amplitude;   // offset 12 (terrain noise; terrain/dither only)
	float texture_w;        // offset 16 (vec2; terrain/dither only)
	float texture_h;        // offset 20
	float time;             // offset 24 (cloud shadows; terrain/dither only; was padding_0)
	float cloud_amplitude;  // offset 28 (cloud shadows; terrain/dither only; was padding_1)
	float sun_x;            // offset 32 (vec3; terrain/dither only)
	float sun_y;            // offset 36
	float sun_z;            // offset 40
	float padding_2;        // offset 44
	float sun_color_r;      // offset 48 (vec3; terrain/dither only)
	float sun_color_g;      // offset 52
	float sun_color_b;      // offset 56
	float padding_3;        // offset 60
	float ambient_color_r;  // offset 64 (vec3; terrain/dither only)
	float ambient_color_g;  // offset 68
	float ambient_color_b;  // offset 72
	float padding_4;        // offset 76
};
static_assert(sizeof(PerProgramState) == 80, "std140 layout of per_program_state");

// The GL_UNIFORM_BUFFER binding point every per-program-state block uses. Only
// one program draws at a time, so a single shared binding point is enough.
constexpr GLuint kPerProgramStateBindingPoint = 0;

// The road, grid and workarea programs declare only `float u_z_value;` in
// their per_program_state block, so its std140 data size is 16 bytes (a single
// float rounded up to the vec4 alignment), not sizeof(PerProgramState). The
// terrain and dither blocks carry the full struct instead.
constexpr size_t kZValueOnlyBlockSize = 16;

// Some GL drivers do not remember the current pipeline state. If you rebind a
// texture that has already bound to the same target, they will happily stall
// the pipeline. We therefore cache the state of the GL driver in this class
// and skip unneeded GL calls.
class State {
public:
	static State& instance();

	void bind_framebuffer(GLuint framebuffer, GLuint texture);

	// Wrapper around glActiveTexture() and glBindTexture(). We never unbind a
	// texture, i.e. calls with texture == 0 are ignored. It costs only time and
	// is only needed when the bounded texture is rendered on - see
	// 'unbind_texture_if_bound'.
	void bind(GLenum target, GLuint texture);

	// Checks if the texture is bound to any target. If so, unbinds it. This is
	// needed before the texture is used as target for rendering.
	void unbind_texture_if_bound(GLuint texture);

	void delete_texture(GLuint texture);

private:
	std::unordered_map<GLenum, GLuint> target_to_texture_;
	std::unordered_map<GLuint, GLenum> texture_to_target_;
	GLenum last_active_texture_;
	GLuint current_framebuffer_{0U};
	GLuint current_framebuffer_texture_{0U};

	State();

	void do_bind(GLenum target, GLuint texture);

	DISALLOW_COPY_AND_ASSIGN(State);
};

// Swap order of rows in pixels, to compensate for the upside-down nature of the
// OpenGL coordinate system.
void swap_rows(int width, int height, int pitch, int bpp, uint8_t* pixels);

}  // namespace Gl

#endif  // end of include guard: WL_GRAPHIC_GL_UTILS_H
