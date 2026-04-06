cmake_minimum_required(VERSION 3.20)

project(CVM LANGUAGES C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

set(CHANCECODE_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../ChanceCode" CACHE PATH "Path to ChanceCode repository")

find_package(PkgConfig REQUIRED)
pkg_check_modules(LIBFFI REQUIRED IMPORTED_TARGET libffi)

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
    PkgConfig::LIBFFI
        ${CMAKE_DL_LIBS}
)
