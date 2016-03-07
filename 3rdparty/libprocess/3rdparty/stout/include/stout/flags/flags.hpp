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

#ifndef __STOUT_FLAGS_FLAGS_HPP__
#define __STOUT_FLAGS_FLAGS_HPP__

#include <algorithm>
#include <map>
#include <ostream>
#include <string>
#include <tuple>
#include <type_traits>
#include <typeinfo> // For typeid.
#include <vector>

#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/path.hpp>
#include <stout/some.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>

#include <stout/flags/fetch.hpp>
#include <stout/flags/flag.hpp>

#include <stout/os/environment.hpp>

namespace flags {

class FlagsBase
{
public:
  FlagsBase() { add(&help, "help", "Prints this help message", false); }
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
      const char* const* argv,
      bool unknowns = false,
      bool duplicates = false);

  // Load any flags from the environment as above but remove processed
  // flags from 'argv' and update 'argc' appropriately. For example:
  //
  // argv = ["/path/program", "--arg1", "hi", "--arg2", "--", "bye"]
  //
  // Becomes:
  //
  // argv = ["/path/program", "hi", "bye"]
  virtual Try<Nothing> load(
      const Option<std::string>& prefix,
      int* argc,
      char*** argv,
      bool unknowns = false,
      bool duplicates = false);

  virtual Try<Nothing> load(
      const std::map<std::string, Option<std::string>>& values,
      bool unknowns = false);

  virtual Try<Nothing> load(
      const std::map<std::string, std::string>& values,
      bool unknowns = false);

  // Returns a string describing the flags, preceded by a "usage
  // message" that will be prepended to that description (see
  // 'FlagsBase::usageMessage_').
  //
  // The optional 'message' passed to this function will be prepended
  // to the generated string returned from this function.
  //
  // Derived classes and clients can modify the standard usage message
  // by setting it before calling this method via 'setUsageMessage()'.
  //
  // This allows one to set a generic message that will be used for
  // each invocation of this method, and to additionally add one for
  // the particular invocation:
  //
  //    MyFlags flags;
  //    flags.setUsageMessage("Custom Usage Message");
  //    ...
  //    if (flags.foo.isNone()) {
  //      std::cerr << flags.usage("Missing required --foo flag");
  //    }
  //
  // The 'message' would be emitted first, followed by a blank line,
  // then the 'usageMessage_', finally followed by the flags'
  // description, for example:
  //
  //    Missing required --foo flag
  //
  //    Custom Usage Message
  //
  //      --[no-]help       Prints this help message. (default: false)
  //      --foo=VALUE       Description about 'foo' here.
  //      --bar=VALUE       Description about 'bar' here. (default: 42)
  //
  std::string usage(const Option<std::string>& message = None()) const;

  // Sets the default message that is prepended to the flags'
  // description in 'usage()'.
  void setUsageMessage(const std::string& message)
  {
    usageMessage_ = Some(message);
  }

  typedef std::map<std::string, Flag>::const_iterator const_iterator;

  const_iterator begin() const { return flags_.begin(); }
  const_iterator end() const { return flags_.end(); }

  typedef std::map<std::string, Flag>::iterator iterator;

  iterator begin() { return flags_.begin(); }
  iterator end() { return flags_.end(); }

  template <typename T1, typename T2, typename F>
  void add(
      T1* t1,
      const std::string& name,
      const std::string& help,
      const T2& t2,
      F validate);

  template <typename T1, typename T2>
  void add(
      T1* t1,
      const std::string& name,
      const std::string& help,
      const T2& t2)
  {
    add(t1, name, help, t2, [](const T1&) { return None(); });
  }

  template <typename T, typename F>
  void add(
      Option<T>* option,
      const std::string& name,
      const std::string& help,
      F validate);

  template <typename T>
  void add(
      Option<T>* option,
      const std::string& name,
      const std::string& help)
  {
    add(option, name, help, [](const Option<T>&) { return None(); });
  }

protected:
  template <typename Flags, typename T1, typename T2, typename F>
  void add(
      T1 Flags::*t1,
      const std::string& name,
      const std::string& help,
      const T2& t2,
      F validate);

  template <typename Flags, typename T1, typename T2>
  void add(
      T1 Flags::*t1,
      const std::string& name,
      const std::string& help,
      const T2& t2)
  {
    add(t1, name, help, t2, [](const T1&) { return None(); });
  }

  template <typename Flags, typename T, typename F>
  void add(
      Option<T> Flags::*option,
      const std::string& name,
      const std::string& help,
      F validate);

  template <typename Flags, typename T>
  void add(
      Option<T> Flags::*option,
      const std::string& name,
      const std::string& help)
  {
    add(option, name, help, [](const Option<T>&) { return None(); });
  }

