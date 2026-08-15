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
#include <unordered_set>
#include <vector>

#include "base/macros.h"
#include "base/wexception.h"
#include "graphic/gl/shader_dialect.h"
#include "graphic/gl/system_headers.h"

namespace Gl {

class Shader;

// Returns the name of the 'error'.
const char* gl_error_to_string(GLenum error);

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
	// struct that fills it). Only meaningful on the core backend, where the
	// block exists in the shader source; callers guard on Gl::backend().
	void bind_uniform_block(const std::string& name, GLuint binding_point, size_t expected_size) const;

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

// Thin wrapper around a OpenGL buffer object to ensure proper cleanup. Throws
// on all errors. Also grows the server memory only when needed. The buffer
// object is created lazily on first use, not in the constructor: on the core
// path the programs draw through the RHI's GlCoreBuffer, so a Buffer member
// that is never bound/updated must not leak a glGenBuffers (C9).
template <typename T> class Buffer {
public:
	Buffer() = default;

	~Buffer() {
		if (object_ != 0u) {
			glDeleteBuffers(1, &object_);
		}
	}

	// Calls glBindBuffer on the underlying buffer data, creating the buffer
	// object on first use.
	void bind() {
		ensure_created();
		glBindBuffer(GL_ARRAY_BUFFER, object_);
	}

	// Copies 'elements' into the buffer, overwriting what was there before.
	// Does not check if the buffer is already bound. Creates the buffer object
	// on first use.
	void update(const std::vector<T>& items) {
		ensure_created();
		// Always re-allocate the buffer. This ends up being much more
		// efficient than trying to do a partial update, because partial
		// updates tend to force the driver to do command buffer flushes.
		glBufferData(GL_ARRAY_BUFFER, items.size() * sizeof(T), items.data(), GL_DYNAMIC_DRAW);
	}

private:
	void ensure_created() {
		if (object_ != 0u) {
			return;
		}
		glGenBuffers(1, &object_);
		if (object_ == 0u) {
			throw wexception("Could not create GL buffer.");
		}
	}

	GLuint object_{0U};

	DISALLOW_COPY_AND_ASSIGN(Buffer);
};

// Describes one vertex attribute in a VertexArray: which location it feeds,
// how many float components it has, and where it lives in the vertex struct.
struct VertexAttribute {
	GLint location;
	GLint num_items;
	GLsizei stride;
	size_t offset;
};

// Wrapper around a vertex array object (VAO), which captures a program's
// attribute layout once so that drawing only has to bind it. On the core
// backend this is a real VAO; on the legacy 2.1 backend (which must not depend
// on ARB_vertex_array_object) define_attributes() only records the layout and
// bind() replays the glVertexAttribPointer calls, i.e. what the code used to do
// on every frame before VAOs existed. The VAO is created lazily in
// define_attributes(), not in the constructor: on the core path the programs
// draw through the RHI and never call define_attributes(), so a VertexArray
// member that sees no use must not leak a glGenVertexArrays (C9).
class VertexArray {
public:
	VertexArray() = default;
	~VertexArray();

	// Captures the attribute layout. The GL_ARRAY_BUFFER that 'attributes'
	// refer to must already be bound. On the core backend this (lazily) creates
	// and binds the VAO, enables every attribute and records its pointer; on
	// the legacy backend it only stores the descriptors for bind().
	void define_attributes(const std::vector<VertexAttribute>& attributes);

	// Makes this vertex array active for drawing. On the core backend this is a
	// single glBindVertexArray call; on the legacy backend it enables the
	// attributes and replays glVertexAttribPointer (the GL_ARRAY_BUFFER must
	// already be bound by the caller).
	void bind() const;

private:
	GLuint vao_{0U};
	std::vector<VertexAttribute> attributes_;

	DISALLOW_COPY_AND_ASSIGN(VertexArray);
};

// The std140 layout of the "per_program_state" uniform block shared by the
// terrain, dither, road, grid and workarea programs (renderer modernization
// plan WP-8, Claude/RENDERER_MODERNIZATION_PLAN.md). Floats come first so the
// vec2 lands on its 8-byte alignment without internal padding. std140 rounds a
// uniform block up to a multiple of 16 bytes, so the 24 bytes of real data are
// followed by 8 bytes of explicit padding and the struct is 32 bytes — binding
// a shorter buffer to a longer block leaves shader results undefined per spec,
// so Program::bind_uniform_block() asserts sizeof(*this) against the block's
// reported GL_UNIFORM_BLOCK_DATA_SIZE.
struct PerProgramState {
	float z_value;          // offset 0
	float value_amplitude;  // offset 4  (terrain noise; terrain/dither only)
	float tint_amplitude;   // offset 8  (terrain noise; terrain/dither only)
	float warp_amplitude;   // offset 12 (terrain noise; terrain/dither only)
	float texture_w;        // offset 16 (vec2; terrain/dither only)
	float texture_h;        // offset 20
	float padding_0;        // offset 24 (std140 rounds the block up to 32)
	float padding_1;        // offset 28
};
static_assert(sizeof(PerProgramState) == 32, "std140 layout of per_program_state");

// The GL_UNIFORM_BUFFER binding point every per-program-state block uses. Only
// one program draws at a time, so a single shared binding point is enough.
constexpr GLuint kPerProgramStateBindingPoint = 0;

// The road, grid and workarea programs declare only `float u_z_value;` in
// their per_program_state block, so its std140 data size is 16 bytes (a single
// float rounded up to the vec4 alignment), not sizeof(PerProgramState). The
// terrain and dither blocks carry the full struct instead.
constexpr size_t kZValueOnlyBlockSize = 16;

// Wrapper around a uniform buffer object (UBO), the core-profile replacement
// for per-frame glUniform* calls on per-program scalar state (WP-8). On the
// legacy 2.1 backend there are no UBOs, so the object is never created and
// update()/bind_base() are no-ops (mirrors VertexArray). The buffer object is
// also created lazily on first use on the core backend: on the core path the
// programs read per-program state through the RHI's GlCoreBuffer, so a
// UniformBuffer member that sees no use must not leak a glGenBuffers (C9).
class UniformBuffer {
public:
	UniformBuffer() = default;
	~UniformBuffer();

	// Uploads 'size' bytes from 'data' as the whole buffer contents.
	void update(const void* data, size_t size) const;

	// Binds the buffer to 'binding_point' for reading by shader uniform blocks.
	void bind_base(GLuint binding_point) const;

private:
	void ensure_created() const;

	// 'mutable' so the const update()/bind_base() can lazily create the buffer
	// on first use (the GL object is a cache, not part of the logical state).
	mutable GLuint object_{0U};

	DISALLOW_COPY_AND_ASSIGN(UniformBuffer);
};

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

// Calls glVertexAttribPointer.
void vertex_attrib_pointer(int vertex_index, int num_items, int stride, size_t offset);

// Swap order of rows in pixels, to compensate for the upside-down nature of the
// OpenGL coordinate system.
void swap_rows(int width, int height, int pitch, int bpp, uint8_t* pixels);

}  // namespace Gl

#endif  // end of include guard: WL_GRAPHIC_GL_UTILS_H
