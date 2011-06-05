#ifndef __CONFIGURATION_HPP__
#define __CONFIGURATION_HPP__

#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <glog/logging.h>
#include "params.hpp"

using std::string;
using std::cout;
using std::cerr;
using std::endl;
using std::ifstream;
using std::map;

namespace nexus { namespace internal {
    

/**
 * Exception type thrown by Configuration.
 */
struct ConfigurationException : std::exception
{
  const char* message;
  ConfigurationException(const char* msg): message(msg) {}
  const char* what() const throw () { return message; }
};


/** 
 * This class populates a Params object, which can be retrieved with
 * getParams(), by reading variables from the command line, a config file
 * or the environment.
 *
 * It currently supports 3 input types:
 * (i) Environment variables. It adds all variables starting with MESOS_.
 * (ii) Command line variables. Supports "--key=val" "-key val" "-opt" "--opt"
 * (iii) Config file. It ignores comments "#". It finds the file using
 * MESOS_CONF or via command line --conf=file. Otherwise, it looks for
 * "mesos.conf" in MESOS_HOME/conf.
 **/
class Configuration 
{
public:
  static const char* DEFAULT_CONFIG_DIR;
  static const char* CONFIG_FILE_NAME;
  static const char* ENV_VAR_PREFIX;

private:
  Params params;

public:
  /** 
   * Constructor that populates Params from environment and any config file
   * located through the environment (through MESOS_HOME or MESOS_CONF).
   **/
  Configuration();

  /** 
   * Constructor that populates Params from environment, command line,
   * and any config file specified through these.
   *
   * @param argc number of paramters in argv
   * @param argv array of c-strings from the command line
   * @param inferMesosHomeFromArg0 whether to set mesos home to directory
   *                               containing argv[0] (the program being run)
   **/
  Configuration(int argc, char** argv, bool inferMesosHomeFromArg0);
  
  /** 
   * Constructor that populates Params from environment, a map,
   * and any config file specified through these.
   *
   * @param argc number of paramters in argv
   * @param argv array of c-strings from the command line
   **/
  Configuration(const map<string, string>& _params);

  /**
   * Returns the Params object parsed by this Configuration.
   * @return Params populated params object
   **/
  Params& getParams();

private:
  /**
   * Parses the environment variables and populates a Params.
   * It picks all environment variables that start with MESOS_.
   * The environment variable MESOS_HOME=/dir would lead to key=HOME val=/dir
   **/
  void loadEnv();

  /**
   * Populates its internal Params with key/value params from command line.
   * It extracts four type of command line parameters:
   * "--key=val", "-key val", "--key", "-key". The two last cases will
   * have default value "1" in the Params. 
   *
   * @param argc is the number of parameters in argv
   * @param argv is an array of c-strings containing params
   * @param inferMesosHomeFromArg0 whether to set mesos home to directory
   *                               containing argv[0] (the program being run)
   **/
  void loadCommandLine(int argc, char** argv, bool inferMesosHomeFromArg0);

  /**
   * Populates its internal Params with key/value params from a config file.
   * The config file should contain key=value pairs, one per line.
   * Comments, which should start with #, are ignored.
   *
   * @param fname is the name of the config file to open
   **/
  void loadConfigFile(const string& fname);

  /**
   * Load the config file set through the command line or environment, if any.
   */
  void loadConfigFileIfGiven();
};

} }   // end nexus :: internal namespace

#endif
