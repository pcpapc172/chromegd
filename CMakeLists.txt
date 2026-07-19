cmake_minimum_required(VERSION 3.21)

project(ChromiumInGD VERSION 1.0.0 LANGUAGES CXX)

if (NOT WIN32)
    message(FATAL_ERROR "ChromiumInGD currently supports Windows only")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

file(GLOB_RECURSE SOURCES CONFIGURE_DEPENDS src/*.cpp)
add_library(${PROJECT_NAME} SHARED ${SOURCES})

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "GEODE_SDK is not set")
endif()

# --- Chromium Embedded Framework ------------------------------------------
# The browser runs in off-screen rendering (windowless) mode and is drawn as
# a texture inside the game's own GL scene, so it stays visible in fullscreen.
set(CEF_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/cef")
if (NOT EXISTS "${CEF_ROOT}/include/cef_version.h")
    message(FATAL_ERROR "CEF binary distribution not found at ${CEF_ROOT}")
endif()

# Build the libcef_dll wrapper ourselves rather than via CEF's own CMake: that
# CMake hard-codes MSVC-style flags (/MP, /W4, /MD ...) which fail under the
# clang toolchain Geode uses on CI. Compiling it here inherits whatever
# compiler is active (MSVC locally, clang on CI).
file(GLOB_RECURSE CEF_WRAPPER_SOURCES CONFIGURE_DEPENDS "${CEF_ROOT}/libcef_dll/*.cc")
add_library(libcef_dll_wrapper STATIC ${CEF_WRAPPER_SOURCES})
target_include_directories(libcef_dll_wrapper PUBLIC "${CEF_ROOT}")
target_compile_features(libcef_dll_wrapper PRIVATE cxx_std_17)
target_compile_definitions(libcef_dll_wrapper PRIVATE
    WRAPPING_CEF_SHARED NOMINMAX WIN32 WIN32_LEAN_AND_MEAN UNICODE _UNICODE
    WINVER=0x0A00 _WIN32_WINNT=0x0A00
)

set(CEF_LIBRARIES
    libcef_dll_wrapper
    "${CEF_ROOT}/Release/libcef.lib"
)

target_include_directories(${PROJECT_NAME} PRIVATE "${CEF_ROOT}")
target_link_libraries(${PROJECT_NAME} ${CEF_LIBRARIES} delayimp)
# libcef.dll lives in the mod's resources dir, not next to GD.exe; the host
# loads it by full path before the first CEF call resolves the delay-load.
# LINKER: keeps the flag correct across the MSVC and clang drivers.
target_link_options(${PROJECT_NAME} PRIVATE "LINKER:/DELAYLOAD:libcef.dll")

# Subprocess helper (renderer/GPU processes).
add_executable(chromium-in-gd-helper WIN32 helper/helper_main.cpp)
target_include_directories(chromium-in-gd-helper PRIVATE "${CEF_ROOT}")
target_link_libraries(chromium-in-gd-helper ${CEF_LIBRARIES})
add_dependencies(${PROJECT_NAME} chromium-in-gd-helper)

# Stage every CEF runtime file, flattened, into cef-dist/. mod.json packages
# that folder into the mod's resources, which is where BrowserHost points
# CefSettings.resources_dir_path / locales_dir_path.
set(CEF_DIST_DIR "${CMAKE_CURRENT_SOURCE_DIR}/cef-dist")
file(GLOB CEF_RUNTIME_FILES
    "${CEF_ROOT}/Release/*.dll"
    "${CEF_ROOT}/Release/*.bin"
    "${CEF_ROOT}/Release/*.dat"
    "${CEF_ROOT}/Resources/*.pak"
    "${CEF_ROOT}/Resources/*.dat"
    "${CEF_ROOT}/Resources/locales/*.pak"
)
file(COPY ${CEF_RUNTIME_FILES} DESTINATION "${CEF_DIST_DIR}")

# The helper must land in cef-dist before Geode packages the mod; this
# POST_BUILD step is registered ahead of setup_geode_mod's, so it runs first.
add_custom_command(TARGET ${PROJECT_NAME} POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:chromium-in-gd-helper>" "${CEF_DIST_DIR}/chromium-in-gd-helper.exe"
)

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)
setup_geode_mod(${PROJECT_NAME})
