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

#define DEFAULT_HOME "./"
#define DEFAULT_CONF_NAME "mesos.conf"
#define DEFAULT_PREFIX "MESOS_"

namespace nexus { namespace internal {
    
/** 
 * Class that populates a Params object, which can be retrieved with getParams().
 * It currently supports 3 input types:
 * (i) Environment variables. It adds all variables starting with MESOS_.
 * (ii) Command line variables. It supports "--key=val" "-key val" "-opt" "--opt"
 * (iii) Config file. It ignores comments "#". It finds the file using
 * MESOS_CONF or via command line --CONF=file. Otherwise, it looks for
 * "mesos.conf" in MESOS_HOME or --HOME=dir. 
 
 **/
    
class Configuration 
{
public:
  /** 
   * Constructor that populates Params from environment and config file only.
   **/
  Configuration();

  /** 
   * Constructor that populates Params from environment, command line,
   * and config file only.
   *
   * @param argc number of paramters in argv
   * @param argv array of c-strings from the command line
   **/
  Configuration(int argc, char **argv);
  
  /** 
   * Constructor that populates Params from environment, a map,
   * and config file only.
   *
   * @param argc number of paramters in argv
   * @param argv array of c-strings from the command line
   **/
  Configuration(const map<string,string> &_params);

  /**
   * Returns a Params that is populated through parse* methods.
   * @return Params populated params object
   **/
  Params &getParams();

private:
  /**
   * Returns the current Mesos home directory
   *
   * @return Home directory is extracted from Params. If it doesn't exist
   * it return the current directory as a default. 
   **/
  string getMesosHome();

  /**
   * Parses the environment variables and populates a Params.
   * It picks all environment variables that start with MESOS_.
   * The environment variable MESOS_HOME=/dir would lead to key=HOME val=/dir
   **/
  void parseEnv();

  /**
   * Populates its internal Params with key value params from a map.
   **/
  void parseMap(const map<string, string> &map);

  /**
   * Populates its internal Params with key/value params from command line.
   * It extracts four type of command line parameters:
   * "--key=val", "-key val", "--key", "-key". The two last cases will
   * have default value "1" in the Params. 
   *
   * @param argc is the number of parameters in argv
   * @param argv is an array of c-strings containing params
   **/
  void parseCmdline(int argc, char **argv);

  /**
   * Populates its internal Params with key/value params from a config file.
   * It ignores comments starting with "#"
   *
   * @param fname is the name of the config file to open
   **/
  int parseConfig(const string &fname);

  string mesosHome;
  Params params;
};

} }   // end nexus :: internal namespace

#endif
