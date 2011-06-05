#include <libgen.h>
#include <stdlib.h>
#include <unistd.h>

#include <algorithm>
#include <iostream>

#include <boost/foreach.hpp>

#include "configuration.hpp"
#include "params.hpp"

extern char** environ;   // libc's environment variable list; for some reason,
                         // this is not in headers on all platforms

using namespace nexus::internal;


const char* Configuration::DEFAULT_CONFIG_DIR = "conf";
const char* Configuration::CONFIG_FILE_NAME = "mesos.conf";
const char* Configuration::ENV_VAR_PREFIX = "MESOS_";


Configuration::Configuration() 
{
  loadEnv();
  loadConfigFileIfGiven();
}


Configuration::Configuration(int argc,
                             char** argv,
                             bool inferMesosHomeFromArg0) 
{
  loadEnv();
  loadCommandLine(argc, argv, inferMesosHomeFromArg0);
  loadConfigFileIfGiven();
}


Configuration::Configuration(const map<string, string>& _params) 
{
  loadEnv();
  params.loadMap(_params);
  loadConfigFileIfGiven();
}


void Configuration::loadConfigFileIfGiven() {
  string confDir = "";
  if (params.contains("conf"))
    confDir = params["conf"];
  else if (params.contains("home")) // find conf dir relative to MESOS_HOME
    confDir = params["home"] + "/" + DEFAULT_CONFIG_DIR;
  if (confDir != "")
    loadConfigFile(confDir + "/" + CONFIG_FILE_NAME);
}


void Configuration::loadEnv()
{
  int i = 0;
  while (environ[i] != NULL) {
    string line = environ[i];
    if (line.find(ENV_VAR_PREFIX) == 0) {
      string key, val;
      size_t eq = line.find_first_of("=");
      if (eq == string::npos) 
        continue; // ignore malformed lines (they shouldn't occur in environ!)
      key = line.substr(strlen(ENV_VAR_PREFIX), eq - strlen(ENV_VAR_PREFIX));
      std::transform(key.begin(), key.end(), key.begin(), ::tolower);
      val = line.substr(eq + 1);
      params[key] = val;
    }
    i++;
  }
}


void Configuration::loadCommandLine(int argc,
                                    char** argv,
                                    bool inferMesosHomeFromArg0)
{
  // Set home based on argument 0 if asked to do so
  if (inferMesosHomeFromArg0) {
    char buf[4096];
    if (realpath(dirname(argv[0]), buf) == 0) {
      throw ConfigurationException(
          "Could not get directory containing argv[0] -- realpath failed");
    }
    params["home"] = buf;
  }

  // Convert args 1 and above to STL strings
  vector<string> args;
  for (int i=1; i < argc; i++) {
    args.push_back(string(argv[i]));
  }

  for (int i = 0; i < args.size(); i++) {
    string key, val;
    bool set = false;
    if (args[i].find("--", 0) == 0) {
      // handle --blah=25 and --blah
      size_t eq = args[i].find_first_of("=");
      if (eq == string::npos) { // handle no value case
        key = args[i].substr(2);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        val = "1";
        set = true;
      } else { // handle value case
        key = args[i].substr(2, eq-2); 
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        val = args[i].substr(eq+1);
        set = true;
      } 
    } else if (args[i].find_first_of("-", 0) == 0) {
      // handle -blah 25 and -blah
      if ((i+1 >= args.size()) ||
          (i+1 < args.size() && args[i+1].find_first_of("-", 0) == 0)) {
        key = args[i].substr(1);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        val = "1";
        set = true;
      } else if (i+1 < args.size() && args[i+1].find_first_of("-", 0) != 0) {
        key = args[i].substr(1);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        val = args[i+1];
        set = true;
        i++;  // we've consumed next parameter as "value"-parameter
      }
    }
    if (set) {
      params[key] = val;
    }
  }
}


void Configuration::loadConfigFile(const string& fname) {
  ifstream cfg(fname.c_str(), std::ios::in);
  if (!cfg.is_open()) {
    string message = "Couldn't read Mesos config file: " + fname;
    throw ConfigurationException(message.c_str());
  }

  Params fileParams;
  string buf, line;

  while (!cfg.eof()) {
    getline(cfg, line);
    size_t beg = line.find_first_of("#"); // strip comments
    beg = line.find_last_not_of("#\t \n\r", beg) + 1; // strip trailing ws
    buf += line.substr(0, beg) + "\n";
  }
  cfg.close();
  fileParams.loadString(buf); // Parse key=value pairs using Params's code

  foreachpair (const string& key, const string& value, fileParams.getMap()) {
    if (!params.contains(key)) {
      params[key] = value;
    }
  }
}


Params& Configuration::getParams() 
{
  return params;
}
