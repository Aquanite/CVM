cmake_minimum_required(VERSION 3.20)

project(CVM LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CHANCECODE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../ChanceCode" CACHE PATH "Path to ChanceCode repository")
option(CVM_BUNDLED_LIBFFI "Set up bundled libffi (prebuilt binaries on Windows, source build on other platforms)" ON)
set(CVM_LIBFFI_GIT_TAG "v3.5.2" CACHE STRING "libffi git tag used when CVM_BUNDLED_LIBFFI is ON")
string(REGEX REPLACE "^v" "" CVM_LIBFFI_VERSION "${CVM_LIBFFI_GIT_TAG}")
set(LIBFFI_ROOT "" CACHE PATH "Optional libffi installation prefix (expects include/ and lib/)")

set(CVM_LIBFFI_TARGET "")
set(CVM_LIBFFI_RUNTIME_DLL "")

if(CVM_BUNDLED_LIBFFI)
    set(CVM_LIBFFI_INSTALL_DIR "${CMAKE_BINARY_DIR}/_deps/libffi-install")
    file(MAKE_DIRECTORY "${CVM_LIBFFI_INSTALL_DIR}/include")
    file(MAKE_DIRECTORY "${CVM_LIBFFI_INSTALL_DIR}/lib")

    if(WIN32)
        if(CMAKE_SIZEOF_VOID_P EQUAL 8)
            set(_libffi_archive_name "libffi-3.5.2-x86-64bit-msvc-binaries.zip")
        else()
            set(_libffi_archive_name "libffi-3.5.2-x86-32bit-msvc-binaries.zip")
        endif()

        set(_libffi_archive_url "https://github.com/libffi/libffi/releases/download/${CVM_LIBFFI_GIT_TAG}/${_libffi_archive_name}")
        set(_libffi_archive_path "${CMAKE_BINARY_DIR}/_deps/${_libffi_archive_name}")
        set(_libffi_extract_dir "${CMAKE_BINARY_DIR}/_deps/libffi-prebuilt")

        if(NOT EXISTS "${_libffi_archive_path}")
            message(STATUS "Downloading prebuilt libffi from ${_libffi_archive_url}")
            file(DOWNLOAD "${_libffi_archive_url}" "${_libffi_archive_path}"
                SHOW_PROGRESS
                STATUS _libffi_download_status
                LOG _libffi_download_log
            )
            list(GET _libffi_download_status 0 _libffi_download_code)
            if(NOT _libffi_download_code EQUAL 0)
                list(GET _libffi_download_status 1 _libffi_download_message)
                message(FATAL_ERROR
                    "Failed to download prebuilt libffi from ${_libffi_archive_url}: ${_libffi_download_message}\n${_libffi_download_log}"
                )
            endif()
        endif()

        file(REMOVE_RECURSE "${_libffi_extract_dir}")
        file(MAKE_DIRECTORY "${_libffi_extract_dir}")
        file(ARCHIVE_EXTRACT INPUT "${_libffi_archive_path}" DESTINATION "${_libffi_extract_dir}")

        file(GLOB_RECURSE _libffi_header_candidates "${_libffi_extract_dir}/ffi.h")
        file(GLOB_RECURSE _libffi_import_lib_candidates
            "${_libffi_extract_dir}/libffi-8.lib"
            "${_libffi_extract_dir}/libffi.lib"
            "${_libffi_extract_dir}/ffi.lib"
        )
        file(GLOB_RECURSE _libffi_dll_candidates
            "${_libffi_extract_dir}/libffi-8.dll"
            "${_libffi_extract_dir}/libffi.dll"
        )

        list(LENGTH _libffi_header_candidates _libffi_header_count)
        list(LENGTH _libffi_import_lib_candidates _libffi_import_lib_count)
        if(_libffi_header_count EQUAL 0 OR _libffi_import_lib_count EQUAL 0)
            message(FATAL_ERROR
                "Downloaded libffi archive did not include expected ffi headers/import library.\n"
                "Archive: ${_libffi_archive_path}\n"
                "Extracted to: ${_libffi_extract_dir}"
            )
        endif()

        list(GET _libffi_header_candidates 0 _libffi_header)
        list(GET _libffi_import_lib_candidates 0 CVM_LIBFFI_IMPORT_LIB)
        get_filename_component(CVM_LIBFFI_INCLUDE_DIR "${_libffi_header}" DIRECTORY)

        add_library(cvm_libffi_imported SHARED IMPORTED)
        set_target_properties(cvm_libffi_imported PROPERTIES
            IMPORTED_IMPLIB "${CVM_LIBFFI_IMPORT_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${CVM_LIBFFI_INCLUDE_DIR}"
        )

        list(LENGTH _libffi_dll_candidates _libffi_dll_count)
        if(_libffi_dll_count GREATER 0)
            list(GET _libffi_dll_candidates 0 CVM_LIBFFI_RUNTIME_DLL)
            set_target_properties(cvm_libffi_imported PROPERTIES
                IMPORTED_LOCATION "${CVM_LIBFFI_RUNTIME_DLL}"
            )
        endif()

        set(CVM_LIBFFI_TARGET cvm_libffi_imported)
    else()
        include(FetchContent)
        include(ExternalProject)

        FetchContent_Declare(libffi_src
            URL https://github.com/libffi/libffi/releases/download/${CVM_LIBFFI_GIT_TAG}/libffi-${CVM_LIBFFI_VERSION}.tar.gz
        )
        FetchContent_GetProperties(libffi_src)
        if(NOT libffi_src_POPULATED)
            FetchContent_Populate(libffi_src)
        endif()

        find_program(CVM_MAKE_EXECUTABLE NAMES make gmake REQUIRED)

        set(CVM_LIBFFI_CANONICAL_LIB "${CVM_LIBFFI_INSTALL_DIR}/lib/libffi.a")
        set(_libffi_configure_cmd
            "${libffi_src_SOURCE_DIR}/configure"
            "--prefix=${CVM_LIBFFI_INSTALL_DIR}"
            "--disable-docs"
            "CC=${CMAKE_C_COMPILER}"
        )
        set(_libffi_build_cmd "${CVM_MAKE_EXECUTABLE}" "-C" "${libffi_src_SOURCE_DIR}" "-j")
        set(_libffi_install_cmd "${CVM_MAKE_EXECUTABLE}" "-C" "${libffi_src_SOURCE_DIR}" "install")

        ExternalProject_Add(cvm_libffi_external
            SOURCE_DIR "${libffi_src_SOURCE_DIR}"
            CONFIGURE_COMMAND ${_libffi_configure_cmd}
            BUILD_COMMAND ${_libffi_build_cmd}
            INSTALL_COMMAND ${_libffi_install_cmd}
            BUILD_IN_SOURCE 1
            UPDATE_COMMAND ""
            BUILD_BYPRODUCTS "${CVM_LIBFFI_CANONICAL_LIB}"
        )

        add_library(cvm_libffi_imported UNKNOWN IMPORTED)
        set_target_properties(cvm_libffi_imported PROPERTIES
            IMPORTED_LOCATION "${CVM_LIBFFI_CANONICAL_LIB}"
            INTERFACE_INCLUDE_DIRECTORIES "${CVM_LIBFFI_INSTALL_DIR}/include"
        )
        add_dependencies(cvm_libffi_imported cvm_libffi_external)
        set(CVM_LIBFFI_TARGET cvm_libffi_imported)
    endif()
endif()

# Prefer CMake package targets when available (common with vcpkg and some package managers).
if(NOT CVM_LIBFFI_TARGET)
    find_package(libffi CONFIG QUIET)
    if(TARGET libffi::ffi)
        set(CVM_LIBFFI_TARGET libffi::ffi)
    elseif(TARGET libffi::libffi)
        set(CVM_LIBFFI_TARGET libffi::libffi)
    elseif(TARGET unofficial::libffi::libffi)
        set(CVM_LIBFFI_TARGET unofficial::libffi::libffi)
    elseif(TARGET ffi)
        set(CVM_LIBFFI_TARGET ffi)
    endif()
endif()

# Fallback to pkg-config where available.
if(NOT CVM_LIBFFI_TARGET)
    find_package(PkgConfig QUIET)
    if(PkgConfig_FOUND)
        pkg_check_modules(LIBFFI QUIET IMPORTED_TARGET libffi)
        if(TARGET PkgConfig::LIBFFI)
            set(CVM_LIBFFI_TARGET PkgConfig::LIBFFI)
        endif()
    endif()
endif()

# Final fallback: locate headers/libs manually (helpful on Windows without pkg-config).
if(NOT CVM_LIBFFI_TARGET)
    find_path(LIBFFI_INCLUDE_DIR
        NAMES ffi.h
        HINTS
            ${LIBFFI_ROOT}
            $ENV{LIBFFI_ROOT}
        PATH_SUFFIXES include include/libffi
    )
    find_library(LIBFFI_LIBRARY
        NAMES ffi libffi libffi-8
        HINTS
            ${LIBFFI_ROOT}
            $ENV{LIBFFI_ROOT}
        PATH_SUFFIXES lib lib64
    )

    if(LIBFFI_INCLUDE_DIR AND LIBFFI_LIBRARY)
        add_library(cvm_libffi UNKNOWN IMPORTED)
        set_target_properties(cvm_libffi PROPERTIES
            IMPORTED_LOCATION "${LIBFFI_LIBRARY}"
            INTERFACE_INCLUDE_DIRECTORIES "${LIBFFI_INCLUDE_DIR}"
        )
        set(CVM_LIBFFI_TARGET cvm_libffi)
    endif()
endif()

if(NOT CVM_LIBFFI_TARGET)
    message(FATAL_ERROR
        "libffi was not found and bundled setup was unavailable.\n"
        "On Windows with CVM_BUNDLED_LIBFFI=ON, CMake downloads prebuilt libffi binaries from GitHub releases.\n"
        "Otherwise set -DLIBFFI_ROOT=<path-to-libffi-prefix> or install a system libffi package."
    )
endif()

add_library(chancecode_core_local
    ${CHANCECODE_ROOT}/src/backend.c
    ${CHANCECODE_ROOT}/src/bytecode.c
    ${CHANCECODE_ROOT}/src/diagnostics.c
    ${CHANCECODE_ROOT}/src/loader.c
)

target_include_directories(chancecode_core_local
    PUBLIC
        ${CHANCECODE_ROOT}/include
)

add_executable(cvm
    src/main.c
    src/vm.c
    src/cclib.c
)

target_include_directories(cvm
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/include
)

target_link_libraries(cvm
    PRIVATE
        chancecode_core_local
        ${CVM_LIBFFI_TARGET}
        ${CMAKE_DL_LIBS}
)

if(WIN32 AND CVM_BUNDLED_LIBFFI AND DEFINED CVM_LIBFFI_RUNTIME_DLL AND EXISTS "${CVM_LIBFFI_RUNTIME_DLL}")
    add_custom_command(TARGET cvm POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${CVM_LIBFFI_RUNTIME_DLL}"
            $<TARGET_FILE_DIR:cvm>
    )
endif()
