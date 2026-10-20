cmake_minimum_required(VERSION 3.22)
project(afmf-linux VERSION 1.3.1 LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_C_VISIBILITY_PRESET hidden)

option(AFMF_WERROR "Treat warnings as errors" ON)
option(AFMF_SANITIZE "Build with AddressSanitizer + UndefinedBehaviorSanitizer" OFF)

include(GNUInstallDirs)

# The layer only needs the headers: a Vulkan layer must never link the loader itself.
# The test host is an ordinary application and does link it.
find_package(Vulkan REQUIRED)
find_package(Threads REQUIRED)
if(NOT TARGET Vulkan::Headers)
    add_library(Vulkan::Headers INTERFACE IMPORTED)
    set_target_properties(Vulkan::Headers PROPERTIES INTERFACE_INCLUDE_DIRECTORIES "${Vulkan_INCLUDE_DIRS}")
endif()
# VK_EXT_swapchain_maintenance1 and VK_KHR_present_id are the oldest structures the layer carries
# for the application; VK_EXT_present_timing is optional (#ifdef in src/swapchain.c) so the
# distribution packages build against the headers Ubuntu 24.04 and Debian trixie ship.
if(DEFINED Vulkan_VERSION AND Vulkan_VERSION VERSION_LESS 1.3.250)
    message(FATAL_ERROR "Vulkan headers ${Vulkan_VERSION} are too old; 1.3.250 or newer is needed")
endif()

set(AFMF_WARNINGS
    -Wall -Wextra
    -Wshadow -Wconversion -Wsign-conversion -Wdouble-promotion
    -Wformat=2 -Werror=format-security
    -Wcast-qual -Wcast-align -Wpointer-arith -Wwrite-strings
    -Wstrict-prototypes -Wold-style-definition -Wmissing-prototypes -Wmissing-declarations
    -Wvla -Walloca -Wstack-protector
    -Wnull-dereference -Wimplicit-fallthrough
    -Werror=implicit-function-declaration -Werror=incompatible-pointer-types -Werror=int-conversion
    $<$<BOOL:${AFMF_WERROR}>:-Werror>
)
set(AFMF_SANITIZE_FLAGS -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=all)

# ---- shaders: GLSL -> SPIR-V -> one embedded header --------------------------------------------

find_program(AFMF_GLSLANG NAMES glslangValidator glslang)
if(NOT AFMF_GLSLANG)
    message(FATAL_ERROR "glslang not found; it compiles the layer's shaders (Fedora: dnf install glslang)")
endif()

# Optional: every compiled module is also checked with spirv-val when SPIRV-Tools is installed.
find_program(AFMF_SPIRV_VAL spirv-val)
if(AFMF_SPIRV_VAL)
    set(AFMF_VALIDATE "${AFMF_SPIRV_VAL}" --target-env vulkan1.1)
else()
    set(AFMF_VALIDATE "${CMAKE_COMMAND}" -E echo "spirv-val not installed, skipping")
endif()
set(AFMF_SPV_DIR "${CMAKE_BINARY_DIR}/shaders")
set(AFMF_FFX_DIR "${CMAKE_SOURCE_DIR}/shaders/fidelityfx")
file(GLOB_RECURSE AFMF_FFX_HEADERS "${AFMF_FFX_DIR}/*.h")
set(AFMF_SPV_FILES)

# afmf_glsl(<output name> <source> [defines...]): compiles one compute shader to ${AFMF_SPV_DIR}/<name>.spv
function(afmf_glsl name source)
    set(spv "${AFMF_SPV_DIR}/${name}.spv")
    add_custom_command(
        OUTPUT "${spv}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${AFMF_SPV_DIR}"
        # Vulkan 1.1 / SPIR-V 1.3: the lowest version with subgroup ops, so the layer works
        # inside any application that asked for 1.1 or newer (see api_version in framegen.c).
        COMMAND "${AFMF_GLSLANG}" -V --target-env vulkan1.1 -S comp ${ARGN}
                "-I${AFMF_FFX_DIR}" -o "${spv}" "${source}"
        COMMAND ${AFMF_VALIDATE} "${spv}"
        DEPENDS "${source}" ${AFMF_FFX_HEADERS}
        COMMENT "glslang ${name}"
        VERBATIM
    )
    set(AFMF_SPV_FILES ${AFMF_SPV_FILES} "${spv}" PARENT_SCOPE)
endfunction()

