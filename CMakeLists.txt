cmake_minimum_required(VERSION 3.20)
project(blub LANGUAGES C CXX)

# blub: Down in the Dumps (Philips Media / Haiku Studios, 1996) as a native program.
# The game program DID.EXE is statically recompiled to C++. The repository contains no code or data
# of the game: the C++ is generated at configure time from the user's own CD (BLUB_GAME_DATA).

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

# ---------------------------------------------------------------- generated game code
set(BLUB_GAME_DATA "" CACHE PATH
    "the original game: folder with the ISO images of the CDs (or their contents), a CD 1 ISO, or DID.EXE")
set(BLUB_GENERATED "${CMAKE_BINARY_DIR}/generated")
find_package(Python3 3.8 REQUIRED COMPONENTS Interpreter)
if(NOT EXISTS "${BLUB_GENERATED}/stamp.txt")
  if(NOT BLUB_GAME_DATA)
    message(FATAL_ERROR
      "The game code is generated from your original CDs. Configure with\n"
      "  -DBLUB_GAME_DATA=<folder with the ISO images of Down in the Dumps>\n"
      "(or the copied CD contents, a CD 1 .iso, or DID.EXE). Needs: pip install capstone")
  endif()
  message(STATUS "Generating the game code from ${BLUB_GAME_DATA}")
  execute_process(
    COMMAND "${Python3_EXECUTABLE}" "${CMAKE_SOURCE_DIR}/generator/generate.py"
            --game "${BLUB_GAME_DATA}" --out "${BLUB_GENERATED}"
    RESULT_VARIABLE gen_result)
  if(NOT gen_result EQUAL 0)
    message(FATAL_ERROR "Generating the game code failed (see above)")
  endif()
endif()
file(GLOB GENERATED_SOURCES "${BLUB_GENERATED}/recomp/*.cpp" "${BLUB_GENERATED}/codec/*.cpp")

# ---------------------------------------------------------------- SDL2
# Windows: linked statically so that the game runs without extra DLLs. Linux/macOS: system SDL2.
if(WIN32)
  option(BLUB_SYSTEM_SDL "use the system SDL2" OFF)
else()
  option(BLUB_SYSTEM_SDL "use the system SDL2" ON)
endif()
if(BLUB_SYSTEM_SDL)
  find_package(SDL2 REQUIRED)
  if(NOT TARGET SDL2::SDL2-static)
    add_library(SDL2::SDL2-static ALIAS SDL2::SDL2)
  endif()
  if(NOT TARGET SDL2::SDL2main)
    add_library(blub_sdlmain INTERFACE)
    add_library(SDL2::SDL2main ALIAS blub_sdlmain)
  endif()
else()
  set(SDL_SHARED OFF CACHE BOOL "" FORCE)
  set(SDL_STATIC ON CACHE BOOL "" FORCE)
  set(SDL_TEST OFF CACHE BOOL "" FORCE)
  set(SDL2_DISABLE_INSTALL ON CACHE BOOL "" FORCE)
  if(EXISTS "${CMAKE_SOURCE_DIR}/third_party/SDL2/CMakeLists.txt")
    add_subdirectory(third_party/SDL2 EXCLUDE_FROM_ALL)
  else()
    include(FetchContent)
    FetchContent_Declare(SDL2
      URL https://github.com/libsdl-org/SDL/releases/download/release-2.32.10/SDL2-2.32.10.tar.gz
      URL_HASH SHA256=5f5993c530f084535c65a6879e9b26ad441169b3e25d789d83287040a9ca5165
      EXCLUDE_FROM_ALL)
    FetchContent_MakeAvailable(SDL2)
  endif()
endif()

# ---------------------------------------------------------------- xBRZ (upscaling filter, vendored)
# always optimised: unoptimised it is far too slow for real-time scaling
add_library(blubxbrz STATIC third_party/xbrz/xbrz.cpp)
target_include_directories(blubxbrz PUBLIC third_party/xbrz)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(blubxbrz PRIVATE -O2 -w)
endif()

# ---------------------------------------------------------------- engine + host
file(GLOB_RECURSE ENGINE_SOURCES CONFIGURE_DEPENDS src/engine/*.cpp)
add_library(blubengine STATIC ${ENGINE_SOURCES} ${GENERATED_SOURCES})
target_include_directories(blubengine PUBLIC src/engine "${BLUB_GENERATED}")
find_package(Threads REQUIRED)
target_link_libraries(blubengine PUBLIC SDL2::SDL2-static blubxbrz Threads::Threads)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(blubengine PRIVATE -Wall -Wextra -Wno-unused-parameter)
  # generated code: plenty of labels/variables that are not used in every function; -O1 keeps the
  # compile time of the very large functions reasonable
  set_source_files_properties(${GENERATED_SOURCES} PROPERTIES COMPILE_OPTIONS
    "-Wno-unused-label;-Wno-unused-variable;-Wno-unused-but-set-variable;-Wno-parentheses-equality;-Wno-unknown-warning-option;-O1")
else()
  message(WARNING "Only GCC and Clang (e.g. LLVM-MinGW on Windows) are supported; the generated code uses GCC builtins")
endif()

# Dear ImGui (settings window) and tinyfiledialogs (native folder picker), both vendored
add_library(blubgui STATIC
  third_party/imgui/imgui.cpp third_party/imgui/imgui_draw.cpp third_party/imgui/imgui_tables.cpp
  third_party/imgui/imgui_widgets.cpp third_party/imgui/backends/imgui_impl_sdl2.cpp
  third_party/imgui/backends/imgui_impl_sdlrenderer2.cpp third_party/tinyfiledialogs/tinyfiledialogs.c)
target_include_directories(blubgui PUBLIC third_party/imgui third_party/imgui/backends third_party/tinyfiledialogs)
target_link_libraries(blubgui PUBLIC SDL2::SDL2-static)
if(WIN32)
  target_link_libraries(blubgui PUBLIC comdlg32 ole32 gdi32)
endif()

# ---------------------------------------------------------------- programs
add_executable(blub src/main.cpp src/launcher.cpp)
target_link_libraries(blub PRIVATE blubengine blubgui SDL2::SDL2main)
if(WIN32)
  set_target_properties(blub PROPERTIES WIN32_EXECUTABLE ON)   # no console window
  target_link_options(blub PRIVATE -static)
endif()
if(CMAKE_BUILD_TYPE STREQUAL "Release" AND CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
  target_link_options(blub PRIVATE -s)                          # stripped release binary
  if(NOT WIN32)
    target_link_options(blub PRIVATE -static-libstdc++ -static-libgcc)
  endif()
endif()

# resource viewer for the GAP archives (development tool)
add_executable(blub_view EXCLUDE_FROM_ALL src/viewer.cpp)
target_link_libraries(blub_view PRIVATE blubengine SDL2::SDL2main)
if(WIN32)
  target_link_options(blub_view PRIVATE -static)
endif()

# tests: codecs against reference checksums made with the original code, recompiled routines
# against the port's reference implementations (need the game data: blub_tests <fixtures> <game>)
enable_testing()
file(GLOB TEST_SOURCES CONFIGURE_DEPENDS tests/*.cpp)
if(TEST_SOURCES)
  add_executable(blub_tests EXCLUDE_FROM_ALL ${TEST_SOURCES})
  target_link_libraries(blub_tests PRIVATE blubengine)
  if(WIN32)
    target_link_options(blub_tests PRIVATE -static)
  endif()
  add_test(NAME blub_tests COMMAND blub_tests ${CMAKE_SOURCE_DIR}/tests/fixtures "${BLUB_GAME_DATA}")
endif()
