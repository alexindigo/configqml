include("${SOURCE_DIR}/cmake/ConfigQmlLint.cmake")
set(root "${BUILD_DIR}/qmllint-policy")
file(MAKE_DIRECTORY "${root}")
file(WRITE "${root}/valid.qml"
    "import QtQuick\nimport ConfigQml 1.0\nItem { id: root
    property ConfigStore store: ConfigStore {}
    function listIndex(): int { return root.store.listModel([\"rows\"]).indexOfRow(\"row\") }
    function mapIndex(): int { return root.store.keyedModel([\"map\"]).indexOfRow(\"key\") }
}\n")
file(WRITE "${root}/invalid.qml"
    "import QtQml\nQtObject { id: root; readonly property string observed: root.configqmlMissingProperty }\n")
execute_process(COMMAND "${QMLLINT}" ${_configqml_qmllint_args} -I "${BUILD_DIR}" "${root}/valid.qml"
    RESULT_VARIABLE valid OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
if(NOT valid EQUAL 0 OR NOT "${out}${err}" STREQUAL "")
    message(FATAL_ERROR "Valid QML must lint without diagnostics: ${valid}\n${out}${err}")
endif()
execute_process(COMMAND "${QMLLINT}" ${_configqml_qmllint_args} -I "${BUILD_DIR}" "${root}/invalid.qml"
    RESULT_VARIABLE invalid OUTPUT_VARIABLE out ERROR_VARIABLE err TIMEOUT 30)
if(invalid EQUAL 0 OR NOT "${out}${err}" MATCHES "\\[missing-property\\]")
    message(FATAL_ERROR "Missing properties must fail qmllint: ${invalid}\n${out}${err}")
endif()
