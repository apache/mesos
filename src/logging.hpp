#ifndef __LOGGING_HPP__
#define __LOGGING_HPP__

#include "configurator.hpp"

namespace nexus { namespace internal {

using std::string;

/**
 * Utility functions for configuring and initializing Mesos logging.
 */
class Logging {
public:
  static void registerOptions(Configurator* conf);
  static void init(const char* programName, const Params& conf);
  static string getLogDir(const Params& conf);
  static bool isQuiet(const Params& conf);
};

}} /* namespace nexus::internal */

#endif
