cmake_minimum_required(VERSION 3.21)
project(configqml VERSION 0.1.0 DESCRIPTION "Layered configuration for Qt" LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Qt6 6.12 REQUIRED COMPONENTS Core)

add_library(configqml_core SHARED configvalue.cpp configvalue.h)
add_library(configqml::core ALIAS configqml_core)
set_target_properties(configqml_core PROPERTIES EXPORT_NAME core)
target_link_libraries(configqml_core PUBLIC Qt6::Core)
target_include_directories(configqml_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/configqml>)
target_compile_options(configqml_core PRIVATE -Wall -Wextra -Wpedantic)

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Test)
    add_executable(test_core_link tests/test_core_link.cpp)
    target_link_libraries(test_core_link PRIVATE configqml::core)
    add_test(NAME core_link COMMAND ${CMAKE_COMMAND}
        -DPROGRAM=$<TARGET_FILE:test_core_link>
        -DLIBRARY=$<TARGET_FILE:configqml_core>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/core_link.cmake)
endif()
