#ifndef __TESTING_UTILS_HPP__
#define __TESTING_UTILS_HPP__

#include <string>

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


}}} // namespace nexus::internal::test

#endif /* __TESTING_UTILS_HPP__ */
