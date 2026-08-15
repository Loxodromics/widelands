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

#include "graphic/render_backend.h"

namespace {

RenderBackend g_obtained_backend = RenderBackend::kOpenGL21;

}  // namespace

std::optional<RenderBackend> render_backend_from_string(const std::string& name) {
	if (name == "vulkan") {
		return RenderBackend::kVulkan;
	}
	if (name == "glcore") {
		return RenderBackend::kOpenGLCore;
	}
	if (name == "gl21") {
		return RenderBackend::kOpenGL21;
	}
	return std::nullopt;
}

bool render_backend_available(const RenderBackend backend) {
	switch (backend) {
	case RenderBackend::kVulkan:
#ifdef WL_BUILD_VULKAN
		return true;
#else
		return false;
#endif
	case RenderBackend::kOpenGLCore:
	case RenderBackend::kOpenGL21:
		return true;
	}
	return false;
}

const char* render_backend_name(const RenderBackend backend) {
	switch (backend) {
	case RenderBackend::kVulkan:
		return "vulkan";
	case RenderBackend::kOpenGLCore:
		return "glcore";
	case RenderBackend::kOpenGL21:
		return "gl21";
	}
	return "?";
}

RenderBackend obtained_render_backend() {
	return g_obtained_backend;
}

void record_obtained_render_backend(const RenderBackend backend) {
	g_obtained_backend = backend;
}
