/*
 * Copyright (C) 2010-2026 by the Widelands Development Team
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
 */

#include "graphic/texture.h"

#include <cassert>
#include <cstddef>
#include <functional>
#include <memory>
#include <vector>

#include <SDL_surface.h>

#include "base/macros.h"
#include "base/multithreading.h"
#include "base/wexception.h"
#include "graphic/gl/blit_program.h"
#include "graphic/gl/draw_line_program.h"
#include "graphic/gl/fill_rect_program.h"
#include "graphic/gl/initialize.h"
#include "graphic/gl/utils.h"
#include "graphic/rhi/gl/gl_device.h"
#include "graphic/sdl_utils.h"
#include "graphic/surface.h"

namespace {

namespace {

/**
 * \return the standard 32-bit RGBA format that we use for our textures.
 */
const SDL_PixelFormat& rgba_format() {
	static SDL_PixelFormat format;
	static bool init = false;
	if (init) {
		return format;
	}

	init = true;
	memset(&format, 0, sizeof(format));
	format.BitsPerPixel = 32;
	format.BytesPerPixel = 4;
	format.Rmask = 0x000000ff;
	format.Gmask = 0x0000ff00;
	format.Bmask = 0x00ff0000;
	format.Amask = 0xff000000;
	format.Rshift = 0;
	format.Gshift = 8;
	format.Bshift = 16;
	format.Ashift = 24;
	return format;
}

}  // namespace

class GlFramebuffer {
public:
	static GlFramebuffer& instance() {
		static GlFramebuffer gl_framebuffer;
		return gl_framebuffer;
	}

	~GlFramebuffer() {
		glDeleteFramebuffers(1, &gl_framebuffer_id_);
	}

	[[nodiscard]] GLuint id() const {
		return gl_framebuffer_id_;
	}

private:
	GlFramebuffer() {
		// Generate the framebuffer for Offscreen rendering.
		glGenFramebuffers(1, &gl_framebuffer_id_);
	}

	GLuint gl_framebuffer_id_;

	DISALLOW_COPY_AND_ASSIGN(GlFramebuffer);
};

bool is_bgr_surface(const SDL_PixelFormat& fmt) {
	return (fmt.Bmask == 0x000000ff && fmt.Gmask == 0x0000ff00 && fmt.Rmask == 0x00ff0000);
}

}  // namespace

Texture::Texture(int w, int h) : owns_texture_(false) {
	init(w, h);

	if (blit_data_.texture_id == 0) {
		return;
	}

	glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA), width(), height(), 0, GL_RGBA,
	             GL_UNSIGNED_BYTE, nullptr);
}

Texture::Texture(SDL_Surface* surface, bool intensity) : owns_texture_(false) {
	init(surface->w, surface->h);

	// Convert image data. BGR Surface support is an extension for
	// OpenGL ES 2, which we rather not rely on. So we convert our
	// surfaces in software.
	// TODO(sirver): SDL_TTF returns all data in BGR format. If we
	// use freetype directly we might be able to avoid that.
	uint8_t bpp = surface->format->BytesPerPixel;

	if ((surface->format->palette != nullptr) || width() != surface->w || height() != surface->h ||
	    (bpp != 4) || is_bgr_surface(*surface->format)) {
		SDL_Surface* converted = empty_sdl_surface(width(), height());
		if (converted == nullptr) {
			throw wexception("Failed to create SDL_Surface");
		}
		SDL_SetSurfaceAlphaMod(converted, SDL_ALPHA_OPAQUE);
		SDL_SetSurfaceBlendMode(converted, SDL_BLENDMODE_NONE);
		SDL_SetSurfaceAlphaMod(surface, SDL_ALPHA_OPAQUE);
		SDL_SetSurfaceBlendMode(surface, SDL_BLENDMODE_NONE);
		SDL_BlitSurface(surface, nullptr, converted, nullptr);
		SDL_FreeSurface(surface);
		surface = converted;
		bpp = surface->format->BytesPerPixel;
		assert(bpp == 4);
	}

	SDL_LockSurface(surface);

	Gl::swap_rows(width(), height(), surface->pitch, bpp, static_cast<uint8_t*>(surface->pixels));

	if (intensity) {
		// The dither mask is single-channel, but surface->pixels is a 4-byte-per-
		// pixel RGBA buffer. Uploading it with a GL_RED format would make GL read
		// 'width' bytes per row out of a 'width * 4'-byte row, unpacking the mask
		// as an interleaved R,G,B,A sequence. Pack the red channel (the first
		// byte of each pixel, Rshift == 0) into a tight buffer honouring
		// surface->pitch instead. GL_UNPACK_ALIGNMENT must be 1 because the packed
		// rows are 'width' bytes with no row padding.
		std::vector<uint8_t> packed(static_cast<size_t>(width()) * height());
		const uint8_t* src = static_cast<const uint8_t*>(surface->pixels);
		uint8_t* dst = packed.data();
		for (int y = 0; y < height(); ++y) {
			for (int x = 0; x < width(); ++x) {
				dst[y * width() + x] = src[y * surface->pitch + x * bpp];
			}
		}
		glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
		glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_R8), width(), height(), 0, GL_RED,
		             GL_UNSIGNED_BYTE, packed.data());
		glPixelStorei(GL_UNPACK_ALIGNMENT, 4);
	} else {
		glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA), width(), height(), 0, GL_RGBA,
		             GL_UNSIGNED_BYTE, surface->pixels);
	}

	SDL_UnlockSurface(surface);
	SDL_FreeSurface(surface);
}

