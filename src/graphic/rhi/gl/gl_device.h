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

#include <memory>

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

// The active device. Only valid when Gl::backend() == Gl::Backend::kOpenGLCore;
// throws wexception otherwise. Created by Graphic::initialize.
Device& device();

// The command buffer currently being recorded (set by begin_frame /
// begin_offscreen, cleared by end_frame / submit_offscreen). Valid inside a
// frame or an offscreen submit, on the initializer thread.
CommandBuffer& command_buffer();

// A non-owning RHI texture handle over an existing GL texture name created by
// graphic::Texture (which retains ownership of the GL object). The core path
// stores this in BlitData.texture; the GL texture's lifecycle stays with
// graphic::Texture because WP-10 moves the draw path, not texture creation.
std::unique_ptr<Texture> wrap_gl_texture(GLuint texture, uint32_t width, uint32_t height);

class GlCoreDevice : public Device {
public:
	explicit GlCoreDevice(GLint max_texture_size);
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

	// Internal (not part of the Rhi contract): the command buffer being
	// recorded, and the offscreen FBO used for render-to-texture.
	CommandBuffer& current_command_buffer();
	GLuint offscreen_framebuffer() const;

private:
	GLuint offscreen_framebuffer_ = 0;
	CommandBuffer* current_ = nullptr;

	DISALLOW_COPY_AND_ASSIGN(GlCoreDevice);
};

}  // namespace Rhi

#endif  // end of include guard: WL_GRAPHIC_RHI_GL_GL_DEVICE_H
