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

#include "graphic/rhi/gl/gl_device.h"

#include <cassert>
#include <memory>
#include <unordered_map>

#include "base/wexception.h"

namespace Rhi {

namespace {

GlCoreDevice* g_device = nullptr;

GLenum to_gl(const BlendFactor factor) {
	switch (factor) {
	case BlendFactor::kZero:
		return GL_ZERO;
	case BlendFactor::kOne:
		return GL_ONE;
	case BlendFactor::kSrcAlpha:
		return GL_SRC_ALPHA;
	case BlendFactor::kOneMinusSrcAlpha:
		return GL_ONE_MINUS_SRC_ALPHA;
	}
	NEVER_HERE();
}

GLenum to_gl(const BlendOp op) {
	switch (op) {
	case BlendOp::kAdd:
		return GL_FUNC_ADD;
	case BlendOp::kReverseSubtract:
		return GL_FUNC_REVERSE_SUBTRACT;
	}
	NEVER_HERE();
}

GLenum to_gl(const CompareOp op) {
	switch (op) {
	case CompareOp::kLess:
		return GL_LESS;
	case CompareOp::kLessOrEqual:
		return GL_LEQUAL;
	case CompareOp::kAlways:
		return GL_ALWAYS;
	}
	NEVER_HERE();
}

GLenum to_gl(const PrimitiveTopology topology) {
	switch (topology) {
	case PrimitiveTopology::kTriangleList:
		return GL_TRIANGLES;
	case PrimitiveTopology::kLineList:
		return GL_LINES;
	}
	NEVER_HERE();
}

int component_count(const VertexFormat format) {
	switch (format) {
	case VertexFormat::kFloat:
		return 1;
	case VertexFormat::kVec2:
		return 2;
	case VertexFormat::kVec3:
		return 3;
	case VertexFormat::kVec4:
		return 4;
	}
	NEVER_HERE();
}

// A vertex attribute with its location resolved from the shader source.
struct ResolvedAttribute {
	GLint location;
	int components;
	uint32_t offset;
};

}  // namespace

// A non-owning handle over a GL texture name. graphic::Texture still owns the
// GL object and manages its upload/readback/deletion (WP-10 moves the draw
// path, not texture creation), so this class only carries the name and the
// dimensions for binding.
class GlCoreTexture : public Texture {
public:
	GlCoreTexture(const GLuint texture, const uint32_t width, const uint32_t height)
	   : gl_id_(texture), width_(width), height_(height) {
	}

	uint32_t width() const override {
		return width_;
	}
	uint32_t height() const override {
		return height_;
	}

	void upload(const void* /* pixels */) override {
		throw wexception("Rhi::GlCoreTexture::upload: texture upload stays in graphic::Texture "
		                 "for WP-10");
	}
	void read_back(uint8_t* /* pixels */) override {
		throw wexception("Rhi::GlCoreTexture::read_back: texture readback stays in graphic::Texture "
		                 "for WP-10");
	}

	GLuint gl_id() const {
		return gl_id_;
	}

private:
	GLuint gl_id_;
	uint32_t width_;
	uint32_t height_;

	DISALLOW_COPY_AND_ASSIGN(GlCoreTexture);
};

class GlCoreBuffer : public Buffer {
public:
	GlCoreBuffer(const uint32_t /* size */, const BufferUsage usage)
	   : target_(usage == BufferUsage::kUniform ? GL_UNIFORM_BUFFER : GL_ARRAY_BUFFER) {
		glGenBuffers(1, &gl_id_);
		if (gl_id_ == 0u) {
			throw wexception("Could not create GL buffer.");
		}
	}

	~GlCoreBuffer() override {
		if (gl_id_ != 0u) {
			glDeleteBuffers(1, &gl_id_);
		}
	}

	void update(const void* data, const uint32_t size) override {
		glBindBuffer(target_, gl_id_);
		glBufferData(target_, size, data, GL_DYNAMIC_DRAW);
	}

	GLuint gl_id() const {
		return gl_id_;
	}

