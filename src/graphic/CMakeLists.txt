add_subdirectory(animation)
add_subdirectory(gl/test)
add_subdirectory(styles)
add_subdirectory(text)

# The render hardware interface (RHI): the backend-neutral drawing contract
# behind which the GL-core and Vulkan backends live (renderer modernization
# plan, WP-9). device.{h,cc} holds the backend-neutral device registry; the
# per-backend implementations (gl/..., vulkan/...) arrive in their own work
# packages.
wl_library(graphic_rhi
  SRCS
    rhi/device.cc
    rhi/device.h
    rhi/pipeline_catalog.cc
    rhi/pipeline_catalog.h
    rhi/rhi.h
  USES_ATOMIC
  DEPENDS
    base_exceptions
    base_geometry
    base_macros
)

# The GL-core implementation of the RHI (WP-10).
wl_library(graphic_rhi_gl
  SRCS
    rhi/gl/gl_device.cc
    rhi/gl/gl_device.h
  DEPENDS
    base_exceptions
    graphic_gl_utils
    graphic_rhi
)

# The Vulkan bootstrap (WP-12) and its render pass / pipeline cache (WP-14):
# instance, device, surface, swapchain, the screen render pass with its depth
# attachment, and the twelve pre-built pipelines. Not a Rhi::Device yet
# (WP-15 converts it), so the eight programs keep their GL paths; the pipeline
# cache consumes the pipeline catalog and the bindings manifest. The library
# exists in every configuration so 'graphic' can depend on it
# unconditionally; without OPTION_BUILD_VULKAN the .cc files compile to stubs
# that throw (see the WL_BUILD_VULKAN splits), and only then is volk linked.
wl_library(graphic_rhi_vulkan
  SRCS
    rhi/vulkan/vulkan_device.cc
    rhi/vulkan/vulkan_device.h
    rhi/vulkan/vulkan_manifest.cc
    rhi/vulkan/vulkan_manifest.h
    rhi/vulkan/vulkan_pipeline_cache.cc
    rhi/vulkan/vulkan_pipeline_cache.h
  DEPENDS
    base
    base_exceptions
    base_macros
    graphic_rhi
    io_fileread
    io_filesystem
  USES_SDL2
)
if (OPTION_BUILD_VULKAN)
  target_link_libraries(graphic_rhi_vulkan third_party_volk)
endif()

