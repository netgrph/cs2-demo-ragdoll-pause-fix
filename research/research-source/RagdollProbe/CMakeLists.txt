cmake_minimum_required(VERSION 3.20)
project(RagdollProbe CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static CRT: the DLL has no runtime dependencies besides Windows system DLLs.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(RagdollProbe SHARED src/RagdollProbe.cpp)
target_compile_definitions(RagdollProbe PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(RagdollProbe PRIVATE /W3 /EHsc /Zi)
target_link_options(RagdollProbe PRIVATE /DEBUG /OPT:REF /OPT:ICF)
set_target_properties(RagdollProbe PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin")
