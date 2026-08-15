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

#ifndef WL_GRAPHIC_RHI_GL_GL_DEVICE_H
#define WL_GRAPHIC_RHI_GL_GL_DEVICE_H

#include <map>
#include <memory>
#include <utility>

#include "graphic/gl/system_headers.h"
#include "graphic/gl/utils.h"
#include "graphic/rhi/rhi.h"

// The GL-core implementation of the RHI (renderer modernization plan, WP-10).
//
// Unlike Vulkan (WP-12 onwards), GL executes draw commands immediately, so the
// "command buffer" here is a thin facade: every method issues the matching gl*
// call right away, consulting the shared Gl::State cache for texture and
// framebuffer binding dedup. The resource classes wrap their GL counterparts:
// a Texture wraps a GL texture name, a Buffer wraps a VBO/UBO, a Pipeline wraps
// a Gl::Program plus its VAO and the blend/depth state, and a DescriptorSet
// wraps the texture units and the uniform-buffer binding point the shader
// reads.
//
// Only the draw path and the state cache are moved behind this interface
// (WP-10's scope). Texture *creation* and readback stay in graphic::Texture /
// the screenshot path for now, exactly as WP-7 left them, so the Texture here
// is a non-owning handle over a GL name that graphic::Texture still owns.
namespace Rhi {

class GlCoreDevice;
class GlCoreTexture;
class GlCoreBuffer;
class GlCorePipeline;
class GlCoreDescriptorSet;
class GlCoreCommandBuffer;

// A non-owning RHI texture handle over an existing GL texture name created by
// graphic::Texture (which retains ownership of the GL object). The core path
// stores this in BlitData.texture; the GL texture's lifecycle stays with
// graphic::Texture because WP-10 moves the draw path, not texture creation.
std::unique_ptr<Texture> wrap_gl_texture(GLuint texture, uint32_t width, uint32_t height);

class GlCoreDevice : public Device {
public:
	GlCoreDevice();
	~GlCoreDevice() override;

	Backend backend() const override;
	std::unique_ptr<CommandBuffer> begin_frame() override;
	void end_frame(std::unique_ptr<CommandBuffer> command_buffer) override;
	std::unique_ptr<CommandBuffer> begin_offscreen() override;
	void submit_offscreen(std::unique_ptr<CommandBuffer> command_buffer) override;
	void read_back_swapchain(uint8_t* pixels) override;
	std::unique_ptr<Texture> create_texture(const TextureDescriptor& desc) override;
	std::unique_ptr<Texture> create_texture_view(Texture& parent, const Recti& subrect) override;
	std::unique_ptr<Buffer> create_buffer(uint32_t size, BufferUsage usage) override;
	std::unique_ptr<Pipeline> create_pipeline(const PipelineDescriptor& desc) override;
	std::unique_ptr<DescriptorSet> create_descriptor_set(const Pipeline& pipeline) override;

	// Internal (not part of the Rhi contract): the offscreen FBO used for
	// render-to-texture.
	GLuint offscreen_framebuffer() const;

	// Internal: returns (creating and populating on first use) the VAO that
	// captures the attribute layout of 'pipeline' over 'buffer'. A VAO captures
	// both the layout and the bound GL_ARRAY_BUFFER, so the cache is keyed on
	// the pair; the renderer uses roughly ten distinct pairs, so the attribute
	// setup happens once per process instead of once per pipeline bind (C5).
	GLuint vao_for(const GlCorePipeline& pipeline, const GlCoreBuffer& buffer);

private:
	GLuint offscreen_framebuffer_ = 0;
	// Keyed on the RHI base types (complete here) so the concrete GL classes
	// can stay out of this header; downcast inside vao_for().
	std::map<std::pair<const Pipeline*, const Buffer*>, GLuint> vao_cache_;

	DISALLOW_COPY_AND_ASSIGN(GlCoreDevice);
};

}  // namespace Rhi

#endif  // end of include guard: WL_GRAPHIC_RHI_GL_GL_DEVICE_H
