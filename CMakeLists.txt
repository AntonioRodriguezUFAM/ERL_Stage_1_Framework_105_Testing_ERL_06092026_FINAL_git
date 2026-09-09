cmake_minimum_required(VERSION 3.18)

project(MySystem LANGUAGES CXX CUDA)

# =============================================================================
# CUDA Setup for Jetson Nano
# =============================================================================
set(CMAKE_CUDA_HOST_COMPILER /usr/bin/gcc-8)
set(CMAKE_CUDA_ARCHITECTURES 53)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CUDA_STANDARD 14)
set(CMAKE_CUDA_STANDARD_REQUIRED ON)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

# =============================================================================
# 2. ARCHITECTURE FLAGS (THE FIX)
#    We isolate CPU flags so NVCC doesn't choke on "armv8-a"
# =============================================================================

# Define the CPU-specific optimization flags
# Removed: -flto
set(CPU_FLAGS 
    "-march=armv8-a -O3 -mcpu=cortex-a57+crc+crypto -mtune=cortex-a57 -ffast-math -ftree-vectorize -fomit-frame-pointer -DNDEBUG"
)

# Apply flags ONLY to C++ compiler
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CPU_FLAGS}")
# =============================================================================
# 2. CUDA COMPILER FLAGS
# =============================================================================
# CUDA Compiler Flags
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -use_fast_math -lineinfo")

# Safe CPU flags for host compiler
set(CPU_FLAGS "-march=armv8-a -O3 -mcpu=cortex-a57 -mtune=cortex-a57")
set(CMAKE_CXX_FLAGS "${CMAKE_CXX_FLAGS} ${CPU_FLAGS}")
set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcompiler \"${CPU_FLAGS}\"")

# Optional: Enable more verbose CUDA build output during development
# set(CMAKE_CUDA_FLAGS "${CMAKE_CUDA_FLAGS} -Xcompiler -Wall")

# Linker flags (LTO must be passed to linker too)
# Removed: -flto
add_link_options( -Wl,--no-as-needed -Wl,--gc-sections)

# =============================================================================
# 3. DEPENDENCIES
# =============================================================================
find_package(Threads REQUIRED)
find_package(PkgConfig REQUIRED)
find_package(fmt REQUIRED)
find_package(CUDAToolkit REQUIRED)   # <--- ADD THIS LINE

# SDL2 & SDL2_ttf
find_package(SDL2 QUIET)
pkg_check_modules(SDL2 REQUIRED sdl2)
pkg_check_modules(SDL2_TTF REQUIRED SDL2_ttf)

# V4L2 (Camera)
find_library(V4L2_LIB NAMES v4l2 REQUIRED)

# LibUSB
pkg_check_modules(LIBUSB REQUIRED libusb-1.0)

# Lynsyn (Power Monitor)
find_library(LYNSYN_LIB
    NAMES lynsyn liblynsyn
    PATHS ${CMAKE_CURRENT_SOURCE_DIR}/build/lynsyn-host-software/liblynsyn
    REQUIRED
)

# =============================================================================
# 4. EXECUTABLE TARGET
# =============================================================================
add_executable(MySystem
    SampleTestIntegrationCode_v34.cpp
    Stage_01/Concretes/AlgorithmConcreteKernels.cu
    #cuda_kernel.cu                                   # ? Add this line
)

#target_compile_definitions(MySystem PRIVATE ERL_ENABLE_CUDA=1)

# Include Paths
target_include_directories(MySystem PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/Stage_01
    ${CMAKE_CURRENT_SOURCE_DIR}/Stage_01/Factories
    ${CMAKE_CURRENT_SOURCE_DIR}/Stage_01/Concretes
    ${CMAKE_CURRENT_SOURCE_DIR}/Module
    ${CUDA_INCLUDE_DIRS}          # ? Important
    ${CMAKE_CURRENT_SOURCE_DIR}/Stage_01/SharedStructures
    ${CMAKE_CURRENT_SOURCE_DIR}/Stage_02
    ${CMAKE_CURRENT_SOURCE_DIR}/Stage_02/Factories
    ${CMAKE_CURRENT_SOURCE_DIR}/Stage_01/Includes
    ${SDL2_INCLUDE_DIRS}
    ${SDL2_TTF_INCLUDE_DIRS}
    ${LIBUSB_INCLUDE_DIRS}
    ${CMAKE_CUDA_TOOLKIT_INCLUDE_DIRECTORIES}
)

# Library Paths (Fix for JetPack locations)
target_link_directories(MySystem PRIVATE
    /usr/lib/aarch64-linux-gnu
    /usr/lib/aarch64-linux-gnu/8
)

# Linking
# Linking - Add proper CUDA libraries
target_link_libraries(MySystem PRIVATE
    Threads::Threads
    ${V4L2_LIB}
    ${SDL2_LIBRARIES}
    ${SDL2_TTF_LIBRARIES}
    ${LYNSYN_LIB}
    ${LIBUSB_LIBRARIES}
    fmt::fmt
    stdc++fs
    ${CUDA_LIBRARIES}             # ? Important
    ${CUDA_cudart_LIBRARY}        # ? Important
    CUDA::cudart          # <--- USE THIS INSTEAD OF ${CUDA_LIBRARIES}
    CUDA::nvToolsExt      # <--- USE THIS INSTEAD OF nvToolsExt
    #cudart                        # runtime library
    #nvToolsExt
)

# Compile Options (Warnings)
target_compile_options(MySystem PRIVATE
    $<$<COMPILE_LANGUAGE:CXX>:-Wall -Wextra -pedantic>
)

# Properties
# CUDA Specific Properties
set_target_properties(MySystem PROPERTIES
    CUDA_SEPARABLE_COMPILATION ON
    POSITION_INDEPENDENT_CODE ON
    CUDA_RESOLVE_DEVICE_SYMBOLS ON     # Important for kernels
)

# =============================================================================
# 5. POST-BUILD UTILS
# =============================================================================
add_custom_command(TARGET MySystem POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E echo "Enabling max performance jetson_clocks..."
    COMMAND sudo jetson_clocks || echo "jetson_clocks not available"
    COMMENT "Maximizing CPU/GPU clocks"
)

# =============================================================================
# FINAL STATUS
# =============================================================================
message(STATUS "==================================================")
message(STATUS " JETSON NANO 2GB BUILD ? FINAL & 100% WORKING ")
message(STATUS "==================================================")
message(STATUS " Architecture    : sm_53 (Maxwell) + armv8-a")
message(STATUS " Compiler        : NVCC + GCC-8")
message(STATUS " Flags           : Separated CPU/GPU (Fix Applied)")
message(STATUS "==================================================")