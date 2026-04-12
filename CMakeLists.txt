cmake_minimum_required(VERSION 3.21)

project(Nox_Engine VERSION 1.0.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(NOX_ENGINE_TARGET nox_engine)

file(GLOB_RECURSE NOX_ENGINE_SOURCES CONFIGURE_DEPENDS
    "src/*.cpp"
    "include/IMGUI/*.cpp"
)

add_executable(${NOX_ENGINE_TARGET} ${NOX_ENGINE_SOURCES})

target_compile_definitions(${NOX_ENGINE_TARGET} PRIVATE
    IMGUI_DEFINE_MATH_OPERATORS
    IMGUIMIZMO_DISABLE_CAPTURE_MOUSE_FROM_APP
)

target_include_directories(${NOX_ENGINE_TARGET} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/GL
    ${CMAKE_CURRENT_SOURCE_DIR}/include/SDL
    ${CMAKE_CURRENT_SOURCE_DIR}/include/GLM
    ${CMAKE_CURRENT_SOURCE_DIR}/include/JSON
    ${CMAKE_CURRENT_SOURCE_DIR}/include/IMGUI
)

if(MSVC)
    target_compile_options(${NOX_ENGINE_TARGET} PRIVATE /W3 /permissive- /MP)
else()
    target_compile_options(${NOX_ENGINE_TARGET} PRIVATE -Wall -Wextra -Wno-unused-parameter)
endif()

if(WIN32)
    set(NOX_GLEW_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/GLEW/Win32)
    set(NOX_SDL_LIB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/lib/SDL/x86)
    set(NOX_ASSIMP_LIB ${CMAKE_CURRENT_SOURCE_DIR}/lib/ASSIMP/assimp-vc143-mt.lib)

    foreach(_required_lib
            "${NOX_GLEW_LIB_DIR}/glew32.lib"
            "${NOX_SDL_LIB_DIR}/SDL2.lib"
            "${NOX_SDL_LIB_DIR}/SDL2main.lib"
            "${NOX_SDL_LIB_DIR}/SDL2_image.lib"
            "${NOX_SDL_LIB_DIR}/SDL2_ttf.lib"
            "${NOX_SDL_LIB_DIR}/SDL2_mixer.lib"
            "${NOX_ASSIMP_LIB}")
        if(NOT EXISTS "${_required_lib}")
            message(FATAL_ERROR "Required prebuilt library is missing: ${_required_lib}")
        endif()
    endforeach()

    target_link_libraries(${NOX_ENGINE_TARGET} PRIVATE
        opengl32
        ${NOX_GLEW_LIB_DIR}/glew32.lib
        ${NOX_SDL_LIB_DIR}/SDL2.lib
        ${NOX_SDL_LIB_DIR}/SDL2main.lib
        ${NOX_SDL_LIB_DIR}/SDL2_image.lib
        ${NOX_SDL_LIB_DIR}/SDL2_ttf.lib
        ${NOX_SDL_LIB_DIR}/SDL2_mixer.lib
        ${NOX_ASSIMP_LIB}
    )

    foreach(_dll
            SDL2.dll
            SDL2_image.dll
            SDL2_mixer.dll
            SDL2_net.dll
            SDL2_ttf.dll
            glew32.dll
            assimp-vc143-mt.dll)
        if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${_dll}")
            add_custom_command(TARGET ${NOX_ENGINE_TARGET} POST_BUILD
                COMMAND ${CMAKE_COMMAND} -E copy_if_different
                        "${CMAKE_CURRENT_SOURCE_DIR}/${_dll}"
                        "$<TARGET_FILE_DIR:${NOX_ENGINE_TARGET}>/${_dll}")
        endif()
    endforeach()
else()
    find_package(OpenGL REQUIRED)
    find_package(GLEW REQUIRED)
    find_package(SDL2 REQUIRED)
    find_package(SDL2_image REQUIRED)
    find_package(SDL2_ttf REQUIRED)
    find_package(SDL2_mixer REQUIRED)
    find_package(assimp REQUIRED)

    target_link_libraries(${NOX_ENGINE_TARGET} PRIVATE
        OpenGL::GL
        GLEW::GLEW
        SDL2::SDL2
        SDL2_image::SDL2_image
        SDL2_ttf::SDL2_ttf
        SDL2_mixer::SDL2_mixer
        assimp::assimp
    )

    if(TARGET SDL2::SDL2main)
        target_link_libraries(${NOX_ENGINE_TARGET} PRIVATE SDL2::SDL2main)
    endif()
endif()

set_target_properties(${NOX_ENGINE_TARGET} PROPERTIES
    VS_DEBUGGER_WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
)
