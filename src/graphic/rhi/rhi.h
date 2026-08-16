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

#ifndef WL_GRAPHIC_RHI_RHI_H
#define WL_GRAPHIC_RHI_RHI_H

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "base/macros.h"
#include "base/rect.h"

// The render hardware interface (RHI): a thin, backend-neutral abstraction
// over the drawing the Widelands renderer needs. It is the seam that lets the
// GL-core and Vulkan backends share one drawing path (Phase C and D of
// Claude/RENDERER_MODERNIZATION_PLAN.md).
//
// This header is the *contract only*, written before any implementation
// (WP-9). The design decisions and the mapping of every existing GL call site
// onto it are in Claude/RHI_INTERFACE.md. Read that before WP-10 (GL core
// behind the RHI) or WP-12 (Vulkan bootstrap).
//
// Two properties are deliberately baked in and must not be relaxed:
//
// - OpenGL 2.1 is *not* a backend of this interface (decision 4 of the plan).
//   It remains the frozen legacy path outside the RHI; the interface assumes
//   VAOs, UBOs and descriptor sets with no degradation path.
//
// - The interface is backend-neutral: it does not include GL or Vulkan
//   headers, and it owns the backend-specific coordinate conventions
//   (clip space, framebuffer origin, depth range) so that callers do not
//   branch on the backend.
namespace Rhi {

// Backends implementing this interface. kVulkan is unused until WP-12; it is
// declared now so the "which backend" value is not GL-specific.
enum class Backend {
	kOpenGLCore,
	kVulkan,
};

// Texture storage formats. The renderer uploads exactly one today
// (src/graphic/texture.cc): RGBA8 for images. These enum values are not
// consumed until WP-16 (Vulkan texture upload); the GL backend creates
// textures in graphic::Texture, not here. Add formats only when a caller
// needs one.
enum class TextureFormat {
	kRGBA8,
};

// Texture addressing mode. Every texture in the tree uses clamp-to-edge
// (texture.cc); kept explicit so the contract does not assume it. Consumed by
// WP-16 (Vulkan sampler/upload); the GL backend always clamps.
enum class TextureWrap {
	kClampToEdge,
};

// Texture filtering. Every texture uses linear filtering (texture.cc:220).
// Consumed by WP-16 (Vulkan sampler); the GL backend always filters linearly.
enum class TextureFilter {
	kLinear,
	kNearest,
};

// The usage state a texture is in. GL has no such concept; Vulkan requires an
// explicit image-layout transition between the uses below. Making it a
// first-class part of the interface is the point of the exercise: a GL-grown
// interface would omit it and pay for the retrofit at every call site later
// (plan WP-9, leak 1). A texture is kUndefined on creation and must be
// transitioned before it is written or sampled. kUndefined is the initial
// state (WP-16) and kPresentSource the swapchain-present state (WP-17); the GL
// backend only ever observes kColorAttachment / kShaderReadOnly.
enum class TextureLayout {
	kUndefined,       // contents unspecified; the only valid destination
	kColorAttachment, // being written as a render target
	kShaderReadOnly,  // being sampled by a shader
	kPresentSource,   // a swapchain image ready to be presented
};

// Attribute component type. The renderer's vertices are all interleaved
// floats; no integer, normalized or half attributes are used.
enum class VertexFormat {
	kFloat,
	kVec2,
	kVec3,
	kVec4,
};

// The primitive topologies actually issued. Every program draws triangle
// lists except grid, which draws GL_LINES (grid_program.cc:67).
enum class PrimitiveTopology {
	kTriangleList,
	kLineList,
};

enum class BlendFactor {
	kZero,
	kOne,
	kSrcAlpha,
	kOneMinusSrcAlpha,
};

enum class BlendOp {
	kAdd,
	kReverseSubtract,
};

enum class CompareOp {
	kLess,
	kLessOrEqual,
	kAlways,
};

// One vertex attribute: which shader input it feeds, its format, and its byte
// offset within the interleaved vertex. 'name' is the shader attribute name
// (e.g. "attr_position"); the backend resolves its location from the shader
// (GLSL layout(location=N) for GL, the SPIR-V reflection for Vulkan), so the
// shader remains the single source of truth and a renamed attribute fails at
// pipeline creation instead of feeding the wrong location (the F7 lesson).
struct VertexAttribute {
	std::string name;
	VertexFormat format;
	uint32_t offset;
};

struct VertexLayout {
	uint32_t stride;  // bytes between consecutive vertices
	std::vector<VertexAttribute> attributes;
};

// Color-blend state of a pipeline. Alpha uses the same factors as color; none
// of the four blend states the renderer needs split the two. The RHI carries
// explicit state rather than the game's BlendMode enum, because BlendMode maps
// onto these states per program and not 1:1 (see Claude/RHI_INTERFACE.md).
struct BlendState {
	BlendFactor src_factor;
	BlendFactor dst_factor;
	BlendOp op;
};

// Depth state of a pipeline. The current RenderQueue enables the depth test
// and depth write for both the opaque and blended passes; the RHI does not
// silently change that (see the design notes).
struct DepthState {
	bool test_enabled;
	bool write_enabled;
	CompareOp compare_op;
};

// The four concrete blend states the renderer needs, as named constants. The
// caller (RenderQueue / the program draw code) chooses which a pipeline uses;
// the RHI does not interpret BlendMode.
constexpr BlendState kBlendOpaque{BlendFactor::kOne, BlendFactor::kZero, BlendOp::kAdd};
constexpr BlendState kBlendAlpha{
   BlendFactor::kSrcAlpha, BlendFactor::kOneMinusSrcAlpha, BlendOp::kAdd};
constexpr BlendState kBlendAdditive{BlendFactor::kOne, BlendFactor::kOne, BlendOp::kAdd};
constexpr BlendState kBlendReverseSubtract{
   BlendFactor::kOne, BlendFactor::kOne, BlendOp::kReverseSubtract};

// How a render pass treats its target's contents on entry. The screen pass
// clears color and depth every frame; the immediate render-to-texture path
// (minimap, font cache) draws over existing contents and does not clear.
struct PassClear {
	bool clear;
	float r;
	float g;
	float b;
	float a;
};

// A buffer's role, which decides where the backend keeps it and how it is
// written. Vertex data is uploaded whole-buffer per frame (Gl::Buffer today);
// uniform data carries the per-program block (Gl::UniformBuffer /
// PerProgramState).
enum class BufferUsage {
	kVertex,
	kUniform,
};

class Texture;
class Buffer;
class Pipeline;
class DescriptorSet;
class CommandBuffer;

// Describes a texture to create. Width and height are in texels.
struct TextureDescriptor {
	uint32_t width;
	uint32_t height;
	TextureFormat format;
	TextureWrap wrap = TextureWrap::kClampToEdge;
	TextureFilter filter = TextureFilter::kLinear;
};

// A sampler uniform a fragment shader declares, and the texture unit (GL) or
// binding (Vulkan) it reads from. The authored GLSL 330 sources have no
// layout(binding=N) qualifier (that needs GLSL 4.2 / ARB_shading_language_420pack),
// so the program code names the samplers explicitly in the PipelineDescriptor
// rather than the backend discovering them from the shader source.
struct SamplerBinding {
	uint32_t binding;
	std::string name;
};

// A uniform block a program reads, and the UBO binding point it is bound to.
// 'expected_size' is the block's std140 data size, which the GL backend asserts
// against GL_UNIFORM_BLOCK_DATA_SIZE so a drifted C++ struct fails at startup
// instead of reading garbage (the WP-8 / F3 lesson).
struct UniformBlockBinding {
	uint32_t binding;
	std::string name;
	uint32_t expected_size;
};

// Describes a pipeline (the RHI's "program"): the shader program, its vertex
// layout, its topology, and the blend/depth state it draws with.
//
// 'program_name' is the shared shader identifier — the basename in
// data/shaders/ (e.g. "blit", "terrain"). The backend owns dialect emission
// (GL) or SPIR-V lookup (Vulkan, WP-13); the caller never hands raw shader
// source to the RHI.
struct PipelineDescriptor {
	std::string program_name;
	VertexLayout vertex_layout;
	PrimitiveTopology topology;
	BlendState blend;
	DepthState depth;

