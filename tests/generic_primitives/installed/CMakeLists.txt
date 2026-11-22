cmake_minimum_required(VERSION 3.21)
project(configqml_installed_slice LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
find_package(configqml REQUIRED COMPONENTS Core)
find_package(Qt6 6.12 REQUIRED COMPONENTS Core Qml)
add_executable(installed-native native.cpp)
target_link_libraries(installed-native PRIVATE configqml::core)
add_executable(installed-plugin ../compat/qml.cpp)
target_link_libraries(installed-plugin PRIVATE Qt6::Qml)
target_compile_definitions(installed-plugin PRIVATE
    CONFIGQML_CLIENT_MARKER="GENERIC_INSTALLED_QML_OK snapshot=3")
include(../../../cmake/ConfigQmlLint.cmake)
add_custom_target(installed-qml-lint
    COMMAND Qt6::qmllint ${_configqml_qmllint_args} -I "${CONFIGQML_IMPORT_ROOT}"
        "${CMAKE_CURRENT_SOURCE_DIR}/slice.qml")
