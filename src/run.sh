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
cmake -G Ninja ../src || { echo -e "\ncmake failed."; exit 1; }
echo -e "\nRunning Ninja...";
ninja_log=$(mktemp);
ninja -j2 2>&1 | tee "$ninja_log";
ninja_exit=${PIPESTATUS[0]};
cmake --install .;
if [ $ninja_exit -ne 0 ]; then
	rm "$ninja_log";
	echo -e "\nNinja failed.";
	exit 1;
fi
if grep -q "no work to do" "$ninja_log"; then
	echo -e "\nNo changes, skipping build number increment.";
else
	echo -e "\nIncrementing build number...";
	../src/increment_build.sh;
fi
rm "$ninja_log";
echo -e "\nRunning compiler tests...";
./asa --runtests || { echo -e "\nCompiler tests failed."; exit 1; }
echo -e "\nRunning language tests...";
tmptime=$(mktemp);
TIMEFORMAT=$'[total] real\t%Rs\n';
{ time ./asa --time ../modules/Tests/main_tests.asa; } 2>"$tmptime";
compile_exit=$?;
grep -vP '^\[llc\]|^\[clang\]|^\[total\]' "$tmptime" >&2;
llc_real=$(grep -oP '\[llc\] real\t\K[\d.]+' "$tmptime" | head -1);
clang_real=$(grep -oP '\[clang\] real\t\K[\d.]+' "$tmptime" | head -1);
total_real=$(grep -oP '\[total\] real\t\K[\d.]+' "$tmptime" | head -1);
rm -f "$tmptime";
if [ -n "$llc_real" ] && [ -n "$clang_real" ] && [ -n "$total_real" ]; then
	asa_real=$(awk "BEGIN {printf \"%.3f\", $total_real - $llc_real - $clang_real}");
	printf "\n[llc] time\t${llc_real}s\n[clang] time\t${clang_real}s\n[asa] time\t${asa_real}s\n[total] time\t${total_real}s\n\n";
fi
if [ $compile_exit -ne 0 ]; then echo -e "\nCompile failed."; exit 1; fi
../modules/Tests/build/main_tests && echo -e "\nAll language tests passed ✔" || { echo -e "\nLanguage tests FAILED ✖"; exit 1; }
