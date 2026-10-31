cmake_minimum_required(VERSION 3.21)
project(late_target_options LANGUAGES CXX)
add_subdirectory(.. child)
# The parent can add instrumentation after the child configured its tests.
target_compile_options(work PRIVATE -fsanitize=undefined)
target_link_options(timing_probe PRIVATE -fsanitize=undefined)
