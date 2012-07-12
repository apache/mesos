#ifndef __FLAGS_FLAGS_HPP__
#define __FLAGS_FLAGS_HPP__

#include <stdlib.h> // For abort.

#include <map>
#include <string>
#include <typeinfo> // For typeid.

#include <tr1/functional>

#include <stout/option.hpp>
#include <stout/stringify.hpp>

#include "flags/flag.hpp"
#include "flags/loader.hpp"

// An abstraction for application/library "flags". An example is
// probably best:
//  -------------------------------------------------------------
// class MyFlags : public virtual FlagsBase // Use 'virtual' for composition!
// {
// public:
//   Flags()
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
// You can also compose flags provided that each has used "virtual
// inheritance":
//  -------------------------------------------------------------
// Flags<MyFlags1, MyFlags2> flags;
// flags.add(...); // Any other flags you want to throw in there.
// flags.load(values);
// flags.flag_from_myflags1 ...
// flags.flag_from_myflags2 ...
//  -------------------------------------------------------------
//
// "Fail early, fail often":
//
// You can not add duplicate flags, this is checked for you at compile
// time for composite flags (e.g., Flag<MyFlags1, MyFlags2>) and also
// checked at runtime for any other flags added via inheritance or
// Flags::add(...).
//
// Flags that can not be loaded (e.g., attempting to use the 'no-'
// prefix for a flag that is not boolean) will print a message to
// standard error and abort the process.

// TODO(benh): Provide a boolean which specifies whether or not to
// abort on duplicates or load errors.

// TODO(benh): Make prefix for environment variables configurable
// (e.g., "MESOS_").

namespace flags {

class FlagsBase
{
public:
  virtual void load(const std::map<std::string, Option<std::string> >& values);
  virtual void load(const std::map<std::string, std::string>& values);

  typedef std::map<std::string, Flag>::const_iterator const_iterator;

  const_iterator begin() const { return flags.begin(); }
  const_iterator end() const { return flags.end(); }

protected:
  template <typename T, typename M1, typename M2>
  void add(M1 T::*m1,
           const std::string& name,
           const std::string& help,
           const M2& m2);

  template <typename T, typename M>
  void add(Option<M> T::*option,
           const std::string& name,
           const std::string& help);

  void add(const Flag& flag);

private:
  std::map<std::string, Flag> flags;
};


// Need to declare/define some explicit subclasses of FlagsBase so
// that we can overload the 'Flags::operator FlagsN () const'
// functions for each possible type.
class _Flags1 : public virtual FlagsBase {};
class _Flags2 : public virtual FlagsBase {};
class _Flags3 : public virtual FlagsBase {};
class _Flags4 : public virtual FlagsBase {};
class _Flags5 : public virtual FlagsBase {};


// TODO(benh): Add some "type constraints" for template paramters to
// make sure they are all of type FlagsBase.
template <typename Flags1 = _Flags1,
          typename Flags2 = _Flags2,
          typename Flags3 = _Flags3,
          typename Flags4 = _Flags4,
          typename Flags5 = _Flags5>
class Flags : public virtual Flags1,
              public virtual Flags2,
              public virtual Flags3,
              public virtual Flags4,
              public virtual Flags5
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
};


template <typename T, typename M1, typename M2>
void FlagsBase::add(
    M1 T::*m1,
    const std::string& name,
    const std::string& help,
    const M2& m2)
{
  T* t = dynamic_cast<T*>(this);
  if (t == NULL) {
    std::cerr << "Attempted to add flag '" << name
              << "' with incompatible type" << std::endl;
    abort();
  } else {
    t->*m1 = m2; // Set the default.
  }

  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(M1) == typeid(bool);
  flag.loader = std::tr1::bind(
      &MemberLoader<T, M1>::load,
      name,
      m1,
      std::tr1::placeholders::_1,
      std::tr1::placeholders::_2);

  // Update the help string to include the default value.
  flag.help += help.size() > 0 && help.find_last_of("\n\r") != help.size() - 1
    ? " (default: " // On same line, add space.
    : "(default: "; // On newline.

  flag.help += flag.boolean
    ? (m2 ? "true" : "false")
    : stringify(m2);

  flag.help += ")";

  add(flag);
}


