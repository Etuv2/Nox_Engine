cmake_minimum_required(VERSION 3.21)

project(Nox_Engine VERSION 1.0.0 LANGUAGES C CXX)

if(CMAKE_SOURCE_DIR STREQUAL CMAKE_BINARY_DIR)
    message(FATAL_ERROR "Use an out-of-source build: cmake --preset vs2022-x64")
endif()

if(NOT WIN32)
    message(FATAL_ERROR "Nox_Engine currently supports Windows builds only.")
endif()

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NOX_ENGINE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}")
set(NOX_ENGINE_TARGET Nox_Engine)
set(NOX_ENGINE_INCLUDE_DIR "${NOX_ENGINE_ROOT}/include")
set(NOX_ENGINE_SRC_DIR "${NOX_ENGINE_ROOT}/src")

# Pinned third-party source dependencies (external/README.md), linked statically.
set(NOX_EXTERNAL_DIR "${NOX_ENGINE_ROOT}/external")
set(NOX_PHYSFS_DIR "${NOX_EXTERNAL_DIR}/physfs-3.2.0/src")
add_library(nox_physfs STATIC
    "${NOX_PHYSFS_DIR}/physfs.c"
    "${NOX_PHYSFS_DIR}/physfs_byteorder.c"
    "${NOX_PHYSFS_DIR}/physfs_unicode.c"
    "${NOX_PHYSFS_DIR}/physfs_platform_windows.c"
    "${NOX_PHYSFS_DIR}/physfs_archiver_dir.c"
    "${NOX_PHYSFS_DIR}/physfs_archiver_unpacked.c"
    "${NOX_PHYSFS_DIR}/physfs_archiver_zip.c"
)
# ZIP is the only archive format NOX ships (docs/asset_io.md); every other archiver is compiled out.
target_compile_definitions(nox_physfs
    PUBLIC PHYSFS_STATIC
    PRIVATE PHYSFS_SUPPORTS_DEFAULT=0 PHYSFS_SUPPORTS_ZIP=1 PHYSFS_NO_CDROM_SUPPORT=1 _CRT_SECURE_NO_WARNINGS
)
target_include_directories(nox_physfs PUBLIC "${NOX_PHYSFS_DIR}")
if(MSVC)
    target_compile_options(nox_physfs PRIVATE /W0)
endif()

set(NOX_LUA_DIR "${NOX_EXTERNAL_DIR}/lua-5.5.1/src")
file(GLOB NOX_LUA_SOURCES CONFIGURE_DEPENDS "${NOX_LUA_DIR}/*.c")
list(REMOVE_ITEM NOX_LUA_SOURCES "${NOX_LUA_DIR}/lua.c" "${NOX_LUA_DIR}/luac.c")
add_library(nox_lua STATIC ${NOX_LUA_SOURCES})
# Lua is compiled as C++ so lua_error unwinds with C++ exceptions instead of longjmp: destructors of
# native frames between an error and its protected call run (docs/lua_api.md). Include lua.h,
# lauxlib.h and lualib.h directly, never lua.hpp (its extern "C" would not match this library).
set_source_files_properties(${NOX_LUA_SOURCES} PROPERTIES LANGUAGE CXX)
target_include_directories(nox_lua PUBLIC "${NOX_LUA_DIR}")
target_compile_definitions(nox_lua PRIVATE _CRT_SECURE_NO_WARNINGS)
if(MSVC)
    target_compile_options(nox_lua PRIVATE /W0)
endif()

