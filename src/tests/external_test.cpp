#include <gtest/gtest.h>

#include <stdlib.h>

#include <string>

#include <boost/lexical_cast.hpp>

#include "external_test.hpp"
#include "fatal.hpp"
#include "testing_utils.hpp"

using std::string;
using namespace nexus::internal::test;


/**
 * Run an external test with the given name. The test is expected to be
 * located in src/tests/external/<testCase>/<testName>.sh.
 * We execute this script in directory test_output/<testCase>/<testName>,
 * piping its output to files called stdout and stderr, and the test
 * passes if the script returns 0.
 */
void nexus::internal::test::runExternalTest(const char* testCase,
                                            const char* testName)
{
  // Create and go into the test's work directory
  enterTestDirectory(testCase, testName);
  // Figure out the absolute path to the test script
  string script = mesosHome + "/tests/external/" + testCase
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
