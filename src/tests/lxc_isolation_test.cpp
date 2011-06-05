#include <gtest/gtest.h>

#include "tests/external_test.hpp"


// Run a number of tests for the LXC isolation module.
// These tests are disabled by default since they require alltests to be run
// with sudo for Linux Container commands to be usable (and of course, they
// also require a Linux version with LXC support).
// You can enable them using ./alltests --gtest_also_run_disabled_tests.
TEST_EXTERNAL(LxcIsolation, DISABLED_TwoSeparateTasks)
TEST_EXTERNAL(LxcIsolation, DISABLED_ScaleUpAndDown)
TEST_EXTERNAL(LxcIsolation, DISABLED_HoldMoreMemThanRequested)
