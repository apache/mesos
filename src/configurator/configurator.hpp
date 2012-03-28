/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __CONFIGURATOR_HPP__
#define __CONFIGURATOR_HPP__

#include <algorithm>
#include <map>
#include <string>

#include <glog/logging.h>

#include "configurator/configuration.hpp"
#include "configurator/option.hpp"


namespace mesos {
namespace internal {

/**
 * Exception type thrown by Configurator.
 */
struct ConfigurationException : std::exception
{
  ConfigurationException(const std::string& _message) : message(_message) {}
  ~ConfigurationException() throw () {}
  const char* what() const throw () { return message.c_str(); }
  const std::string message;
};


/**
 * This class populates a Configuration object, which can be retrieved
 * with getConfiguration(), by reading variables from the command
 * line, a config file or the environment.
 *
 * It currently supports 3 input types:
 * (i) Environment variables. It adds all variables starting with MESOS_.
 * (ii) Command line variables. Supports "--key=val" "-key val" "-opt" "--opt"
 * (iii) Config file. It ignores comments "#". It finds the file mesos.conf
 * in the directory specified by the "conf" option.
 **/
class Configurator
{
public:
  static const char* DEFAULT_CONFIG_DIR;
  static const char* CONFIG_FILE_NAME;
  static const char* ENV_VAR_PREFIX;

private:
  Configuration conf;
  std::map<std::string, ConfigOption> options;

public:

  /**
   * Initializes a Configurator with no options set and only the "conf"
   * option registered.
   **/
  Configurator();


  /**
   * Returns the Configuration object parsed by this Configurator.
   * @return Configuration populated configuration object
   **/
  Configuration& getConfiguration();


  /**
   * Returns a usage string with all registered options
   * @see addOption()
   * @return usage string
   **/
  std::string getUsage() const;


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
   *        The default option is put in the internal conf,
   *        unless the option already has a value in conf.
   **/
  template <class T>
  void addOption(const std::string& optName,
                 const std::string& helpString,
                 bool hasShortName,
                 char shortName,
                 bool hasDefault,
                 const std::string& defaultValue)
  {
    std::string name = optName;
    std::transform(name.begin(), name.end(), name.begin(), ::tolower);
    if (options.find(name) != options.end()) {
      std::string message = "Duplicate option registration: " + name;
      throw ConfigurationException(message.c_str());
    }
    options[name] = ConfigOption(helpString,
                                 Validator<T>(),
                                 hasShortName,
                                 shortName,
                                 hasDefault,
                                 defaultValue);
  }

public:
  /**
   * Adds a registered option together with a help string.
   * @param optName name of the option, e.g. "home"
   * @param helpString description of the option, may contain line breaks
   **/
  template <class T>
  void addOption(const std::string& optName, const std::string& helpString)
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
  void addOption(const std::string& optName,
                 char shortName,
                 const std::string& helpString)
  {
    addOption<T>(optName, helpString, true, shortName, false, "");
  }

  /**
   * Adds a registered option with a default value and a help string.
   * @param optName name of the option, e.g. "home"
   * @param helpString description of the option, may contain line breaks
   * @param defaultValue default value of option.
   *        The default option is put in the internal conf,
   *        unless the option already has a value in conf.
   *        Its type must support operator<<(ostream,...)
   **/
  template <class T>
  void addOption(const std::string& optName,
                 const std::string& helpString,
                 const T& defaultValue)
  {
    std::string defaultStr = boost::lexical_cast<std::string>(defaultValue);
    addOption<T>(optName, helpString, false, '\0', true, defaultStr);
  }

  /**
   * Adds a registered option with a default value, short name and help string.
   * @param optName name of the option, e.g. "home"
   * @param helpString description of the option, may contain line breaks
   * @param defaultValue default value of option.
   *        The default option is put in the internal conf, 
   *        unless the option already has a value in conf.
   *        Its type must support operator<<(ostream,...)
   **/
  template <class T>
  void addOption(const std::string& optName,
                 char shortName,
                 const std::string& helpString,
                 const T& defaultValue) 
  {
    std::string defaultStr = boost::lexical_cast<std::string>(defaultValue);
    addOption<T>(optName, helpString, true, shortName, true, defaultStr);
  }

  /**
   * Returns the names of all registered options.
   * @return name of every registered option
   **/
  std::vector<std::string> getOptions() const;


  /**
   * Validates the values of all keys that it has a default option for.
   * @throws ConfigurationError if a key has the wrong type.
   **/ 
  void validate();


