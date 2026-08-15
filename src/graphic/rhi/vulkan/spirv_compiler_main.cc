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

// Build-time half of the SPIR-V toolchain (renderer modernization plan,
// WP-13): expands #includes, assigns the program-wide descriptor bindings and
// varying locations the Vulkan dialect needs, emits the GLSL 450 source for
// both stages of every shader program, and writes the bindings.json manifest
// that records the assignments for the C++ side. glslangValidator (invoked by
// CMake, never by this tool) then compiles the emitted sources to the SPIR-V
// committed under data/shaders/vulkan/.

#include <cctype>
#include <cstdint>
#include <fstream>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include "base/wexception.h"
#include "graphic/gl/shader_dialect.h"

namespace {

// Reads the whole file at 'path' into a string. Throws wexception on failure.
std::string read_text_file(const std::string& path) {
	std::ifstream stream(path, std::ios::binary);
	if (!stream.is_open()) {
		throw wexception("cannot read '%s'", path.c_str());
	}
	std::stringstream buffer;
	buffer << stream.rdbuf();
	return buffer.str();
}

void write_text_file(const std::string& path, const std::string& content) {
	std::ofstream stream(path, std::ios::binary);
	if (!stream.is_open()) {
		throw wexception("cannot write '%s'", path.c_str());
	}
	stream << content;
	if (!stream.good()) {
		throw wexception("failed while writing '%s'", path.c_str());
	}
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

// Splits a declaration line into words on whitespace, dropping a trailing
// ';' so `out vec2 var_tex;` tokenizes as [out, vec2, var_tex]. This mirrors
// (a subset of) the emitter's own tokenizer; if the two ever disagree, the
// emitter fails loudly with a missing-binding error rather than emitting
// something silently wrong.
std::vector<std::string> split_words(const std::string& line) {
	std::vector<std::string> words;
	std::string current;
	for (const char c : line) {
		if (std::isspace(static_cast<unsigned char>(c)) != 0) {
			if (!current.empty()) {
				words.push_back(current);
				current.clear();
			}
		} else if (c == ';') {
			if (!current.empty()) {
				words.push_back(current);
				current.clear();
			}
		} else {
			current += c;
		}
	}
	if (!current.empty()) {
		words.push_back(current);
	}
	return words;
}

// A uniform block opens with `layout(std140) uniform <name> {` in the
// authored sources; returns the block name or "" if the line is no block
// open. Must agree with the emitter's is_uniform_block_open.
std::string uniform_block_open_name(const std::string& line) {
	const std::string trimmed = trim(line);
	if (trimmed.rfind("layout", 0) != 0 || trimmed.find("uniform") == std::string::npos ||
	    trimmed.find("location") != std::string::npos || trimmed.empty() ||
	    trimmed.back() != '{') {
		return "";
	}
	const std::vector<std::string> words = split_words(trimmed);
	if (words.size() < 3) {
		throw wexception("malformed uniform block declaration: %s", trimmed.c_str());
	}
	return words[2];
}

// The binding/location assignments for one program, kept in declaration order
// so the manifest and the emitted decorations are stable across runs.
struct ProgramAssignments {
	// Samplers first, then uniform blocks: one shared binding counter per
	// program, everything in descriptor set 0. The per_program_state block is
	// declared by both stages, so it appears once here with one binding.
	std::vector<std::pair<std::string, uint32_t>> samplers;
	std::vector<std::pair<std::string, uint32_t>> uniform_blocks;
	// Varying locations, assigned by the vertex stage's output declaration
	// order; the fragment stage looks them up by name at emission time.
	std::vector<std::pair<std::string, uint32_t>> varyings;
	// Which stages declare each uniform block, kept as a JSON array string
	// (e.g. "[\"vertex\", \"fragment\"]"). The Vulkan backend needs this for
	// the descriptor set layout stage flags (renderer modernization plan,
	// WP-14): terrain/dither declare per_program_state in both stages, while
	// road/grid/workarea only declare it in the vertex stage.
	std::unordered_map<std::string, std::string> uniform_block_stages;

	Gl::VulkanBindings as_vulkan_bindings() const {
		Gl::VulkanBindings bindings;
		for (const auto& entry : samplers) {
			bindings.samplers.emplace(entry);
		}
		for (const auto& entry : uniform_blocks) {
			bindings.uniform_blocks.emplace(entry);
		}
		for (const auto& entry : varyings) {
			bindings.varyings.emplace(entry);
		}
		return bindings;
	}
};

void add_assignment(const std::string& kind,
                    const std::string& program_name,
                    const std::string& name,
                    const uint32_t offset,
                    std::vector<std::pair<std::string, uint32_t>>& list) {
	// The shared per_program_state block is declared by both stages; the
	// second declaration must keep the first's binding rather than getting a
	// new one.
	for (const auto& entry : list) {
		if (entry.first == name) {
			return;
		}
	}
	list.emplace_back(name, offset + static_cast<uint32_t>(list.size()));
	if (list.size() > 1000u) {
		throw wexception("too many %ss in shader program '%s'", kind.c_str(), program_name.c_str());
	}
}

// Scans the include-expanded sources of both stages and returns the program's
// assignments. Pass order matters and is part of the manifest contract:
// samplers first (they only exist in the fragment stage, in declaration
// order), then uniform blocks (vertex then fragment), then vertex varying
// locations.
ProgramAssignments build_assignments(const std::string& program_name,
                                     const std::string& vertex_source,
                                     const std::string& fragment_source) {
	ProgramAssignments assignments;

	const auto scan_lines = [](const std::string& source,
	                           const std::function<void(const std::string&)>& handle_line) {
		std::istringstream stream(source);
		std::string line;
		while (std::getline(stream, line)) {
			const std::string trimmed = trim(line);
			if (!trimmed.empty() && trimmed[0] != '#') {
				handle_line(trimmed);
			}
		}
	};

	// Pass 1: samplers, fragment stage declaration order, bindings from 0.
	scan_lines(fragment_source, [&](const std::string& line) {
		const std::vector<std::string> words = split_words(line);
		if (words.size() >= 3 && words[0] == "uniform" && words[1] == "sampler2D") {
			add_assignment("sampler", program_name, words[2], 0, assignments.samplers);
		}
	});

	// Pass 2: uniform blocks, vertex then fragment. Bindings continue after
	// the samplers, so the two never collide within the shared descriptor set.
	// The declaring stage is recorded per block for the manifest (WP-14).
	const auto scan_blocks = [&](const std::string& source, const char* stage) {
		scan_lines(source, [&](const std::string& line) {
			if (split_words(line).empty()) {
				return;
			}
			const std::string block_name = uniform_block_open_name(line);
			if (!block_name.empty()) {
				add_assignment("uniform block", program_name, block_name,
				               static_cast<uint32_t>(assignments.samplers.size()),
				               assignments.uniform_blocks);
				std::string& stages = assignments.uniform_block_stages[block_name];
				if (stages.empty()) {
					stages = "[\"" + std::string(stage) + "\"]";
				} else if (stages.find(stage) == std::string::npos) {
					stages.pop_back();  // Drop the closing ']'.
					stages += ", \"" + std::string(stage) + "\"]";
				}
			}
		});
	};
	scan_blocks(vertex_source, "vertex");
	scan_blocks(fragment_source, "fragment");

	// Pass 3: vertex outputs assign the varying locations.
	scan_lines(vertex_source, [&](const std::string& line) {
		const std::vector<std::string> words = split_words(line);
		if (words.size() >= 3 && words[0] == "out") {
			add_assignment("varying", program_name, words[2], 0, assignments.varyings);
		}
	});

	return assignments;
}

}  // namespace

int main(int argc, char** argv) {
	std::string data_dir;
	std::string out_dir;
	std::vector<std::string> programs;
	for (int i = 1; i < argc; ++i) {
		const std::string arg = argv[i];
		if (arg == "--data-dir" && i + 1 < argc) {
			data_dir = argv[++i];
		} else if (arg == "--out-dir" && i + 1 < argc) {
			out_dir = argv[++i];
		} else if (arg == "--programs") {
			for (++i; i < argc; ++i) {
				programs.push_back(argv[i]);
			}
		} else {
			std::cerr << "Usage: wl_spirv_compiler --data-dir <shaders dir> --out-dir <dir> "
			             "--programs <name>...\n";
			return 1;
		}
	}
	if (data_dir.empty() || out_dir.empty() || programs.empty()) {
		std::cerr << "wl_spirv_compiler: missing --data-dir, --out-dir or --programs\n";
		return 1;
	}

	try {
		std::string manifest = "{\n"
		                       "\t\"note\": \"Descriptor set/binding and vertex attribute assignments "
		                       "for the Vulkan backend, generated by wl_spirv_compiler from the shader "
		                       "sources - do not edit by hand. All bindings live in descriptor set 0; "
		                       "samplers are always fragment-stage only.\",\n"
		                       "\t\"programs\": {\n";
		bool first_program = true;
		for (const std::string& program : programs) {
			const std::string vertex_source = read_text_file(data_dir + "/" + program + ".vp");
			const std::string fragment_source = read_text_file(data_dir + "/" + program + ".fp");

			const auto read_include = [&data_dir](const std::string& include_name) {
				return read_text_file(data_dir + "/" + include_name);
			};
			const std::string expanded_vertex =
			   Gl::expand_includes(vertex_source, program, read_include);
			const std::string expanded_fragment =
			   Gl::expand_includes(fragment_source, program, read_include);

			const ProgramAssignments assignments =
			   build_assignments(program, expanded_vertex, expanded_fragment);
			const Gl::VulkanBindings bindings = assignments.as_vulkan_bindings();

			const Gl::EmittedShader vertex_shader =
			   Gl::emit_dialect(expanded_vertex, Gl::ShaderStage::kVertex, Gl::ShaderDialect::kVulkan,
			                    program, bindings);
			const Gl::EmittedShader fragment_shader =
			   Gl::emit_dialect(expanded_fragment, Gl::ShaderStage::kFragment,
			                    Gl::ShaderDialect::kVulkan, program, bindings);

			write_text_file(out_dir + "/" + program + ".vert.glsl", vertex_shader.source);
			write_text_file(out_dir + "/" + program + ".frag.glsl", fragment_shader.source);

			// Every program gets a manifest entry: the Vulkan backend (WP-14)
			// needs the attribute locations for all eight, not just those with
			// samplers or uniform blocks.
			manifest += first_program ? "" : ",\n";
			first_program = false;
			manifest += "\t\t\"" + program + "\": {\n";

			manifest += "\t\t\t\"samplers\": {";
			for (size_t i = 0; i < assignments.samplers.size(); ++i) {
				manifest += std::string(i == 0u ? "" : ", ") + "\"" +
				            assignments.samplers[i].first + "\": " +
				            std::to_string(assignments.samplers[i].second);
			}
			manifest += "}";

			// Blocks carry their declaring stages so the Vulkan backend can
			// build the descriptor set layout's stage flags without reflecting
			// over SPIR-V (the WP-13 manifest contract).
			manifest += ",\n\t\t\t\"uniform_blocks\": {";
			for (size_t i = 0; i < assignments.uniform_blocks.size(); ++i) {
				const std::string& name = assignments.uniform_blocks[i].first;
				manifest += std::string(i == 0u ? "" : ", ") + "\"" + name +
				            "\": {\"binding\": " +
				            std::to_string(assignments.uniform_blocks[i].second) + ", \"stages\": " +
				            assignments.uniform_block_stages.at(name) + "}";
			}
			manifest += "}";

			// Vertex attribute locations from the authored layout(location=N)
			// declarations (the vertex stage's EmittedShader::attributes).
			manifest += ",\n\t\t\t\"attributes\": {";
			for (size_t i = 0; i < vertex_shader.attributes.size(); ++i) {
				manifest += std::string(i == 0u ? "" : ", ") + "\"" +
				            vertex_shader.attributes[i].name + "\": " +
				            std::to_string(vertex_shader.attributes[i].location);
			}
			manifest += "}\n\t\t}";
		}
		manifest += "\n\t}\n}\n";
		write_text_file(out_dir + "/bindings.json", manifest);
	} catch (const std::exception& e) {
		std::cerr << "wl_spirv_compiler: " << e.what() << '\n';
		return 1;
	}
	return 0;
}
