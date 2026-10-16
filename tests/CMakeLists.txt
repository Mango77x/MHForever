# Repository-level tests. None of them need game data (see CONTRIBUTING.md).
find_package(Python3 COMPONENTS Interpreter)
if(Python3_FOUND)
    add_test(NAME repo_content_guard_selftest
             COMMAND ${Python3_EXECUTABLE} ${PROJECT_SOURCE_DIR}/scripts/check_repo_content.py --self-test)
endif()

add_test(NAME launcher_reports_version COMMAND mhf --version)
set_tests_properties(launcher_reports_version PROPERTIES PASS_REGULAR_EXPRESSION "^${PROJECT_VERSION}")
