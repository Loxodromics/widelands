/*
 * Copyright (C) 2002-2026 by the Widelands Development Team
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

#ifndef WL_GRAPHIC_GRAPHIC_H
#define WL_GRAPHIC_GRAPHIC_H

#include <memory>
#include <string>

#include <SDL_version.h>
#include <SDL_video.h>

#include "graphic/render_backend.h"

#if SDL_VERSION_ATLEAST(2, 0, 5)
#define RESIZABLE_WINDOW
#endif

class RenderTarget;
class Screen;

namespace Rhi {
class Device;
class VulkanDevice;
}

// A graphics card must at least support this size for texture for Widelands to
// run.
constexpr int kMinimumSizeForTextures = 2048;

/**
 * This class is a kind of Swiss Army knife for your graphics need.
 * It initializes the graphic system and provides access to
 * resolutions. It owns an Animation, Image and Surface cache. It
 * also offers functionality to save a screenshot.
 */
class Graphic {
public:
	// Creates a new Graphic object. Must call initialize before first use.
	Graphic();
	~Graphic();

	// Initializes with the given resolution if fullscreen is false, otherwise a
	// window that fills the screen. The 'trace_gl' parameter gets passed on to
	// 'Gl::initialize'. 'requested_backend' is the --renderer selection (WP-3
	// of the renderer modernization plan); what was actually created can be
	// queried via obtained_render_backend() afterwards.
	enum class TraceGl { kNo, kYes };
	void initialize(const TraceGl& trace_gl,
	                int display,
	                int window_mode_w,
	                int window_mode_height,
	                bool fullscreen,
	                bool maximized,
	                RenderBackend requested_backend);
	void rebuild_texture_atlas() const;

	// Gets and sets the resolution.
	// Use 'resize_window = true' to resize the window to the new resolution.
	// Use 'resize_window = false' if the window has already been resized.
	void change_resolution(int w, int h, bool resize_window);
	[[nodiscard]] int get_xres() const;
	[[nodiscard]] int get_yres() const;
	[[nodiscard]] int get_window_mode_xres() const;
	[[nodiscard]] int get_window_mode_yres() const;

	[[nodiscard]] int get_display() const;
	[[nodiscard]] bool maximized() const;
	void set_maximized(bool, int to_display = -1);

	// Changes the window to be fullscreen or not.
	[[nodiscard]] bool fullscreen() const;
	void set_fullscreen(bool, int to_display = -1);

	RenderTarget* get_render_target();
	void refresh();
	[[nodiscard]] SDL_Window* get_sdlwindow() const {
		return sdl_window_;
	}

	[[nodiscard]] int max_texture_size_for_font_rendering() const;

	// Requests a screenshot being taken on the next frame.
	void screenshot(const std::string& fname);

	// True while a requested screenshot has not been written to disk yet.
	[[nodiscard]] bool screenshot_pending() const {
		return !screenshot_filename_.empty();
	}

private:
	[[nodiscard]] int get_display_at(int x, int y) const;
	void move_to_display(int display);

	// Set the window size. Use this instead of calling SDL_SetWindowSize directly.
	void set_window_size(int w, int h);

	// Called when the resolution (might) have changed.
	void resolution_changed();

	// The height & width of the window should we be in window mode.
	int window_mode_width_ = 0;
	int window_mode_height_ = 0;
	bool window_mode_maximized_ = false;

	/// This is the main screen Surface.
	/// A RenderTarget for this can be retrieved with get_render_target()
	std::unique_ptr<Screen> screen_;

	/// This saves a copy of the screen SDL_Surface. This is needed for
	/// opengl rendering as the SurfaceOpenGL does not use it. It allows
	/// manipulation the screen context.
	SDL_Window* sdl_window_ = nullptr;
	/// Under --renderer=vulkan (WP-12): the hidden window the GL context lives
	/// on, since SDL forbids GL and Vulkan on the same window. Null otherwise.
	/// Sized to the visible window so the GL backbuffer matches the screen.
	SDL_Window* gl_context_window_ = nullptr;
	SDL_GLContext gl_context_;

	/// The maximum width or height a texture can have.
	int max_texture_size_ = kMinimumSizeForTextures;

	/// A RenderTarget for screen_. This is initialized during init()
	std::unique_ptr<RenderTarget> render_target_;

	/// Screenshot filename. If a screenshot is requested, this will be set to
	/// the requested filename. On the next frame the screenshot will be written
	/// out and this will be clear()ed again.
	std::string screenshot_filename_;

	/// The RHI device backing the core render path. Only created when the
	/// obtained backend is GL core (WP-10 of the renderer modernization plan);
	/// null on the frozen legacy 2.1 path.
	std::unique_ptr<Rhi::Device> rhi_device_;

	/// The Vulkan RHI device (WP-12 bootstrap, WP-15 conversion). Only
	/// created for --renderer=vulkan; it registers itself with
	/// Rhi::set_device and owns the surface/swapchain, the pipelines and the
	/// per-frame recording/present loop. Must be destroyed before
	/// sdl_window_.
	std::unique_ptr<Rhi::VulkanDevice> vulkan_device_;
};

extern Graphic* g_gr;

#endif  // end of include guard: WL_GRAPHIC_GRAPHIC_H
