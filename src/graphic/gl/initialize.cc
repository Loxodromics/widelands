/*
 * Copyright (C) 2006-2026 by the Widelands Development Team
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

#include "graphic/gl/initialize.h"

#include <optional>
#include <regex>
#include <utility>

#include <SDL_messagebox.h>

#include "base/i18n.h"
#include "base/log.h"
#include "graphic/gl/utils.h"
#include "graphic/text/bidi.h"

namespace Gl {

namespace {

Backend g_obtained_backend = Backend::kOpenGL21;

// Adapts SDL's loader signature to glad2's GLADloadfunc, which returns a
// GLADapiproc function pointer rather than void*.
GLADapiproc glad_get_proc_address(const char* name) {
	return reinterpret_cast<GLADapiproc>(SDL_GL_GetProcAddress(name));
}

// The undocumented command line argument --debug_gl_trace sets Trace::kYes,
// which installs these as glad2's per-call debug hooks (see initialize()
// below). Together they log every OpenGL call made, with its name and the
// glGetError() result after it returns. glad2's variadic debug callback does
// not carry per-argument type information (unlike glbinding's old reflection
// API), so argument values are not decoded here.
//
// glad2's --debug wraps every GL function, including glGetError itself, so
// calling the glGetError() macro from inside this callback would recurse
// into its own wrapper and stack-overflow. glad_glGetError is the raw,
// unwrapped function pointer glad2 always exposes alongside the wrapper;
// calling it directly is the glad2 equivalent of the exclusion glbinding's
// old trace code did with setCallbackMaskExcept(..., {"glGetError"}).
void gl_trace_pre_callback(const char* name, GLADapiproc /* apiproc */, int /* len_args */, ...) {
	log_dbg("%s(", name);
}

void gl_trace_post_callback(void* /* ret */,
                            const char* /* name */,
                            GLADapiproc /* apiproc */,
                            int /* len_args */,
                            ...) {
	const auto error = glad_glGetError();
	log_dbg(") [%s]\n", gl_error_to_string(error));
}

}  // namespace

std::optional<Backend> backend_from_string(const std::string& name) {
	if (name == "gl21") {
		return Backend::kOpenGL21;
	}
	if (name == "glcore") {
		return Backend::kOpenGLCore;
	}
	return std::nullopt;
}

const char* backend_name(Backend backend) {
	switch (backend) {
	case Backend::kOpenGL21:
		return "gl21";
	case Backend::kOpenGLCore:
		return "glcore";
	}
	return "?";
}

Backend backend() {
	return g_obtained_backend;
}

