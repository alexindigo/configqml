cmake_minimum_required(VERSION 3.21)
project(configqml VERSION 0.1.0 DESCRIPTION "Layered configuration for Qt" LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Qt6 6.12 REQUIRED COMPONENTS Core)

add_library(configqml_core SHARED configvalue.cpp configvalue.h configschema.cpp configschema.h
    confignumber.cpp confignumber.h configdiagnostic.cpp
    configlayers.cpp configlayers.h configscope.cpp configscope.h
    configenvironment.cpp configenvironment.h configbackend.h configcodec.h
    configmemorybackend.cpp configmemorybackend.h
    configjsoncodec.cpp configjsoncodec.h configjsonbackend.cpp configjsonbackend.h
    configconsumer_p.h configresult.h configstore.h configstore.cpp configworker_p.h configworker.cpp
    configshutdown_p.h configshutdown.cpp)
target_sources(configqml_core PRIVATE configprojection_p.h configprojection.cpp
    configlistmodel.h configlistmodel.cpp configdraft.h configdraft.cpp configgroup.cpp
    configstorehandle.h configstorehandle.cpp configmigration.cpp)
add_library(configqml::core ALIAS configqml_core)
set_target_properties(configqml_core PROPERTIES EXPORT_NAME core)
target_link_libraries(configqml_core PUBLIC Qt6::Core)
target_include_directories(configqml_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include/configqml>)
target_compile_options(configqml_core PRIVATE -Wall -Wextra -Wpedantic)

option(CONFIGQML_BUILD_QML "Build the ConfigQml QML facade" ON)
if(CONFIGQML_BUILD_QML)
    # QMetaObjectBuilder is a private ABI. Record and check its exact build identity.
    set(CONFIGQML_PRIVATE_QT_VERSION "6.12.0")
    find_package(Qt6 ${CONFIGQML_PRIVATE_QT_VERSION} EXACT REQUIRED COMPONENTS Core Qml)
    find_package(Qt6CorePrivate ${CONFIGQML_PRIVATE_QT_VERSION} EXACT REQUIRED)
    foreach(component Core CorePrivate Qml)
        if(NOT Qt6${component}_VERSION VERSION_EQUAL CONFIGQML_PRIVATE_QT_VERSION)
            message(FATAL_ERROR "Private adapter requires exact matching Qt ${component} ${CONFIGQML_PRIVATE_QT_VERSION}")
        endif()
    endforeach()
    file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/configqml-qt-build.txt"
        "Core=${Qt6Core_VERSION}\nCorePrivate=${Qt6CorePrivate_VERSION}\nQml=${Qt6Qml_VERSION}\n")
    qt_policy(SET QTP0001 NEW)
    qt_extract_metatypes(configqml_core)
    qt_add_qml_module(configqml SHARED URI ConfigQml VERSION 1.0
        OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/ConfigQml"
        SOURCES configqmldeclaration.h configqmldeclaration.cpp
            configqmlstore.h configqmlstore.cpp configqmlvalue.h configqmlvalue.cpp
            confignamespace.cpp configfacade.h configfacade.cpp)
    target_sources(configqml PRIVATE configqmlmodel.h configqmlmigration.cpp configqmlhandles.cpp)
    add_library(configqml::configqml ALIAS configqml)
    target_link_libraries(configqml PUBLIC configqml::core Qt6::Qml PRIVATE Qt6::CorePrivate)
    target_include_directories(configqml PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>)
    target_compile_options(configqml PRIVATE -Wall -Wextra -Wpedantic)
endif()

