#ifndef PARAMS_HPP
#define PARAMS_HPP

#include <map>
#include <sstream>
#include <string>
#include <vector>

#include <boost/lexical_cast.hpp>
#include <boost/foreach.hpp>

#include "foreach.hpp"


namespace nexus { namespace internal {

using std::map;
using std::ostringstream;
using std::string;
using std::vector;
using std::pair;
using std::make_pair;

using boost::lexical_cast;


struct ParseException : std::exception
{
  const char* message;
  ParseException(const char* msg): message(msg) {}
  const char* what() const throw () { return message; }
};


/**
 * Stores a set of key-value pairs that can be accessed as strings, ints, etc
 */
class Params
{
private:
  map<string, string> params;

public:
  Params() {}

  Params(const map<string, string>& params_): params(params_) {}

  Params(const string& str)
  {
    loadString(str);
  }

  /**
   * Load key-value pairs from a map into this Params object.
   */
  void loadMap(const map<string, string>& params_)
  {
    foreachpair(const string& k, const string& v, params_) {
      params[k] = v;
    }
  }

  /**
   * Load key-value pairs from a string into this Params object.
   * The string should contain pairs of the form key=value, one per line.
   */
  void loadString(const string& str)
  {
    vector<string> lines;
    split(str, "\n\r", lines);
    foreach (string& line, lines) {
      vector<string> parts;
      split(line, "=", parts);
      if (parts.size() != 2) {
        ostringstream oss;
        oss << "Malformed line in params: '" << line << "'";
        throw ParseException(oss.str().c_str());
      }
      params[parts[0]] = parts[1];
    }
  }

  string& operator[] (const string& key)
  {
    return params[key];
  }

  const string& get(const string& key, const string& defaultValue) const
  {
    map<string, string>::const_iterator it = params.find(key);
    return (it != params.end()) ? it->second : defaultValue;
  }

  int getInt(const string& key, int defaultValue) const
  {
    map<string, string>::const_iterator it = params.find(key);
    if (it != params.end())
      return lexical_cast<int>(it->second);
    else
      return defaultValue;
  }

  int32_t getInt32(const string& key, int32_t defaultValue) const
  {
    map<string, string>::const_iterator it = params.find(key);
    if (it != params.end())
      return lexical_cast<int32_t>(it->second);
    else
      return defaultValue;
  }

  int64_t getInt64(const string& key, int64_t defaultValue) const
  {
    map<string, string>::const_iterator it = params.find(key);
    if (it != params.end())
      return lexical_cast<int64_t>(it->second);
    else
      return defaultValue;
  }

  template <typename T>
  void set(const string& key, T value)
  {
    params[key] = lexical_cast<string>(value);
  }

  string str() const
  {
    ostringstream oss;
    foreachpair (const string& key, const string& value, params) {
      oss << key << "=" << value << "\n";
    }
    return oss.str();
  }

  map<string, string>& getMap()
  {
    return params;
  }

  const map<string, string>& getMap() const
  {
    return params;
  }

  bool contains(const string& key) const
  {
    return params.find(key) != params.end();
  }

private:
  void split(const string& str, const string& delims, vector<string>& tokens)
  {
    // Start and end of current token; initialize these to the first token in
    // the string, skipping any leading delimiters
    size_t start = str.find_first_not_of(delims, 0);
    size_t end = str.find_first_of(delims, start);
    while (start != string::npos || end != string::npos) {
      // Add current token to the vector
      tokens.push_back(str.substr(start, end-start));
      // Advance start to first non-delimiter past the current end
      start = str.find_first_not_of(delims, end);
      // Advance end to the next delimiter after the new start
      end = str.find_first_of(delims, start);
    }
  }
};


}}

#endif /* PARAMS_HPP */
