cmake_minimum_required(VERSION 3.20)
project(vibribbon_native CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release)
endif()
if(MSVC)
  # Static C runtime: the .exe runs on a clean Windows install with no extra DLLs.
  set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")
  set(SDL_FORCE_STATIC_VCRT ON CACHE BOOL "" FORCE)
endif()

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2
  URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.8/SDL2-2.30.8.zip)
FetchContent_MakeAvailable(SDL2)

add_executable(vibribbon WIN32 src/main.cpp)
target_include_directories(vibribbon PRIVATE src)
target_link_libraries(vibribbon PRIVATE SDL2::SDL2main SDL2::SDL2-static)

# Unit tests (run in CI; need no game files)
enable_testing()
add_executable(pad_map_test tests/pad_map_test.cpp)
add_test(NAME pad_map COMMAND pad_map_test)
add_executable(cpu_test tests/cpu_test.cpp)
add_test(NAME cpu COMMAND cpu_test)

# Headless developer tools (need your disc): disc_info, boot_trace, title_shot
add_executable(disc_info tests/disc_info.cpp)
add_executable(boot_trace tests/boot_trace.cpp)
add_executable(title_shot tests/title_shot.cpp)
add_executable(audio_test tests/audio_test.cpp)
add_executable(bios_boot tests/bios_boot.cpp)
add_executable(bios_title tests/bios_title.cpp)
add_executable(menu_nav tests/menu_nav.cpp)
add_executable(game_try tests/game_try.cpp)
add_executable(game_try2 tests/game_try2.cpp)
add_executable(gpu_capture tests/gpu_capture.cpp)   # menu_nav bios.bin game.cue prefix -- taps buttons, saves screenshots
add_executable(howto_e2e tests/howto_e2e.cpp)   # howto_e2e bios.bin game.cue prefix -- boots and taps through to the HOW TO PLAY tutorial (exit 0 = reached it)
add_executable(script_run tests/script_run.cpp)   # script_run bios.bin game.cue prefix "w1500 X w400 X p" -- drive menus with a tiny script, screenshots + state
add_executable(runlog_test tests/runlog_test.cpp)   # unit test for the run log
add_executable(cdvol_watch tests/cdvol_watch.cpp)   # cdvol_watch bios cue "w1500 X w400 X ..." -- logs SPU CD volume / CD play+mute changes
