cmake_minimum_required(VERSION 3.28)

project(voxel_engine VERSION 0.1.0 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

option(VOX_BUILD_TESTS "Build unit and integration tests" ON)
option(VOX_BUILD_BENCHMARKS "Build benchmarks" ON)
option(VOX_NATIVE_ARCH "Build with -march=native" OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()

if(VOX_NATIVE_ARCH)
    add_compile_options(-march=native)
endif()

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    add_compile_options(
        -Wall
        -Wextra
        -Wpedantic
        -Wshadow
        -Wnon-virtual-dtor
        -Wno-unused-parameter
    )
endif()

find_package(PkgConfig REQUIRED)
pkg_check_modules(SDL3 REQUIRED IMPORTED_TARGET sdl3)
pkg_check_modules(ZSTD REQUIRED IMPORTED_TARGET libzstd)

include(FetchContent)

# CMake 4 removed compatibility with projects declaring < 3.5. ENet still does.
set(CMAKE_POLICY_VERSION_MINIMUM 3.5 CACHE STRING "" FORCE)

# ---- Jolt ------------------------------------------------------------------
set(JPH_USE_VK OFF CACHE BOOL "" FORCE)
set(JPH_USE_DX12 OFF CACHE BOOL "" FORCE)
set(JPH_USE_MTL OFF CACHE BOOL "" FORCE)
set(JPH_USE_CPU_COMPUTE OFF CACHE BOOL "" FORCE)
set(OVERRIDE_CXX_FLAGS OFF CACHE BOOL "" FORCE)
set(ENABLE_ALL_WARNINGS OFF CACHE BOOL "" FORCE)
set(INTERPROCEDURAL_OPTIMIZATION OFF CACHE BOOL "" FORCE)
set(JPH_BUILD_SHARED_LIBS OFF CACHE BOOL "" FORCE)
set(GENERATE_DEBUG_SYMBOLS OFF CACHE BOOL "" FORCE)
set(ENABLE_INSTALL OFF CACHE BOOL "" FORCE)
FetchContent_Declare(
    joltphysics
    GIT_REPOSITORY https://github.com/jrouwe/JoltPhysics.git
    GIT_TAG v5.6.0
    GIT_SHALLOW TRUE
    SOURCE_SUBDIR Build
)
FetchContent_MakeAvailable(joltphysics)

# ---- ENet ------------------------------------------------------------------
FetchContent_Declare(
    enet
    GIT_REPOSITORY https://github.com/lsalzman/enet.git
    GIT_TAG v1.3.18
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(enet)

# ---- Core ------------------------------------------------------------------
file(GLOB_RECURSE VOX_CORE_SOURCES CONFIGURE_DEPENDS
    src/core/*.cpp
    src/voxel/*.cpp
    src/worldgen/*.cpp
)

add_library(vox_core STATIC ${VOX_CORE_SOURCES})
target_include_directories(vox_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/third_party
)
target_link_libraries(vox_core PUBLIC PkgConfig::ZSTD)
target_compile_definitions(vox_core PUBLIC
    VOX_VERSION="${PROJECT_VERSION}"
)

# ---- Networking ------------------------------------------------------------
file(GLOB_RECURSE VOX_NET_SOURCES CONFIGURE_DEPENDS src/net/*.cpp)

add_library(vox_net STATIC ${VOX_NET_SOURCES})
target_include_directories(vox_net PUBLIC ${enet_SOURCE_DIR}/include)
target_link_libraries(vox_net PUBLIC vox_core enet)

# ---- Server ----------------------------------------------------------------
file(GLOB_RECURSE VOX_SERVER_SOURCES CONFIGURE_DEPENDS src/server/*.cpp)

add_library(vox_server_lib STATIC ${VOX_SERVER_SOURCES})
target_link_libraries(vox_server_lib PUBLIC vox_net vox_core)

# ---- Executables -----------------------------------------------------------
add_executable(voxel_server src/apps/server_main.cpp)
target_link_libraries(voxel_server PRIVATE vox_server_lib vox_net vox_core)

add_executable(voxel_client src/apps/client_main.cpp)
target_link_libraries(voxel_client PRIVATE vox_server_lib vox_net vox_core PkgConfig::SDL3)

# ---- Tests -----------------------------------------------------------------
if(VOX_BUILD_TESTS)
    enable_testing()
    add_executable(voxel_tests tests/test_main.cpp)
    target_link_libraries(voxel_tests PRIVATE vox_server_lib vox_net vox_core)
    add_test(NAME voxel_tests COMMAND voxel_tests)
endif()

# ---- Benchmarks ------------------------------------------------------------
if(VOX_BUILD_BENCHMARKS)
    add_executable(voxel_bench benchmarks/bench_chunk.cpp)
    target_link_libraries(voxel_bench PRIVATE vox_core)
endif()
