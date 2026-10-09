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

# Whole directories copied into the output. file(COPY) keeps source timestamps and only
# (re)copies entries whose timestamp differs, so unchanged content costs nothing on rebuild
# and edited content is refreshed (copy_if_different semantics, without deleting extras).
set(NOX_ENGINE_ASSET_DIRS
    config
    fonts
    images
    scenes
    shaders
    sounds
    textures
)

# Git-ignored content: warn when missing instead of failing the build.
set(NOX_ENGINE_OPTIONAL_ASSET_DIRS
    textures/cc0
    hdrs/cc0
)

# Scenes and config whose referenced assets (models, hdrs, textures, images, fonts) are copied
# individually. The full models/ and hdrs/ folders are far too large to mirror.
set(NOX_ENGINE_GAME_SCENE "scenes/nfs_sprint.json")
file(GLOB NOX_ENGINE_REFERENCE_SOURCES "${NOX_ENGINE_ROOT}/scenes/nfs_*.json" "${NOX_ENGINE_ROOT}/config/*.json")
list(APPEND NOX_ENGINE_REFERENCE_SOURCES "${NOX_ENGINE_ROOT}/${NOX_ENGINE_GAME_SCENE}")
list(REMOVE_DUPLICATES NOX_ENGINE_REFERENCE_SOURCES)

set(NOX_ENGINE_REQUIRED_ASSET_FILES
    imgui.ini
    imgui_layout.json
)

file(MAKE_DIRECTORY "${NOX_ENGINE_OUTPUT_DIR}")

foreach(asset_dir IN LISTS NOX_ENGINE_ASSET_DIRS)
    set(source_dir "${NOX_ENGINE_ROOT}/${asset_dir}")
    if(NOT IS_DIRECTORY "${source_dir}")
        message(WARNING "[NoxAssets] Source directory not found and not copied: ${source_dir}")
        continue()
    endif()

    file(COPY "${source_dir}" DESTINATION "${NOX_ENGINE_OUTPUT_DIR}")
    message(STATUS "[NoxAssets] Synced directory: ${asset_dir}")
endforeach()

foreach(optional_dir IN LISTS NOX_ENGINE_OPTIONAL_ASSET_DIRS)
    if(NOT IS_DIRECTORY "${NOX_ENGINE_ROOT}/${optional_dir}")
        message(WARNING "[NoxAssets] Optional git-ignored content missing (${optional_dir}); "
                        "the game scene needs it, see textures/cc0/SOURCES.md and hdrs/cc0 provenance.")
    endif()
endforeach()

# Individually referenced files: models/hdrs/images/fonts named in scene and config JSON.
# Backslashes are normalized first, so "models\\car.glb" and "models/car.glb" both match.
set(NOX_ENGINE_COPIED_REFERENCES "")
foreach(reference_source IN LISTS NOX_ENGINE_REFERENCE_SOURCES)
    if(NOT EXISTS "${reference_source}")
        message(WARNING "[NoxAssets] Scene/config not found, its assets are not copied: ${reference_source}")
        continue()
    endif()

    file(READ "${reference_source}" reference_text)
    string(REPLACE "\\" "/" reference_text "${reference_text}")
    string(REGEX MATCHALL "\"[^\"]+\\.(glb|gltf|bin|fbx|obj|hdr|png|jpg|jpeg|ttf|otf|wav|ogg)\"" reference_matches "${reference_text}")

    foreach(match IN LISTS reference_matches)
        string(REGEX REPLACE "^\"(.*)\"$" "\\1" rel_path "${match}")
        string(REGEX REPLACE "/+" "/" rel_path "${rel_path}")
        string(REGEX REPLACE "^\\./" "" rel_path "${rel_path}")

        # Absolute or parent-relative references are not part of the runtime tree.
        if(rel_path MATCHES "^/" OR rel_path MATCHES "^[A-Za-z]:" OR rel_path MATCHES "\\.\\./")
            continue()
        endif()
        # Whole textures/ tree is already synced above.
        if(rel_path MATCHES "^textures/")
            continue()
        endif()
        list(FIND NOX_ENGINE_COPIED_REFERENCES "${rel_path}" already_index)
        if(NOT already_index EQUAL -1)
            continue()
        endif()
        list(APPEND NOX_ENGINE_COPIED_REFERENCES "${rel_path}")

        set(source_file "${NOX_ENGINE_ROOT}/${rel_path}")
        if(NOT EXISTS "${source_file}")
            message(WARNING "[NoxAssets] Referenced asset missing, not copied: ${rel_path} (from ${reference_source})")
            continue()
        endif()

        get_filename_component(rel_dir "${rel_path}" DIRECTORY)
        set(dest_dir "${NOX_ENGINE_OUTPUT_DIR}")
        if(NOT rel_dir STREQUAL "")
            set(dest_dir "${NOX_ENGINE_OUTPUT_DIR}/${rel_dir}")
        endif()
        file(MAKE_DIRECTORY "${dest_dir}")
        file(COPY "${source_file}" DESTINATION "${dest_dir}")
    endforeach()
endforeach()
list(LENGTH NOX_ENGINE_COPIED_REFERENCES NOX_ENGINE_REFERENCE_COUNT)
message(STATUS "[NoxAssets] Referenced assets checked: ${NOX_ENGINE_REFERENCE_COUNT}")

foreach(asset_file IN LISTS NOX_ENGINE_REQUIRED_ASSET_FILES)
    set(source_file "${NOX_ENGINE_ROOT}/${asset_file}")
    if(NOT EXISTS "${source_file}")
        message(WARNING "[NoxAssets] Source file not found and not copied: ${source_file}")
        continue()
    endif()

    execute_process(
        COMMAND "${CMAKE_COMMAND}" -E copy_if_different "${source_file}" "${NOX_ENGINE_OUTPUT_DIR}/${asset_file}"
        RESULT_VARIABLE copy_result
    )
    if(NOT copy_result EQUAL 0)
        message(FATAL_ERROR "[NoxAssets] Failed to copy file '${asset_file}' to '${NOX_ENGINE_OUTPUT_DIR}'")
    endif()
endforeach()
