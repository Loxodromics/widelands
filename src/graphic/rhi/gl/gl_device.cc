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

#include <memory>
#include <unordered_map>

#include "base/wexception.h"
#include "graphic/rhi/device.h"

namespace Rhi {

namespace {

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

GLint to_gl(const TextureFilter filter) {
	switch (filter) {
	case TextureFilter::kLinear:
		return GL_LINEAR;
	case TextureFilter::kNearest:
		return GL_NEAREST;
	}
	NEVER_HERE();
}

GLint to_gl(const TextureWrap wrap) {
	switch (wrap) {
	case TextureWrap::kClampToEdge:
		return GL_CLAMP_TO_EDGE;
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

// A GL texture name, either non-owning (wrap_gl_texture: graphic::Texture
// still owns the GL object and manages its upload/readback/deletion, WP-10)
// or owning (create_texture: the RHI created the GL object itself and is the
// only owner, WP-3a). 'format_' records what the texture was created with so
// upload() knows which GL enums apply.
class GlCoreTexture : public Texture {
public:
	GlCoreTexture(const GLuint texture,
	              const uint32_t width,
	              const uint32_t height,
	              const bool owns_texture,
	              const TextureFormat format)
	   : gl_id_(texture),
	     width_(width),
	     height_(height),
	     owns_texture_(owns_texture),
	     format_(format) {
	}

	~GlCoreTexture() override {
		if (owns_texture_ && gl_id_ != 0u) {
			glDeleteTextures(1, &gl_id_);
		}
	}

	uint32_t width() const override {
		return width_;
	}
	uint32_t height() const override {
		return height_;
	}

	void upload(const void* pixels) override {
		switch (format_) {
		case TextureFormat::kR16F:
			// Whole-image re-upload, matching graphic::Texture::unlock()'s
			// glTexImage2D pattern (texture.cc) rather than glTexSubImage2D:
			// upload() replaces the whole image, per the RHI contract.
			Gl::State::instance().bind(GL_TEXTURE0, gl_id_);
			glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, static_cast<GLsizei>(width_),
			             static_cast<GLsizei>(height_), 0, GL_RED, GL_FLOAT, pixels);
			return;
		case TextureFormat::kRGBA8:
			throw wexception(
			   "Rhi::GlCoreTexture::upload: kRGBA8 upload stays in graphic::Texture for WP-10");
		}
		NEVER_HERE();
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
	bool owns_texture_;
	TextureFormat format_;

	DISALLOW_COPY_AND_ASSIGN(GlCoreTexture);
};

class GlCoreBuffer : public Buffer {
public:
	GlCoreBuffer(const uint32_t size, const BufferUsage usage)
	   : target_(usage == BufferUsage::kUniform ? GL_UNIFORM_BUFFER : GL_ARRAY_BUFFER) {
		glGenBuffers(1, &gl_id_);
		if (gl_id_ == 0u) {
			throw wexception("Could not create GL buffer.");
		}
		// 'size' is an initial capacity hint; allocate it up front so the first
		// update() is not a surprise re-allocation (C4). Under transient-resource
		// semantics update() may still re-allocate each frame. Zero means "no
		// hint" — allocating nothing is cheaper than allocating a wrong guess
		// and immediately discarding it, which is what the per-frame vertex
		// buffers would otherwise do.
		if (size > 0u) {
			glBindBuffer(target_, gl_id_);
			glBufferData(target_, size, nullptr, GL_DYNAMIC_DRAW);
		}
	}

	~GlCoreBuffer() override {
		if (gl_id_ != 0u) {
			glDeleteBuffers(1, &gl_id_);
		}
	}

	void update(const void* data, const uint32_t size) override {
#ifndef NDEBUG
		// Transient-buffer semantics (rhi.h): an update must happen inside a
		// recorded command buffer so the draw that reads it sees the values
		// current at recording time. On GL this is invisible (immediate mode),
		// so fail loudly here rather than letting a mis-placed update pass
		// silently and then break under Vulkan.
		static_cast<void>(command_buffer());
#endif
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

		// Resolve each attribute's location from the shader by name, so a
		// renamed attribute becomes a startup exception rather than a silently
		// mismatched VAO (the F7 lesson). The actual VAO is created lazily per
		// (pipeline, buffer) pair by GlCoreDevice::vao_for (C5); the pipeline
		// only carries the resolved layout.
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

	~GlCorePipeline() override = default;

	GLuint program() const {
		return program_.object();
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
		apply_blend(gl_pipeline->blend());
		apply_depth(gl_pipeline->depth());
		current_pipeline_ = gl_pipeline;
		// The VAO is keyed on (pipeline, buffer); invalidate it so the next
		// draw re-resolves (and lazily creates) the right one.
		current_vao_ = 0;
	}

	void bind_descriptor_set(const DescriptorSet* set) override {
		static_cast<const GlCoreDescriptorSet*>(set)->bind();
	}

	void bind_vertex_buffer(const Buffer* buffer) override {
		const GlCoreBuffer* gl_buffer = static_cast<const GlCoreBuffer*>(buffer);
		glBindBuffer(GL_ARRAY_BUFFER, gl_buffer->gl_id());
		current_buffer_ = gl_buffer;
		current_vao_ = 0;
	}

	void draw(const uint32_t vertex_offset, const uint32_t vertex_count) override {
		// The VAO captures the attribute layout (from the pipeline) and the
		// bound GL_ARRAY_BUFFER (from the buffer), so it is created once per
		// (pipeline, buffer) pair by the device and simply rebound here (C5).
		// Both must have been bound: throw rather than dereference null, for
		// the same reason current_command_buffer() throws — an assert would
		// make this a silent null dereference in a release build.
		if (current_pipeline_ == nullptr) {
			throw wexception("Rhi::CommandBuffer::draw: no pipeline bound.");
		}
		if (current_buffer_ == nullptr) {
			throw wexception("Rhi::CommandBuffer::draw: no vertex buffer bound.");
		}
		if (current_vao_ == 0u) {
			current_vao_ = device_.vao_for(*current_pipeline_, *current_buffer_);
		}
		glBindVertexArray(current_vao_);
		glDrawArrays(to_gl(current_pipeline_->topology()), vertex_offset, vertex_count);
	}

	void transition(const Texture* texture, const TextureLayout layout) override {
		// GL has no image-layout transitions. The one thing that matters is
		// unbinding a texture before it becomes a render target, which is what
		// Gl::State::bind_framebuffer does anyway; making it explicit here keeps
		// the call-site shape the Vulkan backend needs. A texture that is only
		// ever uploaded to and sampled (never rendered into, e.g. WP-3a's kR16F
		// distance field) has no work to do here for any layout, including
		// kShaderReadOnly: that is by design, not an omission, since upload()
		// alone already leaves the GL texture in a sampleable state.
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
	const GlCoreBuffer* current_buffer_ = nullptr;
	GLuint current_vao_ = 0;

	DISALLOW_COPY_AND_ASSIGN(GlCoreCommandBuffer);
};

GlCoreDevice::GlCoreDevice() {
	set_device(this);
	glGenFramebuffers(1, &offscreen_framebuffer_);
}

GlCoreDevice::~GlCoreDevice() {
	if (offscreen_framebuffer_ != 0u) {
		glDeleteFramebuffers(1, &offscreen_framebuffer_);
	}
	for (const auto& entry : vao_cache_) {
		if (entry.second != 0u) {
			glDeleteVertexArrays(1, &entry.second);
		}
	}
	vao_cache_.clear();
	set_device(nullptr);
}

Backend GlCoreDevice::backend() const {
	return Backend::kOpenGLCore;
}

std::unique_ptr<CommandBuffer> GlCoreDevice::begin_frame() {
	std::unique_ptr<CommandBuffer> command_buffer(new GlCoreCommandBuffer(*this));
	push_command_buffer(command_buffer.get());
	return command_buffer;
}

void GlCoreDevice::end_frame(std::unique_ptr<CommandBuffer> /* command_buffer */) {
	pop_command_buffer();
}

std::unique_ptr<CommandBuffer> GlCoreDevice::begin_offscreen() {
	std::unique_ptr<CommandBuffer> command_buffer(new GlCoreCommandBuffer(*this));
	push_command_buffer(command_buffer.get());
	return command_buffer;
}

void GlCoreDevice::submit_offscreen(std::unique_ptr<CommandBuffer> /* command_buffer */) {
	// GL executes immediately, so the results are already visible. Popping the
	// stack restores whatever command buffer was recording before this
	// offscreen submit (a frame's, when the submit is nested inside one).
	pop_command_buffer();
}

void GlCoreDevice::read_back_swapchain(uint8_t* /* pixels */) {
	throw wexception("Rhi::GlCoreDevice::read_back_swapchain: swapchain readback is WP-18");
}

std::unique_ptr<Texture> GlCoreDevice::create_texture(const TextureDescriptor& desc) {
	if (desc.format != TextureFormat::kR16F) {
		throw wexception("Rhi::GlCoreDevice::create_texture: only kR16F is implemented (kRGBA8 "
		                 "creation stays in graphic::Texture for WP-10)");
	}

	GLuint gl_id = 0;
	glGenTextures(1, &gl_id);
	if (gl_id == 0u) {
		throw wexception("Rhi::GlCoreDevice::create_texture: could not create GL texture.");
	}

	Gl::State::instance().bind(GL_TEXTURE0, gl_id);
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, to_gl(desc.filter));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, to_gl(desc.filter));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, to_gl(desc.wrap));
	glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, to_gl(desc.wrap));

	// Allocate storage with undefined contents (matches TextureLayout::kUndefined,
	// the state a freshly created texture is in per the RHI contract); the first
	// real contents come from the caller's first upload(), not from here.
	glTexImage2D(GL_TEXTURE_2D, 0, GL_R16F, static_cast<GLsizei>(desc.width),
	             static_cast<GLsizei>(desc.height), 0, GL_RED, GL_FLOAT, nullptr);

	return std::unique_ptr<Texture>(
	   new GlCoreTexture(gl_id, desc.width, desc.height, /*owns_texture=*/true, desc.format));
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

GLuint GlCoreDevice::offscreen_framebuffer() const {
	return offscreen_framebuffer_;
}

GLuint GlCoreDevice::vao_for(const GlCorePipeline& pipeline, const GlCoreBuffer& buffer) {
	const std::pair<const Pipeline*, const Buffer*> key{&pipeline, &buffer};
	const auto existing = vao_cache_.find(key);
	if (existing != vao_cache_.end()) {
		return existing->second;
	}

	GLuint vao = 0;
	glGenVertexArrays(1, &vao);
	if (vao == 0u) {
		throw wexception("Could not create GL vertex array object.");
	}
	// glVertexAttribPointer records the currently-bound GL_ARRAY_BUFFER into the
	// VAO, so bind 'buffer' here to make the capture explicit rather than
	// relying on the caller having left it bound.
	glBindVertexArray(vao);
	glBindBuffer(GL_ARRAY_BUFFER, buffer.gl_id());
	for (const ResolvedAttribute& attribute : pipeline.attributes()) {
		glEnableVertexAttribArray(attribute.location);
		glVertexAttribPointer(attribute.location, attribute.components, GL_FLOAT, GL_FALSE,
		                      pipeline.stride(),
		                      reinterpret_cast<void*>(static_cast<uintptr_t>(attribute.offset)));
	}
	vao_cache_[key] = vao;
	return vao;
}

std::unique_ptr<Texture> wrap_gl_texture(const GLuint texture,
                                         const uint32_t width,
                                         const uint32_t height) {
	return std::unique_ptr<Texture>(
	   new GlCoreTexture(texture, width, height, /*owns_texture=*/false, TextureFormat::kRGBA8));
}

}  // namespace Rhi