  void add(const Flag& flag);

public:
  // TODO(marco): IMO the entire --help functionality should be
  // encapsulated inside the FlagsBase class.
  // For now, exposing this for the caller(s) to decide what to
  // do when the user asks for help.
  bool help;

protected:
  // The program's name, extracted from argv[0] by default;
  // declared 'protected' so that derived classes can alter this
  // behavior.
  std::string programName_;

  // An optional custom usage message, will be printed 'as is'
  // just above the list of flags and their explanation.
  // It is 'None' by default, in which case the default
  // behavior is to print "Usage:" followed by the 'programName_'.
  Option<std::string> usageMessage_;

private:
  // Extract environment variable "flags" with the specified prefix.
  std::map<std::string, Option<std::string>> extract(
      const std::string& prefix);

  std::map<std::string, Flag> flags_;
};


template <typename... FlagsTypes>
class Flags : public virtual FlagsTypes...
{
  // Construct tuple types of sizeof...(FlagsTypes) compile-time bools to check
  // non-recursively that all FlagsTypes derive from FlagsBase; as a helper we
  // use is_object<FlagTypes> to construct sizeof...(FlagTypes) true types for
  // the RHS (is_object<T> is a true type for anything one would inherit from).
  static_assert(
    std::is_same<
      std::tuple<typename std::is_base_of<FlagsBase, FlagsTypes>::type...>,
      std::tuple<typename std::is_object<FlagsTypes>::type...>>::value,
    "Can only instantiate Flags with FlagsBase types.");
};

template <>
class Flags<> : public virtual FlagsBase {};


template <typename T1, typename T2, typename F>
void FlagsBase::add(
    T1* t1,
    const std::string& name,
    const std::string& help,
    const T2& t2,
    F validate)
{
  // Don't bother adding anything if the pointer is NULL.
  if (t1 == NULL) {
    return;
  }

  *t1 = t2; // Set the default.

  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(T1) == typeid(bool);

  // NOTE: We need to take FlagsBase* (or const FlagsBase&) as the
  // first argument to match the function signature of the 'load',
  // 'stringify', and 'validate' lambdas used in other overloads of
  // FlagsBase::add. Since we don't need to use the pointer here we
  // don't name it as a parameter.

  flag.load = [t1](FlagsBase*, const std::string& value) -> Try<Nothing> {
    // NOTE: 'fetch' "retrieves" the value if necessary and then
    // invokes 'parse'. See 'fetch' for more details.
    Try<T1> t = fetch<T1>(value);
    if (t.isSome()) {
      *t1 = t.get();
    } else {
      return Error("Failed to load value '" + value + "': " + t.error());
    }
    return Nothing();
  };

  flag.stringify = [t1](const FlagsBase&) -> Option<std::string> {
    return stringify(*t1);
  };

  flag.validate = [t1, validate](const FlagsBase&) -> Option<Error> {
    return validate(*t1);
  };

  // Update the help string to include the default value.
  flag.help += help.size() > 0 && help.find_last_of("\n\r") != help.size() - 1
    ? " (default: " // On same line, add space.
    : "(default: "; // On newline.
  flag.help += stringify(t2);
  flag.help += ")";

  add(flag);
}


template <typename T, typename F>
void FlagsBase::add(
    Option<T>* option,
    const std::string& name,
    const std::string& help,
    F validate)
{
  // Don't bother adding anything if the pointer is NULL.
  if (option == NULL) {
    return;
  }

  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(T) == typeid(bool);

  // NOTE: See comment above in T* overload of FlagsBase::add for why
  // we need to take the FlagsBase* parameter.

  flag.load = [option](FlagsBase*, const std::string& value) -> Try<Nothing> {
    // NOTE: 'fetch' "retrieves" the value if necessary and then
    // invokes 'parse'. See 'fetch' for more details.
    Try<T> t = fetch<T>(value);
    if (t.isSome()) {
      *option = Some(t.get());
    } else {
      return Error("Failed to load value '" + value + "': " + t.error());
    }
    return Nothing();
  };

  flag.stringify = [option](const FlagsBase&) -> Option<std::string> {
    if (option->isSome()) {
      return stringify(option->get());
    }
    return None();
  };

  flag.validate = [option, validate](const FlagsBase&) -> Option<Error> {
    return validate(*option);
  };

  add(flag);
}


