cmake_minimum_required(VERSION 3.24)

project(sui_pow_gpu LANGUAGES CXX CUDA)

option(SUI_POW_PTXAS_VERBOSE "Print CUDA register and spill statistics" ON)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(CUDAToolkit REQUIRED)

add_executable(sui-pow
    src/main.cpp
    src/json_lite.cpp
    src/http_common.cpp
    cuda/pow_cuda.cu
)

target_include_directories(sui-pow PRIVATE include)
target_link_libraries(sui-pow PRIVATE CUDA::cudart)

if(WIN32)
    target_sources(sui-pow PRIVATE src/http_win.cpp)
    target_link_libraries(sui-pow PRIVATE winhttp)
else()
    target_sources(sui-pow PRIVATE src/http_stub.cpp)
endif()

set_target_properties(sui-pow PROPERTIES
    CUDA_ARCHITECTURES 75
    CUDA_STANDARD 17
    CUDA_STANDARD_REQUIRED ON
)
target_compile_options(sui-pow PRIVATE
    $<$<COMPILE_LANGUAGE:CUDA>:-O3>
    $<$<COMPILE_LANGUAGE:CUDA>:-lineinfo>
    $<$<COMPILE_LANGUAGE:CUDA>:-Xptxas=-O3>
)
if(SUI_POW_PTXAS_VERBOSE)
    target_compile_options(sui-pow PRIVATE
        $<$<COMPILE_LANGUAGE:CUDA>:-Xptxas=-v>
    )
endif()

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
add_test(NAME wasm_parity COMMAND sui-pow self-test)
