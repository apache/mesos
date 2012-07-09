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

#ifndef __CONFIGURATION_HPP__
#define __CONFIGURATION_HPP__

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <boost/lexical_cast.hpp>

#include "common/foreach.hpp"
#include "common/option.hpp"
#include "common/strings.hpp"


namespace mesos {
namespace internal {

struct ParseException : std::exception
{
  ParseException(const std::string& _message) : message(_message) {}
  ~ParseException() throw () {}
  const char* what() const throw () { return message.c_str(); }
  const std::string message;
};


/**
 * Stores a set of key-value pairs that can be accessed as strings,
 * ints, etc.
 */
class Configuration
{
private:
  std::map<std::string, std::string> params;

public:
  Configuration() {}

  Configuration(const std::map<std::string, std::string>& params_)
    : params(params_) {}

  Configuration(const std::string& str)
  {
    loadString(str);
  }

  /**
   * Load key-value pairs from a map into this Configuration object.
   */
  void loadMap(const std::map<std::string, std::string>& params_)
  {
    foreachpair (const std::string& k, const std::string& v, params_) {
      params[k] = v;
    }
  }

  /**
   * Load key-value pairs from a string into this Configuration
   * object.  The string should contain pairs of the form key=value,
   * one per line.
   */
  void loadString(const std::string& str)
  {
    std::vector<std::string> lines = strings::split(str, "\n\r");
    foreach (std::string& line, lines) {
      std::vector<std::string> parts = strings::split(line, "=");
      if (parts.size() != 2) {
        const Try<std::string>& error =
          strings::format("Failed to parse '%s'", line);
        throw ParseException(error.isSome() ? error.get() : "Failed to parse");
      }
      params[parts[0]] = parts[1];
    }
  }

  std::string& operator[] (const std::string& key)
  {
    return params[key];
  }

  template <typename T>
  Option<T> get(const std::string& key) const
  {
    std::map<std::string, std::string>::const_iterator it = params.find(key);
    if (it != params.end()) {
      return boost::lexical_cast<T>(it->second);
    }

    return Option<T>::none();
  }

  template <typename T>
  T get(const std::string& key, const T& defaultValue) const
  {
    std::map<std::string, std::string>::const_iterator it = params.find(key);
    if (it != params.end()) {
      return boost::lexical_cast<T>(it->second);
    }

    return defaultValue;
  }

  template <typename T>
  void set(const std::string& key, T value)
  {
    params[key] = boost::lexical_cast<std::string>(value);
  }

  std::string str() const
  {
    std::ostringstream oss;
    foreachpair (const std::string& key, const std::string& value, params) {
      oss << key << "=" << value << "\n";
    }
    return oss.str();
  }

  std::map<std::string, std::string>& getMap()
  {
    return params;
  }

  const std::map<std::string, std::string>& getMap() const
  {
    return params;
  }

  bool contains(const std::string& key) const
  {
    return params.find(key) != params.end();
  }
};


} // namespace internal {
} // namespace mesos {

#endif // __CONFIGURATION_HPP__
