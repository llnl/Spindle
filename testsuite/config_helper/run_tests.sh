#!/bin/bash
# Test suite for spindle-config-helper
# Usage: ./run_tests.sh [path-to-spindle-config-helper]

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HELPER="${1:-../../build/config_helper-serial/src/config_helper/spindle-config-helper}"

if [[ ! -x "$HELPER" ]]; then
    echo "Error: spindle-config-helper not found or not executable: $HELPER"
    echo "Usage: $0 [path-to-spindle-config-helper]"
    exit 1
fi

TESTS_PASSED=0
TESTS_FAILED=0
FAILED_TESTS=""

# Test helper function
run_test() {
    local test_name="$1"
    local test_func="$2"

    echo -n "Running test: $test_name ... "

    if $test_func 2>&1 >/dev/null; then
        echo "PASSED"
        ((TESTS_PASSED++))
        return 0
    else
        echo "FAILED"
        ((TESTS_FAILED++))
        FAILED_TESTS="$FAILED_TESTS\n  - $test_name"
        return 1
    fi
}

# Test 1: Basic smoke test - does it run?
test_01_smoke() {
    $HELPER --commpaths=/tmp --cachepaths=/tmp >/dev/null 2>&1
}

# Test 2: Help flag works
test_02_help() {
    $HELPER --help 2>&1 | grep -q "Usage:"
}

# Test 3: Version flag works
test_03_version() {
    $HELPER --version 2>&1 | grep -q "spindle-config-helper version"
}

# Test 4: Local filesystem detection
test_04_local_fs() {
    output=$($HELPER --commpaths=/dev/shm --cachepaths=/tmp 2>&1)
    echo "$output" | grep -q "OK: /dev/shm" && \
    echo "$output" | grep -q "OK: /tmp"
}

# Test 5: Undefined variable detection
test_05_undefined_var() {
    unset SPINDLE_TEST_VAR
    output=$($HELPER --commpaths='$SPINDLE_TEST_VAR' 2>&1)
    echo "$output" | grep -q "Variable is not set"
}

# Test 6: Empty variable detection
test_06_empty_var() {
    export SPINDLE_TEST_VAR=""
    output=$($HELPER --commpaths='/tmp/$SPINDLE_TEST_VAR/dir' 2>&1)
    echo "$output" | grep -q "empty segment"
}

# Test 7: Multiple paths with colon separator
test_07_multiple_paths() {
    output=$($HELPER --commpaths=/tmp:/dev/shm 2>&1)
    echo "$output" | grep -q "OK: /tmp" && \
    echo "$output" | grep -q "OK: /dev/shm"
}

# Test 8: Kernel filesystem handling
test_08_kernel_fs() {
    output=$($HELPER --local-prefix=/proc:/sys:/dev 2>&1)
    echo "$output" | grep -q "OK: /proc" && \
    echo "$output" | grep -q "OK: /sys" && \
    echo "$output" | grep -q "OK: /dev"
}

# Test 9: Permission check for non-existent paths
test_09_permissions() {
    # /tmp should be writable - warns about non-existence but shows it can be created
    output=$($HELPER --commpaths=/tmp/spindle_test_nonexistent_dir 2>&1)
    echo "$output" | grep -q "/tmp/spindle_test_nonexistent_dir" && \
    echo "$output" | grep -q "will be created by Spindle"
}

# Test 10: Relative path resolution
test_10_relative_path() {
    cd /tmp
    output=$($HELPER --commpaths=./relative_test 2>&1)
    echo "$output" | grep -q "/tmp/relative_test"
    cd "$SCRIPT_DIR"
}

# Test 11: pythonprefix with valid installation
test_11_pythonprefix_valid() {
    # Use /usr/tce if it exists, otherwise skip
    if [[ ! -x /usr/tce/bin/python3 ]]; then
        return 0  # Skip on systems without /usr/tce
    fi
    output=$($HELPER --python-prefix=/usr/tce 2>&1)
    echo "$output" | grep -q "OK: /usr/tce"
}

# Test 12: pythonprefix without Python binaries
test_12_pythonprefix_invalid() {
    output=$($HELPER --python-prefix=/tmp 2>&1)
    echo "$output" | grep -q "No bin/python or bin/python3 found"
}

# Test 13: pythonprefix with undefined variable
test_13_pythonprefix_undefined_var() {
    unset PYTHON_HOME
    output=$($HELPER --python-prefix='$PYTHON_HOME' 2>&1)
    echo "$output" | grep -q "Variable is not set"
}

# Test 14: Security mode detection
test_14_security_mode_detection() {
    output=$($HELPER --commpaths=/tmp 2>&1)
    echo "$output" | grep -q "# Security configuration:" && \
    echo "$output" | grep -q "Current mode:"
}

# Test 15: Security none warning
test_15_security_none_warning() {
    output=$($HELPER --commpaths=/tmp --security=none 2>&1)
    echo "$output" | grep -q "WARNING: No authentication enabled"
}

# Test 16: Munge detection
test_16_munge_detection() {
    output=$($HELPER --commpaths=/tmp 2>&1)
    # Check that munge availability is reported
    echo "$output" | grep -q "Munge availability:"
}

# Test 17: LaunchMON warning
test_17_launchmon_warning() {
    output=$($HELPER --commpaths=/tmp --security=launchmon 2>&1)
    echo "$output" | grep -q "WARNING: LaunchMON security mode"
}

# Run all tests
echo "========================================="
echo "spindle-config-helper Test Suite"
echo "========================================="
echo "Testing: $HELPER"
echo ""

run_test "01_smoke_test" test_01_smoke
run_test "02_help_flag" test_02_help
run_test "03_version_flag" test_03_version
run_test "04_local_filesystem_detection" test_04_local_fs
run_test "05_undefined_variable" test_05_undefined_var
run_test "06_empty_variable" test_06_empty_var
run_test "07_multiple_paths" test_07_multiple_paths
run_test "08_kernel_filesystems" test_08_kernel_fs
run_test "09_permission_check" test_09_permissions
run_test "10_relative_path_resolution" test_10_relative_path
run_test "11_pythonprefix_valid" test_11_pythonprefix_valid
run_test "12_pythonprefix_invalid" test_12_pythonprefix_invalid
run_test "13_pythonprefix_undefined_var" test_13_pythonprefix_undefined_var
run_test "14_security_mode_detection" test_14_security_mode_detection
run_test "15_security_none_warning" test_15_security_none_warning
run_test "16_munge_detection" test_16_munge_detection
run_test "17_launchmon_warning" test_17_launchmon_warning

echo ""
echo "========================================="
echo "Results: $TESTS_PASSED passed, $TESTS_FAILED failed"
echo "========================================="

if [[ $TESTS_FAILED -gt 0 ]]; then
    echo -e "Failed tests:$FAILED_TESTS"
    exit 1
fi

exit 0
