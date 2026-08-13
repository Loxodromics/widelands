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

#include "dev_harness/options.h"

#include <atomic>
#include <cerrno>
#include <cstdlib>
#include <limits>
#include <vector>

#include "base/math.h"

namespace {

using namespace DevHarness;

bool g_capture_enabled = false;
CaptureOptions g_capture_options;
// Written by the logic thread in SinglePlayerGameController::think(), read by
// the UI thread in Capture::think(), so it has to be atomic.
std::atomic<bool> g_capture_frozen{false};
int32_t g_fixed_timestep = 0;
bool g_fixed_timestep_explicit = false;

constexpr uint32_t kCaptureModeFixedTimestep = 50;

bool parse_u32(const std::string& value, uint32_t* result) {
	int64_t v;
	try {
		v = math::to_long(value);
	} catch (const std::exception&) {
		return false;
	}
	if (v < 0 || v > std::numeric_limits<uint32_t>::max()) {
		return false;
	}
	*result = static_cast<uint32_t>(v);
	return true;
}

bool parse_float_component(const std::string& component, float* result) {
	if (component.empty()) {
		return false;
	}
	errno = 0;
	char* end = nullptr;
	const float v = std::strtof(component.c_str(), &end);
	if (errno != 0 || end == component.c_str() || *end != '\0') {
		return false;
	}
	*result = v;
	return true;
}

}  // namespace

namespace DevHarness {

bool capture_enabled() {
	return g_capture_enabled;
}

const CaptureOptions& capture_options() {
	return g_capture_options;
}

bool capture_frozen() {
	return g_capture_frozen;
}

void set_capture_frozen() {
	g_capture_frozen = true;
}

int32_t fixed_timestep() {
	return g_fixed_timestep;
}

int32_t fixed_timestep_or(int32_t real_delta) {
	return g_fixed_timestep > 0 ? g_fixed_timestep : real_delta;
}

void enable_capture(std::string shot_path) {
	g_capture_enabled = true;
	g_capture_options.shot_path = std::move(shot_path);
}

bool parse_capture_at(const std::string& value) {
	return parse_u32(value, &g_capture_options.capture_at);
}

bool parse_capture_view(const std::string& value) {
	// Expected format: <x>,<y>,<zoom> in map pixel / zoom units.
	std::vector<std::string> parts;
	for (std::string::size_type start = 0;;) {
		const std::string::size_type comma = value.find(',', start);
		if (comma == std::string::npos) {
			parts.push_back(value.substr(start));
			break;
		}
		parts.push_back(value.substr(start, comma - start));
		start = comma + 1;
	}
	if (parts.size() != 3) {
		return false;
	}
	ViewSpec spec{Vector2f::zero(), 0.f};
	if (!parse_float_component(parts[0], &spec.viewpoint.x) ||
	    !parse_float_component(parts[1], &spec.viewpoint.y) ||
	    !parse_float_component(parts[2], &spec.zoom) || spec.zoom <= 0.f) {
		return false;
	}
	g_capture_options.view = spec;
	return true;
}

void set_clean_ui(bool clean) {
	g_capture_options.clean_ui = clean;
}

bool parse_fixed_timestep(const std::string& value) {
	uint32_t v;
	if (!parse_u32(value, &v)) {
		return false;
	}
	g_fixed_timestep = static_cast<int32_t>(v);
	g_fixed_timestep_explicit = true;
	return true;
}

void set_capture_mode_defaults() {
	if (!g_fixed_timestep_explicit) {
		g_fixed_timestep = kCaptureModeFixedTimestep;
	}
}

}  // namespace DevHarness
