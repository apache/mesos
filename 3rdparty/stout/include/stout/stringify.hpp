// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#ifndef __STOUT_STRINGIFY_HPP__
#define __STOUT_STRINGIFY_HPP__

#include <iostream> // For 'std::cerr' and 'std::endl'.
#include <list>
#include <map>
#include <set>
#include <sstream> // For 'std::ostringstream'.
#include <string>
#include <vector>

#ifdef __WINDOWS__
// `codecvt` is not available on older versions of Linux. Until it is needed on
// other platforms, it's easiest to just build the UTF converter for Windows.
#include <codecvt>
#include <locale>
#endif // __WINDOWS__

#include "abort.hpp"
#include "error.hpp"
#include "hashmap.hpp"
#include "set.hpp"

template <typename T>
std::string stringify(const T& t)
{
  std::ostringstream out;
  out << t;
  if (!out.good()) {
    ABORT("Failed to stringify!");
  }
  return out.str();
}


// We provide an explicit overload for strings so we do not incur the overhead
// of a stringstream in generic code (e.g., when stringifying containers of
// strings below).
inline std::string stringify(const std::string& str)
{
  return str;
}


#ifdef __WINDOWS__
inline std::string stringify(const std::wstring& str)
{
  // Convert UTF-16 `wstring` to UTF-8 `string`.
  static std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t>
    converter(
        "UTF-16 to UTF-8 conversion failed",
        L"UTF-16 to UTF-8 conversion failed");

  return converter.to_bytes(str);
}


inline std::wstring wide_stringify(const std::string& str)
{
  // Convert UTF-8 `string` to UTF-16 `wstring`.
  static std::wstring_convert<std::codecvt_utf8_utf16<wchar_t>, wchar_t>
    converter(
        "UTF-8 to UTF-16 conversion failed",
        L"UTF-8 to UTF-16 conversion failed");

  return converter.from_bytes(str);
}
#endif // __WINDOWS__


inline std::string stringify(bool b)
{
  return b ? "true" : "false";
}


template <typename T>
std::string stringify(const std::set<T>& set)
{
  std::ostringstream out;
  out << "{ ";
  typename std::set<T>::const_iterator iterator = set.begin();
  while (iterator != set.end()) {
    out << stringify(*iterator);
    if (++iterator != set.end()) {
      out << ", ";
    }
  }
  out << " }";
  return out.str();
}


template <typename T>
std::string stringify(const std::list<T>& list)
{
  std::ostringstream out;
  out << "[ ";
  typename std::list<T>::const_iterator iterator = list.begin();
  while (iterator != list.end()) {
    out << stringify(*iterator);
    if (++iterator != list.end()) {
      out << ", ";
    }
  }
  out << " ]";
  return out.str();
}


template <typename T>
std::string stringify(const std::vector<T>& vector)
{
  std::ostringstream out;
  out << "[ ";
  typename std::vector<T>::const_iterator iterator = vector.begin();
  while (iterator != vector.end()) {
    out << stringify(*iterator);
    if (++iterator != vector.end()) {
      out << ", ";
    }
  }
  out << " ]";
  return out.str();
}


template <typename K, typename V>
std::string stringify(const std::map<K, V>& map)
{
  std::ostringstream out;
  out << "{ ";
  typename std::map<K, V>::const_iterator iterator = map.begin();
  while (iterator != map.end()) {
    out << stringify(iterator->first);
    out << ": ";
    out << stringify(iterator->second);
    if (++iterator != map.end()) {
      out << ", ";
    }
  }
  out << " }";
  return out.str();
}


template <typename T>
std::string stringify(const hashset<T>& set)
{
  std::ostringstream out;
  out << "{ ";
  typename hashset<T>::const_iterator iterator = set.begin();
  while (iterator != set.end()) {
    out << stringify(*iterator);
    if (++iterator != set.end()) {
      out << ", ";
    }
  }
  out << " }";
  return out.str();
}


template <typename K, typename V>
std::string stringify(const hashmap<K, V>& map)
{
  std::ostringstream out;
  out << "{ ";
  typename hashmap<K, V>::const_iterator iterator = map.begin();
  while (iterator != map.end()) {
    out << stringify(iterator->first);
    out << ": ";
    out << stringify(iterator->second);
    if (++iterator != map.end()) {
      out << ", ";
    }
  }
  out << " }";
  return out.str();
}


// TODO(chhsiao): This overload returns a non-const rvalue for consistency.
// Consider the following overloads instead for better performance:
//   const std::string& stringify(const Error&);
//   std::string stringify(Error&&);
inline std::string stringify(const Error& error)
{
  return error.message;
}

#endif // __STOUT_STRINGIFY_HPP__
