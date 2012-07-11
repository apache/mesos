#ifndef __FLAGS_HPP__
#define __FLAGS_HPP__

#include <stdlib.h> // For abort.

#include <algorithm> // For std::max.
#include <iomanip> // For std::ios_base::boolalpha.
#include <map>
#include <sstream> // For istringstream.
#include <string>
#include <typeinfo> // For typeid.

#include <tr1/functional>

#include "common/foreach.hpp"
#include "common/option.hpp"
#include "common/stringify.hpp"

// An abstraction for application/library "flags". An example is
// probably best:
//  -------------------------------------------------------------
// class MyFlags : public FlagsBase
// {
// public:
//   MyFlags()
//   {
//     add(&debug,
//         "debug",
//         "Help string for debug",
//         false);
//
//     add(&name,
//         "name",
//         "Help string for name");
//   }

//   bool debug;
//   Option<string> name;
// };
//
// ...
//
// map<string, Option<string> > values;
// values["no-debug"] = Option<string>::none(); // --no-debug
// values["debug"] = Option<string>::none(); // --debug
// values["debug"] = Option<string>::some("true"); // --debug=true
// values["debug"] = Option<string>::some("false"); // --debug=false
// values["name"] = Option<string>::some("frank"); // --name=frank
// values["name"] = Option<string>::none(); // --name=true
//
// MyFlags flags;
// flags.load(values);
// flags.name.isSome() ...
// flags.debug ...
//  -------------------------------------------------------------
//
// Note that flags that can not be loaded (e.g., setting it via the
// 'no-' prefix when it is not a boolean) will print a message to
// standard error and abort the process (rather harsh, but under the
// "fail early, fail often" mentatility.

// TODO(benh): Provide a boolean which specifies whether or not to
// abort on load errors.

// TODO(benh): Make prefix for environment variables configurable
// (e.g., "MESOS_").

namespace flags {

struct Flag
{
  std::string name;
  std::string help;
  bool boolean;
  std::tr1::function<void(const std::string&)> loader;
};


class FlagsBase
{
public:
  template <typename T1, typename T2>
  void add(T1* t1,
           const std::string& name,
           const std::string& help,
           const T2& t2);

  template <typename T>
  void add(Option<T>* option,
           const std::string& name,
           const std::string& help);

  virtual void load(const std::map<std::string, Option<std::string> >& values);
  virtual void load(const std::map<std::string, std::string>& values);

  std::map<std::string, Flag> flags;

private:
  template <typename T>
  static void load(
      const std::string& value,
      const std::string& name,
      T* t);

  template <typename T>
  static void loadOptional(
      const std::string& value,
      const std::string& name,
      Option<T>* option);
};


// Need to declare/define some explicit subclasses of FlagsBase so
// that we can overload the 'Flags::operator FlagsN () const'
// functions for each possible type.
class _Flags1 : public FlagsBase {};
class _Flags2 : public FlagsBase {};
class _Flags3 : public FlagsBase {};
class _Flags4 : public FlagsBase {};
class _Flags5 : public FlagsBase {};


template <typename Flags1 = _Flags1,
          typename Flags2 = _Flags2,
          typename Flags3 = _Flags3,
          typename Flags4 = _Flags4,
          typename Flags5 = _Flags5>
class Flags : public FlagsBase
{
public:
  Flags()
  {
    // Make sure we have no duplicates.
    foreachkey (const std::string& name, flags1.flags) {
      if (names.count(name) > 0) {
        std::cerr << "Attempted to add duplicate flag '"
                  << name << "'" << std::endl;
        abort();
      }
      names.insert(name);
    }

    foreachkey (const std::string& name, flags2.flags) {
      if (names.count(name) > 0) {
        std::cerr << "Attempted to add duplicate flag '"
                  << name << "'" << std::endl;
        abort();
      }
      names.insert(name);
    }

    foreachkey (const std::string& name, flags3.flags) {
      if (names.count(name) > 0) {
        std::cerr << "Attempted to add duplicate flag '"
                  << name << "'" << std::endl;
        abort();
      }
      names.insert(name);
    }

    foreachkey (const std::string& name, flags4.flags) {
      if (names.count(name) > 0) {
        std::cerr << "Attempted to add duplicate flag '"
                  << name << "'" << std::endl;
        abort();
      }
      names.insert(name);
    }

    foreachkey (const std::string& name, flags5.flags) {
      if (names.count(name) > 0) {
        std::cerr << "Attempted to add duplicate flag '"
                  << name << "'" << std::endl;
        abort();
      }
      names.insert(name);
    }
  }

  virtual void load(const std::map<std::string, Option<std::string> >& values)
  {
    flags1.load(values);
    flags2.load(values);
    flags3.load(values);
    flags4.load(values);
    flags5.load(values);

    FlagsBase::load(values);
  }

  virtual void load(const std::map<std::string, std::string>& values)
  {
    flags1.load(values);
    flags2.load(values);
    flags3.load(values);
    flags4.load(values);
    flags5.load(values);

    FlagsBase::load(values);
  }

  template <typename T1, typename T2>
  void add(T1* t1,
           const std::string& name,
           const std::string& help,
           const T2& t2)
  {
    if (names.count(name) > 0) {
      std::cerr << "Attempted to add duplicate flag '"
                << name << "'" << std::endl;
      abort();
    }

    FlagsBase::add(t1, name, help, t2);
  }

  template <typename T>
  void add(Option<T>* option,
           const std::string& name,
           const std::string& help)
  {
    if (names.count(name) > 0) {
      std::cerr << "Attempted to add duplicate flag '"
                << name << "'" << std::endl;
      abort();
    }

    FlagsBase::add(option, name, help);
  }

