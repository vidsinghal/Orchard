#!/bin/bash

# create a direcotry for the fused code and make a copy of the original code  
rm -r FUSED
mkdir FUSED
cp  ./UNFUSED/* ./FUSED/  

# run grafter on the created copy 
orchard -max-merged-f=1  -max-merged-n=5 ./FUSED/main.cpp -- -I/usr/lib/gcc/x86_64-linux-gnu/9/include/ -I/build/opencilk/lib/clang/14.0.6/include/ -I/usr/local/bin/../lib/clang/3.8.0/include/ -I/usr/local/include/c++/v1/ -std=c++11 greedy

clang-format -i FUSED/main.cpp 