	// The samplers the program reads and the binding each is on. For GL the
	// binding is the texture unit; the authored GLSL 330 shaders have no
	// layout(binding=N) qualifier (that needs GLSL 4.2), so the program code
	// supplies the (name, unit) pairs here exactly as it used glUniform1i to
	// before the RHI. The Vulkan backend derives these from the SPIR-V
	// set/binding decorations (WP-13) and only validates the program name.
	std::vector<SamplerBinding> samplers;

	// The uniform block the program reads, if any, and the UBO binding point it
	// is bound to (same semantics as 'samplers': the program code names it
	// because GLSL 330 cannot). 'expected_size' is the block's std140 data
	// size, asserted against the shader so a drifted struct fails at startup.
	std::optional<UniformBlockBinding> uniform_block;
};

// A whole texture image. A Texture is *also* a render target: there is no
// separate framebuffer object in the interface, matching texture.cc where the
// FBO is a singleton wrapped around the target texture (GlFramebuffer). A
// Texture may be a view over a sub-rect of a parent (create_texture_view),
// which is how the BlitData {parent, subrect} semantics of the texture atlas
// are represented.
class Texture {
public:
	Texture() : id_(next_id()) {
	}
	virtual ~Texture() = default;

	// A dense, small, backend-neutral identity for this texture, assigned in
	// creation order from an atomic counter (textures are created on both the
	// initializer and the UI thread). This is what backend-neutral code keys
	// batching and the render-queue sort key on, replacing the raw GL texture
	// name; it fits the sort key's 44-bit extra-value field (a pointer would
	// not). The backend still uses its own handle internally.
	[[nodiscard]] uint32_t id() const {
		return id_;
	}

