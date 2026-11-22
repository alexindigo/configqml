cmake_minimum_required(VERSION 3.21)
project(configqml_legacy_performance LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Test)
if(NOT EXISTS "${CONFIGQML_SOURCE}/configstore.h" OR
   NOT EXISTS "${CONFIGQML_BUILD}/libconfigqml_core.so.0.5")
    message(FATAL_ERROR "An existing matching source/build pair is required")
endif()
add_executable(legacy-performance legacy.cpp)
target_include_directories(legacy-performance PRIVATE "${CONFIGQML_SOURCE}")
target_link_libraries(legacy-performance PRIVATE
    "${CONFIGQML_BUILD}/libconfigqml_core.so.0.5" Qt6::Core Qt6::Test)
if(CONFIGQML_MEASURE_HANDLES)
    add_executable(handle-performance handles.cpp)
    target_include_directories(handle-performance PRIVATE "${CONFIGQML_SOURCE}" "${CONFIGQML_SOURCE}/tests")
    target_link_libraries(handle-performance PRIVATE
        "${CONFIGQML_BUILD}/libconfigqml_core.so.0.5" Qt6::Core Qt6::Test)
    add_executable(layer-structure structure.cpp)
    target_include_directories(layer-structure PRIVATE "${CONFIGQML_SOURCE}")
    target_link_libraries(layer-structure PRIVATE
        "${CONFIGQML_BUILD}/libconfigqml_core.so.0.5" Qt6::Core Qt6::Test ${CMAKE_DL_LIBS})
    target_compile_definitions(layer-structure PRIVATE
        CONFIGQML_PUT_VALUE_SYMBOL="${CONFIGQML_PUT_VALUE_SYMBOL}")
endif()
