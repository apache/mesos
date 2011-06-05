#ifndef __CONFIGURATOR_HPP__
#define __CONFIGURATOR_HPP__

#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <glog/logging.h>
#include "params.hpp"
#include "foreach.hpp"
#include "option.hpp"

namespace mesos { namespace internal {
    
using std::string;
using std::cout;
using std::cerr;
using std::endl;
using std::ifstream;
using std::map;
using boost::lexical_cast;
using boost::bad_lexical_cast;


/**
 * Exception type thrown by Configurator.
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
class Configurator 
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
   * Initializes an empty Params
   **/
  Configurator() {}


  /**
   * Returns the Params object parsed by this Configurator.
   * @return Params populated params object
   **/
  Params& getParams();


  /**
   * Returns a usage string with all registered options
   * @see addOption()
   * @return usage string
   **/
  string getUsage() const;

  
private:
  /**
   * Private method for adding an option with, potentially, a default
   * value and a short name.
   * @param optName name of the option, e.g. "home"
   * @param helpString description of the option, may contain line breaks
   * @param hasShortName whether the option has a short name
   * @param shortName character representing short name of option, e.g. 'h'
   * @param hasDefault whether the option has a default value
   * @param defaultValue default value of the option, as a string. 
   *        The default option is put in the internal params, 
   *        unless the option already has a value in params.
   **/
  template <class T>
  void addOption(const string& optName,
                 const string& helpString,
                 bool hasShortName,
                 char shortName,
                 bool hasDefault,
                 const string& defaultValue)
  {
    string name = optName;
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    if (options.find(name) != options.end()) {
      string message = "Duplicate option registration: " + name;
      throw ConfigurationException(message.c_str());
    }
    options[name] = Option(helpString,
                           Validator<T>(), 
                           hasShortName,
                           shortName,
                           hasDefault,
                           defaultValue);
    if (hasDefault && !params.contains(name)) {
      // insert default value into params
      params[name] = defaultValue;
    }
  }

public:
  /**
   * Adds a registered option together with a help string.
   * @param optName name of the option, e.g. "home"
   * @param helpString description of the option, may contain line breaks
   **/
  template <class T>
  void addOption(const string& optName, const string& helpString) 
  {
    addOption<T>(optName, helpString, false, '\0', false, "");
  }

  /**
   * Adds a registered option with a short name and help string.
   * @param optName name of the option, e.g. "home"
   * @param shortName character representing short name of option, e.g. 'h'
   * @param helpString description of the option, may contain line breaks
   **/
  template <class T>
  void addOption(const string& optName,
                 char shortName,
                 const string& helpString) 
  {
    addOption<T>(optName, helpString, true, shortName, false, "");
  }

  /**
   * Adds a registered option with a default value and a help string.
   * @param optName name of the option, e.g. "home"
   * @param helpString description of the option, may contain line breaks
   * @param defaultValue default value of option.
   *        The default option is put in the internal params, 
   *        unless the option already has a value in params.
   *        Its type must support operator<<(ostream,...)
   **/
  template <class T>
  void addOption(const string& optName,
                 const string& helpString,
                 const T& defaultValue) 
  {
    string defaultStr = lexical_cast<string>(defaultValue);
    addOption<T>(optName, helpString, false, '\0', true, defaultStr);
  }

  /**
   * Adds a registered option with a default value, short name and help string.
   * @param optName name of the option, e.g. "home"
   * @param helpString description of the option, may contain line breaks
   * @param defaultValue default value of option.
   *        The default option is put in the internal params, 
   *        unless the option already has a value in params.
   *        Its type must support operator<<(ostream,...)
   **/
  template <class T>
  void addOption(const string& optName,
                 char shortName,
                 const string& helpString,
                 const T& defaultValue) 
  {
    string defaultStr = lexical_cast<string>(defaultValue);
    addOption<T>(optName, helpString, true, shortName, true, defaultStr);
  }

  /**
   * Returns the names of all registered options.
   * @return name of every registered option
   **/
  vector<string> getOptions() const;