	[[nodiscard]] virtual uint32_t width() const = 0;
	[[nodiscard]] virtual uint32_t height() const = 0;

	// Uploads the whole texture from 'pixels', tightly packed rows of
	// width() * height() texels in the format given at creation, in the RHI's
	// canonical row order (see the design notes). Callers that upload in
	// row-reversed order (Gl::swap_rows today) are told which order by the
	// contract; the backend compensates so that shader v=0 is the same texel
	// row on every backend. Unused until WP-16: the GL backend creates and
	// uploads textures in graphic::Texture (WP-10 moved only the draw path).
	virtual void upload(const void* pixels) = 0;

	// Reads the whole texture back into 'pixels', RGBA8, row-major, 4 *
	// width() * height() bytes. Backends without direct readback copy through
	// a host-visible buffer (WP-18). Reading a view reads the view's sub-rect.
	// Unused until WP-18 (Texture::lock stays in graphic::Texture for now).
	virtual void read_back(uint8_t* pixels) = 0;

	DISALLOW_COPY_AND_ASSIGN(Texture);

private:
	static uint32_t next_id() {
		static std::atomic<uint32_t> counter{0};
		return counter.fetch_add(1);
	}

	uint32_t id_;
};

// A GPU buffer.
//
// Transient-resource semantics (the bgfx transient-buffer model). update()
// allocates a fresh region in the device's per-frame arena and re-points this
// handle at it; the arena regions are only recycled at the next frame boundary.
// A buffer may therefore be updated and drawn repeatedly *within one command
// buffer*, and each recorded draw reads the region that was current when the
// draw was recorded — which is exactly how the eight programs use it today
// (one buffer, updated between draws). The GL backend implements this as a
// whole-buffer glBufferData re-allocation, which is immediate and already
// behaves correctly; the Vulkan backend (WP-15) must allocate from a ring
// arena that is reset at end_frame / submit_offscreen. This is the property
// that lets today's callers be correct under a recorded command-buffer model
// without being rewritten.
class Buffer {
public:
	Buffer() = default;
	virtual ~Buffer() = default;

	// Uploads 'size' bytes from 'data' as the whole buffer contents, into a
	// fresh per-frame region (see above).
	virtual void update(const void* data, uint32_t size) = 0;

	DISALLOW_COPY_AND_ASSIGN(Buffer);
};

// An immutable pipeline object (the RHI's "program"). It has no per-frame
// methods; drawing binds it and records draws into a CommandBuffer.
class Pipeline {
public:
	Pipeline() = default;
	virtual ~Pipeline() = default;

