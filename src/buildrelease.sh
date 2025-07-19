#!/bin/bash
mkdir ../build;
cd ../build;
echo -e "\nRunning cmake...";
cmake -DCMAKE_BUILD_TYPE=MinSizeRel -DLLVM_TARGETS_TO_BUILD="X86" -G Ninja ../src;
echo -e "\nRunning Ninja...";
ninja -j12;
