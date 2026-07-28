# qemu embedded firmware

## toolchain
- Arm GNU Toolchain 15.3.Rel1 (Build arm-15.149)

## board 
- mps2-an505

## build
```shell
cmake -S . -B build -G Ninja -DBOARD=mps2-an505 -DPROJECT=FreeRTOS -DCMAKE_TOOLCHAIN_FILE="${PWD}/cmake/arm-none-eabi-gcc.cmake"
cmake --build build
```
