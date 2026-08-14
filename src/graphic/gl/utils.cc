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
#include <cctype>
#include <cstddef>
#include <memory>
#include <string>
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

bool is_identifier_char(char c) {
	return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_';
}

// Replaces every occurrence of 'from' with 'to' in 's', matching only where the
// character before 'from' is not an identifier character. This is enough for
// both rewrites the 120 dialect needs: renaming the fragment output identifier
// (which is always a complete identifier) and rewriting the texture() builtin
// to texture2D() without touching identifiers like 'u_texture'.
std::string replace_token(std::string s, const std::string& from, const std::string& to) {
	size_t pos = 0;
	while ((pos = s.find(from, pos)) != std::string::npos) {
		if (pos == 0 || !is_identifier_char(s[pos - 1])) {
			s.replace(pos, from.size(), to);
			pos += to.size();
		} else {
			pos += from.size();
		}
	}
	return s;
}

// Splits a declaration line into tokens on whitespace and the punctuation that
// matters for declarations: ( ) = ; ,. Used to parse the simple single-line
// declarations the shader sources use.
std::vector<std::string> tokenize_declaration(const std::string& line) {
	std::vector<std::string> tokens;
	std::string current;
	for (const char c : line) {
		if (std::isspace(static_cast<unsigned char>(c)) != 0 || c == '(' || c == ')' || c == '=' ||
		    c == ';' || c == ',') {
			if (!current.empty()) {
				tokens.push_back(current);
				current.clear();
			}
		} else {
			current += c;
		}
	}
	if (!current.empty()) {
		tokens.push_back(current);
	}
	return tokens;
}

