# GNUInstallDirs destinations are either prefix-relative or explicitly absolute.
# Use the same resolver for plugin RPATH calculation and installed-consumer tests.
function(configqml_install_path output destination prefix)
    get_filename_component(path "${destination}" ABSOLUTE BASE_DIR "${prefix}")
    set(${output} "${path}" PARENT_SCOPE)
endfunction()
