#ifndef __LOGGING_HPP__
#define __LOGGING_HPP__

#include <string>

#include "configurator/configurator.hpp"


namespace mesos { namespace internal {

/**
 * Utility functions for configuring and initializing Mesos logging.
 */
class Logging {
public:
  static void registerOptions(Configurator* conf);
  static void init(const char* programName, const Configuration& conf);
  static std::string getLogDir(const Configuration& conf);
  static bool isQuiet(const Configuration& conf);
};

}} /* namespace mesos::internal */

#endif
