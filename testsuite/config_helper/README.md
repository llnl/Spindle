# spindle-config-helper Test Suite

Simple integration tests for the `spindle-config-helper` tool.

## Running Tests

```bash
# From the build directory
cd testsuite/config_helper
./run_tests.sh /path/to/spindle-config-helper

# Or let it find the default location
./run_tests.sh
```

## Test Coverage

1. **test_01_smoke** - Basic execution (does it run without crashing?)
2. **test_02_help** - Help flag works
3. **test_03_version** - Version flag works
4. **test_04_local_fs** - Detects local filesystems correctly (/dev/shm, /tmp)
5. **test_05_undefined_var** - Warns about undefined variables
6. **test_06_empty_var** - Detects empty variables creating // in paths
7. **test_07_multiple_paths** - Handles colon-separated path lists
8. **test_08_kernel_fs** - Recognizes kernel filesystems (/proc, /sys, /dev)
9. **test_09_permissions** - Checks parent directory permissions
10. **test_10_relative_path** - Resolves relative paths correctly
11. **test_11_pythonprefix_valid** - Validates Python installation (skips if /usr/tce missing)
12. **test_12_pythonprefix_invalid** - Warns when path lacks bin/python
13. **test_13_pythonprefix_undefined_var** - Detects undefined variables in pythonprefix
14. **test_14_security_mode_detection** - Detects and reports security configuration
15. **test_15_security_none_warning** - Warns when security=none (insecure)
16. **test_16_munge_detection** - Reports munge availability
17. **test_17_launchmon_warning** - Warns when security=launchmon (deprecated)

## Sample Mount Files

The `sample_mounts/` directory contains example `/proc/mounts` files for testing:

- **simple.mounts** - Basic system with NFS and local storage
- **hpc.mounts** - HPC system with Lustre, GPFS, and local storage
- **container.mounts** - Container environment with overlay filesystem

These can be used for future mocking/testing if we implement a `--test-mounts` flag.

## Exit Codes

- `0` - All tests passed
- `1` - One or more tests failed

## Adding New Tests

1. Add a `test_XX_description()` function to `run_tests.sh`
2. Add `run_test "XX_description" test_XX_description` to the test list
3. Test should return 0 on success, non-zero on failure
4. Use `grep -q` for output validation to keep tests simple

## Design Philosophy

These are **integration tests**, not unit tests:
- Run the actual tool binary
- Use real filesystem operations where possible
- Keep tests simple and maintainable
- Focus on catching regressions, not exhaustive coverage
- Avoid heavy mocking - trust the implementation for complex logic
