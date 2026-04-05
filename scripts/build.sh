#!/bin/bash

if [[ "$1" == "debug" ]]; then
    flags="-DCMAKE_BUILD_TYPE=Debug"
else
    flags=""
fi

cmake -B build/ -DCMAKE_EXPORT_COMPILE_COMMANDS=ON $flags
cmake --build build/
