cmake_minimum_required(VERSION 3.21)
project(configqml VERSION 0.1.0 DESCRIPTION "Layered configuration for Qt" LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_AUTOMOC ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(Qt6 6.12 REQUIRED COMPONENTS Core)
include(GNUInstallDirs)
include(CMakePackageConfigHelpers)
include(cmake/ConfigQmlInstallPath.cmake)
set(_configqml_include_dir "${CMAKE_INSTALL_INCLUDEDIR}/configqml")
set(_configqml_cmake_dir "${CMAKE_INSTALL_LIBDIR}/cmake/configqml")
set(_configqml_data_dir "${CMAKE_INSTALL_DATADIR}/configqml")
set(_configqml_license_dir "${CMAKE_INSTALL_DATADIR}/licenses/configqml")

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
set_target_properties(configqml_core PROPERTIES EXPORT_NAME core
    VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR} INSTALL_RPATH "$ORIGIN")
target_compile_definitions(configqml_core PRIVATE CONFIGQML_VERSION="${PROJECT_VERSION}")
target_link_libraries(configqml_core PUBLIC Qt6::Core)
target_include_directories(configqml_core PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
    $<INSTALL_INTERFACE:${_configqml_include_dir}>)
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
    target_include_directories(configqml PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}>
        $<INSTALL_INTERFACE:${_configqml_include_dir}>)
    target_compile_options(configqml PRIVATE -Wall -Wextra -Wpedantic)
    set_target_properties(configqml PROPERTIES VERSION ${PROJECT_VERSION} SOVERSION ${PROJECT_VERSION_MAJOR}
        INSTALL_RPATH "$ORIGIN")

    # Retain Qt's QML-to-library relationship even with a custom GNU libdir.
    get_target_property(_qtpaths Qt6::qtpaths IMPORTED_LOCATION)
    execute_process(COMMAND "${_qtpaths}" --query QT_INSTALL_QML
        OUTPUT_VARIABLE _qt_qml OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    execute_process(COMMAND "${_qtpaths}" --query QT_INSTALL_LIBS
        OUTPUT_VARIABLE _qt_libs OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
    file(RELATIVE_PATH _qml_from_libs "${_qt_libs}" "${_qt_qml}")
    set(_qml_default "${CMAKE_INSTALL_LIBDIR}/${_qml_from_libs}")
    cmake_path(NORMAL_PATH _qml_default)
    # PATH would absolutize an ordinary untyped -D override before we see it.
    set(CONFIGQML_QML_INSTALL_DIR "${_qml_default}" CACHE STRING "QML import directory (prefix-relative or absolute)")
    set(_configqml_module_dir "${CONFIGQML_QML_INSTALL_DIR}/ConfigQml")
    configqml_install_path(_module_full "${_configqml_module_dir}" "${CMAKE_INSTALL_PREFIX}")
    configqml_install_path(_lib_full "${CMAKE_INSTALL_LIBDIR}" "${CMAKE_INSTALL_PREFIX}")
    file(RELATIVE_PATH _plugin_to_lib "${_module_full}" "${_lib_full}")
    set_target_properties(configqmlplugin PROPERTIES INSTALL_RPATH "$ORIGIN/${_plugin_to_lib}")
    install(TARGETS configqml EXPORT configqmlQmlTargets
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
    install(TARGETS configqmlplugin LIBRARY DESTINATION "${_configqml_module_dir}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/ConfigQml/qmldir"
        "${CMAKE_CURRENT_BINARY_DIR}/ConfigQml/configqml.qmltypes"
        DESTINATION "${_configqml_module_dir}")
    install(FILES configqmldeclaration.h configqmlstore.h configqmlvalue.h configqmlmodel.h configfacade.h
        DESTINATION "${_configqml_include_dir}")
    install(FILES "${CMAKE_CURRENT_BINARY_DIR}/configqml-qt-build.txt"
        DESTINATION "${_configqml_data_dir}")
    install(EXPORT configqmlQmlTargets FILE configqmlQmlTargets.cmake NAMESPACE configqml::
        DESTINATION "${_configqml_cmake_dir}")
endif()

install(TARGETS configqml_core EXPORT configqmlCoreTargets
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR})
install(FILES configvalue.h configschema.h confignumber.h configlayers.h configscope.h
    configenvironment.h configbackend.h configcodec.h configmemorybackend.h
    configjsoncodec.h configjsonbackend.h configresult.h configstore.h configlistmodel.h
    configdraft.h configstorehandle.h DESTINATION "${_configqml_include_dir}")
install(FILES COPYING COPYING.LESSER DESTINATION "${_configqml_license_dir}")
install(EXPORT configqmlCoreTargets FILE configqmlCoreTargets.cmake NAMESPACE configqml::
    DESTINATION "${_configqml_cmake_dir}")
configure_package_config_file(configqmlConfig.cmake.in
    "${CMAKE_CURRENT_BINARY_DIR}/configqmlConfig.cmake"
    INSTALL_DESTINATION "${_configqml_cmake_dir}")
write_basic_package_version_file("${CMAKE_CURRENT_BINARY_DIR}/configqmlConfigVersion.cmake"
    VERSION ${PROJECT_VERSION} COMPATIBILITY SameMinorVersion)
install(FILES "${CMAKE_CURRENT_BINARY_DIR}/configqmlConfig.cmake"
    "${CMAKE_CURRENT_BINARY_DIR}/configqmlConfigVersion.cmake"
    DESTINATION "${_configqml_cmake_dir}")

option(BUILD_TESTING "Build the configqml tests" OFF)
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
        get_target_property(_qmllint Qt6::qmllint IMPORTED_LOCATION)
        get_filename_component(_qt_tools_dir "${_qmllint}" DIRECTORY)
        find_program(_qml NAMES qml qml6 HINTS "${_qt_tools_dir}" REQUIRED NO_DEFAULT_PATH)
        execute_process(COMMAND "${_qtpaths}" --query QT_INSTALL_PLUGINS
            OUTPUT_VARIABLE _qt_plugins OUTPUT_STRIP_TRAILING_WHITESPACE COMMAND_ERROR_IS_FATAL ANY)
        # Host tools can come from another prefix; the loader needs the Qt we link.
        get_target_property(_qt_core_library Qt6::Core IMPORTED_LOCATION)
        if(NOT _qt_core_library)
            get_target_property(_qt_core_configs Qt6::Core IMPORTED_CONFIGURATIONS)
            list(GET _qt_core_configs 0 _qt_core_config)
            get_target_property(_qt_core_library Qt6::Core "IMPORTED_LOCATION_${_qt_core_config}")
        endif()
        get_filename_component(_qt_runtime_libs "${_qt_core_library}" DIRECTORY)
        configure_file(tests/installed_prefix.cmake.in installed_prefix.cmake @ONLY)
        add_test(NAME installed_prefix COMMAND ${CMAKE_COMMAND}
            -P ${CMAKE_CURRENT_BINARY_DIR}/installed_prefix.cmake)
        set_tests_properties(installed_prefix PROPERTIES TIMEOUT 120 LABELS distribution)
        configure_file(tests/install_layout.cmake.in install_layout.cmake @ONLY)
        # This is also the CI layout matrix; each child builds only the library
        # and runs installed_prefix, never recursively running this matrix.
        foreach(layout gnu_dirs relative_qml absolute_qml absolute_dirs)
            add_test(NAME install_layout_${layout} COMMAND ${CMAKE_COMMAND}
                -DLAYOUT=${layout} -P ${CMAKE_CURRENT_BINARY_DIR}/install_layout.cmake)
            set_tests_properties(install_layout_${layout} PROPERTIES TIMEOUT 600 LABELS distribution)
        endforeach()
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
    target_compile_definitions(test_core_link PRIVATE CONFIGQML_EXPECTED_VERSION="${PROJECT_VERSION}")
    add_test(NAME core_link COMMAND ${CMAKE_COMMAND}
        -DPROGRAM=$<TARGET_FILE:test_core_link>
        -DLIBRARY=$<TARGET_FILE:configqml_core>
        -P ${CMAKE_CURRENT_SOURCE_DIR}/tests/core_link.cmake)
endif()