  /**
   * Populates its internal Configuration with key/value parameters
   * from environment, command line, and config file.
   *
   * <i>Environment:</i><br>
   * Parses the environment variables and populates a Configuration.
   * It picks all environment variables that start with MESOS_.
   * The environment var MESOS_FOO=/dir would lead to key=foo val=/dir<br>
   *
   * <i>Command line:</i><br>
   * It extracts four type of command line parameters:
   * "--key=val", "-key val", "--key", "-key". The two last cases will
   * have default value "1" in the Configuration. <br>
   *
   * <i>Config file:</i><br>
   * The config file should contain key=value pairs, one per line.
   * Comments, which should start with #, are ignored.
   *
   * @param argc is the number of parameters in argv
   * @param argv is an array of c-strings containing parameters
   * @return the loaded Configuration object
   **/
  Configuration& load(int argc, char** argv);


  /**
   * Populates its internal Configuration with key/value parameters
   * from environment, and config file.
   *
   * <i>Environment:</i><br>
   * Parses the environment variables and populates a Configuration.
   * It picks all environment variables that start with MESOS_.
   * The environment var MESOS_FOO=/dir would lead to key=foo val=/dir <br>
   *
   * <i>Config file:</i><br>
   * The config file should contain key=value pairs, one per line.
   * Comments, which should start with #, are ignored.
   *
   * @return the loaded Configuration object
   **/
  Configuration& load();


  /**
   * Populates its internal Configuration with key/value parameters
   * from environment, a provided map, and config file.
   *
   * <i>Environment:</i><br>
   * Parses the environment variables and populates a Configuration.
   * It picks all environment variables that start with MESOS_.
   * The environment var MESOS_FOO=/dir would lead to key=foo val=/dir <br>
   *
   * <i>Map:</i><br>
   * Containing a string to string map. <br>
   *
   * <i>Config file:</i><br>
   * The config file should contain key=value pairs, one per line.
   * Comments, which should start with #, are ignored.
   *
   * @param params map containing key value pairs to be loaded
   * @return the loaded Configuration object
   **/
  Configuration& load(const std::map<std::string, std::string>& params);

  /**
   * Clears all Mesos environment variables (useful for tests).
   */
  static void clearMesosEnvironmentVars();

private:
  /**
   * Parses the environment variables and populates a Configuration.
   * It picks all environment variables that start with MESOS_.
   * The environment variable MESOS_FOO=/dir would lead to key=foo val=/dir
   * @param overwrite whether to overwrite keys that already have values
   *         in the internal params (true by default)
   **/
  void loadEnv(bool overwrite = true);

  /**
   * Populates its internal Configuration with key/value parameters
   * from command line.
   *
   * It extracts four type of command line parameters:
   * "--key=val", "-key val", "--key", "-key". The two last cases will
   * have default value "1" in the Params.
   *
   * @param argc is the number of parameters in argv
   * @param argv is an array of c-strings containing parameters
   * @param overwrite whether to overwrite keys that already have values
   *         in the internal params (true by default)
   **/
  void loadCommandLine(int argc,
                       char** argv,
                       bool overwrite = true);

  /**
   * Populates its internal Configuration with key/value parameters
   * from a config file.
   *
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
  void loadConfigFile(const std::string& fname, bool overwrite = false);

  /**
   * Load the config file set through the command line or environment, if any.
   * @param overwrite whether to overwrite keys that already have values
   *         in the internal params (false by default)
   */
  void loadConfigFileIfGiven(bool overwrite = false);

  /**
   * Load default values of options whose values have not already been set.
   */
  void loadDefaults();

  /**
   * Gets the first long name option associated with the provided short name.
   * @param shortName character representing the short name of the option
   * @return first long name option matching the short name, "" if none found.
   */
  std::string getLongName(char shortName) const;

  /**
   * Check whether a command-line flag is valid, based on whether it was
   * passed in boolean form (--flag or --no-flag) versus key-value form
   * (--flag=value). We test whether this option was actually registered
   * as a bool (or non-bool), and throw an error if it is passed in
   * the wrong format.
   *
   * @param key the option name parsed from the flag (without --no)
   * @param gotBool whether the option was passed as a bool
   */
  void checkCommandLineParamFormat(const std::string& key, bool gotBool)
    throw(ConfigurationException);

  /**
   * Dump the values of all config options to Google Log
   */
  void dumpToGoogleLog();
};

} // namespace internal {
} // namespace mesos {

#endif // __CONFIGURATOR_HPP__
