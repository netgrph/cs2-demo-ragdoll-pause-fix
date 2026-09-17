cmake_minimum_required(VERSION 3.20)
project(RagdollDemoClock CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(RagdollDemoClock SHARED src/RagdollDemoClock.cpp)
target_compile_definitions(RagdollDemoClock PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(RagdollDemoClock PRIVATE /W3 /EHsc /Zi)
target_link_options(RagdollDemoClock PRIVATE /DEBUG /OPT:REF /OPT:ICF)
set_target_properties(RagdollDemoClock PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin")
