#!/bin/bash
cd ../build;
#echo -e "\nEnabling caching...";
#export CC="ccache gcc";
#export CXX="ccache g++";
echo -e "\nRunning cmake...";
cmake -G Ninja ../src;
echo -e "\nRunning Ninja...";
ninja -j4;
echo -e "\nStarting asa...";
./asa --runtests;
