#ifndef __CONFIGURATION_HPP__
#define __CONFIGURATION_HPP__

#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <boost/any.hpp>
#include <glog/logging.h>
#include "params.hpp"
#include "foreach.hpp"


namespace nexus { namespace internal {
    
using std::string;
using std::cout;
using std::cerr;
using std::endl;
using std::ifstream;
using std::map;


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
 * Registered option with help string and defautl value
 **/
struct Option {
  Option(string _helpString, string _defaultValue="") : 
    helpString(_helpString), defaultValue(_defaultValue) {} 

  Option() {}

  string helpString;
  string defaultValue;
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
  map<string, Option> options;

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

  /**
   * Returns a usage string with all registered options
   * @see addOption()
   * @return usage string
   **/
  string getUsage() const;
  
  /**
   * Adds a registered option together with a default value and a help string.
   * @param optName name of the option, e.g. "home"
   * @param helpString description of the option, may contain line breaks
   * @param defaultValue default value of the option. 
   *        The default option is put in the internal params, 
   *        unless the option already has a value in params.
   *        Its type must support operator<<(ostream,...)
   * @return 0 on success, -1 if option already exists
   **/
  template <class T>
  int addOption(string optName, const string& helpString, 
             const T& defaultValue) 
  {
    std::transform(optName.begin(), optName.end(), optName.begin(), ::tolower);
    if (options.find(optName) != options.end())
      return -1;
    ostringstream os;
    os << defaultValue;
    options[optName] = Option(helpString, os.str());

    if (!params.contains(optName))  // insert default value
      params[optName] = os.str();

    return 0;
  }

  /**
   * Adds a registered option together with a help string
   * It's recommended to use the other version of this method, 
   * which takes a default value.
   * @param optName name of the option, e.g. "home"
   * @param helpString description of the option, may contain line breaks
   * @return 0 on success, -1 if option already exists
   **/
  int addOption(string optName, const string& helpString);

  /**
   * Returns the default string value associated with an option.
   * @param optName name of the option, e.g. "home"
   * @return default value associated with optName
   **/
  string getOptionDefault(string optName) const;

  /**
   * Returns the name of all options.
   * @return name of every registered option
   **/
  vector<string> getOptions() const;

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
   * Values in the config file DO NOT override values already loaded into
   * conf (i.e. having been defined in the environment or command line), as
   * is typically expected for programs that allow configuration both ways.
   *
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
