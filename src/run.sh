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
echo -e "\nRunning compiler tests...";
./asa --runtests;
echo -e "\nRunning language tests...";
./asa ../modules/Tests/main_tests.asa && ../modules/Tests/build/main_tests && echo -e "\nAll language tests passed ✔" || echo -e "\nLanguage tests FAILED ✖";
