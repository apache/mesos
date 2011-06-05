#ifndef __CONFIGURATION_HPP__
#define __CONFIGURATION_HPP__

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <boost/lexical_cast.hpp>

#include "common/foreach.hpp"
#include "common/string_utils.hpp"


namespace mesos { namespace internal {


struct ParseException : std::exception
{
  ParseException(const char* msg): message(msg) {}
  const char* what() const throw () { return message; }
  const char* message;
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
    std::vector<std::string> lines;
    StringUtils::split(str, "\n\r", &lines);
    foreach (std::string& line, lines) {
      std::vector<std::string> parts;
      StringUtils::split(line, "=", &parts);
      if (parts.size() != 2) {
        std::ostringstream oss;
        oss << "Malformed line in params: '" << line << "'";
        throw ParseException(oss.str().c_str());
      }
      params[parts[0]] = parts[1];
    }
  }

  std::string& operator[] (const std::string& key)
  {
    return params[key];
  }

  const std::string& get(const std::string& key,
                         const std::string& defaultValue) const
  {
    std::map<std::string, std::string>::const_iterator it = params.find(key);
    return (it != params.end()) ? it->second : defaultValue;
  }

  int getInt(const std::string& key, int defaultValue) const
  {
    return get<int>(key, defaultValue);
  }

  int32_t getInt32(const std::string& key, int32_t defaultValue) const
  {
    return get<int32_t>(key, defaultValue);
  }

  int64_t getInt64(const std::string& key, int64_t defaultValue) const
  {
    return get<int64_t>(key, defaultValue);
  }

  template <typename T>
  T get(const std::string& key, const T& defaultValue) const
  {
    std::map<std::string, std::string>::const_iterator it = params.find(key);
    if (it != params.end())
      return boost::lexical_cast<T>(it->second);
    else
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


}}

#endif /* CONFIGURATION_HPP */
