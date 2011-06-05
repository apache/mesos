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


void Configuration::validate()
{
  foreachpair(const string& key, const Option& opt, options) {
    if (params.contains(key) && opt.validator && !opt.validator->isValid(params[key])) {
      throw BadOptionValueException(params[key].c_str());
    }
  }
}


void Configuration::load(int argc, char** argv, bool inferMesosHomeFromArg0)
{
  loadEnv();
  loadCommandLine(argc, argv, inferMesosHomeFromArg0);
  loadConfigFileIfGiven();
  validate();
}


void Configuration::load()
{
  loadEnv();
  loadConfigFileIfGiven();
  validate();
}


void Configuration::load(const map<string, string>& _params) 
{
  loadEnv();
  params.loadMap(_params);
  loadConfigFileIfGiven();
  validate();
}


void Configuration::loadConfigFileIfGiven(bool overwrite) {
  string confDir = "";
  if (params.contains("conf"))
    confDir = params["conf"];
  else if (params.contains("home")) // find conf dir relative to MESOS_HOME
    confDir = params["home"] + "/" + DEFAULT_CONFIG_DIR;
  if (confDir != "")
    loadConfigFile(confDir + "/" + CONFIG_FILE_NAME, overwrite);
}


void Configuration::loadEnv(bool overwrite)
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
      if (overwrite || !params.contains(key)) {
        params[key] = val;
      }
    }
    i++;
  }
}


void Configuration::loadCommandLine(int argc,
                                    char** argv,
                                    bool inferMesosHomeFromArg0,
                                    bool overwrite)
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
      if (eq == string::npos) { // handle the no value case
        key = args[i].substr(2);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        val = "1";
        set = true;
      } else { // handle the value case
        key = args[i].substr(2, eq-2); 
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        val = args[i].substr(eq+1);
        set = true;
      } 
    } else if (args[i].find_first_of("-", 0) == 0) {
      // handle -blah 25 and -blah
      if ((i+1 >= args.size()) ||  // last arg || next arg is new arg
          (i+1 < args.size() && args[i+1].find_first_of("-", 0) == 0 &&
           args[i+1].find_first_of("0123456789.", 1) != 1)) {
        key = args[i].substr(1);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        val = "1";
        set = true;
      } else { // not last arg && next arg is a value
        key = args[i].substr(1);
        std::transform(key.begin(), key.end(), key.begin(), ::tolower);
        val = args[i+1];
        set = true;
        i++;  // we've consumed next parameter as a "value"-parameter
      }
    }
    if (set && (overwrite || !params.contains(key))) {
      params[key] = val;
    }
  }
}


void Configuration::loadConfigFile(const string& fname, bool overwrite) 
{
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
    if (overwrite || !params.contains(key)) {
      params[key] = value;
    }
  }
}


string Configuration::getUsage() const 
{
  const int PAD = 10;
  const int PADEXTRA = string("--=VAL").size(); // adjust for "--" and "=VAL"
  string usage = "Parameters:\n\n";
  
  // get max key length
  int maxLen = 0;
  foreachpair(const string& key, _, options) {
    maxLen = key.size() > maxLen ? key.length() : maxLen;
  }
  maxLen += PADEXTRA; 

  foreachpair(const string& key, const Option& val, options) {
    string helpStr = val.helpString;

    if (val.defaultValue != "") {  // add default value
      helpStr += " (default VAL=" + val.defaultValue + ")";
    }

    usage += "--" + key + "=VAL";
    string pad(PAD + maxLen - key.size() - PADEXTRA, ' ');
    usage += pad;
    size_t pos1 = 0, pos2 = 0;
    pos2 = helpStr.find_first_of("\n\r", pos1);
    usage += helpStr.substr(pos1, pos2 - pos1) + "\n";

    while(pos2 != string::npos) {  // handle multi line help strings
      pos1 = pos2 + 1;
      string pad2(PAD + maxLen, ' ');
      usage += pad2;
      pos2 = helpStr.find_first_of("\n\r", pos1);
      usage += helpStr.substr(pos1, pos2 - pos1) + "\n";
    }

  }
  return usage;
}
  

int Configuration::addOption(string optName, const string& helpString) 
{
  std::transform(optName.begin(), optName.end(), optName.begin(), ::tolower);
  if (options.find(optName) != options.end())
    return -1;
  options[optName] = Option(helpString);
  return 0;
}


string Configuration::getOptionDefault(string optName) const
{
  std::transform(optName.begin(), optName.end(), optName.begin(), ::tolower);
  if (options.find(optName) == options.end()) 
    return "";
  return options.find(optName)->second.defaultValue;
}


vector<string> Configuration::getOptions() const 
{
  vector<string> ret;
  foreachpair(const string& key, _, options) {
    ret.push_back(key);
  }
  return ret;
}


Params& Configuration::getParams()
{
  return params;
}
