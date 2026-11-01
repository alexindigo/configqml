# Keep test fixtures and installed examples on the same diagnostic contract.
# Qt 6.12 otherwise permits warnings without affecting qmllint's exit status.
set(_configqml_qmllint_args --missing-property error)
