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

#include "graphic/gl/utils.h"

#include <algorithm>
#include <cassert>
#include <cstddef>
#include <memory>
#include <unordered_set>

#include "base/multithreading.h"
#include "base/wexception.h"
#include "graphic/gl/initialize.h"
#include "io/fileread.h"
#include "io/filesystem/layered_filesystem.h"

namespace Gl {

namespace {

constexpr GLenum NONE = static_cast<GLenum>(0);

// Reads 'filename' from g_fs into a string.
std::string read_file(const std::string& filename) {
	std::string content;
	FileRead fr;
	fr.open(*g_fs, filename);
	content.assign(fr.data(0), fr.get_size());
	fr.close();
	return content;
}

// Expands `#include "name"` lines in shader source. GLSL 1.20 has no #include
// of its own, and terrain.fp and dither.fp must compute identical values from
// the same shared code or every terrain border grows a seam.
//
// Single level by construction: included files are read with plain read_file
// and are not rescanned, so a nested include is reported rather than silently
// ignored.
std::string expand_includes(const std::string& source, const std::string& program_name) {
	const auto is_include_line = [](const std::string& line) {
		const size_t first_token = line.find_first_not_of(" \t\r");
		return first_token != std::string::npos && line.compare(first_token, 8, "#include") == 0;
	};

	std::string expanded;
	size_t pos = 0;
	size_t line_number = 1;
	while (pos < source.size()) {
		const size_t line_end = source.find('\n', pos);
		const std::string line =
		   source.substr(pos, line_end == std::string::npos ? std::string::npos : line_end - pos);
		if (is_include_line(line)) {
			const size_t name_begin = line.find('"');
			const size_t name_end =
			   name_begin == std::string::npos ? std::string::npos : line.find('"', name_begin + 1);
			if (name_begin == std::string::npos || name_end == std::string::npos ||
			    line.find_first_not_of(" \t\r", name_end + 1) != std::string::npos) {
				throw wexception("Malformed #include directive in shader program '%s', line %" PRIuS
				                 ": %s",
				                 program_name.c_str(), line_number, line.c_str());
			}
			const std::string include_name = line.substr(name_begin + 1, name_end - name_begin - 1);
			std::string include_content;
			try {
				include_content = read_file("shaders/" + include_name);
			} catch (const std::exception& e) {
				throw wexception("Shader program '%s', line %" PRIuS
				                 ": cannot read included file 'shaders/%s': %s",
				                 program_name.c_str(), line_number, include_name.c_str(), e.what());
			}
			size_t include_pos = 0;
			size_t include_line = 1;
			while (include_pos < include_content.size()) {
				const size_t include_line_end = include_content.find('\n', include_pos);
				const std::string include_line_str = include_content.substr(
				   include_pos,
				   include_line_end == std::string::npos ? std::string::npos :
				                                           include_line_end - include_pos);
				if (is_include_line(include_line_str)) {
					throw wexception(
					   "Shader program '%s': included file 'shaders/%s' contains an #include "
					   "on line %" PRIuS "; nested includes are not supported",
					   program_name.c_str(), include_name.c_str(), include_line);
				}
				if (include_line_end == std::string::npos) {
					break;
				}
				include_pos = include_line_end + 1;
				++include_line;
			}
			expanded += include_content;
			if (include_content.empty() || include_content.back() != '\n') {
				expanded += '\n';
			}
		} else {
			expanded += line;
			expanded += '\n';
		}
		if (line_end == std::string::npos) {
			break;
		}
		pos = line_end + 1;
		++line_number;
	}
	return expanded;
}

// Returns a readable string for a GL_*_SHADER 'type' for debug output.
std::string shader_to_string(GLenum type) {
	if (type == GL_VERTEX_SHADER) {
		return "vertex";
	}
	if (type == GL_FRAGMENT_SHADER) {
		return "fragment";
	}
	return "unknown";
}

}  // namespace

const char* gl_error_to_string(const GLenum err) {
	CLANG_DIAG_OFF("-Wswitch-enum")
#define LOG(a)                                                                                     \
	case a:                                                                                         \
		return #a
	switch (err) {
		LOG(GL_INVALID_ENUM);
		LOG(GL_INVALID_OPERATION);
		LOG(GL_INVALID_VALUE);
		LOG(GL_NO_ERROR);
		LOG(GL_OUT_OF_MEMORY);
		LOG(GL_STACK_OVERFLOW);
		LOG(GL_STACK_UNDERFLOW);
		LOG(GL_TABLE_TOO_LARGE);
	default:
		break;
	}
#undef LOG
	CLANG_DIAG_ON("-Wswitch-enum")
	return "unknown";
}

// Thin wrapper around a Shader object to ensure proper cleanup.
class Shader {
public:
	explicit Shader(GLenum type);
	~Shader();

