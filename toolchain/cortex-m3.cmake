# Name of the target
set(CMAKE_SYSTEM_NAME arm-none-eabi)
set(CMAKE_SYSTEM_PROCESSOR cortex-m3)

set(MCPU_FLAGS "-mthumb -mcpu=cortex-m3")
set(VFP_FLAGS "")


include(${CMAKE_CURRENT_LIST_DIR}/common/arm-none-eabi.cmake)