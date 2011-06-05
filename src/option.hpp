#ifndef __OPTION_HPP__
#define __OPTION_HPP__

#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>
#include <map>
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

  Option() : hasDefault(false), validator(NULL) {}

  Option(const string& _helpString,
         const ValidatorBase& _validator,
         const string& _defaultValue = "",
         char _shortName = '\0')
    : helpString(_helpString), 
      defaultValue(_defaultValue),
      shortName(_shortName)
  {
    hasDefault = _defaultValue == "" ? false : true;
    hasShort = _shortName == '\0' ? false : true;
    validator = _validator.clone();
  }
  
  Option(const string& _helpString,
         const ValidatorBase& _validator,
         char _shortName)
    : helpString(_helpString), 
      hasDefault(false),
      shortName(_shortName)
  {
    hasShort = _shortName == '\0' ? false : true;
    validator = _validator.clone();
  }
  
  Option(const Option& opt)
    : helpString(opt.helpString), 
      hasDefault(opt.hasDefault),
      hasShort(opt.hasShort),
      shortName(opt.shortName),
      defaultValue(opt.defaultValue)
  {
    validator = opt.validator == NULL ? NULL : opt.validator->clone();
  }

  Option &operator=(const Option& opt)
  {
    helpString = opt.helpString;
    hasDefault = opt.hasDefault;
    hasShort = opt.hasShort;
    shortName = opt.shortName;
    defaultValue = opt.defaultValue;
    validator = opt.validator == NULL ? NULL : opt.validator->clone();
    return *this;
  }

  ~Option() 
  { 
    if (validator != 0) delete validator; 
  }

  string helpString;
  bool hasDefault;
  string defaultValue;
  bool hasShort;
  char shortName;
  ValidatorBase *validator;
};

} }   // end nexus :: internal namespace

#endif