	[[nodiscard]] GLuint object() const {
		return shader_object_;
	}

	// Compiles 'source'. Throws an exception on error.
	void compile(const char* source, const std::string& program_name) const;

private:
	const GLenum type_;
	const GLuint shader_object_;

	DISALLOW_COPY_AND_ASSIGN(Shader);
};

Shader::Shader(GLenum type) : type_(type), shader_object_(glCreateShader(type)) {
	if (shader_object_ == 0u) {
		throw wexception("Could not create %s shader.", shader_to_string(type).c_str());
	}
}

Shader::~Shader() {
	if (shader_object_ != 0u) {
		glDeleteShader(shader_object_);
	}
}

void Shader::compile(const char* source, const std::string& program_name) const {
	glShaderSource(shader_object_, 1, &source, nullptr);

	glCompileShader(shader_object_);
	GLint compiled;
	glGetShaderiv(shader_object_, GL_COMPILE_STATUS, &compiled);
	if (compiled == 0) {
		GLint infoLen = 0;
		glGetShaderiv(shader_object_, GL_INFO_LOG_LENGTH, &infoLen);
		if (infoLen > 1) {
			std::unique_ptr<char[]> infoLog(new char[infoLen]);
			CLANG_DIAG_OFF("-Wunknown-pragmas")
			CLANG_DIAG_OFF("-Wzero-as-null-pointer-constant")
			glGetShaderInfoLog(shader_object_, infoLen, nullptr, infoLog.get());
			CLANG_DIAG_ON("-Wzero-as-null-pointer-constant")
			CLANG_DIAG_ON("-Wunknown-pragmas")
			throw wexception("Error compiling %s shader in program '%s' "
			                 "(line numbers refer to the assembled source):\n%s",
			                 shader_to_string(type_).c_str(), program_name.c_str(), infoLog.get());
		}
	}
}

Program::Program() : program_object_(glCreateProgram()) {
	if (program_object_ == 0u) {
		throw wexception("Could not create GL program.");
	}
}

Program::~Program() {
	if (program_object_ != 0u) {
		glDeleteProgram(program_object_);
	}
}

void Program::build(const std::string& program_name) {
	std::string fragment_shader_source =
	   expand_includes(read_file("shaders/" + program_name + ".fp"), program_name);
	std::string vertex_shader_source =
	   expand_includes(read_file("shaders/" + program_name + ".vp"), program_name);

	vertex_shader_.reset(new Shader(GL_VERTEX_SHADER));
	vertex_shader_->compile(vertex_shader_source.c_str(), program_name);
	glAttachShader(program_object_, vertex_shader_->object());

	fragment_shader_.reset(new Shader(GL_FRAGMENT_SHADER));
	fragment_shader_->compile(fragment_shader_source.c_str(), program_name);
	glAttachShader(program_object_, fragment_shader_->object());

	glLinkProgram(program_object_);

	// Check the link status
	GLint linked;
	glGetProgramiv(program_object_, GL_LINK_STATUS, &linked);
	if (linked == 0) {
		GLint infoLen = 0;
		glGetProgramiv(program_object_, GL_INFO_LOG_LENGTH, &infoLen);

		if (infoLen > 1) {
			std::unique_ptr<char[]> infoLog(new char[infoLen]);
			CLANG_DIAG_OFF("-Wunknown-pragmas")
			CLANG_DIAG_OFF("-Wzero-as-null-pointer-constant")
			glGetProgramInfoLog(program_object_, infoLen, nullptr, infoLog.get());
			CLANG_DIAG_ON("-Wzero-as-null-pointer-constant")
			CLANG_DIAG_ON("-Wunknown-pragmas")
			throw wexception("Error linking:\n%s", infoLog.get());
		}
	}
}

namespace {

// Legacy 2.1 backend only: which vertex attrib arrays are currently enabled.
// Without VAOs the enable/disable state is global, so the per-draw replay must
// track it both to skip redundant calls and to disable arrays a previous
// program left enabled.
std::unordered_set<GLint>& enabled_attrib_arrays() {
	static std::unordered_set<GLint> arrays;
	return arrays;
}

}  // namespace

VertexArray::VertexArray() {
	if (backend() == Backend::kOpenGLCore) {
		glGenVertexArrays(1, &vao_);
		if (vao_ == 0u) {
			throw wexception("Could not create GL vertex array object.");
		}
	}
}

VertexArray::~VertexArray() {
	if (vao_ != 0u) {
		glDeleteVertexArrays(1, &vao_);
	}
}

void VertexArray::define_attributes(const std::vector<VertexAttribute>& attributes) {
	attributes_ = attributes;
	if (backend() != Backend::kOpenGLCore) {
		return;
	}
	glBindVertexArray(vao_);
	for (const VertexAttribute& attribute : attributes) {
		glEnableVertexAttribArray(attribute.location);
		vertex_attrib_pointer(attribute.location, attribute.num_items, attribute.stride, attribute.offset);
	}
}

void VertexArray::bind() const {
	if (backend() == Backend::kOpenGLCore) {
		glBindVertexArray(vao_);
		return;
	}

	// Legacy replay: mirror the old State::enable_vertex_attrib_array plus the
	// per-attribute vertex_attrib_pointer calls.
	auto& enabled = enabled_attrib_arrays();
	for (const VertexAttribute& attribute : attributes_) {
		if (enabled.count(attribute.location) == 0u) {
			glEnableVertexAttribArray(attribute.location);
			enabled.insert(attribute.location);
		}
	}
	for (auto it = enabled.begin(); it != enabled.end();) {
		if (std::none_of(attributes_.begin(), attributes_.end(), [it](const VertexAttribute& attribute) {
			    return attribute.location == *it;
		    })) {
			glDisableVertexAttribArray(*it);
			it = enabled.erase(it);
		} else {
			++it;
		}
	}
	for (const VertexAttribute& attribute : attributes_) {
		vertex_attrib_pointer(attribute.location, attribute.num_items, attribute.stride, attribute.offset);
	}
}

State::State() : last_active_texture_(NONE) {
}

void State::bind(const GLenum target, const GLuint texture) {
	if (texture == 0) {
		return;
	}
	do_bind(target, texture);
}

void State::do_bind(const GLenum target, const GLuint texture) {
	const auto currently_bound_texture = target_to_texture_[target];
	if (currently_bound_texture == texture) {
		return;
	}
	if (last_active_texture_ != target) {
		glActiveTexture(target);
		last_active_texture_ = target;
	}
	glBindTexture(GL_TEXTURE_2D, texture);

	target_to_texture_[target] = texture;
	texture_to_target_[currently_bound_texture] = NONE;
	texture_to_target_[texture] = target;
}

void State::unbind_texture_if_bound(const GLuint texture) {
	if (texture == 0) {
		return;
	}
	const auto target = texture_to_target_[texture];
	if (target != 0) {
		do_bind(target, 0);
	}
}

void State::delete_texture(const GLuint texture) {
	unbind_texture_if_bound(texture);
	glDeleteTextures(1, &texture);

	if (current_framebuffer_texture_ == texture) {
		current_framebuffer_texture_ = 0;
	}
}

void State::bind_framebuffer(const GLuint framebuffer, const GLuint texture) {
	if (current_framebuffer_ == framebuffer && current_framebuffer_texture_ == texture) {
		return;
	}

	// Some graphic drivers inaccurately do not flush their pipeline when
	// switching the framebuffer - and happily do draw calls into the wrong
	// framebuffers. I AM LOOKING AT YOU, INTEL!!!
	glFlush();

	glBindFramebuffer(GL_FRAMEBUFFER, framebuffer);
	if (framebuffer != 0) {
		unbind_texture_if_bound(texture);
		glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);
		assert(glCheckFramebufferStatus(GL_FRAMEBUFFER) == GL_FRAMEBUFFER_COMPLETE);
	}
	current_framebuffer_ = framebuffer;
	current_framebuffer_texture_ = texture;
}

// static
State& State::instance() {
	assert(is_initializer_thread());
	static State binder;
	return binder;
}

void vertex_attrib_pointer(int vertex_index, int num_items, int stride, size_t offset) {
	glVertexAttribPointer(
	   vertex_index, num_items, GL_FLOAT, GL_FALSE, stride, reinterpret_cast<void*>(offset));
}

void swap_rows(const int width, const int height, const int pitch, const int bpp, uint8_t* pixels) {
	uint8_t* begin_row = pixels;
	uint8_t* end_row = pixels + static_cast<ptrdiff_t>(pitch) * (height - 1);
	while (begin_row < end_row) {
		for (int x = 0; x < width * bpp; ++x) {
			std::swap(begin_row[x], end_row[x]);
		}
		begin_row += pitch;
		end_row -= pitch;
	}
}

}  // namespace Gl
