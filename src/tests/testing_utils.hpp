#ifndef __TESTING_UTILS_HPP__
#define __TESTING_UTILS_HPP__

#include <string>

#include <gtest/gtest.h>

namespace nexus { namespace internal { namespace test {

/**
 * The location where Mesos is installed, used by tests to locate various
 * frameworks and binaries. For now it points to the src directory, until
 * we clean up our directory structure a little. Initialized in main.cpp.
 */
extern std::string mesosHome;


/**
 * Create and clean up the work directory for a given test, and cd into it,
 * given the test's test case name and test name.
 * Test directories are placed in <mesosHome>/test_output/<testCase>/<testName>.
 */
void enterTestDirectory(const char* testCase, const char* testName);


/**
 * Macro for running a test in a work directory (using enterTestDirectory).
 * Used in a similar way to gtest's TEST macro (by adding a body in braces).
 */
#define TEST_WITH_WORKDIR(testCase, testName) \
  void runTestBody_##testCase##_##testName(); \
  TEST(testCase, testName) { \
    enterTestDirectory(#testCase, #testName); \
    runTestBody_##testCase##_##testName(); \
  } \
  void runTestBody_##testCase##_##testName() /* User code block follows */


}}} // namespace nexus::internal::test


#endif /* __TESTING_UTILS_HPP__ */