Texture::Texture(const Rhi::Texture* texture,
                 const uint32_t texture_id,
                 const Recti& subrect,
                 int parent_w,
                 int parent_h)
   : owns_texture_(false) {
	if (parent_w == 0 || parent_h == 0) {
		throw wexception("Created a sub Texture with zero height and width parent.");
	}

	blit_data_ = BlitData{
	   texture,
	   texture_id,
	   parent_w,
	   parent_h,
	   subrect.cast<float>(),
	};
}

Texture::~Texture() {
	if (owns_texture_) {
		uint32_t texture_id = blit_data_.texture_id;
		NoteThreadSafeFunction::instantiate(
		   [texture_id]() { Gl::State::instance().delete_texture(texture_id); }, false);
	}
}

int Texture::width() const {
	return blit_data_.rect.w;
}

int Texture::height() const {
	return blit_data_.rect.h;
}

void Texture::init(uint16_t w, uint16_t h) {
	assert(is_initializer_thread());

	blit_data_ = {
	   nullptr,  // texture (RHI handle): null on the legacy path
	   0,        // texture_id, initialized below
	   w,
	   h,
	   Rectf(0.f, 0.f, w, h),
	};
	if (w * h == 0) {
		return;
	}

	owns_texture_ = true;
	glGenTextures(1, &blit_data_.texture_id);
	Gl::State::instance().bind(GL_TEXTURE0, blit_data_.texture_id);

	// set texture filter to use linear filtering. This looks nicer for resized
	// texture. Most textures and images are not resized so the filtering
	// makes no difference.
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, static_cast<GLint>(GL_LINEAR));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, static_cast<GLint>(GL_LINEAR));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, static_cast<GLint>(GL_CLAMP_TO_EDGE));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, static_cast<GLint>(GL_CLAMP_TO_EDGE));

	// On the core path, expose this GL texture as an RHI handle for the draw
	// code (descriptor-set binding). It stays non-owning: the GL texture's
	// lifecycle is managed here, not by the RHI (WP-10 moves the draw path, not
	// texture creation).
	if (Gl::backend() == Gl::Backend::kOpenGLCore) {
		rhi_texture_ = Rhi::wrap_gl_texture(blit_data_.texture_id, w, h);
		blit_data_.texture = rhi_texture_.get();
	}
}

void Texture::lock() {
	assert(is_initializer_thread());

	if (blit_data_.texture_id == 0) {
		return;
	}

	if (pixels_) {
		throw wexception("Called lock() on locked surface.");
	}
	if (!owns_texture_) {
		throw wexception("A surface that does not own its pixels can not be locked..");
	}

	pixels_.reset(new uint8_t[4ULL * width() * height()]);

	setup_gl();
	glReadPixels(0, 0, width(), height(), GL_RGBA, GL_UNSIGNED_BYTE, pixels_.get());
}

