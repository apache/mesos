#include "logging.hpp"

#include <glog/logging.h>

using std::string;
using namespace mesos::internal;

namespace {
const string DEFAULT_LOG_DIR = "/tmp";
}


void Logging::registerOptions(Configurator* conf)
{
  conf->addOption<bool>("quiet", 'q', "Disable logging to stderr", false);
  conf->addOption<string>("log_dir", "Where to place logs", DEFAULT_LOG_DIR);
}


void Logging::init(const char* programName, const Params& conf)
{
  // Set glog's parameters through Google Flags variables
  FLAGS_log_dir = conf.get("log_dir", DEFAULT_LOG_DIR);
  FLAGS_logbufsecs = 1;
  google::InitGoogleLogging(programName);

  if (!isQuiet(conf))
    google::SetStderrLogging(google::INFO);
}


string Logging::getLogDir(const Params& conf)
{
  return conf.get("log_dir", DEFAULT_LOG_DIR);
}


bool Logging::isQuiet(const Params& conf)
{
  return conf.get<bool>("quiet", false);
}