file(GLOB_RECURSE NOX_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${NOX_ENGINE_SRC_DIR}/*.cpp"
    "${NOX_ENGINE_INCLUDE_DIR}/IMGUI/*.cpp"
)
list(REMOVE_ITEM NOX_ENGINE_SOURCES "${NOX_ENGINE_SRC_DIR}/main.cpp")

# Compiled racing gameplay that is being migrated to project Lua/data (docs/engine_boundary.md).
# It is linked only into the legacy application target and never into NoxEngineCore.
set(NOX_RACING_LEGACY_SOURCES
    "${NOX_ENGINE_SRC_DIR}/DriveAudio.cpp"
    "${NOX_ENGINE_SRC_DIR}/DriveMode.cpp"
    "${NOX_ENGINE_SRC_DIR}/GameEditorPanels.cpp"
    "${NOX_ENGINE_SRC_DIR}/GameSession.cpp"
    "${NOX_ENGINE_SRC_DIR}/LegacyNfsPolicy.cpp"
    "${NOX_ENGINE_SRC_DIR}/NightStreetPacks.cpp"
    "${NOX_ENGINE_SRC_DIR}/NoxRacingGame.cpp"
    "${NOX_ENGINE_SRC_DIR}/ProceduralStreetKit.cpp"
    "${NOX_ENGINE_SRC_DIR}/RacingHUD.cpp"
    "${NOX_ENGINE_SRC_DIR}/WorldMapUI.cpp"
)
foreach(racing_source IN LISTS NOX_RACING_LEGACY_SOURCES)
    if(EXISTS "${racing_source}")
        list(REMOVE_ITEM NOX_ENGINE_SOURCES "${racing_source}")
        list(APPEND NOX_RACING_LEGACY_EXISTING "${racing_source}")
    endif()
endforeach()

# Shared compile settings for every NOX C++ target.
add_library(nox_settings INTERFACE)
target_compile_features(nox_settings INTERFACE cxx_std_20)
target_compile_definitions(nox_settings INTERFACE
    IMGUI_DEFINE_MATH_OPERATORS
    IMGUIMIZMO_DISABLE_CAPTURE_MOUSE_FROM_APP
)
target_include_directories(nox_settings INTERFACE
    "${NOX_ENGINE_ROOT}"
    "${NOX_ENGINE_SRC_DIR}"
    "${NOX_ENGINE_INCLUDE_DIR}"
    "${NOX_ENGINE_INCLUDE_DIR}/GL"
    "${NOX_ENGINE_INCLUDE_DIR}/SDL"
    "${NOX_ENGINE_INCLUDE_DIR}/GLM"
    "${NOX_ENGINE_INCLUDE_DIR}/JSON"
    "${NOX_ENGINE_INCLUDE_DIR}/IMGUI"
)
if(MSVC)
    target_compile_options(nox_settings INTERFACE /W3 /MP /permissive-)
else()
    target_compile_options(nox_settings INTERFACE -Wall -Wextra -Wno-unused-parameter)
endif()

if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    set(NOX_ENGINE_SDL_LIB_DIR "${NOX_ENGINE_ROOT}/lib/SDL/x64")
    set(NOX_ENGINE_GLEW_LIB_DIR "${NOX_ENGINE_ROOT}/lib/GLEW/x64")
else()
    set(NOX_ENGINE_SDL_LIB_DIR "${NOX_ENGINE_ROOT}/lib/SDL/x86")
    set(NOX_ENGINE_GLEW_LIB_DIR "${NOX_ENGINE_ROOT}/lib/GLEW/Win32")
endif()

set(NOX_ENGINE_ASSIMP_LIB "${NOX_ENGINE_ROOT}/lib/ASSIMP/assimp-vc143-mt.lib")

set(NOX_ENGINE_REQUIRED_LIBS
    "${NOX_ENGINE_GLEW_LIB_DIR}/glew32.lib"
    "${NOX_ENGINE_SDL_LIB_DIR}/SDL2.lib"
    "${NOX_ENGINE_SDL_LIB_DIR}/SDL2main.lib"
    "${NOX_ENGINE_SDL_LIB_DIR}/SDL2_image.lib"
    "${NOX_ENGINE_SDL_LIB_DIR}/SDL2_ttf.lib"
    "${NOX_ENGINE_SDL_LIB_DIR}/SDL2_mixer.lib"
    "${NOX_ENGINE_ASSIMP_LIB}"
)

foreach(required_lib IN LISTS NOX_ENGINE_REQUIRED_LIBS)
    if(NOT EXISTS "${required_lib}")
        message(FATAL_ERROR "Required prebuilt library is missing: ${required_lib}")
    endif()
endforeach()

add_library(NoxEngineCore STATIC ${NOX_ENGINE_SOURCES})
target_link_libraries(NoxEngineCore PUBLIC
    nox_settings
    nox_physfs
    nox_lua
    opengl32
    ${NOX_ENGINE_REQUIRED_LIBS}
)

add_library(NoxRacingLegacy STATIC ${NOX_RACING_LEGACY_EXISTING})
target_link_libraries(NoxRacingLegacy PUBLIC NoxEngineCore)

add_executable(${NOX_ENGINE_TARGET} "${NOX_ENGINE_SRC_DIR}/main.cpp")
target_link_libraries(${NOX_ENGINE_TARGET} PRIVATE NoxEngineCore NoxRacingLegacy)
if(MSVC)
    # Keep registration/translation units that are only reached through registries.
    target_link_options(${NOX_ENGINE_TARGET} PRIVATE
        "/WHOLEARCHIVE:$<TARGET_FILE:NoxEngineCore>"
        "/WHOLEARCHIVE:$<TARGET_FILE:NoxRacingLegacy>"
    )
endif()

set_target_properties(${NOX_ENGINE_TARGET} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${NOX_ENGINE_ROOT}"
)

# Headless native tests (tests/native): asset I/O, scripting, ECS handles. No GPU or window needed.
option(NOX_BUILD_TESTS "Build the headless native test executable" ON)
if(NOX_BUILD_TESTS)
    file(GLOB NOX_NATIVE_TEST_SOURCES CONFIGURE_DEPENDS "${NOX_ENGINE_ROOT}/tests/native/*.cpp")
    if(NOX_NATIVE_TEST_SOURCES)
        add_executable(nox_tests ${NOX_NATIVE_TEST_SOURCES})
        target_link_libraries(nox_tests PRIVATE NoxEngineCore)
        enable_testing()
        add_test(NAME nox_tests COMMAND nox_tests WORKING_DIRECTORY "${NOX_ENGINE_ROOT}")
    endif()
endif()

# Development binaries resolve the source asset tree in main.cpp. Only explicit
# packaging (tools/Package.ps1) stages content; ordinary builds copy no assets.

set(NOX_ENGINE_RUNTIME_DLLS
    "${NOX_ENGINE_ROOT}/SDL2.dll"
    "${NOX_ENGINE_ROOT}/SDL2_image.dll"
    "${NOX_ENGINE_ROOT}/SDL2_mixer.dll"
    "${NOX_ENGINE_ROOT}/SDL2_ttf.dll"
    "${NOX_ENGINE_ROOT}/glew32.dll"
    "${NOX_ENGINE_ROOT}/assimp-vc143-mt.dll"
)

foreach(runtime_dll IN LISTS NOX_ENGINE_RUNTIME_DLLS)
    if(EXISTS "${runtime_dll}")
        add_custom_command(TARGET ${NOX_ENGINE_TARGET} POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
                "${runtime_dll}"
                "$<TARGET_FILE_DIR:${NOX_ENGINE_TARGET}>"
        )
    endif()
endforeach()

# Headless procedural cook (tools/cook/main.cpp, contract C10): runs the registered procedural generators
# without a window or GL context and writes generated/cooked/<generator>-<key>/ results.
add_executable(nox_cook "${NOX_ENGINE_ROOT}/tools/cook/main.cpp")
target_link_libraries(nox_cook PRIVATE NoxEngineCore NoxRacingLegacy)
if(MSVC)
    target_link_options(nox_cook PRIVATE
        "/WHOLEARCHIVE:$<TARGET_FILE:NoxEngineCore>"
        "/WHOLEARCHIVE:$<TARGET_FILE:NoxRacingLegacy>"
    )
endif()