void Texture::unlock(UnlockMode mode) {
	assert(is_initializer_thread());

	if (width() <= 0 || height() <= 0) {
		return;
	}
	assert(pixels_);

	if (mode == Unlock_Update) {
		Gl::State::instance().bind(GL_TEXTURE0, blit_data_.texture_id);
		glTexImage2D(GL_TEXTURE_2D, 0, static_cast<GLint>(GL_RGBA), width(), height(), 0, GL_RGBA,
		             GL_UNSIGNED_BYTE, pixels_.get());
	}

	pixels_.reset(nullptr);
}

RGBAColor Texture::get_pixel(uint16_t x, uint16_t y) {
	assert(pixels_);
	assert(x < width());
	assert(y < height());

	RGBAColor color;

	SDL_GetRGBA(*reinterpret_cast<uint32_t*>(&pixels_[(height() - y - 1) * 4 * width() + 4 * x]),
	            &rgba_format(), &color.r, &color.g, &color.b, &color.a);
	return color;
}

void Texture::set_pixel(uint16_t x, uint16_t y, const RGBAColor& color) {
	assert(pixels_);
	assert(x < width());
	assert(y < height());

	uint8_t* data = &pixels_[(height() - y - 1) * 4 * width() + 4 * x];
	uint32_t packed_color = SDL_MapRGBA(&rgba_format(), color.r, color.g, color.b, color.a);
	*(reinterpret_cast<uint32_t*>(data)) = packed_color;
}

void Texture::setup_gl() const {
	assert(blit_data_.texture_id != 0);
	Gl::State::instance().bind_framebuffer(GlFramebuffer::instance().id(), blit_data_.texture_id);
	glViewport(0, 0, width(), height());
}

void Texture::draw_to_self(const std::function<void()>& draw) {
	if (Gl::backend() == Gl::Backend::kOpenGLCore) {
		std::unique_ptr<Rhi::CommandBuffer> command_buffer = Rhi::device().begin_offscreen();
		command_buffer->transition(blit_data_.texture, Rhi::TextureLayout::kColorAttachment);
		command_buffer->begin_pass(blit_data_.texture, Rhi::PassClear{false, 0.f, 0.f, 0.f, 0.f});
		draw();
		command_buffer->end_pass();
		command_buffer->transition(blit_data_.texture, Rhi::TextureLayout::kShaderReadOnly);
		Rhi::device().submit_offscreen(std::move(command_buffer));
	} else {
		setup_gl();
		draw();
	}
}

void Texture::do_blit(const Rectf& dst_rect,
                      const BlitData& texture,
                      float opacity,
                      BlendMode blend_mode) {
	if (blit_data_.texture_id == 0) {
		return;
	}
	draw_to_self([&]() {
		BlitProgram::instance().draw(dst_rect, 0.f, texture, BlitData{nullptr, 0, 0, 0, Rectf()},
		                             RGBAColor(0, 0, 0, 255 * opacity), blend_mode);
	});
}

void Texture::do_blit_blended(const Rectf& dst_rect,
                              const BlitData& texture,
                              const BlitData& mask,
                              const RGBColor& blend) {

	if (blit_data_.texture_id == 0) {
		return;
	}
	draw_to_self([&]() {
		BlitProgram::instance().draw(dst_rect, 0.f, texture, mask, blend, BlendMode::UseAlpha);
	});
}

void Texture::do_blit_monochrome(const Rectf& dst_rect,
                                 const BlitData& texture,
                                 const RGBAColor& blend) {
	if (blit_data_.texture_id == 0) {
		return;
	}
	draw_to_self([&]() { BlitProgram::instance().draw_monochrome(dst_rect, 0.f, texture, blend); });
}

void Texture::do_draw_line_strip(std::vector<DrawLineProgram::PerVertexData> vertices) {
	if (blit_data_.texture_id == 0) {
		return;
	}
	draw_to_self([&]() {
		DrawLineProgram::instance().draw(
		   {DrawLineProgram::Arguments{std::move(vertices), 0.f, BlendMode::UseAlpha}});
	});
}

void Texture::do_fill_rect(const Rectf& dst_rect, const RGBAColor& color, BlendMode blend_mode) {
	if (blit_data_.texture_id == 0) {
		return;
	}
	draw_to_self(
	   [&]() { FillRectProgram::instance().draw(dst_rect, 0.f, color, blend_mode); });
}

const BlitData& Texture::blit_data() const {
	return blit_data_;
}
