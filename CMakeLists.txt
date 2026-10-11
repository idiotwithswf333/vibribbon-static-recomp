cmake_minimum_required(VERSION 3.20)
project(vibribbon_native CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(FetchContent)
set(SDL_SHARED OFF CACHE BOOL "" FORCE)
set(SDL_STATIC ON CACHE BOOL "" FORCE)
set(SDL_TEST OFF CACHE BOOL "" FORCE)
FetchContent_Declare(SDL2
  URL https://github.com/libsdl-org/SDL/releases/download/release-2.30.8/SDL2-2.30.8.zip)
FetchContent_MakeAvailable(SDL2)

add_executable(vibribbon WIN32 src/main.cpp)
target_link_libraries(vibribbon PRIVATE SDL2::SDL2main SDL2::SDL2-static)

enable_testing()
add_executable(pad_map_test tests/pad_map_test.cpp)
add_test(NAME pad_map COMMAND pad_map_test)

add_executable(disc_info tests/disc_info.cpp)  # manual: disc_info game.cue (needs your disc)
