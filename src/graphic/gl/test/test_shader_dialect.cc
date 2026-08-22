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

#include "base/test.h"
#include "graphic/gl/utils.h"

using Gl::EmittedShader;
using Gl::ShaderDialect;
using Gl::ShaderStage;
using Gl::emit_dialect;

TESTSUITE_START(shader_dialect)

TESTCASE(vertex_330_passthrough) {
	const std::string input = "#version 330\n"
	                          "layout(location = 2) in vec3 attr_position;\n";
	const EmittedShader out =
	   emit_dialect(input, ShaderStage::kVertex, ShaderDialect::kGLSL330, "test");
	check_equal(out.source, "#version 330\nlayout(location = 2) in vec3 attr_position;\n");
	check_equal(out.attributes.size(), 1u);
	check_equal(out.attributes[0].location, 2);
	check_equal(out.attributes[0].name, "attr_position");
}

TESTCASE(fragment_300es_precision) {
	const std::string input = "#version 150\n"
	                          "precision highp float;\n"
	                          "in vec2 var_tex;\n"
	                          "out vec4 frag_color;\n"
	                          "void main() {\n"
	                          "\tfrag_color = vec4(var_tex, 0.0, 1.0);\n"
	                          "}\n";
	const EmittedShader out =
	   emit_dialect(input, ShaderStage::kFragment, ShaderDialect::kGLSL300es, "test");
	check_equal(out.source, "#version 300 es\n"
	                        "precision mediump float;\n"
	                        "precision highp float;\n"
	                        "in vec2 var_tex;\n"
	                        "out vec4 frag_color;\n"
	                        "void main() {\n"
	                        "\tfrag_color = vec4(var_tex, 0.0, 1.0);\n"
	                        "}\n");
	check_equal(out.attributes.size(), 0u);
}

TESTCASE(fragment_300es_hoists_precision) {
	// A trailing precision statement is hoisted above the uniform block and the
	// input declaration: GLSL ES fixes a variable's precision at its
	// declaration, so the statement only takes effect for what follows it.
	const std::string input = "#version 150\n"
	                          "layout(std140) uniform per_program_state {\n"
	                          "\tfloat u_value_amplitude;\n"
	                          "};\n"
	                          "in vec2 var_tex;\n"
	                          "out vec4 frag_color;\n"
	                          "precision highp float;\n"
	                          "void main() {\n"
	                          "\tfrag_color = vec4(1.0);\n"
	                          "}\n";
	const EmittedShader out =
	   emit_dialect(input, ShaderStage::kFragment, ShaderDialect::kGLSL300es, "test");
	check_equal(out.source, "#version 300 es\n"
	                        "precision mediump float;\n"
	                        "precision highp float;\n"
	                        "layout(std140) uniform per_program_state {\n"
	                        "\tfloat u_value_amplitude;\n"
	                        "};\n"
	                        "in vec2 var_tex;\n"
	                        "out vec4 frag_color;\n"
	                        "void main() {\n"
	                        "\tfrag_color = vec4(1.0);\n"
	                        "}\n");
	check_equal(out.attributes.size(), 0u);
}

TESTCASE(fragment_330_drops_precision) {
	const std::string input = "#version 330\n"
	                          "precision highp float;\n"
	                          "out vec4 frag_color;\n"
	                          "void main() {\n"
	                          "\tfrag_color = vec4(1.0);\n"
	                          "}\n";
	const EmittedShader out =
	   emit_dialect(input, ShaderStage::kFragment, ShaderDialect::kGLSL330, "test");
	check_equal(out.source, "#version 330\n"
	                        "out vec4 frag_color;\n"
	                        "void main() {\n"
	                        "\tfrag_color = vec4(1.0);\n"
	                        "}\n");
	check_equal(out.attributes.size(), 0u);
}

TESTCASE(vertex_without_layout_throws) {
	check_error(WException, "layout(location", [] {
		emit_dialect("#version 150\nin vec2 attr_position;\n", ShaderStage::kVertex,
		             ShaderDialect::kGLSL330, "test");
	});
}

TESTCASE(vertex_330_uniform_block_passthrough) {
	const std::string input = "#version 330\n"
	                          "layout(std140) uniform per_program_state {\n"
	                          "\tfloat u_z_value;\n"
	                          "};\n";
	const EmittedShader out =
	   emit_dialect(input, ShaderStage::kVertex, ShaderDialect::kGLSL330, "test");
	check_equal(out.source, "#version 330\n"
	                        "layout(std140) uniform per_program_state {\n"
	                        "\tfloat u_z_value;\n"
	                        "};\n");
	check_equal(out.attributes.size(), 0u);
}

TESTCASE(fragment_300es_uniform_block) {
	const std::string input = "#version 150\n"
	                          "layout(std140) uniform per_program_state {\n"
	                          "\tfloat u_value_amplitude;\n"
	                          "\tvec2 u_texture_dimensions;\n"
	                          "};\n"
	                          "out vec4 frag_color;\n"
	                          "void main() {\n"
	                          "\tfrag_color = vec4(1.0);\n"
	                          "}\n";
	const EmittedShader out =
	   emit_dialect(input, ShaderStage::kFragment, ShaderDialect::kGLSL300es, "test");
	check_equal(out.source, "#version 300 es\n"
	                        "precision mediump float;\n"
	                        "layout(std140) uniform per_program_state {\n"
	                        "\tfloat u_value_amplitude;\n"
	                        "\tvec2 u_texture_dimensions;\n"
	                        "};\n"
	                        "out vec4 frag_color;\n"
	                        "void main() {\n"
	                        "\tfrag_color = vec4(1.0);\n"
	                        "}\n");
	check_equal(out.attributes.size(), 0u);
}

TESTCASE(unterminated_uniform_block_throws) {
	check_error(WException, "Unterminated uniform block", [] {
		emit_dialect(
		   "#version 150\nlayout(std140) uniform per_program_state {\n\tfloat u_z_value;\n",
		   ShaderStage::kVertex, ShaderDialect::kGLSL330, "test");
	});
}

TESTSUITE_END()
