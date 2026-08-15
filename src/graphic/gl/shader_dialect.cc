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

#include "graphic/gl/shader_dialect.h"

#include <algorithm>
#include <cctype>
#include <string>
#include <vector>

#include "base/wexception.h"

namespace Gl {

namespace {

// Expands `#include "name"` lines in shader source. Single level by
// construction: included files are read with plain read_include and are not
// rescanned, so a nested include is reported rather than silently ignored.
std::string expand_includes_impl(const std::string& source,
                                 const std::string& program_name,
                                 const IncludeReader& read_include) {
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
				include_content = read_include(include_name);
			} catch (const std::exception& e) {
				throw wexception("Shader program '%s', line %" PRIuS
				                 ": cannot read included file '%s': %s",
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
					   "Shader program '%s': included file '%s' contains an #include "
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

bool is_identifier_char(const char c) {
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

// A uniform block opens with `layout(std140) uniform <name> {`. The "uniform"
// keyword is what distinguishes it from the attribute `layout(location=N) in`
// declarations the vertex stage also uses.
bool is_uniform_block_open(const std::string& trimmed_line) {
	return trimmed_line.rfind("layout", 0) == 0 &&
	       trimmed_line.find("uniform") != std::string::npos &&
	       trimmed_line.find("location") == std::string::npos && !trimmed_line.empty() &&
	       trimmed_line.back() == '{';
}

bool is_uniform_block_close(const std::string& trimmed_line) {
	return trimmed_line == "};";
}

}  // namespace

std::string expand_includes(const std::string& source,
                            const std::string& program_name,
                            const IncludeReader& read_include) {
	return expand_includes_impl(source, program_name, read_include);
}

EmittedShader emit_dialect(const std::string& expanded_source,
                           const ShaderStage stage,
                           const ShaderDialect dialect,
                           const std::string& program_name,
                           const VulkanBindings& vulkan_bindings) {
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
	case ShaderDialect::kVulkan:
		version_line = "#version 450";
		break;
	}

	EmittedShader result;
	std::string fragment_output_name;
	std::vector<std::string> output_lines;
	std::vector<std::string> precision_lines;
	bool in_uniform_block = false;
	bool vulkan_main_seen = false;

	size_t pos = 0;
	while (pos < expanded_source.size()) {
		const size_t line_end = expanded_source.find('\n', pos);
		const std::string line = expanded_source.substr(
		   pos, line_end == std::string::npos ? std::string::npos : line_end - pos);

		bool drop_line = false;
		std::string emitted = line;
		const std::string trimmed = trim(line);

		if (in_uniform_block) {
			// Inside a `layout(std140) uniform ... { ... };` block. The 120
			// dialect has no uniform blocks, so lower each member to a loose
			// `uniform <type> <name>;` declaration and drop the block braces;
			// 330, 300 es and Vulkan pass the whole block through unchanged.
			if (is_uniform_block_close(trimmed)) {
				in_uniform_block = false;
				drop_line = dialect == ShaderDialect::kGLSL120;
			} else if (dialect == ShaderDialect::kGLSL120 && !trimmed.empty()) {
				emitted = "uniform " + trimmed;
			}
		} else if (is_uniform_block_open(trimmed)) {
			in_uniform_block = true;
			if (dialect == ShaderDialect::kGLSL120) {
				drop_line = true;
			} else if (dialect == ShaderDialect::kVulkan) {
				// `layout(std140) uniform <name> {` → decorate with the block's
				// descriptor binding. std140 is Vulkan's default UBO layout, so
				// keeping the authored qualifier is harmless.
				const std::vector<std::string> tokens = tokenize_declaration(trimmed);
				if (tokens.size() < 4) {
					throw wexception("Malformed uniform block declaration in shader program '%s': %s",
					                 program_name.c_str(), trimmed.c_str());
				}
				const auto binding_it = vulkan_bindings.uniform_blocks.find(tokens[3]);
				if (binding_it == vulkan_bindings.uniform_blocks.end()) {
					throw wexception(
					   "Uniform block '%s' in shader program '%s' has no assigned Vulkan "
					   "binding — the binding map is stale",
					   tokens[3].c_str(), program_name.c_str());
				}
				emitted = "layout(set = 0, binding = " + std::to_string(binding_it->second) +
				          ", std140) uniform " + tokens[3] + " {";
			}
		} else if (trimmed.rfind("#version", 0) == 0) {
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
			// Precision statements are a GLSL ES concept; the desktop dialects
			// drop them. The 300 es dialect hoists them to just after the
			// version line (and the injected default) — GLSL ES fixes a
			// variable's precision at its declaration, so a precision statement
			// is only meaningful above the declarations it applies to.
			if (dialect == ShaderDialect::kGLSL300es) {
				precision_lines.push_back(trimmed);
			}
			drop_line = true;
		} else {
			const std::vector<std::string> tokens = tokenize_declaration(trimmed);
			if (!tokens.empty() && tokens[0] == "uniform") {
				// Single-line uniform declarations (the samplers). Vulkan GLSL
				// has no default-block uniforms: every sampler must carry an
				// explicit set/binding decoration, and anything else is a
				// mistake the emitter refuses to pass through.
				if (dialect == ShaderDialect::kVulkan) {
					if (tokens.size() < 3 || tokens[1] != "sampler2D") {
						throw wexception(
						   "Unsupported uniform declaration in shader program '%s': %s — the "
						   "Vulkan dialect only supports sampler2D uniforms (everything else must "
						   "live in a uniform block)",
						   program_name.c_str(), trimmed.c_str());
					}
					const auto binding_it = vulkan_bindings.samplers.find(tokens[2]);
					if (binding_it == vulkan_bindings.samplers.end()) {
						throw wexception(
						   "Sampler '%s' in shader program '%s' has no assigned Vulkan binding — "
						   "the binding map is stale",
						   tokens[2].c_str(), program_name.c_str());
					}
					emitted = "layout(set = 0, binding = " + std::to_string(binding_it->second) +
					          ") uniform sampler2D " + tokens[2] + ";";
				}
			} else if (!tokens.empty() && stage == ShaderStage::kVertex) {
				if (dialect == ShaderDialect::kVulkan && trimmed == "void main() {") {
					// The Vulkan dialect wraps main so the clip-space
					// compensation from RHI_INTERFACE.md §2.4 can run after the
					// authored body; the wrapper is appended below.
					emitted = "void wl_main() {";
					vulkan_main_seen = true;
				} else if (tokens[0] == "layout") {
					// layout(location = N) in <type> <name>;
					const auto location_it = std::find(tokens.begin(), tokens.end(), "location");
					const auto in_it = std::find(tokens.begin(), tokens.end(), "in");
					if (location_it == tokens.end() || in_it == tokens.end() ||
					    location_it + 1 >= in_it || in_it + 2 >= tokens.end()) {
						throw wexception("Malformed vertex input declaration in shader program '%s': %s",
						                 program_name.c_str(), trimmed.c_str());
					}
					int32_t location = 0;
					try {
						location = std::stoi(*(location_it + 1));
					} catch (...) {
						throw wexception("Malformed attribute location in shader program '%s': %s",
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
					} else if (dialect == ShaderDialect::kVulkan) {
						// SPIR-V matches vertex outputs to fragment inputs by
						// location, so every output needs an explicit one.
						const auto location_it = vulkan_bindings.varyings.find(tokens[2]);
						if (location_it == vulkan_bindings.varyings.end()) {
							throw wexception(
							   "Vertex output '%s' in shader program '%s' has no assigned Vulkan "
							   "location — the varying map is stale",
							   tokens[2].c_str(), program_name.c_str());
						}
						emitted = "layout(location = " + std::to_string(location_it->second) +
						          ") out " + tokens[1] + " " + tokens[2] + ";";
					}
				} else if (tokens[0] == "in") {
					throw wexception(
					   "Vertex input without layout(location=N) in shader program '%s': %s "
					   "(renderer modernization plan, decision 5)",
					   program_name.c_str(), trimmed.c_str());
				}
			} else if (!tokens.empty()) {  // fragment stage
				if (tokens[0] == "out" && tokens.size() >= 3 && tokens[1] == "vec4") {
					fragment_output_name = tokens[2];
					if (dialect == ShaderDialect::kGLSL120) {
						drop_line = true;
					} else if (dialect == ShaderDialect::kVulkan) {
						// All eight programs have exactly one fragment output;
						// giving it location 0 explicitly avoids relying on the
						// single-output default.
						emitted = "layout(location = 0) out vec4 " + tokens[2] + ";";
					}
				} else if (tokens[0] == "in") {
					if (tokens.size() < 3) {
						throw wexception(
						   "Malformed fragment input declaration in shader program '%s': %s",
						   program_name.c_str(), trimmed.c_str());
					}
					if (dialect == ShaderDialect::kGLSL120) {
						emitted = "varying " + tokens[1] + " " + tokens[2] + ";";
					} else if (dialect == ShaderDialect::kVulkan) {
						// Fragment inputs look their location up by *name*, so a
						// declaration order differing from the vertex stage
						// cannot silently swap two varyings.
						const auto location_it = vulkan_bindings.varyings.find(tokens[2]);
						if (location_it == vulkan_bindings.varyings.end()) {
							throw wexception(
							   "Fragment input '%s' in shader program '%s' has no matching vertex "
							   "output — the varying map is stale",
							   tokens[2].c_str(), program_name.c_str());
						}
						emitted = "layout(location = " + std::to_string(location_it->second) +
						          ") in " + tokens[1] + " " + tokens[2] + ";";
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

	if (in_uniform_block) {
		throw wexception("Unterminated uniform block in shader program '%s'", program_name.c_str());
	}

	// Hoist authored precision statements to just after the emitted #version
	// line (which, for a 300 es fragment, already carries the injected default
	// `precision mediump float;`). They keep source order, so an authored
	// `precision highp float;` still overrides the default for everything that
	// follows it.
	if (!precision_lines.empty()) {
		const auto version_it =
		   std::find_if(output_lines.begin(), output_lines.end(),
		                [](const std::string& line) { return line.rfind("#version", 0) == 0; });
		if (version_it == output_lines.end()) {
			output_lines.insert(output_lines.begin(), precision_lines.begin(), precision_lines.end());
		} else {
			output_lines.insert(version_it + 1, precision_lines.begin(), precision_lines.end());
		}
	}

	// The Vulkan dialect wraps the vertex main in a second main so the
	// clip-space compensation from RHI_INTERFACE.md §2.4 runs after the
	// authored body: Vulkan's clip space has Y down and Z in [0, 1], while the
	// RHI's canonical convention (and every authored vertex) is GL's — Y up,
	// Z in [-1, 1]. All eight vertex shaders compute w = 1, so the z remap
	// (z + w) / 2 is exact.
	if (dialect == ShaderDialect::kVulkan && stage == ShaderStage::kVertex) {
		if (!vulkan_main_seen) {
			throw wexception(
			   "Vertex shader program '%s' has no `void main() {` — the Vulkan dialect needs "
			   "that exact line to wrap the entry point",
			   program_name.c_str());
		}
		output_lines.emplace_back("void main() {");
		output_lines.emplace_back("\twl_main();");
		output_lines.emplace_back("\tgl_Position.y = -gl_Position.y;");
		output_lines.emplace_back("\tgl_Position.z = (gl_Position.z + gl_Position.w) * 0.5;");
		output_lines.emplace_back("}");
	}

	for (const std::string& output_line : output_lines) {
		result.source += output_line;
		result.source += '\n';
	}

	// The 120 dialect has no declared fragment output; the authored `out vec4
	// frag_color;` was dropped above, so its uses become gl_FragColor.
	if (stage == ShaderStage::kFragment && dialect == ShaderDialect::kGLSL120) {
		if (!fragment_output_name.empty()) {
			result.source =
			   replace_token(std::move(result.source), fragment_output_name, "gl_FragColor");
		}
		result.source = replace_token(std::move(result.source), "texture(", "texture2D(");
	}

	return result;
}

}  // namespace Gl
