#ifndef __STOUT_FLAGS_FLAGS_HPP__
#define __STOUT_FLAGS_FLAGS_HPP__

#include <stdlib.h> // For abort.

#include <map>
#include <string>
#include <typeinfo> // For typeid.

#include <tr1/functional>

#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/flags/flag.hpp>
#include <stout/flags/loader.hpp>
#include <stout/flags/parse.hpp>

namespace flags {

class FlagsBase
{
public:
  virtual ~FlagsBase() {}

  // Load any flags from the environment given the variable prefix,
  // i.e., given prefix 'STOUT_' will load a flag named 'foo' via
  // environment variables 'STOUT_foo' or 'STOUT_FOO'.
  virtual Try<Nothing> load(
      const std::string& prefix,
      bool unknowns = false);

  // Load any flags from the environment given the variable prefix
  // (see above) followed by loading from the command line (via 'argc'
  // and 'argv'). If 'unknowns' is true then we'll ignore unknown
  // flags we see while loading. If 'duplicates' is true then we'll
  // ignore any duplicates we see while loading.
  virtual Try<Nothing> load(
      const Option<std::string>& prefix,
      int argc,
      char** argv,
      bool unknowns = false,
      bool duplicates = false);

  Try<Nothing> load(
      const std::string& prefix,
      int argc,
      char** argv,
      bool unknowns = false,
      bool duplicates = false);

  virtual Try<Nothing> load(
      const std::map<std::string, Option<std::string> >& values,
      bool unknowns = false);

  virtual Try<Nothing> load(
      const std::map<std::string, std::string>& values,
      bool unknowns = false);

  // Returns a string describing the flags.
  std::string usage() const;

  typedef std::map<std::string, Flag>::const_iterator const_iterator;

  const_iterator begin() const { return flags.begin(); }
  const_iterator end() const { return flags.end(); }

  template <typename T1, typename T2>
  void add(T1* t1,
           const std::string& name,
           const std::string& help,
           const T2& t2);

  template <typename T>
  void add(Option<T>* option,
           const std::string& name,
           const std::string& help);

protected:
  template <typename Flags, typename T1, typename T2>
  void add(T1 Flags::*t1,
           const std::string& name,
           const std::string& help,
           const T2& t2);

  template <typename Flags, typename T>
  void add(Option<T> Flags::*option,
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
              public virtual Flags5 {};


template <typename T1, typename T2>
void FlagsBase::add(
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
      t1,
      std::tr1::function<Try<T1>(const std::string&)>(
          std::tr1::bind(&parse<T1>, std::tr1::placeholders::_1)),
      name,
      std::tr1::placeholders::_2); // Use _2 because ignore FlagsBase*.

  // Update the help string to include the default value.
  flag.help += help.size() > 0 && help.find_last_of("\n\r") != help.size() - 1
    ? " (default: " // On same line, add space.
    : "(default: "; // On newline.
  flag.help += stringify(t2);
  flag.help += ")";

  FlagsBase::add(flag);
}


template <typename T>
void FlagsBase::add(
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
      option,
      std::tr1::function<Try<T>(const std::string&)>(
          std::tr1::bind(&parse<T>, std::tr1::placeholders::_1)),
      name,
      std::tr1::placeholders::_2); // Use _2 because ignore FlagsBase*.

