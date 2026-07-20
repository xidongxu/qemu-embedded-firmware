set(CMAKE_SYSTEM_NAME Generic)

set(CMAKE_SYSTEM_PROCESSOR cortex-m)

set(ARM_GCC_PREFIX arm-none-eabi)
set(CMAKE_C_COMPILER ${ARM_GCC_PREFIX}-gcc)
set(CMAKE_CXX_COMPILER ${ARM_GCC_PREFIX}-g++)
set(CMAKE_ASM_COMPILER ${ARM_GCC_PREFIX}-gcc)

set(CMAKE_OBJCOPY ${ARM_GCC_PREFIX}-objcopy)
set(CMAKE_EXECUTABLE_SUFFIX "" CACHE STRING "Executable suffix")

set(CMAKE_C_FLAGS_INIT "-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mcmse -Wall -fdata-sections -ffunction-sections -fno-exceptions")
set(CMAKE_ASM_FLAGS_INIT "-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mcmse -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "-mcpu=cortex-m33 -mthumb -mfpu=fpv5-sp-d16 -mfloat-abi=hard -mcmse --specs=nosys.specs -Wl,-gc-sections")

set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)