# FidelityFX Optical Flow passes, SDR permutation, exactly as the SDK's Vulkan backend built them.
set(AFMF_FFX_DEFINES -DFFX_GPU=1 -DFFX_GLSL=1 -DFFX_OPTICALFLOW_OPTION_HDR_COLOR_INPUT=0)
foreach(pass
        ffx_opticalflow_prepare_luma_pass
        ffx_opticalflow_compute_luminance_pyramid_pass
        ffx_opticalflow_generate_scd_histogram_pass
        ffx_opticalflow_compute_scd_divergence_pass
        ffx_opticalflow_compute_optical_flow_advanced_pass_v5
        ffx_opticalflow_filter_optical_flow_pass_v5
        ffx_opticalflow_scale_optical_flow_advanced_pass_v5)
    afmf_glsl(${pass} "${AFMF_FFX_DIR}/passes/${pass}.glsl" ${AFMF_FFX_DEFINES})
endforeach()
# The block search once more with the SAD on packed 16-bit pairs, for devices with shaderInt16.
afmf_glsl(ffx_opticalflow_compute_optical_flow_advanced_pass_v5_int16
    "${AFMF_FFX_DIR}/passes/ffx_opticalflow_compute_optical_flow_advanced_pass_v5.glsl"
    ${AFMF_FFX_DEFINES} -DAFMF_SAD_INT16=1)

# The interpolator, once per output format it can write (see src/framegen_format.c).
set(AFMF_INTERP "${CMAKE_SOURCE_DIR}/shaders/afmf_interpolate.comp")
afmf_glsl(afmf_interpolate_rgba8 "${AFMF_INTERP}" -DAFMF_OUT_FORMAT=rgba8 -DAFMF_SWAP_RB=0)
afmf_glsl(afmf_interpolate_rgba8_bgra "${AFMF_INTERP}" -DAFMF_OUT_FORMAT=rgba8 -DAFMF_SWAP_RB=1)
afmf_glsl(afmf_interpolate_rgb10a2 "${AFMF_INTERP}" -DAFMF_OUT_FORMAT=rgb10_a2 -DAFMF_SWAP_RB=0)
afmf_glsl(afmf_interpolate_rgba16f "${AFMF_INTERP}" -DAFMF_OUT_FORMAT=rgba16f -DAFMF_SWAP_RB=0)
afmf_glsl(afmf_interpolate_noformat "${AFMF_INTERP}" -DAFMF_OUT_FORMAT_NONE=1 -DAFMF_SWAP_RB=0)

afmf_glsl(afmf_batch_rgba8 "${AFMF_INTERP}" -DAFMF_BATCHED=1 -DAFMF_OUT_FORMAT=rgba8 -DAFMF_SWAP_RB=0)
afmf_glsl(afmf_batch_rgba8_bgra "${AFMF_INTERP}" -DAFMF_BATCHED=1 -DAFMF_OUT_FORMAT=rgba8 -DAFMF_SWAP_RB=1)
afmf_glsl(afmf_batch_rgb10a2 "${AFMF_INTERP}" -DAFMF_BATCHED=1 -DAFMF_OUT_FORMAT=rgb10_a2 -DAFMF_SWAP_RB=0)
afmf_glsl(afmf_batch_rgba16f "${AFMF_INTERP}" -DAFMF_BATCHED=1 -DAFMF_OUT_FORMAT=rgba16f -DAFMF_SWAP_RB=0)
afmf_glsl(afmf_batch_noformat "${AFMF_INTERP}" -DAFMF_BATCHED=1 -DAFMF_OUT_FORMAT_NONE=1 -DAFMF_SWAP_RB=0)

# The fused ingest (colour ring + luma in one read of the game's frame), once per ring format.
set(AFMF_INGEST "${CMAKE_SOURCE_DIR}/shaders/afmf_ingest.comp")
afmf_glsl(afmf_ingest_rgba8 "${AFMF_INGEST}" -DAFMF_OUT_FORMAT=rgba8)
afmf_glsl(afmf_ingest_rgb10a2 "${AFMF_INGEST}" -DAFMF_OUT_FORMAT=rgb10_a2)
afmf_glsl(afmf_ingest_rgba16f "${AFMF_INGEST}" -DAFMF_OUT_FORMAT=rgba16f)
afmf_glsl(afmf_ingest_noformat "${AFMF_INGEST}" -DAFMF_OUT_FORMAT_NONE=1)

