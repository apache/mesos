#ifndef __TEST_UTILS_HPP__
#define __TEST_UTILS_HPP__

#include <string>

// The location where Mesos is installed, used by tests to locate various
// frameworks and binaries. For now it points to the src directory, until
// we clean up our directory structure a little. Initialized in main.cpp.
extern std::string MESOS_HOME;

#endif /* __TEST_UTILS_HPP__ */
