# The headless Vulkan recording test (renderer modernization plan, WP-15):
# a windowless device records real VulkanCommandBuffer draws into an
# offscreen target and asserts on copied-back pixels. Links the Vulkan
# backend library directly (no SDL, no swapchain); skipped trivially when no
# Vulkan loader/ICD is installed.

wl_test(test_vulkan
  SRCS
    vulkan_test_main.cc
    test_command_recording.cc
  DEPENDS
    base_test
    graphic_rhi_vulkan
)
# volk is linked (and its <volk.h> include directory provided) outside
# DEPENDS: build_deps does not attribute angle-bracket includes to a target,
# which is why the backend library links it the same way.
target_link_libraries(test_vulkan third_party_volk)

# The test loads a real Vulkan ICD, whose own allocations leak under
# LeakSanitizer (libdrm/libdbus and an unsymbolized module, exactly the
# driver-noise class documented in Claude/DEV_HARNESS.md - the game's
# harness works around the same thing with LSAN_OPTIONS=detect_leaks=0 on
# NVIDIA). Leak detection is therefore off for this binary only; ASan's
# memory-access checks stay on.
set_tests_properties(test_vulkan PROPERTIES ENVIRONMENT "LSAN_OPTIONS=detect_leaks=0")
