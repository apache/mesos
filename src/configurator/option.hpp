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

#ifndef __CONFIGURATOR_OPTION_HPP__
#define __CONFIGURATOR_OPTION_HPP__

#include <string>


namespace mesos { namespace internal {
    

/**
 * Interface of a validator
 **/
class ValidatorBase {
public:
  virtual ValidatorBase* clone() const = 0;
  virtual bool isBool() const = 0;
  /**
   * Checks if the provided string can be cast to a T.
   * @param val value associated with some option
   * @return true if val can be cast to a T, otherwise false.
   **/
  virtual bool isValid(const std::string& val) const = 0;

protected:
  template <class T>
  bool isValidInternal(const std::string& val) const
  {
    try {
      boost::lexical_cast<T>(val);
    }
    catch(const boost::bad_lexical_cast& ex) {
      return false;
    }
    return true;
  }
};


/**
 * Validator that checks if a string can be cast to its templated type.
 **/
template <typename T>
class Validator : public ValidatorBase {
public:
  virtual bool isValid(const std::string& val) const { 
    return isValidInternal<T>(val); 
  }
  
  virtual bool isBool() const { return false; }

  virtual ValidatorBase* clone() const  { 
    return new Validator<T>(); 
  }
};


/**
 * Validator for bools that checks if a string can be cast to its templated type.
 **/
template <>
class Validator <bool> : public ValidatorBase {
public:
  bool isValid(const std::string& val) const { 
    return isValidInternal<bool>(val); 
  }

  bool isBool() const { return true; }

  ValidatorBase* clone() const { 
    return new Validator<bool>(); 
  }
};


/**
 * Registered option with help string and default value
 **/
class ConfigOption {
public:
  ConfigOption() : hasDefault(false), validator(NULL) {}

  ConfigOption(const std::string& _helpString,
               const ValidatorBase& _validator,
               bool _hasShortName,
               char _shortName,
               bool _hasDefault,
               const std::string& _defaultValue)
    : helpString(_helpString), 
      hasDefault(_hasDefault),
      defaultValue(_defaultValue),
      hasShortName(_hasShortName),
      shortName(_shortName)
  {
    validator = _validator.clone();
  }

  ConfigOption(const ConfigOption& opt)
    : helpString(opt.helpString),
      hasDefault(opt.hasDefault),
      hasShortName(opt.hasShortName),
      shortName(opt.shortName),
      defaultValue(opt.defaultValue)
  {
    validator = (opt.validator == NULL) ? NULL : opt.validator->clone();
  }

  ConfigOption &operator=(const ConfigOption& opt)
  {
    helpString = opt.helpString;
    hasDefault = opt.hasDefault;
    hasShortName = opt.hasShortName;
    shortName = opt.shortName;
    defaultValue = opt.defaultValue;
    validator = opt.validator == NULL ? NULL : opt.validator->clone();
    return *this;
  }

  ~ConfigOption()
  {
    if (validator != 0) delete validator;
  }

  std::string helpString;
  bool hasDefault;
  std::string defaultValue;
  bool hasShortName;
  char shortName;
  ValidatorBase *validator;
};

} }   // end mesos :: internal namespace

#endif // __CONFIGURATOR_OPTION_HPP__
