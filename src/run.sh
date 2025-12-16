#!/bin/bash
mkdir ../build;
cd ../build;
#echo -e "\nEnabling caching...";
#export CC="ccache gcc";
#export CXX="ccache g++";
echo -e "\nCleaning up build files...";
find ../modules -name "*.ll.s" -type f -delete;
find ../modules -name "*.ll" -type f -delete;
echo -e "\nRunning cmake...";
cmake -G Ninja ../src;
echo -e "\nRunning Ninja...";
ninja -j2
cmake --install .;
echo -e "\nIncrementing build number...";
../src/increment_build.sh;
echo -e "\nStarting asa...";
./asa --runtests;
