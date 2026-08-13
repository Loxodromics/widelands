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

#include "dev_harness/capture.h"

#include "base/log.h"
#include "dev_harness/options.h"
#include "graphic/graphic.h"
#include "io/filesystem/filesystem.h"
#include "io/filesystem/layered_filesystem.h"
#include "logic/filesystem_constants.h"
#include "logic/game.h"
#include "logic/game_controller.h"
#include "ui/wui/interactive_base.h"
#include "ui/wui/mapview.h"

namespace DevHarness {

Capture& Capture::instance() {
	static Capture capture;
	return capture;
}

void Capture::think(InteractiveBase& ibase) {
	if (!capture_enabled() || state_ == State::kDone) {
		return;
	}

	switch (state_) {
	case State::kWaiting:
		// The controller freezes the game from the logic thread at the first
		// tick at or after the requested gametime, so the freeze point does not
		// depend on frame timing. The editor has no game controller; its
		// gametime still advances, by wall clock (EditorInteractive::think),
		// which is why capture mode pins it and rejects --capture-at (see the
		// comment in that file).
		if (capture_frozen() || ibase.get_game() == nullptr) {
			state_ = State::kFreezing;
		}
		break;

	case State::kFreezing:
		// Ensure the game stays stopped even if the logic thread was in the
		// middle of a tick, then apply the camera and hide the chrome.
		if (Widelands::Game* game = ibase.get_game(); game != nullptr) {
			game->game_controller()->set_desired_speed(0);
		}
		if (const std::optional<ViewSpec>& view = capture_options().view; view.has_value()) {
			ibase.map_view()->set_view(
			   MapView::View(view->viewpoint, view->zoom), MapView::Transition::Jump);
		}
		ibase.set_chrome_visible(!capture_options().clean_ui);
		if (capture_options().clean_ui) {
			// The editor turns grid and resource overlays on in its constructor and
			// the build help on in map_changed() (ui/editor/editorinteractive.cc),
			// all of which would sit on top of whatever the capture is meant to
			// show. The build help in particular puts a symbol on nearly every
			// field, i.e. exactly the lattice terrain work is judged against.
			// Harmless in game mode, where all three are already off.
			ibase.set_display_flag(InteractiveBase::dfShowGrid, false);
			ibase.set_display_flag(InteractiveBase::dfShowResources, false);
			ibase.set_display_flag(InteractiveBase::dfShowBuildhelp, false);
		}
		settle_frames_left_ = capture_options().settle_frames;
		state_ = State::kSettling;
		break;

	case State::kSettling:
		// Let the jumped view take effect and any in-flight logic frame
		// complete before requesting the screenshot.
		if (settle_frames_left_ > 0) {
			--settle_frames_left_;
		} else {
			state_ = State::kRequested;
		}
		break;

	case State::kRequested:
		// The PNG is written at the end of the next Graphic::refresh(), i.e.
		// by the current frame's redraw. The layered filesystem cannot write
		// to absolute paths, so the capture goes to a file named after the
		// requested path inside the home directory's screenshots directory;
		// Claude/wl.py moves it to the requested location afterwards.
		{
			const std::string filename = capture_options().shot_path;
			const std::string rel_name =
			   kScreenshotsDir + FileSystem::file_separator() + FileSystem::fs_filename(filename.c_str());
			g_fs->ensure_directory_exists(kScreenshotsDir);
			log_info("Dev harness: capturing to %s at gametime %u ms\n", rel_name.c_str(),
			         ibase.egbase().get_gametime().get());
			g_gr->screenshot(rel_name);
		}
		state_ = State::kWaitingForWrite;
		break;

	case State::kWaitingForWrite:
		if (!g_gr->screenshot_pending()) {
			// File is on disk; close the game so the process exits.
			state_ = State::kDone;
			ibase.end_modal<UI::Panel::Returncodes>(UI::Panel::Returncodes::kBack);
		}
		break;

	case State::kDone:
		break;
	}
}

}  // namespace DevHarness
