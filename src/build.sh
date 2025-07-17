#!/bin/bash
cd ../build;
echo -e "\nRunning cmake...";
cmake -G Ninja ../src;
echo -e "\nRunning Ninja...";
ninja -j12;