  operator Flags1 () const
  {
    return flags1;
  }

  operator Flags2 () const
  {
    return flags2;
  }

  operator Flags3 () const
  {
    return flags3;
  }

  operator Flags4 () const
  {
    return flags4;
  }

  operator Flags5 () const
  {
    return flags5;
  }

  template <typename T>
  T as() const
  {
    return (T) *this;
  }

  Flags1 flags1;
  Flags2 flags2;
  Flags3 flags3;
  Flags4 flags4;
  Flags5 flags5;

  std::set<std::string> names; // Aggregate names, used for detecting duplicates.
};


template <typename T1, typename T2>
void FlagsBase::add(
    T1* t1,
    const std::string& name,
    const std::string& help,
    const T2& t2)
{
  if (flags.count(name) > 0) {
    std::cerr << "Attempted to add duplicate flag '"
              << name << "'" << std::endl;
    abort();
  }

  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(T1) == typeid(bool);
  flag.loader =
    std::tr1::bind(&load<T1>, name, std::tr1::placeholders::_1, t1);

  // Update the help string to include the default value.
  flag.help += help.size() > 0 && help.find_last_of("\n\r") != help.size() - 1
    ? " (default: " // On same line, add space.
    : "(default: "; // On newline.

  flag.help += flag.boolean
    ? (t2 ? "true" : "false")
    : stringify(t2);

  flag.help += ")";

  flags[name] = flag;

  *t1 = t2; // Set the default.
}


template <typename T>
void FlagsBase::add(
    Option<T>* option,
    const std::string& name,
    const std::string& help)
{
  if (flags.count(name) > 0) {
    std::cerr << "Attempted to add duplicate flag '"
              << name << "'" << std::endl;
    abort();
  }

  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(T) == typeid(bool);
  flag.loader =
    std::tr1::bind(&loadOptional<T>, name, std::tr1::placeholders::_1, option);

  flags[name] = flag;
}


inline void FlagsBase::load(const std::map<std::string, Option<std::string> >& values)
{
  std::map<std::string, Option<std::string> >::const_iterator iterator;
  
  for (iterator = values.begin(); iterator != values.end(); ++iterator) {
    const std::string& name = iterator->first;
    const Option<std::string>& value = iterator->second;

    if (flags.count(name) > 0) {
      if (value.isSome()) {
        flags[name].loader(value.get());                // --name=value
      } else if (flags[name].boolean) {
        flags[name].loader("true");                     // --name
      } else {
        std::cerr << "Failed to load non-boolean flag via '"
                  << name << "'" << std::endl;
        abort();
      }
    } else if (name.find("no-") == 0 && flags.count(name.substr(3)) > 0) {
      if (flags[name.substr(3)].boolean) {
        if (value.isNone()) {
          flags[name.substr(3)].loader("false");        // --no-name
        } else {
          std::cerr << "Failed to load boolean flag '"
                    << name.substr(3) << "' via '" << name
                    << "' with value '" << value.get()
                    << "'" << std::endl;
          abort();
        }
      } else {
        std::cerr << "Failed to load non-boolean flag '"
                  << name.substr(3) << "' via '"
                  << name << "'" << std::endl;
        abort();
      }
    }
  }
}


inline void FlagsBase::load(const std::map<std::string, std::string>& values)
{
  std::map<std::string, Option<std::string> > values2;

  foreachpair (const std::string& key, const std::string& value, values) {
    values2[key] = Option<std::string>::some(value);
  }

  load(values2);
}


template <typename T>
void FlagsBase::load(
    const std::string& name,
    const std::string& value,
    T* t)
{
  std::istringstream in(value);
  in >> *t;
  if (!in.good() && !in.eof()) {
    std::cerr << "Failed to load value '" << value
              << "' for flag '" << name
              << "'" << std::endl;
    abort();
  }
}


template <typename T>
void FlagsBase::loadOptional(
    const std::string& name,
    const std::string& value,
    Option<T>* option)
{
  T t;
  std::istringstream in(value);
  in >> t;
  if (!in.good() && !in.eof()) {
    std::cerr << "Failed to load value '" << value
              << "' for flag '" << name
              << "'" << std::endl;
    abort();
  }
  *option = Option<T>::some(t);
}


// Template specialization of std::string is necessary because some
// values may have white spaces (e.g., "billy joel") and we want to
// capture the entire value.
template <>
inline void FlagsBase::load<std::string>(
    const std::string& name,
    const std::string& value,
    std::string* s)
{
  *s = value;
}


template <>
inline void FlagsBase::loadOptional<std::string>(
    const std::string& name,
    const std::string& value,
    Option<std::string>* option)
{
  *option = Option<std::string>(value);
}


// Template specialization of bool is necessary in order to capture
// both alpha versions of booleans (e.g. "true", "false") and numeric
// versions (e.g., "1", "0").
template <>
inline void FlagsBase::load<bool>(
    const std::string& name,
    const std::string& value,
    bool* b)
{
  if (value == "true" || value == "1") {
    *b = true;
  } else if (value == "false" || value == "0") {
    *b = false;
  } else {
    std::cerr << "Failed to load value '" << value
              << "' for flag '" << name
              << "'" << std::endl;
    abort();
  }
}


template <>
inline void FlagsBase::loadOptional<bool>(
    const std::string& name,
    const std::string& value,
    Option<bool>* option)
{
  if (value == "true" || value == "1") {
    *option = Option<bool>::some(true);
  } else if (value == "false" || value == "0") {
    *option = Option<bool>::some(false);
  } else {
    std::cerr << "Failed to load value '" << value
              << "' for flag '" << name
              << "'" << std::endl;
    abort();
  }
}

} // namespace flags {

#endif // __FLAGS_HPP__
