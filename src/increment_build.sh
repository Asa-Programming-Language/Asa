#!/bin/bash

current_build=$(cat ../build_num)

new_build=$((current_build + 1))

echo "$new_build" > ../build_num

echo "Build $new_build"