std::string trim(const std::string& s) {
	size_t begin = 0;
	while (begin < s.size() && std::isspace(static_cast<unsigned char>(s[begin])) != 0) {
		++begin;
	}
	size_t end = s.size();
	while (end > begin && std::isspace(static_cast<unsigned char>(s[end - 1])) != 0) {
		--end;
	}
	return s.substr(begin, end - begin);
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

EmittedShader emit_dialect(const std::string& expanded_source,
                           const ShaderStage stage,
                           const ShaderDialect dialect,
                           const std::string& program_name) {
	std::string version_line;
	switch (dialect) {
	case ShaderDialect::kGLSL120:
		version_line = "#version 120";
		break;
	case ShaderDialect::kGLSL330:
		version_line = "#version 330";
		break;
	case ShaderDialect::kGLSL300es:
		version_line = "#version 300 es";
		break;
	}

	EmittedShader result;
	std::string fragment_output_name;
	std::vector<std::string> output_lines;

	size_t pos = 0;
	while (pos < expanded_source.size()) {
		const size_t line_end = expanded_source.find('\n', pos);
		const std::string line =
		   expanded_source.substr(pos, line_end == std::string::npos ? std::string::npos : line_end - pos);

		bool drop_line = false;
		std::string emitted = line;
		const std::string trimmed = trim(line);

		if (trimmed.rfind("#version", 0) == 0) {
			emitted = version_line;
			// GLSL ES 3.00 fragment shaders have no default float precision, so
			// the 300 es dialect injects the mandatory default precision
			// preamble right after the version. Shaders that need highp declare
			// an explicit `precision highp float;` further down (see the
			// terrain/dither fragment sources), which overrides this.
			if (dialect == ShaderDialect::kGLSL300es && stage == ShaderStage::kFragment) {
				emitted += "\nprecision mediump float;";
			}
		} else if (trimmed.rfind("precision", 0) == 0) {
			// Precision statements are a GLSL ES concept. Keep them for 300 es,
			// drop them for the desktop dialects, which do not support them.
			if (dialect != ShaderDialect::kGLSL300es) {
				drop_line = true;
			}
		} else {
			const std::vector<std::string> tokens = tokenize_declaration(trimmed);
			if (!tokens.empty()) {
				if (stage == ShaderStage::kVertex) {
					if (tokens[0] == "layout") {
						// layout(location = N) in <type> <name>;
						const auto location_it = std::find(tokens.begin(), tokens.end(), "location");
						const auto in_it = std::find(tokens.begin(), tokens.end(), "in");
						if (location_it == tokens.end() || in_it == tokens.end() ||
						    location_it + 1 >= in_it || in_it + 2 >= tokens.end()) {
							throw wexception(
							   "Malformed vertex input declaration in shader program '%s': %s",
							   program_name.c_str(), trimmed.c_str());
						}
						GLint location = 0;
						try {
							location = std::stoi(*(location_it + 1));
						} catch (...) {
							throw wexception(
							   "Malformed attribute location in shader program '%s': %s",
							   program_name.c_str(), trimmed.c_str());
						}
						result.attributes.push_back({location, *(in_it + 2)});
						if (dialect == ShaderDialect::kGLSL120) {
							emitted = "attribute " + *(in_it + 1) + " " + *(in_it + 2) + ";";
						}
					} else if (tokens[0] == "out") {
						if (tokens.size() < 3) {
							throw wexception(
							   "Malformed vertex output declaration in shader program '%s': %s",
							   program_name.c_str(), trimmed.c_str());
						}
						if (dialect == ShaderDialect::kGLSL120) {
							emitted = "varying " + tokens[1] + " " + tokens[2] + ";";
						}
					} else if (tokens[0] == "in") {
						throw wexception(
						   "Vertex input without layout(location=N) in shader program '%s': %s "
						   "(renderer modernization plan, decision 5)",
						   program_name.c_str(), trimmed.c_str());
					}
				} else {  // fragment stage
					if (tokens[0] == "out" && tokens.size() >= 3 && tokens[1] == "vec4") {
						fragment_output_name = tokens[2];
						if (dialect == ShaderDialect::kGLSL120) {
							drop_line = true;
						}
					} else if (tokens[0] == "in") {
						if (tokens.size() < 3) {
							throw wexception(
							   "Malformed fragment input declaration in shader program '%s': %s",
							   program_name.c_str(), trimmed.c_str());
						}
						if (dialect == ShaderDialect::kGLSL120) {
							emitted = "varying " + tokens[1] + " " + tokens[2] + ";";
						}
					}
				}
			}
		}

		if (!drop_line) {
			output_lines.push_back(emitted);
		}
		if (line_end == std::string::npos) {
			break;
		}
		pos = line_end + 1;
	}

	for (const std::string& output_line : output_lines) {
		result.source += output_line;
		result.source += '\n';
	}

	// The 120 dialect has no declared fragment output; the authored `out vec4
	// frag_color;` was dropped above, so its uses become gl_FragColor.
	if (stage == ShaderStage::kFragment && dialect == ShaderDialect::kGLSL120) {
		if (!fragment_output_name.empty()) {
			result.source = replace_token(std::move(result.source), fragment_output_name, "gl_FragColor");
		}
		result.source = replace_token(std::move(result.source), "texture(", "texture2D(");
	}

	return result;
}

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
	const ShaderDialect dialect =
	   backend() == Backend::kOpenGLCore ? ShaderDialect::kGLSL330 : ShaderDialect::kGLSL120;

	const EmittedShader fragment_shader = emit_dialect(
	   expand_includes(read_file("shaders/" + program_name + ".fp"), program_name),
	   ShaderStage::kFragment, dialect, program_name);
	const EmittedShader vertex_shader = emit_dialect(
	   expand_includes(read_file("shaders/" + program_name + ".vp"), program_name),
	   ShaderStage::kVertex, dialect, program_name);

	vertex_shader_.reset(new Shader(GL_VERTEX_SHADER));
	vertex_shader_->compile(vertex_shader.source.c_str(), program_name);
	glAttachShader(program_object_, vertex_shader_->object());

	fragment_shader_.reset(new Shader(GL_FRAGMENT_SHADER));
	fragment_shader_->compile(fragment_shader.source.c_str(), program_name);
	glAttachShader(program_object_, fragment_shader_->object());

	// GLSL 1.20 has no layout(location=N) qualifier, so the 120 output strips it
	// and the emitter records the (name, location) pairs; bind them before
	// linking. No extension dependency (decision 5 of the renderer modernization
	// plan). The 330 output carries the locations in the source, so nothing needs
	// binding there.
	if (dialect == ShaderDialect::kGLSL120) {
		for (const AttributeBinding& binding : vertex_shader.attributes) {
			glBindAttribLocation(program_object_, binding.location, binding.name.c_str());
		}
	}

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