	DISALLOW_COPY_AND_ASSIGN(Pipeline);
};

// A descriptor set: the bundle of sampled textures and (optionally) one
// uniform buffer a pipeline reads. This replaces Gl::State::bind and the
// per-program glUniform*/UBO plumbing. A DescriptorSet is created for a
// specific Pipeline, whose shader is the single source of truth for which
// bindings exist; binding indices are the shader's declared binding points
// (explicit set/binding decorations in the Vulkan dialect, WP-13).
//
// The binding state is snapshotted at bind_descriptor_set, so a set may be
// re-pointed (set_texture / set_uniform_buffer) and re-bound freely between
// draws, and each recorded draw reads the bindings that were current when it
// was recorded — which is how the eight programs use it today (one set,
// re-pointed per draw). The Vulkan backend (WP-16) must allocate the set's
// storage from a per-frame descriptor pool at bind time so that mutating the
// set after a recorded draw does not disturb that draw.
class DescriptorSet {
public:
	DescriptorSet() = default;
	virtual ~DescriptorSet() = default;

	// Re-points texture binding 'binding' at 'texture' (or null to unbind).
	virtual void set_texture(uint32_t binding, const Texture* texture) = 0;

	// Re-points uniform-buffer binding 'binding' at a range of 'buffer'. The
	// per-program block is written in place before each draw, so 'offset' and
	// 'size' let the caller address a slice of a larger buffer.
	virtual void set_uniform_buffer(uint32_t binding,
	                                const Buffer* buffer,
	                                uint32_t offset,
	                                uint32_t size) = 0;

	DISALLOW_COPY_AND_ASSIGN(DescriptorSet);
};

// A recorded sequence of draw commands. Command recording happens on the UI
// thread (see the design notes); the backend submits the command buffer when
// the Device is told to. This is the RHI's "immediate mode" replacement: the
// caller records, the backend decides when the GPU runs it.
class CommandBuffer {
public:
	CommandBuffer() = default;
	virtual ~CommandBuffer() = default;

	// Begins a render pass. 'target' == nullptr means the swapchain back buffer
	// (the screen). Otherwise the pass renders into 'texture', which the caller
	// must have transitioned to kColorAttachment (or left kUndefined and then
	// transitioned). 'clear' says whether the attachment is cleared to the
	// given color on entry; depth is always cleared for a pass that has a
	// depth attachment.
	virtual void begin_pass(const Texture* target, const PassClear& clear) = 0;

	// Sets the viewport for the current pass, in pixels, in the canonical
	// framebuffer coordinate space (origin bottom-left, matching GL; the
	// Vulkan backend compensates for its top-left origin internally).
	virtual void set_viewport(const Recti& viewport) = 0;

	// Sets the scissor rectangle for the current pass, in pixels, same
	// coordinate space as set_viewport. Matches the ScopedScissor usage in
	// render_queue.cc for the terrain passes.
	virtual void set_scissor(const Recti& rect) = 0;

	// Disables the scissor test for the current pass. The terrain passes set a
	// scissor around each item and disable it again afterwards, so non-terrain
	// draws in the same pass are not clipped by a leftover rectangle.
	virtual void disable_scissor() = 0;

	// Binds a pipeline for subsequent draws.
	virtual void bind_pipeline(const Pipeline* pipeline) = 0;

	// Binds a descriptor set for subsequent draws.
	virtual void bind_descriptor_set(const DescriptorSet* set) = 0;

	// Binds a vertex buffer for subsequent draws (there is no index buffer;
	// every draw in the renderer is a glDrawArrays-style offset draw).
	virtual void bind_vertex_buffer(const Buffer* buffer) = 0;

	// Records a draw of 'vertex_count' vertices starting at 'vertex_offset' in
	// the currently bound vertex buffer, using the currently bound pipeline
	// and descriptor set.
	virtual void draw(uint32_t vertex_offset, uint32_t vertex_count) = 0;

	// Transitions 'texture' to 'layout'. This is the explicit resource-state
	// contract (plan WP-9, leak 1): the GL backend implements it as the
	// existing bind/unbind dance (Gl::State::bind_framebuffer /
	// unbind_texture_if_bound), Vulkan as an image-layout barrier.
	virtual void transition(const Texture* texture, TextureLayout layout) = 0;

	// Ends the current render pass.
	virtual void end_pass() = 0;

	DISALLOW_COPY_AND_ASSIGN(CommandBuffer);
};

// The backend device: the factory for all resources and the owner of the
// frame lifecycle and presentation. There is one Device per running game,
// chosen by --renderer.
class Device {
public:
	Device() = default;
	virtual ~Device() = default;

