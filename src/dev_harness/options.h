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

#ifndef WL_DEV_HARNESS_OPTIONS_H
#define WL_DEV_HARNESS_OPTIONS_H

#include <cstdint>
#include <optional>
#include <string>

#include "base/vector.h"

// Command line configuration for the dev harness (see Claude/DEV_HARNESS.md).
// The switches are parsed in WLApplication::handle_commandline_parameters
// through the helpers below; everything else reads the results via the
// accessors. The fixed timestep lives here rather than in capture.h because it
// is read from the game controller, which must not depend on the UI.
namespace DevHarness {

// Map-pixel coordinates of the top-left corner of the view, plus zoom.
struct ViewSpec {
	Vector2f viewpoint;
	float zoom;
};

// Which chrome the capture draws. The info panel carries three independent
// real-time readouts (fps, the real-time clock, and the game-speed average)
// plus a one-pixel anti-aliasing flake in its icons, so it cannot be made
// reproducible readout-by-readout; a deterministic capture either omits it or
// accepts the noise.
enum class UiMode {
	kHidden,  // no chrome at all; the fully deterministic default
	kStable,  // the minimap only: the info panel — and therefore its toolbar
	          // child — stays hidden, so the capture is reproducible while still
	          // exercising the immediate render-to-texture path (the gate uses
	          // this; see Claude/DEV_HARNESS.md)
	kAll,     // everything visible; nondeterministic between runs
};

struct CaptureOptions {
	std::string shot_path;
	uint32_t capture_at = 0;  ///< gametime in ms at which to freeze and capture
	std::optional<ViewSpec> view;
	/// Defaults to kHidden: the info panel draws real-time-dependent content, so
	/// a capture that includes the chrome is not reproducible. --capture-ui=
	/// selects the mode (--capture-show-ui is an alias for kAll).
	UiMode ui_mode = UiMode::kHidden;
	uint32_t settle_frames = 2;  ///< UI frames to let the frozen scene settle
};

/// True once --capture was given and validated.
bool capture_enabled();
const CaptureOptions& capture_options();

/// True once the simulation has been frozen at the capture gametime. Set by
/// SinglePlayerGameController::think(), read by Capture::think().
bool capture_frozen();
void set_capture_frozen();

/// Fixed timestep in ms per logic tick, 0 = disabled (derive from wall clock).
int32_t fixed_timestep();
/// Returns the fixed timestep if enabled, otherwise the given real-time delta.
int32_t fixed_timestep_or(int32_t real_delta);

// The following are called from WLApplication::handle_commandline_parameters.
// The parse helpers return false if the value is malformed; the caller throws
// a ParameterError with a suitable message then.
void enable_capture(std::string shot_path);
bool parse_capture_at(const std::string& value);
bool parse_capture_view(const std::string& value);
bool parse_capture_ui(const std::string& value);
bool parse_fixed_timestep(const std::string& value);
/// Applies the capture-mode default fixed timestep unless one was given.
void set_capture_mode_defaults();

}  // namespace DevHarness

#endif  // end of include guard: WL_DEV_HARNESS_OPTIONS_H