SDL_GLContext initialize(const Trace& trace,
                         SDL_Window* sdl_window,
                         GLint* max_texture_size,
                         Backend requested_backend) {
	// GL context attributes for one request, parameterised by version and
	// profile. WP-4 of the renderer modernization plan
	// (Claude/RENDERER_MODERNIZATION_PLAN.md): the legacy 2.1 request keeps
	// exactly the attributes this function has always used, and the core
	// request is 3.3 with the forward-compatible flag (decision 6).
	auto set_attributes = [](int major_version, int minor_version, int profile_mask,
	                         int context_flags) {
		SDL_GL_SetAttribute(SDL_GL_ACCELERATED_VISUAL, 1);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, major_version);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, minor_version);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, profile_mask);
		SDL_GL_SetAttribute(SDL_GL_CONTEXT_FLAGS, context_flags);
		SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, 16);
		SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
	};
	auto set_legacy_attributes = [&set_attributes]() {
		set_attributes(2, 1, 0, 0);
	};

	// Create a context from the attributes currently set, make it current,
	// load the GL library and install the trace hooks. Runs for every context
	// attempt: the loader and the debug wrappers are per context, so a fallback
	// that creates a second context must reload. Returns nullptr if context
	// creation failed.
	//
	// The undocumented command line argument --debug_gl_trace sets Trace::kYes,
	// which logs every OpenGL call made, together with the glGetError() result
	// after it returns (gl_trace_pre_callback/gl_trace_post_callback above).
	// glad2's debug wrapper is live by default as soon as it is generated with
	// --debug (its default callbacks already call glGetError() after every
	// call), so gladUninstallGLDebug() is required in the untraced case to get
	// a build with no per-call overhead, matching a non-debug build exactly.
	auto create_context = [&trace](SDL_Window* window) -> SDL_GLContext {
		SDL_GLContext context = SDL_GL_CreateContext(window);
		if (context == nullptr) {
			return nullptr;
		}
		SDL_GL_MakeCurrent(window, context);
		SDL_GL_SetSwapInterval(0);

		if (gladLoadGL(glad_get_proc_address) == 0) {
			log_err("gladLoadGL failed\nYour OpenGL installation must be __very__ broken.\n");
			throw wexception("gladLoadGL failed: Broken OpenGL installation.");
		}
		if (trace == Trace::kYes) {
			gladSetGLPreCallback(gl_trace_pre_callback);
			gladSetGLPostCallback(gl_trace_post_callback);
			gladInstallGLDebug();
		} else {
			gladUninstallGLDebug();
		}
		return context;
	};

	// Parse an OpenGL version string like "3.3.0 NVIDIA 580.159.03" into its
	// major.minor, without failing the process -- the fallback decision below
	// needs a non-fatal read. Returns std::nullopt if the string is not
	// parseable.
	auto parse_version = [](const std::string& version_string) -> std::optional<std::pair<int, int>> {
		std::vector<std::string> version_vector;
		split(version_vector, version_string, {'.', ' '});
		if (version_vector.size() < 2) {
			return std::nullopt;
		}
		try {
			return std::make_pair(std::stoi(version_vector[0]), std::stoi(version_vector[1]));
		} catch (...) {
			return std::nullopt;
		}
	};

	// Request the chosen backend. There is no ladder of intermediate versions
	// (decision 6): a 3.3 core request is made once, and if it is not granted
	// the only fallback is the legacy 2.1 request, made again on the same
	// window. On a machine that genuinely cannot do 3.3 core this is exactly
	// what the code has always requested, so the legacy path is unchanged.
	const bool want_core = (requested_backend == Backend::kOpenGLCore);
	if (want_core) {
		set_attributes(3, 3, SDL_GL_CONTEXT_PROFILE_CORE, SDL_GL_CONTEXT_FORWARD_COMPATIBLE_FLAG);
	} else {
		set_legacy_attributes();
	}
	SDL_GLContext gl_context = create_context(sdl_window);

	// A core request that failed to create a context at all: fall back to
	// legacy before deciding anything.
	if (gl_context == nullptr && want_core) {
		SDL_ClearError();
		set_legacy_attributes();
		gl_context = create_context(sdl_window);
	}

	// Decide what was actually obtained, and fall back to legacy if a core
	// request came back with a version below 3.3. Drivers differ: NVIDIA grants
	// exactly the version asked (RENDERER_MODERNIZATION.md §9.2), while macOS
	// upgrades any core request to 4.1 (§9.1), which satisfies 3.3. An
	// unreadable version string on the core attempt is treated as not granted.
	Backend obtained_backend = Backend::kOpenGL21;
	if (gl_context != nullptr) {
		const char* const opengl_version_string =
		   reinterpret_cast<const char*>(glGetString(GL_VERSION));
		const auto version = opengl_version_string == nullptr ?
		                        std::nullopt :
		                        parse_version(opengl_version_string);
		const bool core_granted = want_core && version.has_value() &&
		                          (version->first > 3 ||
		                           (version->first == 3 && version->second >= 3));
		if (want_core && !core_granted) {
			SDL_GL_MakeCurrent(sdl_window, nullptr);
			SDL_GL_DeleteContext(gl_context);
			SDL_ClearError();
			set_legacy_attributes();
			gl_context = create_context(sdl_window);
		} else if (core_granted) {
			obtained_backend = Backend::kOpenGLCore;
		}
	}

	if (gl_context == nullptr) {
		throw wexception("SDL_GL_CreateContext failed: %s", SDL_GetError());
	}

	// Show a basic SDL window with an error message, and log it too, then exit 1. Since font support
	// does not exist for all languages, we show both the original and a localized text.
	auto show_opengl_error_and_exit = [](const std::string& message,
	                                     const std::string& localized_message) {
		std::string display_message = message;
		if (message != localized_message) {
			display_message += "\n\n";
			display_message +=
			   (i18n::has_rtl_character(localized_message.c_str()) ?
			       i18n::line2bidi(i18n::make_ligatures(localized_message.c_str()).c_str()) :
			       localized_message);
		}

		log_err("%s\n", display_message.c_str());
		SDL_ShowSimpleMessageBox(
		   SDL_MESSAGEBOX_ERROR, "OpenGL Error", display_message.c_str(), nullptr);
		exit(1);
	};

	// Exit because we couldn't detect the shading language version, so there must be a problem
	// communicating with the graphics adapter.
	auto handle_unreadable_opengl_shading_language = [show_opengl_error_and_exit]() {
		show_opengl_error_and_exit(
		   "Widelands won't work because we were unable to detect the shading language version.\n"
		   "There is an unknown problem with reading the information from the graphics driver.",
		   format("%s\n%s",
		          /** TRANSLATORS: Basic error message when we can't handle the graphics driver. Font
		             support is limited here, so do not use advanced typography **/
		          _("Widelands won't work because we were unable to detect the shading language "
		            "version."),
		          /** TRANSLATORS: Basic error message when we can't handle the graphics driver. Font
		             support is limited here, so do not use advanced typography **/
		          _("There is an unknown problem with reading the information from the graphics "
		            "driver.")));
	};
	auto handle_unreadable_opengl_version = [show_opengl_error_and_exit]() {
		show_opengl_error_and_exit(
		   "Widelands won't work because we were unable to detect the OpenGL version.\n"
		   "There is an unknown problem with reading the information from the graphics driver.",
		   format("%s\n%s",
		          /** TRANSLATORS: Basic error message when we can't handle the graphics driver. Font
		             support is limited here, so do not use advanced typography **/
		          _("Widelands won't work because we were unable to detect the OpenGL version."),
		          /** TRANSLATORS: Basic error message when we can't handle the graphics driver. Font
		             support is limited here, so do not use advanced typography **/
		          _("There is an unknown problem with reading the information from the graphics "
		            "driver.")));
	};
	auto check_version = [show_opengl_error_and_exit](
	                        const std::string& version_string, const std::string& name,
	                        const std::string& descname, const int required_major_version,
	                        const int required_minor_version, const std::function<void()>& error) {
		std::vector<std::string> version_vector;
		split(version_vector, version_string, {'.', ' '});
		if (version_vector.size() >= 2) {
			int major_version = 0;
			int minor_version = 0;
			try {
				major_version = std::stoi(version_vector[0]);
				minor_version = std::stoi(version_vector[1]);
			} catch (...) {
				error();
			}
			// The version has been detected properly. Exit if the version is too old.
			if (major_version < required_major_version ||
			    (major_version == required_major_version && minor_version < required_minor_version)) {
				show_opengl_error_and_exit(
				   format("Widelands won’t work because your graphics driver is too old.\n"
				          "The %s version needs to be version %u.%u or newer.",
				          name, required_major_version, required_minor_version),
				   format("%s\n%s",
				          /** TRANSLATORS: Basic error message when we can't handle the graphics driver.
				             Font support is limited here, so do not use advanced typography **/
				          _("Widelands won’t work because your graphics driver is too old."),
				          /** TRANSLATORS: Basic error message when we can't handle the graphics driver.
				             Font support is limited here, so do not use advanced typography **/
				          format(_("The %1$s version needs to be version %2$u.%3$u or newer."),
				                 descname, required_major_version, required_minor_version)));
			}
		} else {
			// We don't have a minor version. Ensure that the string to compare is a valid integer
			// before conversion
			std::regex re("\\d+");
			if (std::regex_match(version_string, re)) {
				if (std::stol(version_string) < required_major_version + 1) {
					show_opengl_error_and_exit(
					   format("Widelands won’t work because your graphics driver is too old.\n"
					          "The %s needs to be version %u.%u or newer.",
					          name, required_major_version, required_minor_version),
					   format(
					      "%s\n%s",
					      /** TRANSLATORS: Basic error message when we can't handle the graphics driver.
					         Font support is limited here, so do not use advanced typography **/
					      _("Widelands won’t work because your graphics driver is too old."),
					      /** TRANSLATORS: Basic error message when we can't handle the graphics driver.
					         Font support is limited here, so do not use advanced typography **/
					      format(_("The %1$s needs to be version %2$u.%3$u or newer."), descname,
					             required_major_version, required_minor_version)));
				}
			} else {
				// We don't know how to interpret the version info
				error();
			}
		}
	};

	const char* const opengl_version_string = reinterpret_cast<const char*>(glGetString(GL_VERSION));
	if (opengl_version_string == nullptr) {
		handle_unreadable_opengl_version();
	}
	log_info("Graphics: OpenGL: Version \"%s\"\n", opengl_version_string);
	const int required_major_version = obtained_backend == Backend::kOpenGLCore ? 3 : 2;
	const int required_minor_version = obtained_backend == Backend::kOpenGLCore ? 3 : 1;
	check_version(opengl_version_string, "OpenGL", _("OpenGL"), required_major_version,
	              required_minor_version, handle_unreadable_opengl_version);

	// Record what was actually created, not what was requested (WP-3/WP-4 of the
	// renderer modernization plan). The log line is what the dev harness
	// (Claude/wl.py) greps for the obtained backend.
	g_obtained_backend = obtained_backend;

	verb_log_info("Graphics: Render backend requested: %s\n", backend_name(requested_backend));
	log_info("Graphics: Render backend: %s\n", backend_name(g_obtained_backend));

