#include <gtest/gtest.h>

#include "config/config.hpp"

#include "tests/external_test.hpp"


// Run each of the sample frameworks in local mode
TEST_EXTERNAL(SampleFrameworks, CppFramework)
#if MESOS_HAS_JAVA
  TEST_EXTERNAL(SampleFrameworks, JavaFramework)
  TEST_EXTERNAL(SampleFrameworks, JavaExceptionFramework)
#endif 
#if MESOS_HAS_PYTHON
  TEST_EXTERNAL(SampleFrameworks, PythonFramework)
#endif

// TODO(*): Add the tests below as C++ tests.
// // Some tests for command-line and environment configuration
// TEST_EXTERNAL(SampleFrameworks, CFrameworkCmdlineParsing)
// TEST_EXTERNAL(SampleFrameworks, CFrameworkInvalidCmdline)
// TEST_EXTERNAL(SampleFrameworks, CFrameworkInvalidEnv)