  /**
   * Validates the values of all keys that it has a default option for.
   * @throws ConfigurationError if a key has the wrong type.
   **/ 
  void validate();


  /**
   * Populates its internal Params with key/value params from environment, 
   * command line, and config file.
   * <i>Environment:</i><br>
   * Parses the environment variables and populates a Params.
   * It picks all environment variables that start with MESOS_.
   * The environment var MESOS_HOME=/dir would lead to key=HOME val=/dir<br>
   * <i>Command line:</i><br>
   * It extracts four type of command line parameters:
   * "--key=val", "-key val", "--key", "-key". The two last cases will
   * have default value "1" in the Params. <br>
   * <i>Config file:</i><br>
   * The config file should contain key=value pairs, one per line.
   * Comments, which should start with #, are ignored.
   *
   * @param argc is the number of parameters in argv
   * @param argv is an array of c-strings containing params
   * @param inferMesosHomeFromArg0 whether to set mesos home to directory
   *                               containing argv[0] (the program being run)
   * @return the loaded Params object
   **/
  Params& load(int argc, char** argv, bool inferMesosHomeFromArg0=false);


  /**
   * Populates its internal Params with key/value params from environment, 
   * and config file.
   * <i>Environment:</i><br>
   * Parses the environment variables and populates a Params.
   * It picks all environment variables that start with MESOS_.
   * The environment var MESOS_HOME=/dir would lead to key=home val=/dir <br>
   * <i>Config file:</i><br>
   * The config file should contain key=value pairs, one per line.
   * Comments, which should start with #, are ignored.
   *
   * @return the loaded Params object
   **/
  Params& load();


  /** 
   * Populates its internal Params with key/value params from environment, 
   * a provided map, and config file.
   * <i>Environment:</i><br>
   * Parses the environment variables and populates a Params.
   * It picks all environment variables that start with MESOS_.
   * The environment var MESOS_HOME=/dir would lead to key=HOME val=/dir <br>
   * <i>Map:</i><br>
   * Containing a string to string map. <br>
   * <i>Config file:</i><br>
   * The config file should contain key=value pairs, one per line.
   * Comments, which should start with #, are ignored.
   *
   * @param _params map containing key value pairs to be loaded
   * @return the loaded Params object
   **/
  Params& load(const map<string, string>& _params);

  /**
   * Clears all Mesos environment variables (useful for tests).
   */
  static void clearMesosEnvironmentVars();

private:
  /**
   * Parses the environment variables and populates a Params.
   * It picks all environment variables that start with MESOS_.
   * The environment variable MESOS_HOME=/dir would lead to key=HOME val=/dir
   * @param overwrite whether to overwrite keys that already have values 
   *         in the internal params (true by default)
   **/
  void loadEnv(bool overwrite=true);

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
   * @param overwrite whether to overwrite keys that already have values 
   *         in the internal params (true by default)
   **/
  void loadCommandLine(int argc,
                       char** argv,
                       bool inferMesosHomeFromArg0, 
                       bool overwrite=true);

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
   * @param overwrite whether to overwrite keys that already have values 
   *         in the internal params (false by default)
   **/
  void loadConfigFile(const string& fname, bool overwrite=false);

  /**
   * Load the config file set through the command line or environment, if any.
   * @param overwrite whether to overwrite keys that already have values 
   *         in the internal params (false by default)
   */
  void loadConfigFileIfGiven(bool overwrite=false);

  /**
   * Gets the first long name option associated with the provided short name.
   * @param shortName character representing the short name of the option
   * @return first long name option matching the short name, "" if none found.
   */
  string getLongName(char shortName) const;

  void isParamConsistent(const string& key, bool gotBool) 
    throw(ConfigurationException)
  {
    if (options.find(key) != options.end() && 
        options[key].validator->isBool() != gotBool) {
      string message = "Option '" + key + "' should ";
      if (gotBool)
        message += "not ";
      message += "be a boolean.";

      throw ConfigurationException(message.c_str());
    }
  }
};

} }   // end mesos :: internal namespace

#endif
