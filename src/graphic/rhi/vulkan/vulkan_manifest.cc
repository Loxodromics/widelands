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

#include "graphic/rhi/vulkan/vulkan_manifest.h"

#include <cctype>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>

#include "base/macros.h"
#include "base/wexception.h"
#include "io/fileread.h"
#include "io/filesystem/layered_filesystem.h"

namespace Rhi {

namespace {

// A minimal JSON parser for the one fixed, machine-generated shape
// wl_spirv_compiler writes: objects of objects, arrays of strings, string keys
// and integer values. Anything else throws - the format is ours, so a
// deviation is a build/tooling mistake worth failing loudly on.
class ManifestJsonParser {
public:
	explicit ManifestJsonParser(const std::string& text) : text_(text) {
	}

	// Parses the whole document; it must be one object.
	std::vector<std::pair<std::string, std::vector<std::pair<std::string, int64_t>>>> run() {
		// The top level is { "note": "...", "programs": { <name>: <object> } }.
		std::vector<std::pair<std::string, std::vector<std::pair<std::string, int64_t>>>> result;
		parse_object([this, &result]() {
			const std::string key = parse_string();
			skip_whitespace();
			if (!take(':')) {
				fail("expected ':' after key");
			}
			if (key == "note") {
				parse_string();
			} else if (key == "programs") {
				parse_programs(result);
			} else {
				fail(std::string("unknown top-level key '") + key + "'");
			}
		});
		skip_whitespace();
		if (pos_ != text_.size()) {
			fail("trailing characters after the manifest");
		}
		return result;
	}

private:
	// Parses the "programs" object: program name -> { "samplers": {...},
	// "uniform_blocks": {...}, "attributes": {...} }. Each inner map is
	// flattened into a (key, value) list appended to 'out'.
	void parse_programs(
	   std::vector<std::pair<std::string, std::vector<std::pair<std::string, int64_t>>>>& out) {
		parse_object([this, &out]() {
			const std::string program = parse_string();
			skip_whitespace();
			if (!take(':')) {
				fail("expected ':' after program name");
			}
			out.emplace_back(program, std::vector<std::pair<std::string, int64_t>>());
			parse_program_object(out.back().second);
		});
	}

	// Parses one program's object into flat "section.key" entries:
	// sampler names and attribute names are recorded as
	// ("samplers.<name>", binding) and ("attributes.<name>", location); each
	// uniform block records ("uniform_blocks.<name>.binding", N) and
	// ("uniform_blocks.<name>.stages.<stage>", 1).
	void parse_program_object(std::vector<std::pair<std::string, int64_t>>& out) {
		parse_object([this, &out]() {
			const std::string section = parse_string();
			skip_whitespace();
			if (!take(':')) {
				fail("expected ':' after section name");
			}
			if (section == "samplers" || section == "attributes") {
				parse_flat_map(section + ".", out);
			} else if (section == "uniform_blocks") {
				parse_uniform_blocks(out);
			} else {
				fail(std::string("unknown program section '") + section + "'");
			}
		});
	}

	// Parses '{' (entry separated by ',')* '}', where 'parse_entry' consumes
	// one entry's value (keys are parsed by the callers). Consumes the closing
	// '}' itself and fails when a separator is neither ',' nor '}' - leaving
	// it for a caller's next take('}') would silently desync the parse.
	void parse_object(const std::function<void()>& parse_entry) {
		skip_whitespace();
		if (!take('{')) {
			fail("expected '{'");
		}
		while (!take('}')) {
			parse_entry();
			skip_whitespace();
			if (take(',')) {
				continue;
			}
			if (!take('}')) {
				fail("expected ',' or '}'");
			}
			break;
		}
	}

	void parse_flat_map(const std::string& prefix,
	                    std::vector<std::pair<std::string, int64_t>>& out) {
		parse_object([this, &prefix, &out]() {
			const std::string key = parse_string();
			skip_whitespace();
			if (!take(':')) {
				fail("expected ':' after map key");
			}
			out.emplace_back(prefix + key, parse_integer());
		});
	}

	void parse_uniform_blocks(std::vector<std::pair<std::string, int64_t>>& out) {
		parse_object([this, &out]() {
			const std::string block = parse_string();
			skip_whitespace();
			if (!take(':')) {
				fail("expected ':' after block name");
			}
			parse_object([this, &block, &out]() {
				const std::string field = parse_string();
				skip_whitespace();
				if (!take(':')) {
					fail("expected ':' after block field");
				}
				if (field == "binding") {
					out.emplace_back("uniform_blocks." + block + ".binding", parse_integer());
				} else if (field == "stages") {
					parse_string_array("uniform_blocks." + block + ".stages.", out);
				} else {
					fail(std::string("unknown uniform block field '") + field + "'");
				}
			});
		});
	}