# SPIR-V generation for the eight shader programs (renderer modernization
# plan, WP-13). glslangValidator compiles the Vulkan dialect emitted by
# wl_spirv_compiler to SPIR-V at build time - never at runtime - and the
# default build fails if the committed artifacts in data/shaders/vulkan/ do
# not match their sources byte-for-byte. `ninja update_spirv` regenerates the
# committed artifacts after a shader edit. The toolchain is a build-time
# dependency: without glslangValidator the game still builds and runs, only
# the regeneration/check is skipped (with a warning).
if (OPTION_BUILD_VULKAN)
  find_program(GLSLANG_VALIDATOR_EXECUTABLE glslangValidator)
  find_program(SPIRV_VAL_EXECUTABLE spirv-val)

  if (NOT GLSLANG_VALIDATOR_EXECUTABLE)
    message(WARNING "glslangValidator not found: the committed SPIR-V shaders will not be "
                    "regenerated or checked against their sources.")
  else()
    # The one list of the eight programs both shaders and CMake agree on.
    set(WL_SHADER_PROGRAMS blit dither draw_line fill_rect grid road terrain workarea)
    set(WL_SPIRV_OUTPUT_DIR "${CMAKE_BINARY_DIR}/shaders")
    set(WL_SPIRV_COMMITTED_DIR "${WL_ROOT_DIR}/data/shaders/vulkan")

    # Emits the Vulkan dialect of all eight programs plus the bindings.json
    # manifest. NOINSTALL: a build tool, not a shipped binary.
    wl_binary(wl_spirv_compiler
      NOINSTALL
      SRCS
        rhi/vulkan/spirv_compiler_main.cc
      DEPENDS
        base_exceptions
        graphic_shader_dialect
    )

    set(WL_SHADER_SOURCES "")
    set(WL_SPIRV_INTERMEDIATES "")
    set(WL_SPIRV_BUILD_ARTIFACTS "")
    foreach(program IN LISTS WL_SHADER_PROGRAMS)
      list(APPEND WL_SHADER_SOURCES
        "${WL_ROOT_DIR}/data/shaders/${program}.vp"
        "${WL_ROOT_DIR}/data/shaders/${program}.fp")
      list(APPEND WL_SPIRV_INTERMEDIATES
        "${WL_SPIRV_OUTPUT_DIR}/${program}.vert.glsl"
        "${WL_SPIRV_OUTPUT_DIR}/${program}.frag.glsl")
      list(APPEND WL_SPIRV_BUILD_ARTIFACTS
        "${WL_SPIRV_OUTPUT_DIR}/${program}.vert.spv"
        "${WL_SPIRV_OUTPUT_DIR}/${program}.frag.spv")
    endforeach()

    # Step 1: expand includes and emit the Vulkan dialect + binding manifest.
    add_custom_command(
      OUTPUT
        ${WL_SPIRV_INTERMEDIATES}
        "${WL_SPIRV_OUTPUT_DIR}/bindings.json"
      COMMAND wl_spirv_compiler
        --data-dir "${WL_ROOT_DIR}/data/shaders"
        --out-dir "${WL_SPIRV_OUTPUT_DIR}"
        --programs ${WL_SHADER_PROGRAMS}
      DEPENDS
        wl_spirv_compiler
        ${WL_SHADER_SOURCES}
        "${WL_ROOT_DIR}/data/shaders/terrain_variation.glsl"
      COMMENT "Emitting the Vulkan dialect and bindings for the eight shader programs"
      VERBATIM
    )

    # Step 2: compile each emitted stage to SPIR-V.
    foreach(program IN LISTS WL_SHADER_PROGRAMS)
      foreach(stage IN ITEMS vert frag)
        add_custom_command(
          OUTPUT "${WL_SPIRV_OUTPUT_DIR}/${program}.${stage}.spv"
          COMMAND "${GLSLANG_VALIDATOR_EXECUTABLE}"
            -V
            --target-env vulkan1.0
            -S ${stage}
            -o "${WL_SPIRV_OUTPUT_DIR}/${program}.${stage}.spv"
            "${WL_SPIRV_OUTPUT_DIR}/${program}.${stage}.glsl"
          DEPENDS "${WL_SPIRV_OUTPUT_DIR}/${program}.${stage}.glsl"
          COMMENT "Compiling ${program}.${stage} to SPIR-V"
          VERBATIM
        )
      endforeach()
    endforeach()

    # Step 3: verify the committed artifacts match the freshly built ones.
    add_custom_target(wl_spirv_check ALL
      COMMAND ${CMAKE_COMMAND}
        -DBUILD_DIR=${WL_SPIRV_OUTPUT_DIR}
        -DSOURCE_DIR=${WL_ROOT_DIR}
        -DSPIRV_VAL=${SPIRV_VAL_EXECUTABLE}
        "-DPROGRAMS=${WL_SHADER_PROGRAMS}"
        -P ${WL_ROOT_DIR}/cmake/CheckSpirvArtifacts.cmake
      DEPENDS ${WL_SPIRV_BUILD_ARTIFACTS} "${WL_SPIRV_OUTPUT_DIR}/bindings.json"
      COMMENT "Checking the committed SPIR-V artifacts against their sources"
      VERBATIM
    )

    # Convenience target: copy the freshly built artifacts over the committed
    # ones after a shader edit (then commit them).
    add_custom_target(update_spirv
      COMMAND ${CMAKE_COMMAND} -E make_directory ${WL_SPIRV_COMMITTED_DIR}
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        ${WL_SPIRV_BUILD_ARTIFACTS}
        "${WL_SPIRV_OUTPUT_DIR}/bindings.json"
        ${WL_SPIRV_COMMITTED_DIR}
      DEPENDS ${WL_SPIRV_BUILD_ARTIFACTS} "${WL_SPIRV_OUTPUT_DIR}/bindings.json"
      COMMENT "Updating the committed SPIR-V artifacts"
      VERBATIM
    )
  endif()