# The half-resolution downsample for performance mode: 8-bit, or half floats for scRGB.
set(AFMF_DOWNSAMPLE "${CMAKE_SOURCE_DIR}/shaders/afmf_downsample.comp")
afmf_glsl(afmf_downsample_rgba8 "${AFMF_DOWNSAMPLE}" -DAFMF_OUT_FORMAT=rgba8)
afmf_glsl(afmf_downsample_rgba16f "${AFMF_DOWNSAMPLE}" -DAFMF_OUT_FORMAT=rgba16f)

set(AFMF_SPIRV_HEADER "${AFMF_SPV_DIR}/afmf_spirv.h")
list(JOIN AFMF_SPV_FILES "|" AFMF_SPV_JOINED)
add_custom_command(
    OUTPUT "${AFMF_SPIRV_HEADER}"
    COMMAND "${CMAKE_COMMAND}" "-DOUTPUT=${AFMF_SPIRV_HEADER}" "-DINPUTS=${AFMF_SPV_JOINED}"
            -P "${CMAKE_SOURCE_DIR}/cmake/embed_spirv.cmake"
    DEPENDS ${AFMF_SPV_FILES} "${CMAKE_SOURCE_DIR}/cmake/embed_spirv.cmake"
    COMMENT "Embedding SPIR-V"
    VERBATIM
)

# ---- the layer: libafmf-linux.so + afmf-linux.json ----------------------------------------------

