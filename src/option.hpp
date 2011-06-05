#ifndef __OPTION_HPP__
#define __OPTION_HPP__

#include <algorithm>
#include <iostream>
#include <fstream>
#include <string>
#include <map>
#include <typeinfo>
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
  virtual bool isBool() const = 0;
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

  virtual bool isBool() const
  {
    return (typeid(bool) == typeid(T));
  }

};


/**
 * Registered option with help string and default value
 **/
class Option {
public:
  Option() : hasDefault(false), validator(NULL) {}

  Option(const string& _helpString,
         const ValidatorBase& _validator,
         bool _hasShortName,
         char _shortName,
         bool _hasDefault,
         const string& _defaultValue)
    : helpString(_helpString), 
      hasDefault(_hasDefault),
      defaultValue(_defaultValue),
      hasShortName(_hasShortName),
      shortName(_shortName)
  {
    validator = _validator.clone();
  }
  
  Option(const Option& opt)
    : helpString(opt.helpString), 
      hasDefault(opt.hasDefault),
      hasShortName(opt.hasShortName),
      shortName(opt.shortName),
      defaultValue(opt.defaultValue)
  {
    validator = (opt.validator == NULL) ? NULL : opt.validator->clone();
  }

  Option &operator=(const Option& opt)
  {
    helpString = opt.helpString;
    hasDefault = opt.hasDefault;
    hasShortName = opt.hasShortName;
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
  bool hasShortName;
  char shortName;
  ValidatorBase *validator;
};

} }   // end nexus :: internal namespace

#endif
