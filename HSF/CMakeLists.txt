cmake_minimum_required(VERSION 3.23)
project(hsf VERSION 0.3.0 LANGUAGES C CXX)

# HSF (Hunter's Sloppy Frames) by Capt-Hunter. DLSS Frame Generation for ReShade.
# One ReShade add-on, built twice: a 32-bit configure makes hsf.addon32, a 64-bit one makes hsf.addon64. See ../docs/.

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
add_compile_options(/utf-8 /W4 /permissive- /EHsc)

# ReShade add-on API headers (tag v6.0.0) and the Dear ImGui header that tag pins.
set(HSF_RESHADE_INCLUDE "${CMAKE_SOURCE_DIR}/third_party/reshade/include")
set(HSF_IMGUI_INCLUDE "${CMAKE_SOURCE_DIR}/third_party/imgui")

# MinHook (BSD-2-Clause), for the kernel hooks of the DX9 bridge's GPU fences (src/gpu_fence9.cpp).
set(HSF_MINHOOK "${CMAKE_SOURCE_DIR}/third_party/minhook")
add_library(hsf_minhook STATIC "${HSF_MINHOOK}/src/buffer.c" "${HSF_MINHOOK}/src/hook.c" "${HSF_MINHOOK}/src/trampoline.c"
  "${HSF_MINHOOK}/src/hde/hde32.c" "${HSF_MINHOOK}/src/hde/hde64.c")
target_include_directories(hsf_minhook SYSTEM PUBLIC "${HSF_MINHOOK}/include")
target_compile_options(hsf_minhook PRIVATE /W0)

