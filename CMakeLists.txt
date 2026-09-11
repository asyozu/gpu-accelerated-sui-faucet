cmake_minimum_required(VERSION 3.24)

project(sui_pow LANGUAGES CXX CUDA)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

include(FetchContent)
find_package(CUDAToolkit REQUIRED)

FetchContent_Declare(
    argon2_gpu
    URL https://github.com/WebDollar/argon2-gpu/archive/f5fce223d16769f202449c9b0837ad3533304acd.tar.gz
    URL_HASH SHA256=8B53B2C2FDB4DAF51C596E4881377AEDBBBE9F8204D26DE03A1994C9D8A87EE8
)
cmake_policy(SET CMP0169 OLD)
FetchContent_GetProperties(argon2_gpu)
if(NOT argon2_gpu_POPULATED)
    FetchContent_Populate(argon2_gpu)
endif()

add_library(argon2-gpu-common STATIC
    ${argon2_gpu_SOURCE_DIR}/lib/argon2-gpu-common/argon2params.cpp
    ${argon2_gpu_SOURCE_DIR}/lib/argon2-gpu-common/blake2b.cpp
)
target_include_directories(argon2-gpu-common PUBLIC
    ${argon2_gpu_SOURCE_DIR}/include
    ${argon2_gpu_SOURCE_DIR}/include/argon2-gpu-common
    ${argon2_gpu_SOURCE_DIR}/lib/argon2-gpu-common
)
target_precompile_headers(argon2-gpu-common PRIVATE <cstddef>)

add_library(argon2-cuda STATIC
    ${argon2_gpu_SOURCE_DIR}/lib/argon2-cuda/kernels.cu
)
target_include_directories(argon2-cuda PUBLIC
    ${argon2_gpu_SOURCE_DIR}/include
    ${argon2_gpu_SOURCE_DIR}/include/argon2-cuda
    ${argon2_gpu_SOURCE_DIR}/include/argon2-gpu-common
)
target_compile_definitions(argon2-cuda PUBLIC HAVE_CUDA=1)
target_link_libraries(argon2-cuda PUBLIC argon2-gpu-common CUDA::cudart)
target_compile_options(argon2-cuda PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:-O3>
    $<$<COMPILE_LANGUAGE:CUDA>:-lineinfo>
)
set_target_properties(argon2-cuda PROPERTIES CUDA_ARCHITECTURES 75)

add_executable(sui-pow
    src/main.cpp
    src/json_lite.cpp
    src/http_common.cpp
    src/pow.cpp
)

target_include_directories(sui-pow PRIVATE include)
target_link_libraries(sui-pow PRIVATE argon2-cuda CUDA::cudart)

if(WIN32)
    target_sources(sui-pow PRIVATE src/http_win.cpp)
    target_link_libraries(sui-pow PRIVATE winhttp)
else()
    target_sources(sui-pow PRIVATE src/http_stub.cpp)
endif()

set_target_properties(sui-pow PROPERTIES
    CXX_STANDARD 17
    CXX_STANDARD_REQUIRED ON
    CXX_EXTENSIONS OFF
    CUDA_ARCHITECTURES 75
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
)

if(MSVC)
    target_compile_options(sui-pow PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:/O2>
        $<$<COMPILE_LANGUAGE:CXX>:/EHsc>
    )
else()
    target_compile_options(sui-pow PRIVATE
        $<$<COMPILE_LANGUAGE:CXX>:-O3>
        $<$<COMPILE_LANGUAGE:CXX>:-Wall>
        $<$<COMPILE_LANGUAGE:CXX>:-Wextra>
        $<$<COMPILE_LANGUAGE:CXX>:-Wpedantic>
    )
endif()

enable_testing()
add_test(NAME pow_v3_vectors COMMAND sui-pow self-test)