endif()

# TODO(sirver): Separate this directory into a base directory and one
# that is Widelands aware (can include logic stuff).

# Align and color

wl_library(graphic_toolbar_imageset
  SRCS
    toolbar_imageset.cc
    toolbar_imageset.h
  DEPENDS
    graphic
    graphic_surface
    scripting_lua_table
)

wl_library(graphic_align
  SRCS
    align.h
    align.cc
  DEPENDS
    base_geometry
    base_exceptions
)

wl_library(graphic_color
  SRCS
    color.h
    color.cc
  DEPENDS
    base
  USES_SDL2
)

wl_library(graphic_color_palette
  SRCS
    color_palette.h
    color_palette.cc
  DEPENDS
    base
    base_exceptions
    graphic_color
)

wl_library(graphic_playercolor
  SRCS
    playercolor.cc
    playercolor.h
  DEPENDS
    base
    graphic
    graphic_color
    graphic_surface
    io_filesystem
)


# Rendering utils

wl_library(graphic_build_texture_atlas
  SRCS
    build_texture_atlas.h
    build_texture_atlas.cc
  DEPENDS
    base
    graphic
    graphic_image_io
    graphic_surface
    graphic_texture_atlas
    io_filesystem
    io_profile
    logic_filesystem_constants
)

wl_library(graphic_image_io
  SRCS
    image_io.h
    image_io.cc
  USES_PNG
  USES_SDL2_IMAGE
  DEPENDS
    base_exceptions
    graphic_surface
    io_fileread
    io_filesystem
    io_stream
)

wl_library(graphic_sdl_utils
  SRCS
    sdl_utils.h
    sdl_utils.cc
  USES_SDL2
)

# The shader dialect emitter (renderer modernization plan, WP-6) and include
# expansion (backlog phase 1b), kept dependency-free (no SDL2/OpenGL) so the
# build-time SPIR-V tool (WP-13) can link it without dragging in the graphic
# stack. graphic_gl_utils re-exports it through gl/utils.h.
wl_library(graphic_shader_dialect
  SRCS
    gl/shader_dialect.cc
    gl/shader_dialect.h
  DEPENDS
    base_exceptions
)

wl_library(graphic_gl_utils
  SRCS
    gl/blit_data.h
    gl/coordinate_conversion.h
    gl/initialize.cc
    gl/initialize.h
    gl/system_headers.h
    gl/utils.cc
    gl/utils.h
  USES_SDL2
  USES_OPENGL
  DEPENDS
    base
    base_exceptions
    base_geometry
    base_macros
    graphic_rhi
    graphic_shader_dialect
    graphic_text
    io_fileread
    io_filesystem
)

wl_library(graphic_surface
  SRCS
    image.h
    screen.cc
    screen.h
    surface.cc
    surface.h
    texture.cc
    texture.h
  USES_SDL2
  DEPENDS
    base
    base_exceptions
    base_geometry
    base_macros
    graphic_color
    graphic_draw_programs
    graphic_gl_utils
    graphic_render_queue
    graphic_rhi
    graphic_rhi_gl
    graphic_sdl_utils
)

wl_library(graphic_texture_atlas
  SRCS
    texture_atlas.h
    texture_atlas.cc
  DEPENDS
    base_exceptions
    base_macros
    graphic_surface
)

wl_library(graphic_draw_programs
  SRCS
    blend_mode.h
    blit_mode.h
    gl/blit_program.cc
    gl/blit_program.h
    gl/draw_line_program.cc
    gl/draw_line_program.h
    gl/fill_rect_program.cc
    gl/fill_rect_program.h
  DEPENDS
    base_exceptions
    base_macros
    base_math
    base_geometry
    graphic_fields_to_draw
    graphic_gl_utils
    graphic_color
    graphic_rhi
)


# Logic-aware stuff

wl_library(graphic_render_queue
  SRCS
    render_queue.cc
    render_queue.h
  DEPENDS
    base_exceptions
    base_geometry
    base_macros
    graphic_color
    graphic_draw_programs
    graphic_fields_to_draw
    graphic_gl_utils
    graphic_rhi
    graphic_terrain_programs
    logic_map_objects
    logic_map_objects_description_maintainer
)

