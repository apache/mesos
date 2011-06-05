#include <stdlib.h>

#include <gtest/gtest.h>

#include <string>

#include <boost/lexical_cast.hpp>

#include "external_test.hpp"
#include "testing_utils.hpp"

#include "common/fatal.hpp"

using std::string;
using namespace mesos::internal::test;


/**
 * Run an external test with the given name. The test is expected to be
 * located in src/tests/external/<testCase>/<testName>.sh.
 * We execute this script in directory test_output/<testCase>/<testName>,
 * piping its output to files called stdout and stderr, and the test
 * passes if the script returns 0.
 */
void mesos::internal::test::runExternalTest(const char* testCase,
                                            const char* testName)
{
  // Remove DISABLED_ prefix from test name if this is a disabled test
  if (strncmp(testName, "DISABLED_", strlen("DISABLED_")) == 0)
    testName += strlen("DISABLED_");
  // Create and go into the test's work directory
  enterTestDirectory(testCase, testName);
  // Figure out the absolute path to the test script
  string script = mesosHome + "/src/tests/external/" + testCase
                             + "/" + testName + ".sh";
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
    // In child process. Redirect standard output and error to files,
    // set MESOS_HOME environment variable, and exec the test script.
    if (freopen("stdout", "w", stdout) == NULL)
      fatalerror("freopen failed");
    if (freopen("stderr", "w", stderr) == NULL)
      fatalerror("freopen failed");
    setenv("MESOS_HOME", mesosHome.c_str(), 1);
    execl(script.c_str(), script.c_str(), (char*) NULL);
    // If we get here, execl failed; report the error
    fatalerror("Could not execute %s", script.c_str());
  }
}
