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

#ifndef WL_DEV_HARNESS_CAPTURE_H
#define WL_DEV_HARNESS_CAPTURE_H

#include <cstdint>

class InteractiveBase;

// Deterministic screenshot capture for the dev harness (see
// Claude/DEV_HARNESS.md). The simulation is frozen by
// SinglePlayerGameController::think() at an exactly reproducible gametime
// (options.h); this singleton runs on the UI thread and only positions the
// camera, hides the chrome, and requests the screenshot once the game has come
// to rest.
namespace DevHarness {

class Capture {
public:
	static Capture& instance();

	/// UI thread, once per frame from InteractiveBase::think().
	void think(InteractiveBase& ibase);

private:
	Capture() = default;

	enum class State { kWaiting, kFreezing, kSettling, kRequested, kWaitingForWrite, kDone };

	State state_ = State::kWaiting;
	uint32_t settle_frames_left_ = 0;
};

}  // namespace DevHarness

#endif  // end of include guard: WL_DEV_HARNESS_CAPTURE_H
