cmake_minimum_required(VERSION 3.25)
project(predeclared_leveldb LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(
  modern_leveldb_reference
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/source"
)
include("${MODERN_LEVELDB_SOURCE_DIR}/cmake/LevelDbReference.cmake")
modern_leveldb_add_reference()