	// Binds this buffer to a uniform-buffer binding point (glBindBufferBase).
	void bind_base(const uint32_t binding) const {
		glBindBufferBase(GL_UNIFORM_BUFFER, binding, gl_id_);
	}

private:
	GLuint gl_id_ = 0;
	GLenum target_;

	DISALLOW_COPY_AND_ASSIGN(GlCoreBuffer);
};

class GlCorePipeline : public Pipeline {
public:
	explicit GlCorePipeline(const PipelineDescriptor& desc)
	   : program_(),
	     topology_(desc.topology),
	     blend_(desc.blend),
	     depth_(desc.depth),
	     stride_(desc.vertex_layout.stride) {
		program_.build(desc.program_name);

		glGenVertexArrays(1, &vao_);
		if (vao_ == 0u) {
			throw wexception("Could not create GL vertex array object.");
		}

		// Resolve each attribute's location from the shader by name, so a
		// renamed attribute becomes a startup exception rather than a silently
		// mismatched VAO (the F7 lesson).
		for (const VertexAttribute& attribute : desc.vertex_layout.attributes) {
			attributes_.push_back({program_.attribute_location(attribute.name),
			                       component_count(attribute.format), attribute.offset});
		}

		// The authored GLSL 330 shaders carry no layout(binding=N) on their
		// samplers (that needs GLSL 4.2), so the program code supplies the
		// (name, unit) pairs and we bind them here, once, exactly as the
		// programs used glUniform1i before the RHI.
		glUseProgram(program_.object());
		for (const SamplerBinding& sampler : desc.samplers) {
			const GLint location = glGetUniformLocation(program_.object(), sampler.name.c_str());
			if (location >= 0) {
				glUniform1i(location, sampler.binding);
			}
		}
		if (desc.uniform_block.has_value()) {
			program_.bind_uniform_block(
			   desc.uniform_block->name, desc.uniform_block->binding, desc.uniform_block->expected_size);
		}
	}

	~GlCorePipeline() override {
		if (vao_ != 0u) {
			glDeleteVertexArrays(1, &vao_);
		}
	}

	GLuint program() const {
		return program_.object();
	}
	GLuint vao() const {
		return vao_;
	}
	uint32_t stride() const {
		return stride_;
	}
	const std::vector<ResolvedAttribute>& attributes() const {
		return attributes_;
	}
	const BlendState& blend() const {
		return blend_;
	}
	const DepthState& depth() const {
		return depth_;
	}
	PrimitiveTopology topology() const {
		return topology_;
	}

private:
	Gl::Program program_;
	GLuint vao_ = 0;
	PrimitiveTopology topology_;
	BlendState blend_;
	DepthState depth_;
	uint32_t stride_;
	std::vector<ResolvedAttribute> attributes_;

	DISALLOW_COPY_AND_ASSIGN(GlCorePipeline);
};

class GlCoreDescriptorSet : public DescriptorSet {
public:
	explicit GlCoreDescriptorSet(const GlCorePipeline& /* pipeline */) {
	}

	void set_texture(const uint32_t binding, const Texture* texture) override {
		const GlCoreTexture* gl_texture = static_cast<const GlCoreTexture*>(texture);
		textures_[binding] = gl_texture == nullptr ? 0u : gl_texture->gl_id();
	}

	// 'offset' and 'size' are ignored: GL binds the whole buffer at the binding
	// point (the per-program block is written in place and bound whole, as it is
	// today). They exist so the call shape matches the Vulkan descriptor range.
	void set_uniform_buffer(const uint32_t binding,
	                        const Buffer* buffer,
	                        uint32_t /* offset */,
	                        uint32_t /* size */) override {
		uniform_buffer_ = UniformBinding{binding, static_cast<const GlCoreBuffer*>(buffer)};
	}

	// Applies the recorded bindings (textures + uniform buffer).
	void bind() const {
		for (const auto& [unit, texture] : textures_) {
			Gl::State::instance().bind(GL_TEXTURE0 + unit, texture);
		}
		if (uniform_buffer_.buffer != nullptr) {
			uniform_buffer_.buffer->bind_base(uniform_buffer_.binding);
		}
	}

private:
	struct UniformBinding {
		uint32_t binding = 0;
		const GlCoreBuffer* buffer = nullptr;
	};

