#include <gtest/gtest.h>

#include <ctype.h>
#include <stdlib.h>

#include <string>

#include <boost/lexical_cast.hpp>

#include "external_test.hpp"
#include "fatal.hpp"
#include "testing_utils.hpp"

using std::string;

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

}


/**
 * Run an external test with the given name. The test is expected to be
 * located in src/tests/external/<testCase>/<testName>.sh.
 * We execute this script in directory test_output/<testCase>/<testName>,
 * piping its output to files called stdout and stderr, and the test
 * passes if the script returns 0.
 */
void nexus::test::runExternalTest(const char* testCase, const char* testName)
{
  // Remove DISABLED_ prefix from test name if this is a disabled test
  if (strncmp(testName, "DISABLED_", strlen("DISABLED_")) == 0)
    testName += strlen("DISABLED_");
  // Check that the test name is valid
  if (!isValidTestName(testCase) || !isValidTestName(testName)) {
    FAIL() << "Invalid test name for external test (name should " 
           << "only contain alphanumeric and underscore characters)";
  }
  // Figure out the absolute path to the test script
  string script = MESOS_HOME + "/tests/external/" + testCase
                             + "/" + testName + ".sh";
  // Make the work directory for this test
  string workDir = MESOS_HOME + "/test_output/" + testCase + "/" + testName;
  string command = "rm -fr '" + workDir + "'";
  ASSERT_EQ(0, system(command.c_str())) << "Command failed: " << command;
  command = "mkdir -p '" + workDir + "'";
  ASSERT_EQ(0, system(command.c_str())) << "Command failed: " << command;
  // Fork a process to change directory and run the test
  pid_t pid;
  if ((pid = fork()) == -1) {
    FAIL() << "Failed to fork to launch external test";
  }
  if (pid) {
    // In parent process
    int exitCode;
    wait(&exitCode);
    ASSERT_EQ(0, exitCode) << "External test " << testName << " failed";
  } else {
    // In child process. Go into to the work directory, redirect IO to files,
    // set MESOS_HOME environment variable, and exec the test script.
    chdir(workDir.c_str());
    if (freopen("stdout", "w", stdout) == NULL)
      fatalerror("freopen failed");
    if (freopen("stderr", "w", stderr) == NULL)
      fatalerror("freopen failed");
    setenv("MESOS_HOME", MESOS_HOME.c_str(), 1);
    execl(script.c_str(), script.c_str(), (char*) NULL);
    // If we get here, execl failed; report the error
    fatalerror("Could not execute %s", script.c_str());
  }
}
