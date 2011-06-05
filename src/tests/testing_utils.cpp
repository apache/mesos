#include <ctype.h>
#include <stdlib.h>
#include <unistd.h>

#include <gtest/gtest.h>

#include "testing_utils.hpp"

using std::string;

using namespace nexus::internal;


string test::mesosHome;


namespace {

// Check that a test name contains only letters, numbers and underscores, to
// prevent messing with directories outside test_output in runExternalTest.
bool isValidTestName(const char* name) {
  for (const char* p = name; *p != 0; p++) {
    if (!isalnum(*p) && *p != '_') {
      return false;
    }
  }
  return true;
}

} // namespace


/**
 * Create and clean up the work directory for a given test, and cd into it,
 * given the test's test case name and test name.
 * Test directories are placed in <mesosHome>/test_output/<testCase>/<testName>.
 */
void test::enterTestDirectory(const char* testCase, const char* testName)
{
  // Check that the test name is valid
  if (!isValidTestName(testCase) || !isValidTestName(testName)) {
    FAIL() << "Invalid test name for external test (name should " 
           << "only contain alphanumeric and underscore characters)";
  }
  // Make the work directory for this test
  string workDir = mesosHome + "/test_output/" + testCase + "/" + testName;
  string command = "rm -fr '" + workDir + "'";
  ASSERT_EQ(0, system(command.c_str())) << "Command failed: " << command;
  command = "mkdir -p '" + workDir + "'";
  ASSERT_EQ(0, system(command.c_str())) << "Command failed: " << command;
  // Change dir into it
  chdir(workDir.c_str());
}
