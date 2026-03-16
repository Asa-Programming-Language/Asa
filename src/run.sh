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
ninja_output=$(ninja -j2 2>&1);
echo "$ninja_output";
cmake --install .;
if echo "$ninja_output" | grep -q "no work to do"; then
	echo -e "\nNo changes, skipping build number increment.";
else
	echo -e "\nIncrementing build number...";
	../src/increment_build.sh;
fi
echo -e "\nRunning compiler tests...";
./asa --runtests;
echo -e "\nRunning language tests...";
./asa ../modules/Tests/main_tests.asa && ../modules/Tests/build/main_tests && echo -e "\nAll language tests passed ✔" || echo -e "\nLanguage tests FAILED ✖";
