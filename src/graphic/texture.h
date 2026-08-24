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

#ifndef WL_GRAPHIC_TEXTURE_H
#define WL_GRAPHIC_TEXTURE_H

#include <functional>
#include <memory>

#include "base/rect.h"
#include "graphic/gl/system_headers.h"
#include "graphic/surface.h"

struct SDL_Surface;

namespace Rhi {
class Texture;
enum class TextureFormat;
}

class Texture : public Surface, public Image {
public:
	// Create a new surface from an SDL_Surface. Ownership is taken.
	// 'intensity' selects a single-channel (R8) upload, used for the dither
	// mask; false uploads RGBA8 (WP-16).
	explicit Texture(SDL_Surface* surface, bool intensity = false);

	// Create a new empty (that is randomly filled) Surface with the given
	// dimensions.
	Texture(int w, int h);

	// Create a logical texture that is a 'subrect' (in Pixel) in another
	// texture. 'texture' is the parent's RHI handle (null on the legacy path)
	// and 'texture_id' its GL name; ownership is not taken.
	Texture(const Rhi::Texture* texture,
	        uint32_t texture_id,
	        const Recti& subrect,
	        int parent_w,
	        int parent_h);

	~Texture() override;

	// Implements Surface
	[[nodiscard]] int width() const override;
	[[nodiscard]] int height() const override;

	// Implements Image.
	[[nodiscard]] const BlitData& blit_data() const override;

	enum UnlockMode {
		/**
		 * Update mode will ensure that any changes in the pixel data
		 * will appear in subsequent operations.
		 */
		Unlock_Update = 0,

		/**
		 * NoChange mode indicates that the caller changed no pixel data.
		 *
		 * \note If the caller did change pixel data but specifies NoChange
		 * mode, the results are undefined.
		 */
		Unlock_NoChange
	};

	// Lock/Unlock pairs must guard any of the direct pixel access using the
	// functions below. Lock/Unlock pairs cannot be nested.
	void lock();
	void unlock(UnlockMode);

	// Returns the color of the pixel.
	RGBAColor get_pixel(uint16_t x, uint16_t y);

	// Sets the pixel to the 'clr'.
	void set_pixel(uint16_t x, uint16_t y, const RGBAColor& color);

private:
	// Configures OpenGL to draw to this surface.
	void setup_gl() const;
	// Creates the GL texture (and, on the RHI path, the RHI texture handle:
	// the GL-core wrap or a real Vulkan texture under --renderer=vulkan).
	// 'format' is the RHI storage format (the dither mask is single-channel).
	// Under Vulkan, 'zero_fill' controls whether the RHI texture gets an
	// immediate zero-filled upload: true for a render target (the minimap,
	// the font and image caches), which must be samplable before anything is
	// ever drawn into it; false for a constructor that uploads real pixel
	// data right after init() returns, so that upload is not thrown away.
	void init(uint16_t w, uint16_t h, Rhi::TextureFormat format, bool zero_fill = true);

	// Runs 'draw' with this texture as the render target: on the core path it
	// brackets the draw in the RHI's offscreen pass (begin_offscreen /
	// transition / begin_pass / ... / end_pass / transition / submit_offscreen);
	// on the legacy path it just calls setup_gl(). The draw itself records into
	// the current command buffer (the program draw code does).
	void draw_to_self(const std::function<void()>& draw);

	// Implements surface.
	void do_blit(const Rectf& dst_rect,
	             const BlitData& texture,
	             float opacity,
	             BlendMode blend_mode,
	             const Vector3f& light) override;
	void do_blit_blended(const Rectf& dst_rect,
	                     const BlitData& texture,
	                     const BlitData& mask,
	                     const RGBColor& blend,
	                     const Vector3f& light) override;
	void do_blit_monochrome(const Rectf& dst_rect,
	                        const BlitData& texture,
	                        const RGBAColor& blend) override;
	void do_draw_line_strip(std::vector<DrawLineProgram::PerVertexData> vertices) override;
	void do_fill_rect(const Rectf& dst_rect, const RGBAColor& color, BlendMode blend_mode) override;

	// True if we own the texture, i.e. if we need to delete it.
	bool owns_texture_;

	BlitData blit_data_;

	// The RHI handle over the GL texture backing this image (the core path
	// draws through it via descriptor sets). Null on the legacy path and for
	// sub-textures (which borrow the parent's handle through blit_data_.texture).
	std::unique_ptr<Rhi::Texture> rhi_texture_;

	/// Pixel data, while the texture is locked
	std::unique_ptr<uint8_t[]> pixels_;

	DISALLOW_COPY_AND_ASSIGN(Texture);
};

#endif  // end of include guard: WL_GRAPHIC_TEXTURE_H
