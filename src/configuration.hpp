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
using boost::lexical_cast;
using boost::bad_lexical_cast;

/**
 * Interface of a validator
 **/
class ValidatorBase {
public:
  virtual bool isValid(const string& val) const = 0;
  virtual ValidatorBase* clone() const = 0;
};

/**
 * Validator that checks if a string can be cast to its templated type.
 **/
template <class T>
class Validator : public ValidatorBase {
public:
  Validator() {}

  /**
   * Checks if the provided string can be cast to a T.
   * @param val value associated with some option
   * @return true if val can be cast to a T, otherwise false.
   **/
  virtual bool isValid(const string& val) const
  {
    try {
      lexical_cast<T>(val);
    }
    catch(const bad_lexical_cast& ex) {
      return false;
    }
    return true;
  }

  virtual ValidatorBase* clone() const
  {
    return new Validator<T>();
  }

};

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
 * Exception type thrown if the the value of an Option 
 * doesn't match the default value type.
 */
struct BadOptionValueException : std::exception
{
  const char* message;
  BadOptionValueException(const char* msg): message(msg) {}
  const char* what() const throw () { return message; }
};

/**
 * Registered option with help string and default value
 **/
struct Option {
  Option(string _helpString) : 
    helpString(_helpString), defaultValue(""), validator(NULL) {} 


  Option(string _helpString, string _defaultValue, 
         const ValidatorBase& _validator) : 
    helpString(_helpString), defaultValue(_defaultValue) 
  {
    validator = _validator.clone();
  } 


  Option() : validator(NULL) {}


  Option(const Option& opt) : 
    helpString(opt.helpString), defaultValue(opt.defaultValue)
  {
    validator = opt.validator == NULL ? NULL : opt.validator->clone();
  }


  Option &operator=(const Option& opt)
  {
    helpString = opt.helpString;
    defaultValue = opt.defaultValue;
    validator = opt.validator == NULL ? NULL : opt.validator->clone();
    return *this;
  }


  ~Option() 
  { 
    if (validator != 0) delete validator; 
  }


  string helpString;
  string defaultValue;
  ValidatorBase *validator;
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
   * Initializes an empty Params
   **/
  Configuration() {}


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
    options[optName] = Option(helpString, os.str(), Validator<T>());

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


  /**
   * Validates the values of all keys that it has a default option for.
   * @throws BadOptionValueException with the key of the parameter 
   * that has the wrong type.
   **/ 
  void validate();


  /**
   * Populates its internal Params with key/value params from environment, 
   * command line, and config file.
   * <i>Environment:</i><br>
   * Parses the environment variables and populates a Params.
   * It picks all environment variables that start with MESOS_.
   * The environment variable MESOS_HOME=/dir would lead to key=HOME val=/dir<br>
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
   **/
  void load(int argc, char** argv, bool inferMesosHomeFromArg0=false);


  /**
   * Populates its internal Params with key/value params from environment, 
   * and config file.
   * <i>Environment:</i><br>
   * Parses the environment variables and populates a Params.
   * It picks all environment variables that start with MESOS_.
   * The environment variable MESOS_HOME=/dir would lead to key=HOME val=/dir <br>
   * <i>Config file:</i><br>
   * The config file should contain key=value pairs, one per line.
   * Comments, which should start with #, are ignored.
   **/
  void load();


  /** 
   * Populates its internal Params with key/value params from environment, 
   * a provided map, and config file.
   * <i>Environment:</i><br>
   * Parses the environment variables and populates a Params.
   * It picks all environment variables that start with MESOS_.
   * The environment variable MESOS_HOME=/dir would lead to key=HOME val=/dir <br>
   * <i>Map:</i><br>
   * Containing a string to string map. <br>
   * <i>Config file:</i><br>
   * The config file should contain key=value pairs, one per line.
   * Comments, which should start with #, are ignored.
   *
   * @param _params map containing key value pairs to be loaded
   **/
  void load(const map<string, string>& _params);

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
  void loadCommandLine(int argc, char** argv, bool inferMesosHomeFromArg0, 
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

};

} }   // end nexus :: internal namespace

#endif