add_library(afmf-linux SHARED
    src/config.c
    src/live_config.c
    src/live_runtime.c
    src/output_ring.c
    src/framegen.c
    src/framegen_descriptors.c
    src/framegen_dump.c
    src/framegen_format.c
    src/framegen_passes.c
    src/framegen_pipelines.c
    src/framegen_profiler.c
    src/framegen_record.c
    src/framegen_record_frame.c
    src/framegen_resources.c
    src/layer.c
    src/layer_device.c
    src/layer_device_fns.c
    src/layer_device_probe.c
    src/layer_device_queues.c
    src/layer_instance.c
    src/layer_queue.c
    src/layer_registry.c
    src/log.c
    src/swapchain.c
    src/swapchain_cadence.c
    src/swapchain_chain.c
    src/swapchain_create.c
    src/swapchain_destroy.c
    src/swapchain_present_chain.c
    src/swapchain_presenter.c
    src/swapchain_record.c
    src/swapchain_resources.c
    src/swapchain_threads.c
    src/swapchain_worker.c
    "${AFMF_SPIRV_HEADER}"
)
target_include_directories(afmf-linux PRIVATE src "${AFMF_SPV_DIR}")
target_link_libraries(afmf-linux PRIVATE Vulkan::Headers Threads::Threads)
target_compile_definitions(afmf-linux PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(afmf-linux PRIVATE ${AFMF_WARNINGS})
set_target_properties(afmf-linux PROPERTIES LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/layer")

# Two manifests from one template: the build tree's points at the built .so by absolute path (usable
# straight away with VK_ADD_IMPLICIT_LAYER_PATH=<build>/layer), the installed one at the installed .so.
set(AFMF_LAYER_LIBRARY_PATH "$<TARGET_FILE:afmf-linux>")
configure_file(layer/afmf-linux.json.in "${CMAKE_BINARY_DIR}/layer/afmf-linux.json.genex" @ONLY)
file(GENERATE OUTPUT "${CMAKE_BINARY_DIR}/layer/afmf-linux.json"
     INPUT "${CMAKE_BINARY_DIR}/layer/afmf-linux.json.genex")

set(AFMF_LAYER_LIBRARY_PATH "${CMAKE_INSTALL_FULL_LIBDIR}/libafmf-linux.so")
configure_file(layer/afmf-linux.json.in "${CMAKE_BINARY_DIR}/install/afmf-linux.json" @ONLY)

install(TARGETS afmf-linux LIBRARY DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(FILES "${CMAKE_BINARY_DIR}/install/afmf-linux.json"
        DESTINATION "${CMAKE_INSTALL_DATADIR}/vulkan/implicit_layer.d")

# ---- tests -----------------------------------------------------------------------------------

enable_testing()

# The loader dlcloses the driver at instance destruction; LeakSanitizer then reports the driver's
# once-initialised allocations (reachable from its static data) as leaks with no symbols. Keeping
# the libraries mapped makes the leak check mean something.
# An installed afmf-linux package (an implicit layer of the same name) would win over the build's
# manifest in VK_ADD_LAYER_PATH and the tests would exercise the package: implicit layers are
# disabled for the tests, except the device selector MESA_VK_DEVICE_SELECT needs.
set(AFMF_TEST_LOADER_ENV "VK_LOADER_DISABLE_DYNAMIC_LIBRARY_UNLOADING=1;VK_LOADER_LAYERS_DISABLE=~implicit~;VK_LOADER_LAYERS_ENABLE=VK_LAYER_MESA_device_select")

add_executable(afmf_headless
    tests/headless.c
    tests/headless_checks.c
    tests/headless_device.c
    tests/headless_frame.c
    tests/headless_instance.c
    tests/headless_swapchain.c
)
target_link_libraries(afmf_headless PRIVATE Vulkan::Vulkan xcb)
target_compile_definitions(afmf_headless PRIVATE VK_USE_PLATFORM_XCB_KHR)
target_compile_definitions(afmf_headless PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(afmf_headless PRIVATE ${AFMF_WARNINGS})


add_executable(gsf_queue_depth tests/queue_depth.c)
target_include_directories(gsf_queue_depth PRIVATE src)
target_compile_options(gsf_queue_depth PRIVATE -UNDEBUG)
add_test(NAME queue_depth COMMAND gsf_queue_depth)

add_executable(gsf_presentation_deadline tests/presentation_deadline.c)
target_include_directories(gsf_presentation_deadline PRIVATE src)
target_compile_options(gsf_presentation_deadline PRIVATE -UNDEBUG)
add_test(NAME presentation_deadline COMMAND gsf_presentation_deadline)

add_executable(gsf_output_plan tests/output_plan.c)
target_include_directories(gsf_output_plan PRIVATE src)
target_compile_options(gsf_output_plan PRIVATE -UNDEBUG)
add_test(NAME output_plan COMMAND gsf_output_plan)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(NAME vulkan_x11_matrix COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/matrix.py --build ${CMAKE_BINARY_DIR})
set_tests_properties(vulkan_x11_matrix PROPERTIES TIMEOUT 900)

if(NOT DEFINED GSF_SOURCE_COMMIT)
    set(GSF_SOURCE_COMMIT "development-unqualified")
endif()
configure_file(build_info.json.in "${CMAKE_BINARY_DIR}/BUILD_INFO.json" @ONLY)

add_executable(gsf_present_wait_contract tests/present_wait_contract.c)
target_include_directories(gsf_present_wait_contract PRIVATE src)
target_link_libraries(gsf_present_wait_contract PRIVATE Vulkan::Headers Threads::Threads)
target_compile_definitions(gsf_present_wait_contract PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(gsf_present_wait_contract PRIVATE ${AFMF_WARNINGS} -UNDEBUG)
add_test(NAME present_wait_contract COMMAND gsf_present_wait_contract)

add_executable(gsf_live_config tests/live_config.c src/live_config.c)
target_include_directories(gsf_live_config PRIVATE src)
target_compile_definitions(gsf_live_config PRIVATE _POSIX_C_SOURCE=200809L)
target_compile_options(gsf_live_config PRIVATE ${AFMF_WARNINGS} -UNDEBUG)
add_test(NAME live_config COMMAND gsf_live_config)
add_test(NAME vulkan_live_pacing COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/live_pacing.py --build ${CMAKE_BINARY_DIR})
set_tests_properties(vulkan_live_pacing PROPERTIES TIMEOUT 180)

add_test(NAME vulkan_pipeline_metrics COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/source_preservation.py --build ${CMAKE_BINARY_DIR})
set_tests_properties(vulkan_pipeline_metrics PROPERTIES TIMEOUT 300)

add_test(NAME vulkan_live_batch COMMAND ${Python3_EXECUTABLE} ${CMAKE_SOURCE_DIR}/tests/live_pacing.py --build ${CMAKE_BINARY_DIR} --out ${CMAKE_BINARY_DIR}/Testing/live-batch)
set_tests_properties(vulkan_live_batch PROPERTIES TIMEOUT 180 ENVIRONMENT "GSF_NEXT_WARP_BATCH=1")

add_executable(gsf_renderer_choice tests/renderer_choice.c)
target_include_directories(gsf_renderer_choice PRIVATE src)
target_compile_options(gsf_renderer_choice PRIVATE ${AFMF_WARNINGS} -UNDEBUG)
add_test(NAME renderer_choice COMMAND gsf_renderer_choice)