	std::unordered_map<uint32_t, GLuint> textures_;
	UniformBinding uniform_buffer_;

	DISALLOW_COPY_AND_ASSIGN(GlCoreDescriptorSet);
};

class GlCoreCommandBuffer : public CommandBuffer {
public:
	explicit GlCoreCommandBuffer(GlCoreDevice& device) : device_(device) {
	}

	void begin_pass(const Texture* target, const PassClear& clear) override {
		if (target == nullptr) {
			Gl::State::instance().bind_framebuffer(0, 0);
		} else {
			const GlCoreTexture* texture = static_cast<const GlCoreTexture*>(target);
			Gl::State::instance().bind_framebuffer(
			   device_.offscreen_framebuffer(), texture->gl_id());
			glViewport(0, 0, texture->width(), texture->height());
		}
		if (clear.clear) {
			glClearColor(clear.r, clear.g, clear.b, clear.a);
			glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
		}
	}

	void set_viewport(const Recti& viewport) override {
		glViewport(viewport.x, viewport.y, viewport.w, viewport.h);
	}

	void set_scissor(const Recti& rect) override {
		glScissor(rect.x, rect.y, rect.w, rect.h);
		glEnable(GL_SCISSOR_TEST);
	}

	void disable_scissor() override {
		glDisable(GL_SCISSOR_TEST);
	}

	void bind_pipeline(const Pipeline* pipeline) override {
		const GlCorePipeline* gl_pipeline = static_cast<const GlCorePipeline*>(pipeline);
		glUseProgram(gl_pipeline->program());
		glBindVertexArray(gl_pipeline->vao());
		apply_blend(gl_pipeline->blend());
		apply_depth(gl_pipeline->depth());
		current_pipeline_ = gl_pipeline;
		attributes_dirty_ = true;
	}

	void bind_descriptor_set(const DescriptorSet* set) override {
		static_cast<const GlCoreDescriptorSet*>(set)->bind();
	}

	void bind_vertex_buffer(const Buffer* buffer) override {
		glBindBuffer(GL_ARRAY_BUFFER, static_cast<const GlCoreBuffer*>(buffer)->gl_id());
		attributes_dirty_ = true;
	}

	void draw(const uint32_t vertex_offset, const uint32_t vertex_count) override {
		// Set up the vertex attributes lazily, once per (pipeline, buffer)
		// change. The pointers are captured into the pipeline's VAO, which
		// bind_pipeline already bound.
		if (attributes_dirty_) {
			for (const ResolvedAttribute& attribute : current_pipeline_->attributes()) {
				glEnableVertexAttribArray(attribute.location);
				glVertexAttribPointer(attribute.location, attribute.components, GL_FLOAT, GL_FALSE,
				                      current_pipeline_->stride(),
				                      reinterpret_cast<void*>(static_cast<uintptr_t>(attribute.offset)));
			}
			attributes_dirty_ = false;
		}
		glDrawArrays(to_gl(current_pipeline_->topology()), vertex_offset, vertex_count);
	}

	void transition(const Texture* texture, const TextureLayout layout) override {
		// GL has no image-layout transitions. The one thing that matters is
		// unbinding a texture before it becomes a render target, which is what
		// Gl::State::bind_framebuffer does anyway; making it explicit here keeps
		// the call-site shape the Vulkan backend needs.
		if (layout == TextureLayout::kColorAttachment) {
			Gl::State::instance().unbind_texture_if_bound(
			   static_cast<const GlCoreTexture*>(texture)->gl_id());
		}
	}

	void end_pass() override {
	}

private:
	void apply_blend(const BlendState& blend) {
		if (blend.src_factor == BlendFactor::kOne && blend.dst_factor == BlendFactor::kZero &&
		    blend.op == BlendOp::kAdd) {
			glDisable(GL_BLEND);
			return;
		}
		glEnable(GL_BLEND);
		glBlendFunc(to_gl(blend.src_factor), to_gl(blend.dst_factor));
		glBlendEquation(to_gl(blend.op));
	}

