#!/bin/bash
cd ../build;
cmake ../src;
make -j12;
./asa;