template <typename Flags, typename T1, typename T2, typename F>
void FlagsBase::add(
    T1 Flags::*t1,
    const std::string& name,
    const std::string& help,
    const T2& t2,
    F validate)
{
  // Don't bother adding anything if the pointer is NULL.
  if (t1 == NULL) {
    return;
  }

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

  // NOTE: We need to take FlagsBase* (or const FlagsBase&) as the
  // first argument to 'load', 'stringify', and 'validate' so that we
  // use the correct instance of FlagsBase. In other words, we can't
  // capture 'this' here because it's possible that the FlagsBase
  // object that we're working with when we invoke FlagsBase::add is
  // not the same instance as 'this' when the these lambdas get
  // invoked.

  flag.load = [t1](FlagsBase* base, const std::string& value) -> Try<Nothing> {
    Flags* flags = dynamic_cast<Flags*>(base);
    if (base != NULL) {
      // NOTE: 'fetch' "retrieves" the value if necessary and then
      // invokes 'parse'. See 'fetch' for more details.
      Try<T1> t = fetch<T1>(value);
      if (t.isSome()) {
        flags->*t1 = t.get();
      } else {
        return Error("Failed to load value '" + value + "': " + t.error());
      }
    }
    return Nothing();
  };

  flag.stringify = [t1](const FlagsBase& base) -> Option<std::string> {
    const Flags* flags = dynamic_cast<const Flags*>(&base);
    if (flags != NULL) {
      return stringify(flags->*t1);
    }
    return None();
  };

  flag.validate = [t1, validate](const FlagsBase& base) -> Option<Error> {
    const Flags* flags = dynamic_cast<const Flags*>(&base);
    if (flags != NULL) {
      return validate(flags->*t1);
    }
    return None();
  };

  // Update the help string to include the default value.
  flag.help += help.size() > 0 && help.find_last_of("\n\r") != help.size() - 1
    ? " (default: " // On same line, add space.
    : "(default: "; // On newline.
  flag.help += stringify(t2);
  flag.help += ")";

  add(flag);
}


template <typename Flags, typename T, typename F>
void FlagsBase::add(
    Option<T> Flags::*option,
    const std::string& name,
    const std::string& help,
    F validate)
{
  // Don't bother adding anything if the pointer is NULL.
  if (option == NULL) {
    return;
  }

  Flags* flags = dynamic_cast<Flags*>(this);
  if (flags == NULL) {
    ABORT("Attempted to add flag '" + name + "' with incompatible type");
  }

  Flag flag;
  flag.name = name;
  flag.help = help;
  flag.boolean = typeid(T) == typeid(bool);

  // NOTE: See comment above in Flags::T* overload of FLagsBase::add
  // for why we need to pass FlagsBase* (or const FlagsBase&) as a
  // parameter.

  flag.load =
    [option](FlagsBase* base, const std::string& value) -> Try<Nothing> {
      Flags* flags = dynamic_cast<Flags*>(base);
      if (flags != NULL) {
        // NOTE: 'fetch' "retrieves" the value if necessary and then
        // invokes 'parse'. See 'fetch' for more details.
        Try<T> t = fetch<T>(value);
        if (t.isSome()) {
          flags->*option = Some(t.get());
        } else {
          return Error("Failed to load value '" + value + "': " + t.error());
        }
      }
      return Nothing();
    };

  flag.stringify = [option](const FlagsBase& base) -> Option<std::string> {
    const Flags* flags = dynamic_cast<const Flags*>(&base);
    if (flags != NULL) {
      if ((flags->*option).isSome()) {
        return stringify((flags->*option).get());
      }
    }
    return None();
  };

  flag.validate = [option, validate](const FlagsBase& base) -> Option<Error> {
    const Flags* flags = dynamic_cast<const Flags*>(&base);
    if (flags != NULL) {
      return validate(flags->*option);
    }
    return None();
  };

  add(flag);
}


inline void FlagsBase::add(const Flag& flag)
{
  if (flags_.count(flag.name) > 0) {
    EXIT(EXIT_FAILURE)
      << "Attempted to add duplicate flag '" << flag.name << "'";
  } else if (flag.name.find("no-") == 0) {
    EXIT(EXIT_FAILURE)
      << "Attempted to add flag '" << flag.name
      << "' that starts with the reserved 'no-' prefix";
  }

  flags_[flag.name] = flag;
}


