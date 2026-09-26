cmake_minimum_required(VERSION 3.25)
project(parent_benchmark VERSION 1.9.5 LANGUAGES CXX)
add_library(benchmark INTERFACE)
add_library(benchmark::benchmark ALIAS benchmark)