	[[nodiscard]] virtual Backend backend() const = 0;

	// Frame lifecycle. begin_frame acquires a swapchain image and returns a
	// command buffer recording into it; end_frame submits the command buffer
	// and presents. Everything the RenderQueue draws for one frame is recorded
	// between these two calls. The acquired image is the target of
	// begin_pass(nullptr, ...).
	virtual std::unique_ptr<CommandBuffer> begin_frame() = 0;
	virtual void end_frame(std::unique_ptr<CommandBuffer> command_buffer) = 0;

	// A short-lived command buffer for immediate render-to-texture, recorded
	// and submitted outside the frame (texture.cc's do_* methods; plan WP-9,
	// leak 2). submit_offscreen guarantees the recorded results are visible to
	// later sampling in the current frame — a fence wait is acceptable, and
	// WP-16b owns the optimization. This path must also work between frames,
	// since font and image-cache rendering happen on the initializer thread.
	virtual std::unique_ptr<CommandBuffer> begin_offscreen() = 0;
	virtual void submit_offscreen(std::unique_ptr<CommandBuffer> command_buffer) = 0;

	// Reads the swapchain back buffer (the last presented frame) back into
	// 'pixels' (RGBA8, row-major). This is the screenshot path (Screen::to_texture
	// today). Implemented in WP-18; declared here so the screenshot call site
	// is expressible in the contract.
	virtual void read_back_swapchain(uint8_t* pixels) = 0;

	// Factories. Resource creation may happen on the initializer thread (as
	// texture creation does today); see the threading contract in the design
	// notes. create_texture / create_texture_view are unused until WP-16 (the
	// GL backend creates textures in graphic::Texture and models sub-textures
	// via BlitData), and read_back_swapchain until WP-18.
	virtual std::unique_ptr<Texture> create_texture(const TextureDescriptor& desc) = 0;
	virtual std::unique_ptr<Texture>
	create_texture_view(Texture& parent, const Recti& subrect) = 0;
	// 'size' is an initial capacity hint: the buffer is created with at least
	// this much storage reserved so the first update() is not a surprise
	// re-allocation. Under transient-resource semantics update() may still
	// re-allocate a fresh region each frame, so the hint only avoids the first
	// one. Pass the steady-state size when it is known (the per-program uniform
	// blocks are exactly one struct), and **0 when it is not** — the per-frame
	// vertex buffers size themselves from the scene, so any constant would be a
	// guess that the first update() discards. Zero is a valid hint meaning
	// "reserve nothing", not a missing argument.
	virtual std::unique_ptr<Buffer> create_buffer(uint32_t size, BufferUsage usage) = 0;
	virtual std::unique_ptr<Pipeline> create_pipeline(const PipelineDescriptor& desc) = 0;
	virtual std::unique_ptr<DescriptorSet> create_descriptor_set(const Pipeline& pipeline) = 0;

	// Internal registry plumbing, not part of the drawing contract. The device
	// pushes/pops the command buffer it hands out from begin_frame /
	// begin_offscreen, so a nested offscreen submit inside a frame restores the
	// frame's buffer when it finishes. Rhi::command_buffer() reads the top.
	CommandBuffer& current_command_buffer();
	void push_command_buffer(CommandBuffer* command_buffer);
	void pop_command_buffer();

	DISALLOW_COPY_AND_ASSIGN(Device);

private:
	std::vector<CommandBuffer*> command_buffer_stack_;
};

// The upper bound of the RenderQueue's logical depth. The sort key is an
// integer in [0, kLogicalDepthMax]; larger means closer to the camera.
constexpr int kLogicalDepthMax = 65535;

// Maps the RenderQueue's logical depth to canonical clip-space depth in
// [-1, 1], larger logical depth closer to the camera. The RHI owns this
// mapping rather than the sort key (plan WP-9, leak 4); the Vulkan backend
// converts the canonical [-1, 1] range to [0, 1] in its shader emission
// (WP-13), so callers only ever deal in canonical depth.
inline float logical_to_clip_depth(const int logical_depth) {
	return 1.f - (2.f * static_cast<float>(logical_depth)) / kLogicalDepthMax;
}

}  // namespace Rhi

#endif  // end of include guard: WL_GRAPHIC_RHI_RHI_H