// Extract environment variable "flags" with the specified prefix.
inline std::map<std::string, Option<std::string>> FlagsBase::extract(
    const std::string& prefix)
{
  std::map<std::string, Option<std::string>> values;

  foreachpair (const std::string& key,
               const std::string& value,
               os::environment()) {
    if (key.find(prefix) == 0) {
      std::string name = key.substr(prefix.size());
      name = strings::lower(name); // Allow PREFIX_NAME or PREFIX_name.

      // Only add if it's a known flag.
      if (flags_.count(name) > 0 ||
          (name.find("no-") == 0 && flags_.count(name.substr(3)) > 0)) {
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
    const char* const *argv,
    bool unknowns,
    bool duplicates)
{
  std::map<std::string, Option<std::string>> envValues;
  std::map<std::string, Option<std::string>> cmdValues;

  // Grab the program name from argv[0].
  programName_ = argc > 0 ? Path(argv[0]).basename() : "";

  if (prefix.isSome()) {
    envValues = extract(prefix.get());
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
      if (cmdValues.count(name) > 0 ||
          (name.find("no-") == 0 && cmdValues.count(name.substr(3)) > 0)) {
        return Error("Duplicate flag '" + name + "' on command line");
      }
    }

    cmdValues[name] = value;
  }

  cmdValues.insert(envValues.begin(), envValues.end());

  return load(cmdValues, unknowns);
}


inline Try<Nothing> FlagsBase::load(
    const Option<std::string>& prefix,
    int* argc,
    char*** argv,
    bool unknowns,
    bool duplicates)
{
  std::map<std::string, Option<std::string>> envValues;
  std::map<std::string, Option<std::string>> cmdValues;

  if (prefix.isSome()) {
    envValues = extract(prefix.get());
  }

  // Grab the program name from argv, without removing it.
  programName_ = *argc > 0 ? Path(*(argv[0])).basename() : "";

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
      if (cmdValues.count(name) > 0 ||
          (name.find("no-") == 0 && cmdValues.count(name.substr(3)) > 0)) {
        return Error("Duplicate flag '" + name + "' on command line");
      }
    }

    cmdValues[name] = value;
  }

  cmdValues.insert(envValues.begin(), envValues.end());

  Try<Nothing> result = load(cmdValues, unknowns);

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
    const std::map<std::string, Option<std::string>>& values,
    bool unknowns)
{
  std::map<std::string, Option<std::string>>::const_iterator iterator;

  for (iterator = values.begin(); iterator != values.end(); ++iterator) {
    const std::string& name = iterator->first;
    const Option<std::string>& value = iterator->second;

    if (flags_.count(name) > 0) {
      if (value.isSome()) {                        // --name=value
        if (flags_[name].boolean && value.get() == "") {
          flags_[name].load(this, "true"); // Should never fail.
        } else {
          Try<Nothing> load = flags_[name].load(this, value.get());
          if (load.isError()) {
            return Error(
                "Failed to load flag '" + name + "': " + load.error());
          }
        }
      } else {                                     // --name
        if (flags_[name].boolean) {
          flags_[name].load(this, "true"); // Should never fail.
        } else {
          return Error(
              "Failed to load non-boolean flag '" + name + "': Missing value");
        }
      }
    } else if (name.find("no-") == 0) {
      if (flags_.count(name.substr(3)) > 0) {       // --no-name
        if (flags_[name.substr(3)].boolean) {
          if (value.isNone() || value.get() == "") {
            flags_[name.substr(3)].load(this, "false"); // Should never fail.
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

  // Validate the flags value.
  //
  // TODO(benh): Consider validating all flags at the same time in
  // order to provide more feedback rather than requiring a user to
  // fix one at a time.
  foreachvalue (const Flag& flag, flags_) {
    Option<Error> error = flag.validate(*this);
    if (error.isSome()) {
      return error.get();
    }
  }

  return Nothing();
}


inline Try<Nothing> FlagsBase::load(
    const std::map<std::string, std::string>& _values,
    bool unknowns)
{
  std::map<std::string, Option<std::string>> values;
  std::map<std::string, std::string>::const_iterator iterator;
  for (iterator = _values.begin(); iterator != _values.end(); ++iterator) {
    const std::string& name = iterator->first;
    const std::string& value = iterator->second;
    values[name] = Some(value);
  }
  return load(values, unknowns);
}


inline std::string FlagsBase::usage( const Option<std::string>& message) const
{
  const int PAD = 5;

  std::string usage;

  if (message.isSome()) {
    usage = message.get() + "\n\n";
  }

  if (usageMessage_.isNone()) {
    usage += "Usage: " + programName_ + " [options]\n\n";
  } else {
    usage += usageMessage_.get() + "\n\n";
  }

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


inline std::ostream& operator<<(std::ostream& stream, const FlagsBase& flags)
{
  std::vector<std::string> _flags;

  foreachvalue (const flags::Flag& flag, flags) {
    const Option<std::string> value = flag.stringify(flags);
    if (value.isSome()) {
      _flags.push_back("--" + flag.name + "=\"" + value.get() + '"');
    }
  }

  return stream << strings::join(" ", _flags);
}

} // namespace flags {

#endif // __STOUT_FLAGS_FLAGS_HPP__