#define LOG_SDL_GL_ATTRIBUTE(x)                                                                    \
	{                                                                                               \
		int value;                                                                                   \
		SDL_GL_GetAttribute(x, &value);                                                              \
		verb_log_info("Graphics: %s is %d\n", #x, value);                                            \
	}

	LOG_SDL_GL_ATTRIBUTE(SDL_GL_RED_SIZE)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_GREEN_SIZE)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_BLUE_SIZE)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_ALPHA_SIZE)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_BUFFER_SIZE)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_DOUBLEBUFFER)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_DEPTH_SIZE)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_STENCIL_SIZE)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_ACCUM_RED_SIZE)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_ACCUM_GREEN_SIZE)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_ACCUM_BLUE_SIZE)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_ACCUM_ALPHA_SIZE)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_STEREO)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_MULTISAMPLEBUFFERS)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_MULTISAMPLESAMPLES)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_ACCELERATED_VISUAL)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_CONTEXT_MAJOR_VERSION)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_CONTEXT_MINOR_VERSION)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_CONTEXT_FLAGS)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_CONTEXT_PROFILE_MASK)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_SHARE_WITH_CURRENT_CONTEXT)
	LOG_SDL_GL_ATTRIBUTE(SDL_GL_FRAMEBUFFER_SRGB_CAPABLE)