add_library(hsf_addon SHARED src/hsf_addon.cpp src/ui.cpp src/dlssg_remote.cpp src/dx9_bridge.cpp src/gpu_fence9.cpp src/vk_bridge.cpp src/vk_hook.cpp src/score.cpp src/depth.cpp src/hud.cpp)
target_include_directories(hsf_addon SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include")
target_link_libraries(hsf_addon PRIVATE hsf_minhook)
# DLSS-G for D3D11 and DX9 games runs in hsf-dlssg-helper64.exe; the add-on hands it frames on the game's (or the DX9 bridge's) Direct3D 11 device.
target_link_libraries(hsf_addon PRIVATE d3d11 dxgi d3dcompiler version)
if(CMAKE_SIZEOF_VOID_P EQUAL 4)
  set_target_properties(hsf_addon PROPERTIES OUTPUT_NAME "hsf" SUFFIX ".addon32")
  # Stand-in 32-bit game that proves the DLSS-G helper dies with it (tools/helper_exit_test.ps1).
  add_executable(helper_exit_test tests/helper_exit_test.cpp)
else()
  set_target_properties(hsf_addon PROPERTIES OUTPUT_NAME "hsf" SUFFIX ".addon64")
  # The D3D12 routes (64-bit only): Streamline SDK 2.14 next to HSF. sl.interposer.dll is delay-loaded, so the add-on loads without it and
  # the bring-in route loads it from HSF_streamline first. Listed after dxgi and d3d11 so their own exports keep coming from the system DLLs.
  set(HSF_STREAMLINE "${CMAKE_SOURCE_DIR}/../Streamline SDK")
  # NVIDIA Optical Flow SDK 5.0 interface headers (NVIDIA's permissive licence, in each file). The engine itself is the driver's nvofapi64.dll.
  set(HSF_NVOF "${CMAKE_SOURCE_DIR}/third_party/nvof")
  target_sources(hsf_addon PRIVATE src/dlssg_core.cpp src/hdr_out.cpp src/compose.cpp src/frame_checks.cpp src/sl_game.cpp src/sl_bring.cpp src/debug_view.cpp src/ngx_input.cpp src/nvof_flow.cpp)
  target_compile_definitions(hsf_addon PRIVATE HSF_SL_GAME)
  target_include_directories(hsf_addon SYSTEM PRIVATE "${HSF_STREAMLINE}/include" "${HSF_NVOF}")
  target_link_libraries(hsf_addon PRIVATE "${HSF_STREAMLINE}/lib/x64/sl.interposer.lib" delayimp version)
  target_link_options(hsf_addon PRIVATE "/DELAYLOAD:sl.interposer.dll")
  # DLSS-G for D3D11 and DX9 games: this 64-bit helper runs next to Streamline's DLLs in <game>\HSF_streamline; the add-on starts it
  # (dlssg_remote.cpp, dlssg_link.h). A Windows-subsystem exe, so no console window ever takes the foreground (DLSS-G stops without focus).
  add_executable(hsf_dlssg_helper WIN32 src/dlssg_helper.cpp src/dlssg_core.cpp src/hdr_out.cpp src/compose.cpp src/frame_checks.cpp src/nvof_flow.cpp)
  set_target_properties(hsf_dlssg_helper PROPERTIES OUTPUT_NAME "hsf-dlssg-helper64")
  target_include_directories(hsf_dlssg_helper SYSTEM PRIVATE "${HSF_STREAMLINE}/include" "${HSF_NVOF}")
  target_link_libraries(hsf_dlssg_helper PRIVATE d3d12 dxgi d3dcompiler "${HSF_STREAMLINE}/lib/x64/sl.interposer.lib" delayimp version)
  target_link_options(hsf_dlssg_helper PRIVATE "/ENTRY:wmainCRTStartup" "/DELAYLOAD:sl.interposer.dll")
  # Offline test of the compose pass, no game needed (buildd\compose_test.exe).
  add_executable(compose_test tests/compose_test.cpp src/compose.cpp src/frame_checks.cpp)
  target_link_libraries(compose_test PRIVATE d3d12 dxgi d3dcompiler)
  target_link_options(compose_test PRIVATE "/ENTRY:wmainCRTStartup")
  # Offline test of the hooks on a game's Streamline: needs only a copy of sl.interposer.dll, which it hooks and calls.
  add_executable(sl_game_test tests/sl_game_test.cpp src/sl_game.cpp)
  target_include_directories(sl_game_test SYSTEM PRIVATE "${HSF_STREAMLINE}/include")
  target_link_libraries(sl_game_test PRIVATE d3d12 dxgi)
  target_link_options(sl_game_test PRIVATE "/ENTRY:wmainCRTStartup")
  # Offline test of the frame checks, no game needed.
  # A small program with no Streamline of its own: HSF must bring Streamline to it. Needs a folder with Streamline's DLLs and a GPU.
  add_executable(sl_bring_test tests/sl_bring_test.cpp src/sl_bring.cpp src/debug_view.cpp src/compose.cpp src/frame_checks.cpp src/nvof_flow.cpp)
  target_include_directories(sl_bring_test SYSTEM PRIVATE "${HSF_STREAMLINE}/include" "${HSF_NVOF}")
  target_link_libraries(sl_bring_test PRIVATE d3d12 dxgi d3dcompiler)
  target_link_options(sl_bring_test PRIVATE "/ENTRY:wmainCRTStartup")

  # The hook on this machine's real _nvngx.dll, and a stand-in DLSS call on a real device.
  add_executable(ngx_input_test tests/ngx_input_test.cpp src/ngx_input.cpp src/sl_game.cpp)
  target_include_directories(ngx_input_test SYSTEM PRIVATE "${HSF_STREAMLINE}/include")
  target_link_libraries(ngx_input_test PRIVATE d3d12 dxgi)

  # Offline test of the depth and motion view, no game needed.
  add_executable(debug_view_test tests/debug_view_test.cpp src/debug_view.cpp)
  target_link_libraries(debug_view_test PRIVATE d3d12 dxgi d3dcompiler)
  target_link_options(debug_view_test PRIVATE "/ENTRY:wmainCRTStartup")

  # Offline test of NVIDIA Optical Flow as a motion source: a picture moving by a known amount (build\x64\nvof_test.exe). Needs an RTX card.
  add_executable(nvof_test tests/nvof_test.cpp src/nvof_flow.cpp)
  target_include_directories(nvof_test SYSTEM PRIVATE "${HSF_NVOF}")
  target_link_libraries(nvof_test PRIVATE d3d12 dxgi d3dcompiler)

  add_executable(frame_checks_test tests/frame_checks_test.cpp src/frame_checks.cpp)
  target_link_libraries(frame_checks_test PRIVATE d3d12 dxgi d3dcompiler)
  target_link_options(frame_checks_test PRIVATE "/ENTRY:wmainCRTStartup")

  # Offline test of the FP16 to HDR10 / sRGB pass for DLSS-G, no game needed (build\x64\hdr_out_test.exe).
  add_executable(hdr_out_test tests/hdr_out_test.cpp src/hdr_out.cpp)
  target_link_libraries(hdr_out_test PRIVATE d3d12 dxgi d3dcompiler)
  target_link_options(hdr_out_test PRIVATE "/ENTRY:wmainCRTStartup")

  # A small Vulkan program for bringing up the Vulkan route without a game (build\x64\vk_scene.exe). Run by hand, not part of the suite.
  add_executable(vk_scene tests/vk_scene.cpp)
  target_include_directories(vk_scene SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include")
  target_link_options(vk_scene PRIVATE "/ENTRY:wmainCRTStartup")

  # HSF's Vulkan layer: adds the shared-semaphore extension to a Vulkan game's device, so the bridge hands frames over in the same frame.
  # Its manifest (hsf_vk_layer64.json) goes next to it.
  add_library(hsf_vk_layer SHARED src/vk_layer.cpp)
  set_target_properties(hsf_vk_layer PROPERTIES OUTPUT_NAME "hsf_vk_layer64")
  target_include_directories(hsf_vk_layer SYSTEM PRIVATE "${CMAKE_SOURCE_DIR}/third_party/vulkan-headers/include")
  add_custom_command(TARGET hsf_vk_layer POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different "${CMAKE_SOURCE_DIR}/src/hsf_vk_layer64.json"
                     "$<TARGET_FILE_DIR:hsf_vk_layer>/hsf_vk_layer64.json")
endif()
# Offline test of the DX9 bridge GPU route and its GPU fences on a D3D9Ex device of its own (build\<arch>\dx9_bridge_test.exe).
add_executable(dx9_bridge_test tests/dx9_bridge_test.cpp src/dx9_bridge.cpp src/gpu_fence9.cpp)
target_link_libraries(dx9_bridge_test PRIVATE hsf_minhook d3d9 d3d11 dxgi)
# Offline test of the camera work, no game or GPU needed (build\<arch>\camera_test.exe).
add_executable(camera_test tests/camera_test.cpp)
# Studies a recording of the camera work made in a game (build\<arch>\camera_replay.exe).
add_executable(camera_replay tests/camera_replay.cpp)
# Offline test of the motion quality score, no game needed (build\<arch>\score_test.exe).
add_executable(score_test tests/score_test.cpp src/score.cpp)
target_link_libraries(score_test PRIVATE d3d11 d3dcompiler)
# Offline test of taking the game's HUD out, no game needed (build\<arch>\hud_test.exe).
add_executable(hud_test tests/hud_test.cpp src/hud.cpp)
target_link_libraries(hud_test PRIVATE d3d11 d3dcompiler)
# Offline test of the GPU reserve: its governor, and one live reading of the card's load (build\<arch>\gpu_reserve_test.exe).
add_executable(gpu_reserve_test tests/gpu_reserve_test.cpp)
# Offline test of the LaunchPad tune button's choices (build\<arch>\launchpad_tune_test.exe).
add_executable(launchpad_tune_test tests/launchpad_tune_test.cpp)
# The same for real depth (build\<arch>\depth_test.exe).
add_executable(depth_test tests/depth_test.cpp src/depth.cpp)
target_link_libraries(depth_test PRIVATE d3d11 d3dcompiler)
target_include_directories(hsf_addon SYSTEM PRIVATE "${HSF_RESHADE_INCLUDE}" "${HSF_IMGUI_INCLUDE}")
target_compile_definitions(hsf_addon PRIVATE ImTextureID=ImU64 IMGUI_DISABLE_OBSOLETE_FUNCTIONS)
