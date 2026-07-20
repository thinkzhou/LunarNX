# CMake toolchain for Nintendo Switch (devkitA64)
# Based on devkitPro's switch-dev toolchain

if(NOT DEFINED ENV{DEVKITPRO})
    message(FATAL_ERROR "DEVKITPRO environment variable not set. Please install devkitPro first.")
endif()

set(DEVKITPRO "$ENV{DEVKITPRO}")
set(DEVKITA64 "${DEVKITPRO}/devkitA64")

if(NOT EXISTS "${DEVKITA64}")
    message(FATAL_ERROR "devkitA64 not found at ${DEVKITA64}. Run: sudo dkp-pacman -S switch-dev")
endif()

# Toolchain settings
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR aarch64)
set(CMAKE_CROSSCOMPILING ON)

# Compilers
set(CMAKE_C_COMPILER "${DEVKITA64}/bin/aarch64-none-elf-gcc")
set(CMAKE_CXX_COMPILER "${DEVKITA64}/bin/aarch64-none-elf-g++")
set(CMAKE_ASM_COMPILER "${DEVKITA64}/bin/aarch64-none-elf-gcc")
set(CMAKE_AR "${DEVKITA64}/bin/aarch64-none-elf-ar" CACHE STRING "")
set(CMAKE_RANLIB "${DEVKITA64}/bin/aarch64-none-elf-ranlib" CACHE STRING "")
set(CMAKE_OBJCOPY "${DEVKITA64}/bin/aarch64-none-elf-objcopy" CACHE STRING "")
set(CMAKE_OBJDUMP "${DEVKITA64}/bin/aarch64-none-elf-objdump" CACHE STRING "")
set(CMAKE_NM "${DEVKITA64}/bin/aarch64-none-elf-nm" CACHE STRING "")

# Compiler flags for Switch
set(ARCH_FLAGS "-march=armv8-a+crc+crypto -mtune=cortex-a57 -mtp=soft -fPIC -ftls-model=local-exec")
set(CMAKE_C_FLAGS "${ARCH_FLAGS} -ffunction-sections -fdata-sections -D__SWITCH__" CACHE STRING "")
set(CMAKE_CXX_FLAGS "${ARCH_FLAGS} -ffunction-sections -fdata-sections -D__SWITCH__ -fno-exceptions -fno-rtti" CACHE STRING "")
set(CMAKE_ASM_FLAGS "${ARCH_FLAGS}" CACHE STRING "")
set(CMAKE_EXE_LINKER_FLAGS "-specs=${DEVKITA64}/libnx/switch.specs -Wl,--gc-sections" CACHE STRING "")

# Find libraries
set(CMAKE_FIND_ROOT_PATH "${DEVKITA64};${DEVKITPRO}/portlibs/switch;${DEVKITPRO}/portlibs/switch/bin")
set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_PACKAGE ONLY)

message(STATUS "Switch toolchain: ${DEVKITA64}")