#undef LOG_SDL_GL_ATTRIBUTE

	// The profile of the context actually created. glGetIntegerv reports what
	// the driver granted, unlike the SDL_GL_GetAttribute calls above, which
	// report what was requested (WP-4: do not assume the request was honoured).
	GLint context_profile_mask = 0;
	glGetIntegerv(GL_CONTEXT_PROFILE_MASK, &context_profile_mask);
	const char* profile_name = "none";
	if ((context_profile_mask & GL_CONTEXT_CORE_PROFILE_BIT) != 0) {
		profile_name = "core";
	} else if ((context_profile_mask & GL_CONTEXT_COMPATIBILITY_PROFILE_BIT) != 0) {
		profile_name = "compatibility";
	}
	log_info("Graphics: OpenGL: Profile: %s (mask 0x%x)\n", profile_name, context_profile_mask);

	GLboolean glBool;
	glGetBooleanv(GL_DOUBLEBUFFER, &glBool);
	log_info(
	   "Graphics: OpenGL: Double buffering %s\n", (glBool == GL_TRUE) ? "enabled" : "disabled");

	glGetIntegerv(GL_MAX_TEXTURE_SIZE, max_texture_size);
	log_info("Graphics: OpenGL: Max texture size: %d\n", *max_texture_size);

	const char* const shading_language_version_string =
	   reinterpret_cast<const char*>(glGetString(GL_SHADING_LANGUAGE_VERSION));
	if (shading_language_version_string == nullptr) {
		handle_unreadable_opengl_shading_language();
	}
	log_info("Graphics: OpenGL: ShadingLanguage: \"%s\"\n", shading_language_version_string);
	check_version(shading_language_version_string, "Shading Language", _("Shading Language"), 1, 20,
	              handle_unreadable_opengl_shading_language);

	glDrawBuffer(GL_BACK);

	glDisable(GL_DEPTH_TEST);
	glDepthFunc(GL_LEQUAL);

	glEnable(GL_BLEND);
	glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);

	glClear(GL_COLOR_BUFFER_BIT);

	return gl_context;
}

}  // namespace Gl