  FlagsBase::add(flag);
}


template <typename Flags, typename T1, typename T2>
void FlagsBase::add(
    T1 Flags::*t1,
    const std::string& name,
    const std::string& help,
    const T2& t2)
{
  Flags* flags = dynamic_cast<Flags*>(this);
  if (flags == NULL) {
    std::cerr << "Attempted to add flag '" << name
              << "' with incompatible type" << std::endl;
    abort();
  } else {
    flags->*t1 = t2; // Set the default.
  }

  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(T1) == typeid(bool);
  flag.loader = std::tr1::bind(
      &MemberLoader<Flags, T1>::load,
      std::tr1::placeholders::_1,
      t1,
      std::tr1::function<Try<T1>(const std::string&)>(
          std::tr1::bind(&parse<T1>, std::tr1::placeholders::_1)),
      name,
      std::tr1::placeholders::_2);

  // Update the help string to include the default value.
  flag.help += help.size() > 0 && help.find_last_of("\n\r") != help.size() - 1
    ? " (default: " // On same line, add space.
    : "(default: "; // On newline.
  flag.help += stringify(t2);
  flag.help += ")";

  add(flag);
}


template <typename Flags, typename T>
void FlagsBase::add(
    Option<T> Flags::*option,
    const std::string& name,
    const std::string& help)
{
  Flags* flags = dynamic_cast<Flags*>(this);
  if (flags == NULL) {
    std::cerr << "Attempted to add flag '" << name
              << "' with incompatible type" << std::endl;
    abort();
  }

  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(T) == typeid(bool);
  flag.loader = std::tr1::bind(
      &OptionMemberLoader<Flags, T>::load,
      std::tr1::placeholders::_1,
      option,
      std::tr1::function<Try<T>(const std::string&)>(
          std::tr1::bind(&parse<T>, std::tr1::placeholders::_1)),
      name,
      std::tr1::placeholders::_2);

  add(flag);
}


inline void FlagsBase::add(const Flag& flag)
{
  if (flags.count(flag.name) > 0) {
    EXIT(1) << "Attempted to add duplicate flag '" << flag.name << "'";
  } else if (flag.name.find("no-") == 0) {
    EXIT(1) << "Attempted to add flag '" << flag.name
            << "' that starts with the reserved 'no-' prefix";
  }

  flags[flag.name] = flag;
}


// Extract environment variable "flags" with the specified prefix.
inline std::map<std::string, Option<std::string> > extract(
    const std::string& prefix)
{
  char** environ = os::environ();

  std::map<std::string, Option<std::string> > values;

  for (int i = 0; environ[i] != NULL; i++) {
    std::string variable = environ[i];
    if (variable.find(prefix) == 0) {
      size_t eq = variable.find_first_of("=");
      if (eq == std::string::npos) {
        continue; // Not expecting a missing '=', but ignore anyway.
      }
      std::string name = variable.substr(prefix.size(), eq - prefix.size());
      name = strings::lower(name); // Allow PREFIX_NAME or PREFIX_name.
      std::string value = variable.substr(eq + 1);
      values[name] = Option<std::string>::some(value);
    }
  }

  return values;
}


inline Try<Nothing> FlagsBase::load(
    const std::string& prefix,
    bool unknowns)
{
  return load(extract(prefix), unknowns);
}


inline Try<Nothing> FlagsBase::load(
    const Option<std::string>& prefix,
    int argc,
    char** argv,
    bool unknowns,
    bool duplicates)
{
  std::map<std::string, Option<std::string> > values;

  if (prefix.isSome()) {
    values = extract(prefix.get());
  }

  // Read flags from the command line.
  for (int i = 1; i < argc; i++) {
    const std::string arg(argv[i]);

    std::string name;
    Option<std::string> value = None();
    if (arg.find("--") == 0) {
      size_t eq = arg.find_first_of("=");
      if (eq == std::string::npos && arg.find("--no-") == 0) { // --no-name
        name = arg.substr(2);
      } else if (eq == std::string::npos) {                    // --name
        name = arg.substr(2);
      } else {                                                 // --name=value
        name = arg.substr(2, eq - 2);
        value = arg.substr(eq + 1);
      }
    }
    name = strings::lower(name);

    if (!duplicates) {
      if (values.count(name) > 0 ||
          (name.find("no-") == 0 && values.count(name.substr(3)) > 0)) {
        return Error("Duplicate flag '" + name + "' on command line");
      }
    }

    values[name] = value;
  }

  return load(values, unknowns);
}


inline Try<Nothing> FlagsBase::load(
    const std::string& prefix,
    int argc,
    char** argv,
    bool unknowns,
    bool duplicates)
{
  return load(Option<std::string>::some(prefix),
              argc,
              argv,
              unknowns,
              duplicates);
}


inline Try<Nothing> FlagsBase::load(
    const std::map<std::string, Option<std::string> >& values,
    bool unknowns)
{
  std::map<std::string, Option<std::string> >::const_iterator iterator;

  for (iterator = values.begin(); iterator != values.end(); ++iterator) {
    const std::string& name = iterator->first;
    const Option<std::string>& value = iterator->second;

    if (flags.count(name) > 0) {
      if (value.isSome()) {                        // --name=value
        if (flags[name].boolean && value.get() == "") {
          flags[name].loader(this, "true"); // Should never fail.
        } else {
          Try<Nothing> loader = flags[name].loader(this, value.get());
          if (loader.isError()) {
            return Error(
                "Failed to load flag '" + name + "': " + loader.error());
          }
        }
      } else {                                     // --name
        if (flags[name].boolean) {
          flags[name].loader(this, "true"); // Should never fail.
        } else {
          return Error(
              "Failed to load non-boolean flag '" + name + "': Missing value");
        }
      }
    } else if (name.find("no-") == 0) {
      if (flags.count(name.substr(3)) > 0) {       // --no-name
        if (flags[name.substr(3)].boolean) {
          if (value.isNone() || value.get() == "") {
            flags[name.substr(3)].loader(this, "false"); // Should never fail.
          } else {
            return Error(
                "Failed to load boolean flag '" + name.substr(3) +
                "' via '" + name + "' with value '" + value.get() + "'");
          }
        } else {
          return Error(
              "Failed to load non-boolean flag '" + name.substr(3) +
              "' via '" + name + "'");
        }
      } else {
        return Error(
            "Failed to load unknown flag '" + name.substr(3) +
            "' via '" + name + "'");
      }
    } else if (!unknowns) {
      return Error("Failed to load unknown flag '" + name + "'");
    }
  }

  return Nothing();
}


inline Try<Nothing> FlagsBase::load(
    const std::map<std::string, std::string>& _values,
    bool unknowns)
{
  std::map<std::string, Option<std::string> > values;
  std::map<std::string, std::string>::const_iterator iterator;
  for (iterator = _values.begin(); iterator != _values.end(); ++iterator) {
    const std::string& name = iterator->first;
    const std::string& value = iterator->second;
    values[name] = Option<std::string>::some(value);
  }
  return load(values, unknowns);
}


inline std::string FlagsBase::usage() const
{
  const int PAD = 5;

  std::string usage;

  std::map<std::string, std::string> col1; // key -> col 1 string

  // Construct string for the first column and store width of column.
  size_t width = 0;

  foreachvalue (const flags::Flag& flag, *this) {
    if (flag.boolean) {
      col1[flag.name] = "  --[no-]" + flag.name;
    } else {
      col1[flag.name] = "  --" + flag.name + "=VALUE";
    }
    width = std::max(width, col1[flag.name].size());
  }

  foreachvalue (const flags::Flag& flag, *this) {
    std::string line = col1[flag.name];

    std::string pad(PAD + width - line.size(), ' ');
    line += pad;

    size_t pos1 = 0, pos2 = 0;
    pos2 = flag.help.find_first_of("\n\r", pos1);
    line += flag.help.substr(pos1, pos2 - pos1) + "\n";
    usage += line;

    while (pos2 != std::string::npos) {  // Handle multi-line help strings.
      line = "";
      pos1 = pos2 + 1;
      std::string pad2(PAD + width, ' ');
      line += pad2;
      pos2 = flag.help.find_first_of("\n\r", pos1);
      line += flag.help.substr(pos1, pos2 - pos1) + "\n";
      usage += line;
    }
  }
  return usage;
}

} // namespace flags {

#endif // __STOUT_FLAGS_FLAGS_HPP__
