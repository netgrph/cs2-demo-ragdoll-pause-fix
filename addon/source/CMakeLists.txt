cmake_minimum_required(VERSION 3.20)
project(RagdollPauseFix CXX RC)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Static C runtime: the DLL has no dependencies besides Windows itself.
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

add_library(RagdollPauseFix SHARED src/RagdollPauseFix.cpp src/RagdollPauseFix.rc)
target_compile_definitions(RagdollPauseFix PRIVATE WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(RagdollPauseFix PRIVATE $<$<COMPILE_LANGUAGE:CXX>:/W3 /EHsc /Zi>)
target_link_options(RagdollPauseFix PRIVATE /DEBUG /OPT:REF /OPT:ICF)
set_target_properties(RagdollPauseFix PROPERTIES
  RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/bin"
  RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/bin")