	void apply_depth(const DepthState& depth) {
		if (depth.test_enabled) {
			glEnable(GL_DEPTH_TEST);
		} else {
			glDisable(GL_DEPTH_TEST);
		}
		glDepthMask(depth.write_enabled ? GL_TRUE : GL_FALSE);
		glDepthFunc(to_gl(depth.compare_op));
	}

	GlCoreDevice& device_;
	const GlCorePipeline* current_pipeline_ = nullptr;
	bool attributes_dirty_ = false;

	DISALLOW_COPY_AND_ASSIGN(GlCoreCommandBuffer);
};

GlCoreDevice::GlCoreDevice(GLint /* max_texture_size */) {
	g_device = this;
	glGenFramebuffers(1, &offscreen_framebuffer_);
}

GlCoreDevice::~GlCoreDevice() {
	if (offscreen_framebuffer_ != 0u) {
		glDeleteFramebuffers(1, &offscreen_framebuffer_);
	}
	g_device = nullptr;
}

Backend GlCoreDevice::backend() const {
	return Backend::kOpenGLCore;
}

std::unique_ptr<CommandBuffer> GlCoreDevice::begin_frame() {
	std::unique_ptr<CommandBuffer> command_buffer(new GlCoreCommandBuffer(*this));
	current_ = command_buffer.get();
	return command_buffer;
}

void GlCoreDevice::end_frame(std::unique_ptr<CommandBuffer> /* command_buffer */) {
	current_ = nullptr;
}

std::unique_ptr<CommandBuffer> GlCoreDevice::begin_offscreen() {
	std::unique_ptr<CommandBuffer> command_buffer(new GlCoreCommandBuffer(*this));
	current_ = command_buffer.get();
	return command_buffer;
}

void GlCoreDevice::submit_offscreen(std::unique_ptr<CommandBuffer> /* command_buffer */) {
	// GL executes immediately, so the results are already visible.
	current_ = nullptr;
}

void GlCoreDevice::read_back_swapchain(uint8_t* /* pixels */) {
	throw wexception("Rhi::GlCoreDevice::read_back_swapchain: swapchain readback is WP-18");
}

std::unique_ptr<Texture> GlCoreDevice::create_texture(const TextureDescriptor& /* desc */) {
	throw wexception("Rhi::GlCoreDevice::create_texture: texture creation stays in graphic::Texture "
	                 "for WP-10");
}

std::unique_ptr<Texture> GlCoreDevice::create_texture_view(Texture& /* parent */,
                                                           const Recti& /* subrect */) {
	throw wexception("Rhi::GlCoreDevice::create_texture_view: sub-textures are modelled by BlitData");
}

std::unique_ptr<Buffer> GlCoreDevice::create_buffer(const uint32_t size, const BufferUsage usage) {
	return std::unique_ptr<Buffer>(new GlCoreBuffer(size, usage));
}

std::unique_ptr<Pipeline> GlCoreDevice::create_pipeline(const PipelineDescriptor& desc) {
	return std::unique_ptr<Pipeline>(new GlCorePipeline(desc));
}

std::unique_ptr<DescriptorSet> GlCoreDevice::create_descriptor_set(const Pipeline& pipeline) {
	return std::unique_ptr<DescriptorSet>(
	   new GlCoreDescriptorSet(static_cast<const GlCorePipeline&>(pipeline)));
}

CommandBuffer& GlCoreDevice::current_command_buffer() {
	assert(current_ != nullptr);
	return *current_;
}

GLuint GlCoreDevice::offscreen_framebuffer() const {
	return offscreen_framebuffer_;
}

Device& device() {
	if (g_device == nullptr) {
		throw wexception("Rhi::device(): no GL-core device (did you start with --renderer=glcore?)");
	}
	return *g_device;
}

CommandBuffer& command_buffer() {
	if (g_device == nullptr) {
		throw wexception("Rhi::command_buffer(): no GL-core device (did you start with "
		                 "--renderer=glcore?)");
	}
	return g_device->current_command_buffer();
}

std::unique_ptr<Texture> wrap_gl_texture(const GLuint texture,
                                         const uint32_t width,
                                         const uint32_t height) {
	return std::unique_ptr<Texture>(new GlCoreTexture(texture, width, height));
}

}  // namespace Rhi
