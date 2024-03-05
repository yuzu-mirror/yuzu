# SPDX-FileCopyrightText: 2017 yuzu Emulator Project
# SPDX-License-Identifier: GPL-2.0-or-later

# This function downloads a binary library package from our external repo.
# Params:
#   remote_path: path to the file to download, relative to the remote repository root
#   prefix_var: name of a variable which will be set with the path to the extracted contents
function(download_bundled_external remote_path lib_name prefix_var)

set(package_base_url "https://github.com/yuzu-mirror/")
set(package_repo "no_platform")
set(package_extension "no_platform")
if (WIN32)
    set(package_repo "ext-windows-bin/raw/master/")
    set(package_extension ".7z")
elseif (${CMAKE_SYSTEM_NAME} STREQUAL "Linux")
    set(package_repo "ext-linux-bin/raw/main/")
    set(package_extension ".tar.xz")
elseif (ANDROID)    
    set(package_repo "ext-android-bin/raw/main/")
    set(package_extension ".tar.xz")
else()
    message(FATAL_ERROR "No package available for this platform")
endif()
set(package_url "${package_base_url}${package_repo}")

set(prefix "${CMAKE_BINARY_DIR}/externals/${lib_name}")
if (NOT EXISTS "${prefix}")
    message(STATUS "Downloading binaries for ${lib_name}...")
    file(DOWNLOAD
        ${package_url}${remote_path}${lib_name}${package_extension}
        "${CMAKE_BINARY_DIR}/externals/${lib_name}${package_extension}" SHOW_PROGRESS)
    execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${CMAKE_BINARY_DIR}/externals/${lib_name}${package_extension}"
        WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/externals")
endif()
message(STATUS "Using bundled binaries at ${prefix}")
set(${prefix_var} "${prefix}" PARENT_SCOPE)
endfunction()

function(download_moltenvk_external platform version)
    set(MOLTENVK_DIR "${CMAKE_BINARY_DIR}/externals/MoltenVK")
    set(MOLTENVK_TAR "${CMAKE_BINARY_DIR}/externals/MoltenVK.tar")
    if (NOT EXISTS ${MOLTENVK_DIR})
        if (NOT EXISTS ${MOLTENVK_TAR})
            file(DOWNLOAD https://github.com/KhronosGroup/MoltenVK/releases/download/${version}/MoltenVK-${platform}.tar
                ${MOLTENVK_TAR} SHOW_PROGRESS)
        endif()

        execute_process(COMMAND ${CMAKE_COMMAND} -E tar xf "${MOLTENVK_TAR}"
            WORKING_DIRECTORY "${CMAKE_BINARY_DIR}/externals")
    endif()

    # Add the MoltenVK library path to the prefix so find_library can locate it.
    list(APPEND CMAKE_PREFIX_PATH "${MOLTENVK_DIR}/MoltenVK/dylib/${platform}")
    set(CMAKE_PREFIX_PATH ${CMAKE_PREFIX_PATH} PARENT_SCOPE)
endfunction()
