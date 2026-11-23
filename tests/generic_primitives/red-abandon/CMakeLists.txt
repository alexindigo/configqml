cmake_minimum_required(VERSION 3.21)
project(configqml_abandon_red LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
# Public API only: builds against the v0.5.3 baseline and later candidates.
find_package(configqml 0.5.3 REQUIRED COMPONENTS Qml)
find_package(Qt6 6.12 REQUIRED COMPONENTS Qml)
if(NOT EXISTS "${CONFIGQML_IMPORT_ROOT}/ConfigQml/qmldir")
    message(FATAL_ERROR "CONFIGQML_IMPORT_ROOT must name the installed QML root")
endif()
add_executable(configqml-abandon-red main.cpp)
target_link_libraries(configqml-abandon-red PRIVATE configqml::configqml Qt6::Qml)
target_compile_definitions(configqml-abandon-red PRIVATE
    RED_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/failed-apply.qml"
    RED_IMPORT_ROOT="${CONFIGQML_IMPORT_ROOT}")