include(CTest)
if(BUILD_TESTING)
    find_package(Qt6 6.12 REQUIRED COMPONENTS Test Gui)
    add_executable(test_schema tests/test_schema.cpp)
    target_link_libraries(test_schema PRIVATE configqml::core Qt6::Test)
    add_test(NAME schema COMMAND test_schema)
    add_executable(test_layers tests/test_layers.cpp)
    target_link_libraries(test_layers PRIVATE configqml::core Qt6::Test)
    add_test(NAME layers COMMAND test_layers)
    add_executable(test_routing tests/test_routing.cpp)
    target_link_libraries(test_routing PRIVATE configqml::core Qt6::Test)
    add_test(NAME routing COMMAND test_routing)
    add_executable(test_scopes_env tests/test_scopes_env.cpp)
    target_link_libraries(test_scopes_env PRIVATE configqml::core Qt6::Test)
    add_test(NAME scopes_env COMMAND test_scopes_env)
    add_executable(test_backend tests/test_backend.cpp)
    target_link_libraries(test_backend PRIVATE configqml::core Qt6::Test)
    add_test(NAME backend COMMAND test_backend)
    add_executable(test_contract_edges tests/test_contract_edges.cpp)
    target_link_libraries(test_contract_edges PRIVATE configqml::core Qt6::Test)
    add_test(NAME contract_edges COMMAND test_contract_edges)
    add_executable(test_worker tests/test_worker.cpp)
    target_link_libraries(test_worker PRIVATE configqml::core Qt6::Test)
    add_test(NAME worker COMMAND test_worker)
    add_executable(test_editors tests/test_editors.cpp)
    target_link_libraries(test_editors PRIVATE configqml::core Qt6::Test)
    add_test(NAME editors COMMAND test_editors)
    if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
        target_compile_definitions(test_editors PRIVATE CONFIGQML_INSTRUMENTED_BUILD)
    endif()
    add_executable(test_models tests/test_models.cpp)
    target_link_libraries(test_models PRIVATE configqml::core Qt6::Test Qt6::Gui)
    add_test(NAME models COMMAND test_models)
    if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
        target_compile_definitions(test_models PRIVATE CONFIGQML_INSTRUMENTED_BUILD)
    endif()
    set_tests_properties(models PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
    add_executable(file_writer tests/file_writer.cpp)
    target_link_libraries(file_writer PRIVATE configqml::core)
    add_executable(test_json_files tests/test_json_files.cpp)
    target_link_libraries(test_json_files PRIVATE configqml::core Qt6::Test)
    add_dependencies(test_json_files file_writer)
    add_test(NAME json_files COMMAND test_json_files)
    add_executable(test_consumer_lifetime tests/test_consumer_lifetime.cpp)
    target_link_libraries(test_consumer_lifetime PRIVATE configqml::core Qt6::Test)
    add_test(NAME consumer_lifetime COMMAND test_consumer_lifetime)
    if(CONFIGQML_BUILD_QML)
        find_package(Qt6 6.12 REQUIRED COMPONENTS Quick)
        add_executable(test_m5_boundaries tests/test_m5_boundaries.cpp)
        target_link_libraries(test_m5_boundaries PRIVATE configqml::configqml Qt6::Test)
        target_compile_definitions(test_m5_boundaries PRIVATE QML_IMPORT_ROOT="${CMAKE_CURRENT_BINARY_DIR}")
        add_test(NAME m5_boundaries COMMAND test_m5_boundaries)
        add_executable(test_qml_editors tests/test_qml_editors.cpp)
        target_link_libraries(test_qml_editors PRIVATE configqml::configqml Qt6::Test)
        target_compile_definitions(test_qml_editors PRIVATE
            QML_IMPORT_ROOT="${CMAKE_CURRENT_BINARY_DIR}"
            QML_EDITOR_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_editors.qml")
        add_test(NAME qml_editors COMMAND test_qml_editors)
        if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
            target_compile_definitions(test_qml_editors PRIVATE CONFIGQML_INSTRUMENTED_BUILD)
        endif()
        add_test(NAME editors_qml_lint COMMAND $<TARGET_FILE:Qt6::qmllint>
            -I ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_editors.qml)
        add_executable(test_qml_models tests/test_qml_models.cpp)
        target_link_libraries(test_qml_models PRIVATE configqml::configqml Qt6::Test Qt6::Quick)
        target_compile_definitions(test_qml_models PRIVATE
            QML_IMPORT_ROOT="${CMAKE_CURRENT_BINARY_DIR}"
            QML_MODEL_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_models.qml")
        add_test(NAME qml_models COMMAND test_qml_models)
        if(CMAKE_CXX_FLAGS MATCHES "fsanitize")
            target_compile_definitions(test_qml_models PRIVATE CONFIGQML_INSTRUMENTED_BUILD)
        endif()
        set_tests_properties(qml_models PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen;QSG_RHI_BACKEND=software")
        add_test(NAME models_qml_lint COMMAND $<TARGET_FILE:Qt6::qmllint>
            -I ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_models.qml)
        add_executable(test_qml tests/test_qml.cpp)
        target_link_libraries(test_qml PRIVATE configqml::configqml Qt6::Test)
        target_compile_definitions(test_qml PRIVATE
            QML_IMPORT_ROOT="${CMAKE_CURRENT_BINARY_DIR}"
            QML_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_facade.qml")
        add_test(NAME qml COMMAND test_qml)
        set_tests_properties(qml PROPERTIES ENVIRONMENT "QT_QPA_PLATFORM=offscreen")
        add_executable(test_gate_lifetime tests/test_gate_lifetime.cpp)
        target_link_libraries(test_gate_lifetime PRIVATE configqml::configqml Qt6::Test)
        add_test(NAME gate_lifetime COMMAND test_gate_lifetime)
        add_executable(test_qml_lifetime tests/test_qml_lifetime.cpp)
        target_link_libraries(test_qml_lifetime PRIVATE configqml::configqml Qt6::Test)
        target_compile_definitions(test_qml_lifetime PRIVATE
            QML_IMPORT_ROOT="${CMAKE_CURRENT_BINARY_DIR}"
            QML_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_facade.qml")
        add_test(NAME qml_lifetime COMMAND test_qml_lifetime)
        add_executable(test_import tests/test_import.cpp)
        target_link_libraries(test_import PRIVATE Qt6::Qml)
        add_dependencies(test_import configqmlplugin)
        target_compile_definitions(test_import PRIVATE
            QML_IMPORT_ROOT="${CMAKE_CURRENT_BINARY_DIR}"
            QML_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_facade.qml")
        add_test(NAME import_only COMMAND test_import)
        add_executable(test_import_files tests/test_import_files.cpp)
        target_link_libraries(test_import_files PRIVATE Qt6::Qml)
        add_dependencies(test_import_files configqmlplugin)
        target_compile_definitions(test_import_files PRIVATE
            QML_IMPORT_ROOT="${CMAKE_CURRENT_BINARY_DIR}"
            QML_FILE_FIXTURE="${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_json_files.qml")
        add_test(NAME import_files COMMAND test_import_files)
        add_test(NAME json_qml_lint COMMAND $<TARGET_FILE:Qt6::qmllint>
            -I ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_json_files.qml)
        add_test(NAME private_isolation COMMAND ${CMAKE_COMMAND}
            -DSOURCE_DIR=${CMAKE_CURRENT_SOURCE_DIR}
            -DBUILD_DIR=${CMAKE_CURRENT_BINARY_DIR}
            -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/private_isolation.cmake)
        add_test(NAME qml_lint COMMAND $<TARGET_FILE:Qt6::qmllint>
            -I ${CMAKE_CURRENT_BINARY_DIR} ${CMAKE_CURRENT_SOURCE_DIR}/tests/tst_facade.qml)
    endif()
    add_executable(test_core_link tests/test_core_link.cpp)
    target_link_libraries(test_core_link PRIVATE configqml::core)
    add_test(NAME core_link COMMAND ${CMAKE_COMMAND}
        -DPROGRAM=$<TARGET_FILE:test_core_link>
        -DLIBRARY=$<TARGET_FILE:configqml_core>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/core_link.cmake)
endif()
