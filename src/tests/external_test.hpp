#ifndef __EXTERNAL_TEST_HPP__
#define __EXTERNAL_TEST_HPP__

#include <gtest/gtest.h>

/**
 * Run an external test with the given name. The test is expected to be
 * located in src/tests/external/<testCase>/<testName>.sh.
 * We execute this script in directory test_output/<testCase>/<testName>,
 * piping its output to files called stdout and stderr, and the test
 * passes if the script returns 0.
 */
#define TEST_EXTERNAL(testCase, testName) \
  TEST(testCase, testName) { \
    mesos::internal::test::runExternalTest(#testCase, #testName); \
  }

namespace mesos { namespace internal { namespace test {

/**
 * Function called by TEST_EXTERNAL to execute external tests. See
 * explanation of parameters at definition of TEST_EXTERNAL.
 */
void runExternalTest(const char* testCase, const char* testName);

}}} /* namespace mesos::internal::test */

#endif /* __EXTERNAL_TEST_HPP__ */