template <typename T, typename M>
void FlagsBase::add(
    Option<M> T::*option,
    const std::string& name,
    const std::string& help)
{
  T* t = dynamic_cast<T*>(this);
  if (t == NULL) {
    std::cerr << "Attempted to add flag '" << name
              << "' with incompatible type" << std::endl;
    abort();
  }

  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(M) == typeid(bool);
  flag.loader = std::tr1::bind(
      &OptionMemberLoader<T, M>::load,
      name,
      option,
      std::tr1::placeholders::_1,
      std::tr1::placeholders::_2);

  add(flag);
}


inline void FlagsBase::add(const Flag& flag)
{
  if (flags.count(flag.name) > 0) {
    std::cerr << "Attempted to add duplicate flag '"
              << flag.name << "'" << std::endl;
    abort();
  }

  flags[flag.name] = flag;
}


inline void FlagsBase::load(const std::map<std::string, Option<std::string> >& values)
{
  std::map<std::string, Option<std::string> >::const_iterator iterator;

  for (iterator = values.begin(); iterator != values.end(); ++iterator) {
    const std::string& name = iterator->first;
    const Option<std::string>& value = iterator->second;

    if (flags.count(name) > 0) {
      if (value.isSome()) {
        flags[name].loader(this, value.get());                // --name=value
      } else if (flags[name].boolean) {
        flags[name].loader(this, "true");                     // --name
      } else {
        std::cerr << "Failed to load non-boolean flag via '"
                  << name << "'" << std::endl;
        abort();
      }
    } else if (name.find("no-") == 0 && flags.count(name.substr(3)) > 0) {
      if (flags[name.substr(3)].boolean) {
        if (value.isNone()) {
          flags[name.substr(3)].loader(this, "false");        // --no-name
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


inline void FlagsBase::load(const std::map<std::string, std::string>& _values)
{
  std::map<std::string, Option<std::string> > values;
  std::map<std::string, std::string>::const_iterator iterator;
  for (iterator = _values.begin(); iterator != _values.end(); ++iterator) {
    const std::string& name = iterator->first;
    const std::string& value = iterator->second;
    values[name] = Option<std::string>::some(value);
  }
  load(values);
}



template <typename Flags1,
          typename Flags2,
          typename Flags3,
          typename Flags4,
          typename Flags5>
template <typename T1, typename T2>
void Flags<Flags1, Flags2, Flags3, Flags4, Flags5>::add(
    T1* t1,
    const std::string& name,
    const std::string& help,
    const T2& t2)
{
  *t1 = t2; // Set the default.

  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(T1) == typeid(bool);
  flag.loader = std::tr1::bind(
      &Loader<T1>::load,
      name,
      t1,
      std::tr1::placeholders::_2); // Use _2 because ignore FlagsBase*.

  // Update the help string to include the default value.
  flag.help += help.size() > 0 && help.find_last_of("\n\r") != help.size() - 1
    ? " (default: " // On same line, add space.
    : "(default: "; // On newline.

  flag.help += flag.boolean
    ? (t2 ? "true" : "false")
    : stringify(t2);

  flag.help += ")";

  FlagsBase::add(flag);
}


template <typename Flags1,
          typename Flags2,
          typename Flags3,
          typename Flags4,
          typename Flags5>
template <typename T>
void Flags<Flags1, Flags2, Flags3, Flags4, Flags5>::add(
    Option<T>* option,
    const std::string& name,
    const std::string& help)
{
  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(T) == typeid(bool);
  flag.loader = std::tr1::bind(
      &OptionLoader<T>::load,
      name,
      option,
      std::tr1::placeholders::_2); // Use _2 because ignore FlagsBase*.

  FlagsBase::add(flag);
}

} // namespace flags {

#endif // __FLAGS_FLAGS_HPP__
