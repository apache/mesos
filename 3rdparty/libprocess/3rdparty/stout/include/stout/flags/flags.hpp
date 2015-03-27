/**
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *  http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#ifndef __STOUT_FLAGS_FLAGS_HPP__
#define __STOUT_FLAGS_FLAGS_HPP__

#include <map>
#include <ostream>
#include <string>
#include <typeinfo> // For typeid.

#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/os.hpp>
#include <stout/some.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/flags/fetch.hpp>
#include <stout/flags/flag.hpp>
#include <stout/flags/loader.hpp>
#include <stout/flags/stringifier.hpp>

namespace flags {

class FlagsBase
{
public:
  virtual ~FlagsBase() {}

  // Load any flags from the environment given the variable prefix,
  // i.e., given prefix 'STOUT_' will load a flag named 'foo' via
  // environment variables 'STOUT_foo' or 'STOUT_FOO'.
  virtual Try<Nothing> load(const std::string& prefix);

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

  // Load any flags from the environment as above but remove processed
  // flags from 'argv' and update 'argc' appropriately. For example:
  //
  // argv = ["/path/to/program", "--arg1", "hello", "--arg2", "--", "world"]
  //
  // Becomes:
  //
  // argv = ["/path/to/program", "hello", "world"]
  virtual Try<Nothing> load(
      const Option<std::string>& prefix,
      int* argc,
      char*** argv,
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

  typedef std::map<std::string, Flag>::iterator iterator;

  iterator begin() { return flags.begin(); }
  iterator end() { return flags.end(); }

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
  // Extract environment variable "flags" with the specified prefix.
  std::map<std::string, Option<std::string> > extract(
      const std::string& prefix);

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
  flag.loader = lambda::bind(
      &Loader<T1>::load,
      t1,
      lambda::function<Try<T1>(const std::string&)>(
          lambda::bind(&fetch<T1>, lambda::_1)),
      name,
      lambda::_2); // Use _2 because ignore FlagsBase*.
  flag.stringify = lambda::bind(&Stringifier<T1>, t1);

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
  flag.loader = lambda::bind(
      &OptionLoader<T>::load,
      option,
      lambda::function<Try<T>(const std::string&)>(
          lambda::bind(&fetch<T>, lambda::_1)),
      name,
      lambda::_2); // Use _2 because ignore FlagsBase*.
  flag.stringify = lambda::bind(&OptionStringifier<T>, option);

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
    ABORT("Attempted to add flag '" + name + "' with incompatible type");
  } else {
    flags->*t1 = t2; // Set the default.
  }

  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(T1) == typeid(bool);
  flag.loader = lambda::bind(
      &MemberLoader<Flags, T1>::load,
      lambda::_1,
      t1,
      lambda::function<Try<T1>(const std::string&)>(
          lambda::bind(&fetch<T1>, lambda::_1)),
      name,
      lambda::_2);
  flag.stringify = lambda::bind(
      &MemberStringifier<Flags, T1>,
      lambda::_1,
      t1);

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
    ABORT("Attempted to add flag '" + name + "' with incompatible type");
  }

  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(T) == typeid(bool);
  flag.loader = lambda::bind(
      &OptionMemberLoader<Flags, T>::load,
      lambda::_1,
      option,
      lambda::function<Try<T>(const std::string&)>(
          lambda::bind(&fetch<T>, lambda::_1)),
      name,
      lambda::_2);
  flag.stringify = lambda::bind(
      &OptionMemberStringifier<Flags, T>,
      lambda::_1,
      option);

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
inline std::map<std::string, Option<std::string> > FlagsBase::extract(
    const std::string& prefix)
{
  std::map<std::string, Option<std::string> > values;

  foreachpair (const std::string& key,
               const std::string& value,
               os::environment()) {
    if (key.find(prefix) == 0) {
      std::string name = key.substr(prefix.size());
      name = strings::lower(name); // Allow PREFIX_NAME or PREFIX_name.

      // Only add if it's a known flag.
      if (flags.count(name) > 0 ||
          (name.find("no-") == 0 && flags.count(name.substr(3)) > 0)) {
        values[name] = Some(value);
      }
    }
  }

  return values;
}


inline Try<Nothing> FlagsBase::load(const std::string& prefix)
{
  return load(extract(prefix));
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
    const std::string arg(strings::trim(argv[i]));

    // Stop parsing flags after '--' is encountered.
    if (arg == "--") {
      break;
    }

    // Skip anything that doesn't look like a flag.
    if (arg.find("--") != 0) {
      continue;
    }

    std::string name;
    Option<std::string> value = None();

    size_t eq = arg.find_first_of("=");
    if (eq == std::string::npos && arg.find("--no-") == 0) { // --no-name
      name = arg.substr(2);
    } else if (eq == std::string::npos) {                    // --name
      name = arg.substr(2);
    } else {                                                 // --name=value
      name = arg.substr(2, eq - 2);
      value = arg.substr(eq + 1);
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
    const Option<std::string>& prefix,
    int* argc,
    char*** argv,
    bool unknowns,
    bool duplicates)
{
  std::map<std::string, Option<std::string> > values;

  if (prefix.isSome()) {
    values = extract(prefix.get());
  }

  // Keep the arguments that are not being processed as flags.
  std::vector<char*> args;

  // Read flags from the command line.
  for (int i = 1; i < *argc; i++) {
    const std::string arg(strings::trim((*argv)[i]));

    // Stop parsing flags after '--' is encountered.
    if (arg == "--") {
      // Save the rest of the arguments.
      for (int j = i + 1; j < *argc; j++) {
        args.push_back((*argv)[j]);
      }
      break;
    }

    // Skip anything that doesn't look like a flag.
    if (arg.find("--") != 0) {
      args.push_back((*argv)[i]);
      continue;
    }

    std::string name;
    Option<std::string> value = None();

    size_t eq = arg.find_first_of("=");
    if (eq == std::string::npos && arg.find("--no-") == 0) { // --no-name
      name = arg.substr(2);
    } else if (eq == std::string::npos) {                    // --name
      name = arg.substr(2);
    } else {                                                 // --name=value
      name = arg.substr(2, eq - 2);
      value = arg.substr(eq + 1);
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

  Try<Nothing> result = load(values, unknowns);

  // Update 'argc' and 'argv' if we successfully loaded the flags.
  if (!result.isError()) {
    CHECK_LE(args.size(), (size_t) *argc);
    size_t i = 1; // Start at '1' to skip argv[0].
    foreach (char* arg, args) {
      (*argv)[i++] = arg;
    }

    *argc = i;

    // Now null terminate the array. Note that we'll "leak" the
    // arguments that were processed here but it's not like they would
    // have gotten deleted in normal operations anyway.
    (*argv)[i++] = NULL;
  }

  return result;
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
    values[name] = Some(value);
  }
  return load(values, unknowns);
}


inline std::string FlagsBase::usage() const
{
  const int PAD = 5;

  std::string usage;

  std::map<std::string, std::string> col1; // key -> col 1 string.

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


inline std::ostream& operator << (std::ostream& stream, const FlagsBase& flags)
{
  std::vector<std::string> _flags;

  foreachvalue (const flags::Flag& flag, flags) {
    const Option<std::string>& value = flag.stringify(flags);
    if (value.isSome()) {
      _flags.push_back("--" + flag.name + "=\"" + value.get() + '"');
    }
  }

  return stream << strings::join(" ", _flags);
}

} // namespace flags {

#endif // __STOUT_FLAGS_FLAGS_HPP__
