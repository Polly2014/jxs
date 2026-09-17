set(CMAKE_SYSTEM_NAME Linux)
set(CMAKE_SYSTEM_PROCESSOR aarch64)

if(DEFINED ENV{CROSS_COMPILE})
    set(_JXS_CROSS_PREFIX "$ENV{CROSS_COMPILE}")
else()
    set(_JXS_CROSS_PREFIX "aarch64-linux-gnu-")
endif()
set(CMAKE_C_COMPILER "${_JXS_CROSS_PREFIX}gcc")
set(CMAKE_CXX_COMPILER "${_JXS_CROSS_PREFIX}g++")

# Keep the instruction set common to A76 and A55; tune scheduling for A76.
set(CMAKE_C_FLAGS_INIT "-march=armv8-a -mtune=cortex-a76")
set(CMAKE_CXX_FLAGS_INIT "-march=armv8-a -mtune=cortex-a76")
