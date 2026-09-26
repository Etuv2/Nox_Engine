if(NOT DEFINED NOX_ENGINE_ROOT OR NOX_ENGINE_ROOT STREQUAL "")
    message(FATAL_ERROR "NOX_ENGINE_ROOT was not provided to EnsureRuntimeAssets.cmake")
endif()

if(NOT DEFINED NOX_ENGINE_OUTPUT_DIR OR NOX_ENGINE_OUTPUT_DIR STREQUAL "")
    message(FATAL_ERROR "NOX_ENGINE_OUTPUT_DIR was not provided to EnsureRuntimeAssets.cmake")
endif()

if(NOT DEFINED NOX_ENGINE_BUILD_CONFIG OR NOX_ENGINE_BUILD_CONFIG STREQUAL "")
    set(NOX_ENGINE_BUILD_CONFIG "Unknown")
endif()

# Normalize potentially quoted values from command-line -D assignments.
foreach(_nox_var NOX_ENGINE_ROOT NOX_ENGINE_OUTPUT_DIR NOX_ENGINE_BUILD_CONFIG)
    string(REGEX REPLACE "^\"(.*)\"$" "\\1" ${_nox_var} "${${_nox_var}}")
endforeach()

set(NOX_ENGINE_RELEASE_CONFIGS Release RelWithDebInfo MinSizeRel)
list(FIND NOX_ENGINE_RELEASE_CONFIGS "${NOX_ENGINE_BUILD_CONFIG}" NOX_ENGINE_IS_RELEASE_CONFIG_INDEX)
if(NOX_ENGINE_IS_RELEASE_CONFIG_INDEX EQUAL -1)
    return()
endif()

set(NOX_ENGINE_REQUIRED_ASSET_DIRS
    config
    fonts
    hdrs
    images
    models
    scenes
    shaders
    sounds
)

# Engine code rather than user data: always refreshed, so a Release build never runs stale shaders.
# The other folders are only copied when missing so edits made in the output folder survive.
set(NOX_ENGINE_ALWAYS_SYNC_ASSET_DIRS
    shaders
)

set(NOX_ENGINE_REQUIRED_ASSET_FILES
    imgui.ini
    imgui_layout.json
)

foreach(asset_dir IN LISTS NOX_ENGINE_REQUIRED_ASSET_DIRS)
    set(source_dir "${NOX_ENGINE_ROOT}/${asset_dir}")
    set(output_dir "${NOX_ENGINE_OUTPUT_DIR}/${asset_dir}")

    if(NOT EXISTS "${source_dir}")
        message(WARNING "[NoxAssets] Source directory not found and cannot be copied: ${source_dir}")
        continue()
    endif()

    list(FIND NOX_ENGINE_ALWAYS_SYNC_ASSET_DIRS "${asset_dir}" always_sync_index)
    set(output_existed FALSE)
    if(EXISTS "${output_dir}")
        if(always_sync_index EQUAL -1)
            continue()
        endif()
        set(output_existed TRUE)
    endif()

    file(MAKE_DIRECTORY "${output_dir}")
    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_directory "${source_dir}" "${output_dir}"
        RESULT_VARIABLE copy_result
    )

    if(NOT copy_result EQUAL 0)
        message(FATAL_ERROR "[NoxAssets] Failed to copy directory '${asset_dir}' to '${NOX_ENGINE_OUTPUT_DIR}'")
    endif()

    if(output_existed)
        message(STATUS "[NoxAssets] Refreshed directory: ${asset_dir}")
    else()
        message(STATUS "[NoxAssets] Copied missing directory: ${asset_dir}")
    endif()
endforeach()

foreach(asset_file IN LISTS NOX_ENGINE_REQUIRED_ASSET_FILES)
    set(source_file "${NOX_ENGINE_ROOT}/${asset_file}")
    set(output_file "${NOX_ENGINE_OUTPUT_DIR}/${asset_file}")

    if(NOT EXISTS "${source_file}")
        message(WARNING "[NoxAssets] Source file not found and cannot be copied: ${source_file}")
        continue()
    endif()

    if(EXISTS "${output_file}")
        continue()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${source_file}" "${output_file}"
        RESULT_VARIABLE copy_result
    )

    if(NOT copy_result EQUAL 0)
        message(FATAL_ERROR "[NoxAssets] Failed to copy file '${asset_file}' to '${NOX_ENGINE_OUTPUT_DIR}'")
    endif()

    message(STATUS "[NoxAssets] Copied missing file: ${asset_file}")
endforeach()
