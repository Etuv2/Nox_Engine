cmake_minimum_required(VERSION 3.21)

project(Nox_Engine VERSION 1.0.0 LANGUAGES CXX)

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

file(GLOB_RECURSE NOX_ENGINE_SOURCES CONFIGURE_DEPENDS
    "${NOX_ENGINE_SRC_DIR}/*.cpp"
    "${NOX_ENGINE_INCLUDE_DIR}/IMGUI/*.cpp"
)

add_executable(${NOX_ENGINE_TARGET} ${NOX_ENGINE_SOURCES})

target_compile_definitions(${NOX_ENGINE_TARGET} PRIVATE
    IMGUI_DEFINE_MATH_OPERATORS
    IMGUIMIZMO_DISABLE_CAPTURE_MOUSE_FROM_APP
)

target_include_directories(${NOX_ENGINE_TARGET} PRIVATE
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
    target_compile_options(${NOX_ENGINE_TARGET} PRIVATE /W3 /MP /permissive-)
else()
    target_compile_options(${NOX_ENGINE_TARGET} PRIVATE -Wall -Wextra -Wno-unused-parameter)
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

target_link_libraries(${NOX_ENGINE_TARGET} PRIVATE
    opengl32
    "${NOX_ENGINE_GLEW_LIB_DIR}/glew32.lib"
    "${NOX_ENGINE_SDL_LIB_DIR}/SDL2.lib"
    "${NOX_ENGINE_SDL_LIB_DIR}/SDL2main.lib"
    "${NOX_ENGINE_SDL_LIB_DIR}/SDL2_image.lib"
    "${NOX_ENGINE_SDL_LIB_DIR}/SDL2_ttf.lib"
    "${NOX_ENGINE_SDL_LIB_DIR}/SDL2_mixer.lib"
    "${NOX_ENGINE_ASSIMP_LIB}"
)

set_target_properties(${NOX_ENGINE_TARGET} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${NOX_ENGINE_ROOT}"
)

set(NOX_ENGINE_RUNTIME_ASSET_CHECK_SCRIPT "${NOX_ENGINE_ROOT}/cmake/EnsureRuntimeAssets.cmake")

if(EXISTS "${NOX_ENGINE_RUNTIME_ASSET_CHECK_SCRIPT}")
    add_custom_command(TARGET ${NOX_ENGINE_TARGET} POST_BUILD
        COMMAND ${CMAKE_COMMAND}
            -DNOX_ENGINE_ROOT="${NOX_ENGINE_ROOT}"
            -DNOX_ENGINE_OUTPUT_DIR="$<TARGET_FILE_DIR:${NOX_ENGINE_TARGET}>"
            -DNOX_ENGINE_BUILD_CONFIG="$<CONFIG>"
            -P "${NOX_ENGINE_RUNTIME_ASSET_CHECK_SCRIPT}"
        VERBATIM
    )
else()
    message(WARNING "Runtime asset check script missing: ${NOX_ENGINE_RUNTIME_ASSET_CHECK_SCRIPT}")
endif()

set(NOX_ENGINE_RUNTIME_DLLS
    "${NOX_ENGINE_ROOT}/SDL2.dll"
    "${NOX_ENGINE_ROOT}/SDL2_image.dll"
    "${NOX_ENGINE_ROOT}/SDL2_mixer.dll"
    "${NOX_ENGINE_ROOT}/SDL2_net.dll"
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
