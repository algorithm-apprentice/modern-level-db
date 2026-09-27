cmake_minimum_required(VERSION 3.25)
project(provider_leveldb LANGUAGES CXX)

include(FetchContent)
FetchContent_SetPopulated(
  modern_leveldb_reference
  SOURCE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/source"
  BINARY_DIR "${CMAKE_CURRENT_BINARY_DIR}/source-build"
)
include("${MODERN_LEVELDB_SOURCE_DIR}/cmake/LevelDbReference.cmake")
modern_leveldb_add_reference()
