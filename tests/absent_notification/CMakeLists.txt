cmake_minimum_required(VERSION 3.21)
project(absent_notification_consumer LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_AUTOMOC ON)
find_package(configqml 0.5.2 REQUIRED COMPONENTS Core Qml)
find_package(Qt6 REQUIRED COMPONENTS Test Qml)
add_executable(absent-notification ../test_qml_absent_notification.cpp)
target_link_libraries(absent-notification PRIVATE configqml::configqml Qt6::Test)
target_compile_definitions(absent-notification PRIVATE
    QML_IMPORT_ROOT="${CONFIGQML_INSTALLED_IMPORT_ROOT}"
    ABSENT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../tst_absent_notification.qml")
add_executable(import-only-absent import.cpp)
target_link_libraries(import-only-absent PRIVATE Qt6::Qml Qt6::Test)
target_compile_definitions(import-only-absent PRIVATE
    QML_IMPORT_ROOT="${CONFIGQML_INSTALLED_IMPORT_ROOT}"
    ABSENT_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/../tst_absent_notification.qml")