	void parse_string_array(const std::string& prefix,
	                        std::vector<std::pair<std::string, int64_t>>& out) {
		skip_whitespace();
		if (!take('[')) {
			fail("expected '[' for a stages array");
		}
		while (!take(']')) {
			const std::string stage = parse_string();
			out.emplace_back(prefix + stage, 1);
			skip_whitespace();
			if (take(',')) {
				continue;
			}
			if (!take(']')) {
				fail("expected ',' or ']'");
			}
			break;
		}
	}

	std::string parse_string() {
		skip_whitespace();
		if (!take('"')) {
			fail("expected a string");
		}
		std::string result;
		while (pos_ < text_.size() && text_[pos_] != '"') {
			if (text_[pos_] == '\\') {
				++pos_;
				if (pos_ >= text_.size()) {
					fail("truncated escape sequence");
				}
			}
			result += text_[pos_++];
		}
		if (!take('"')) {
			fail("unterminated string");
		}
		return result;
	}

	int64_t parse_integer() {
		skip_whitespace();
		const size_t begin = pos_;
		while (pos_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[pos_])) != 0) {
			++pos_;
		}
		if (begin == pos_) {
			fail("expected an integer");
		}
		return std::stoll(text_.substr(begin, pos_ - begin));
	}

	bool take(const char c) {
		skip_whitespace();
		if (pos_ < text_.size() && text_[pos_] == c) {
			++pos_;
			return true;
		}
		return false;
	}

	void skip_whitespace() {
		while (pos_ < text_.size() &&
		       std::isspace(static_cast<unsigned char>(text_[pos_])) != 0) {
			++pos_;
		}
	}

	[[noreturn]] void fail(const std::string& message) const {
		throw wexception("Malformed bindings manifest at offset %" PRIuS ": %s", pos_,
		                 message.c_str());
	}

	const std::string& text_;
	size_t pos_ = 0;
};

}  // namespace

const ManifestProgram* VulkanManifest::find_program(const std::string& name) const {
	for (const auto& entry : programs) {
		if (entry.first == name) {
			return &entry.second;
		}
	}
	return nullptr;
}

VulkanManifest parse_manifest(const std::string& text) {
	ManifestJsonParser parser(text);
	VulkanManifest manifest;
	for (auto& program : parser.run()) {
		ManifestProgram parsed;
		for (auto& entry : program.second) {
			const std::string& key = entry.first;
			if (key.rfind("samplers.", 0) == 0) {
				parsed.samplers.emplace_back(key.substr(9), static_cast<uint32_t>(entry.second));
			} else if (key.rfind("attributes.", 0) == 0) {
				parsed.attributes.emplace_back(key.substr(11), static_cast<uint32_t>(entry.second));
			} else if (key.rfind("uniform_blocks.", 0) == 0) {
				const std::string rest = key.substr(15);
				const size_t dot = rest.find('.');
				if (dot == std::string::npos) {
					throw wexception("Malformed bindings manifest: entry '%s'", key.c_str());
				}
				const std::string block_name = rest.substr(0, dot);
				const std::string field = rest.substr(dot + 1);
				ManifestUniformBlock* block = nullptr;
				for (ManifestUniformBlock& existing : parsed.uniform_blocks) {
					if (existing.name == block_name) {
						block = &existing;
						break;
					}
				}
				if (block == nullptr) {
					parsed.uniform_blocks.push_back({block_name, 0, false, false});
					block = &parsed.uniform_blocks.back();
				}
				if (field == "binding") {
					block->binding = static_cast<uint32_t>(entry.second);
				} else if (field == "stages.vertex") {
					block->vertex_stage = true;
				} else if (field == "stages.fragment") {
					block->fragment_stage = true;
				} else {
					throw wexception("Malformed bindings manifest: entry '%s'", key.c_str());
				}
			} else {
				throw wexception("Malformed bindings manifest: entry '%s'", key.c_str());
			}
		}
		manifest.programs.emplace_back(std::move(program.first), std::move(parsed));
	}
	return manifest;
}

VulkanManifest load_manifest() {
	FileRead fr;
	fr.open(*g_fs, "shaders/vulkan/bindings.json");
	std::string content;
	content.assign(fr.data(0), fr.get_size());
	fr.close();
	return parse_manifest(content);
}

}  // namespace Rhi