wl_library(graphic_game_renderer
  SRCS
    game_renderer.cc
    game_renderer.h
  DEPENDS
    graphic
    graphic_fields_to_draw
    graphic_render_queue
    graphic_surface
    logic
    logic_map_objects
)

wl_library(graphic_minimap_renderer
  SRCS
    minimap_renderer.cc
    minimap_renderer.h
  USES_SDL2
  DEPENDS
    base_geometry
    economy
    graphic_playercolor
    graphic_surface
    logic
    logic_constants
    logic_map
    logic_map_objects
    wui_mapviewpixelfunctions
)

wl_library(graphic_fields_to_draw
  SRCS
    gl/fields_to_draw.cc
    gl/fields_to_draw.h
  DEPENDS
    base
    base_geometry
    graphic
    graphic_gl_utils
    logic
    logic_constants
    logic_map_objects
    logic_widelands_geometry
    wui_mapviewpixelfunctions
)

wl_library(graphic_terrain_programs
  SRCS
    gl/grid_program.cc
    gl/grid_program.h
    gl/road_program.cc
    gl/road_program.h
    gl/terrain_program.cc
    gl/terrain_program.h
    gl/terrain_noise.h
    gl/dither_program.cc
    gl/dither_program.h
    gl/workarea_program.cc
    gl/workarea_program.h
  USES_ATOMIC
  DEPENDS
    base_exceptions
    base_geometry
    base_macros
    graphic
    graphic_fields_to_draw
    graphic_gl_utils
    graphic_image_io
    graphic_rhi
    graphic_surface
    io_filesystem
    logic
    logic_map_objects
    logic_map_objects_description_maintainer
    wui_mapviewpixelfunctions
)


# Font handler and text
wl_library(graphic_fonthandler
  SRCS
    font_handler.cc
    font_handler.h
  DEPENDS
    base
    base_macros
    graphic
    graphic_text
)

wl_library(graphic_text_layout
  SRCS
    text_layout.cc
    text_layout.h
  DEPENDS
    base
    graphic # TODO(Gunchleoc): For text_width safety only - rewrite that function
    graphic_align
    graphic_color
    graphic_surface
    graphic_styles
    graphic_text
    graphic_fonthandler
)

wl_library(graphic_wordwrap
  SRCS
    wordwrap.cc
    wordwrap.h
  USES_ICU
  DEPENDS
    base
    base_geometry
    graphic
    graphic_align
    graphic_color
    graphic_fonthandler
    graphic_text
    graphic_text_layout
    ui_basic
)


# Mouse cursor

wl_library(graphic_mouse_cursor
  SRCS
    mouse_cursor.cc
    mouse_cursor.h
  USES_SDL2
  DEPENDS
    base_geometry
    graphic
    graphic_image_io
    io_filesystem
)


# Misc high-level helper and convenience functions

wl_library(graphic_functions
  SRCS
    graphic_functions.cc
    graphic_functions.h
  DEPENDS
    graphic
    graphic_fonthandler
    graphic_text_layout
    widelands_options
)

# Base library

wl_library(graphic
  SRCS
    default_resolution.h
    graphic.cc
    graphic.h
    note_graphic_resolution_changed.h
    hyperlink.h
    image_cache.cc
    image_cache.h
    render_backend.cc
    render_backend.h
    style_manager.cc
    style_manager.h
    rendertarget.cc
    rendertarget.h
    road_segments.cc
    road_segments.h
  USES_SDL2
  DEPENDS
    base
    base_exceptions
    base_geometry
    base_macros
    base_scoped_timer
    base_times
    build_info
    graphic_align
    graphic_animation
    graphic_build_texture_atlas
    graphic_color
    graphic_draw_programs
    graphic_gl_utils
    graphic_image_io
    graphic_render_queue
    graphic_rhi_gl
    graphic_rhi_vulkan
    graphic_styles
    graphic_surface
    io_filesystem
    io_stream
    logic_widelands_geometry
    notifications
    scripting_lua_interface
    scripting_lua_table
    widelands_options
)

