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
#include <typeinfo> // For typeid.
#include <vector>

#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/foreach.hpp>
#include <stout/lambda.hpp>
#include <stout/multimap.hpp>
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
  FlagsBase()
  {
    add(&FlagsBase::help, "help", "Prints this help message", false);
  }

  virtual ~FlagsBase() = default;

  // Explicitly disable rvalue constructors and assignment operators
  // since we plan for this class to be used in virtual inheritance
  // scenarios. Here e.g., constructing from an rvalue will be
  // problematic since we can potentially have multiple lineages
  // leading to the same base class, and could then potentially use a
  // moved from base object.
  // All of the following functions would be implicitly generated for
  // C++14, but in C++11 only the versions taking lvalue references
  // should be. GCC seems to create all of these even in C++11 mode so
  // we need to explicitly disable them.
  FlagsBase(const FlagsBase&) = default;
  FlagsBase(FlagsBase&&) = delete;
  FlagsBase& operator=(const FlagsBase&) = default;
  FlagsBase& operator=(FlagsBase&&) = delete;

  // Load any flags from the environment given the variable prefix,
  // i.e., given prefix 'STOUT_' will load a flag named 'foo' via
  // environment variables 'STOUT_foo' or 'STOUT_FOO'.
  virtual Try<Warnings> load(const std::string& prefix);

  // Load any flags from the environment given the variable prefix
  // (see above) followed by loading from the command line (via 'argc'
  // and 'argv'). If 'unknowns' is true then we'll ignore unknown
  // flags we see while loading. If 'duplicates' is true then we'll
  // ignore any duplicates we see while loading. Note that if a flag
  // exists in the environment and the command line, the latter takes
  // precedence.
  virtual Try<Warnings> load(
      const Option<std::string>& prefix,
      int argc,
      const char* const* argv,
      bool unknowns = false,
      bool duplicates = false);

  // Loads any flags from the environment as above but remove processed
  // flags from 'argv' and update 'argc' appropriately. For example:
  //
  // argv = ["/path/program", "--arg1", "hi", "--arg2", "--", "bye"]
  //
  // Becomes:
  //
  // argv = ["/path/program", "hi", "bye"]
  virtual Try<Warnings> load(
      const Option<std::string>& prefix,
      int* argc,
      char*** argv,
      bool unknowns = false,
      bool duplicates = false);

  // Loads flags from the given map where the keys represent the flag
  // name and the values represent the flag values. For example:
  //
  // values = { 'arg1': 'hi',
  //            'arg2': 'bye' }
  //
  // Optionally, if `prefix` is specified, flags will also be loaded
  // from environment variables with the given prefix.
  // Note that if a flag exists in both the environment and the values map,
  // the latter takes precedence.
  virtual Try<Warnings> load(
      const std::map<std::string, Option<std::string>>& values,
      bool unknowns = false,
      const Option<std::string>& prefix = None());

  // Loads flags from the map and optionally the environment vars
  // if a prefix is specified. Follows the behavior of the
  // method `load(values, unknowns, prefix)` above in terms
  // of precedence and load order.
  virtual Try<Warnings> load(
      const std::map<std::string, std::string>& values,
      bool unknowns = false,
      const Option<std::string>& prefix = None());

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

  // In the overloaded function signatures for `add` found below, we use a `T2*`
  // for the default flag value where applicable. This is used instead of an
  // `Option<T2>&` because when string literals are passed to this parameter,
  // `Option` infers a character array type, which causes problems in the
  // current implementation of `Option`. See MESOS-5471.

  template <typename Flags, typename T1, typename T2, typename F>
  void add(
      T1 Flags::*t1,
      const Name& name,
      const Option<Name>& alias,
      const std::string& help,
      const T2* t2,
      F validate);

  template <typename Flags, typename T1, typename T2, typename F>
  void add(
      T1 Flags::*t1,
      const Name& name,
      const Option<Name>& alias,
      const std::string& help,
      const T2& t2,
      F validate)
  {
    add(t1, name, alias, help, &t2, validate);
  }

  template <typename Flags, typename T1, typename T2, typename F>
  void add(
      T1 Flags::*t1,
      const Name& name,
      const std::string& help,
      const T2& t2,
      F validate)
  {
    add(t1, name, None(), help, &t2, validate);
  }

  template <typename Flags, typename T1, typename T2>
  void add(
      T1 Flags::*t1,
      const Name& name,
      const std::string& help,
      const T2& t2)
  {
    add(t1, name, None(), help, &t2, [](const T1&) { return None(); });
  }

  template <typename Flags, typename T>
  void add(
      T Flags::*t,
      const Name& name,
      const std::string& help)
  {
    add(t,
        name,
        None(),
        help,
        static_cast<const T*>(nullptr),
        [](const T&) { return None(); });
  }

  template <typename Flags, typename T1, typename T2>
  void add(
      T1 Flags::*t1,
      const Name& name,
      const Option<Name>& alias,
      const std::string& help,
      const T2& t2)
  {
    add(t1, name, alias, help, &t2, [](const T1&) { return None(); });
  }

  template <typename Flags, typename T, typename F>
  void add(
      Option<T> Flags::*option,
      const Name& name,
      const Option<Name>& alias,
      const std::string& help,
      F validate);

  template <typename Flags, typename T, typename F>
  void add(
      Option<T> Flags::*option,
      const Name& name,
      const std::string& help,
      F validate)
  {
    add(option, name, None(), help, validate);
  }

  template <typename Flags, typename T>
  void add(
      Option<T> Flags::*option,
      const Name& name,
      const std::string& help)
  {
    add(option, name, None(), help, [](const Option<T>&) { return None(); });
  }

  template <typename Flags, typename T>
  void add(
      Option<T> Flags::*option,
      const Name& name,
      const Option<Name>& alias,
      const std::string& help)
  {
    add(option, name, alias, help, [](const Option<T>&) { return None(); });
  }

  void add(const Flag& flag);

  // TODO(marco): IMO the entire --help functionality should be
  // encapsulated inside the FlagsBase class.
  // For now, exposing this for the caller(s) to decide what to
  // do when the user asks for help.
  bool help;

  // Extract environment variable "flags" with the specified prefix.
  std::map<std::string, Option<std::string>> extract(
      const std::string& prefix) const;

  // Build environment variables from the flags.
  std::map<std::string, std::string> buildEnvironment(
      const Option<std::string>& prefix = None()) const;

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
  Try<Warnings> load(
      Multimap<std::string, Option<std::string>>& values,
      bool unknowns = false,
      bool duplicates = false,
      const Option<std::string>& prefix = None());

  // Maps flag's name to flag.
  std::map<std::string, Flag> flags_;

  // Maps flag's alias to flag's name.
  std::map<std::string, std::string> aliases;
};


template <typename Flags, typename T1, typename T2, typename F>
void FlagsBase::add(
    T1 Flags::*t1,
    const Name& name,
    const Option<Name>& alias,
    const std::string& help,
    const T2* t2,
    F validate)
{
  // Don't bother adding anything if the pointer is `nullptr`.
  if (t1 == nullptr) {
    return;
  }

  Flags* flags = dynamic_cast<Flags*>(this);
  if (flags == nullptr) {
    ABORT("Attempted to add flag '" + name.value +
          "' with incompatible type");
  }

  Flag flag;
  flag.name = name;
  flag.alias = alias;
  flag.help = help;
  flag.boolean = typeid(T1) == typeid(bool);

  if (t2 != nullptr) {
    flags->*t1 = *t2; // Set the default.
    flag.required = false;
  } else {
    flag.required = true;
  }

  // NOTE: We need to take FlagsBase* (or const FlagsBase&) as the
  // first argument to 'load', 'stringify', and 'validate' so that we
  // use the correct instance of FlagsBase. In other words, we can't
  // capture 'this' here because it's possible that the FlagsBase
  // object that we're working with when we invoke FlagsBase::add is
  // not the same instance as 'this' when these lambdas get invoked.

  flag.load = [t1](FlagsBase* base, const std::string& value) -> Try<Nothing> {
    Flags* flags = dynamic_cast<Flags*>(base);
    if (flags != nullptr) {
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
    if (flags != nullptr) {
      return stringify(flags->*t1);
    }
    return None();
  };

  flag.validate = [t1, validate](const FlagsBase& base) -> Option<Error> {
    const Flags* flags = dynamic_cast<const Flags*>(&base);
    if (flags != nullptr) {
      return validate(flags->*t1);
    }
    return None();
  };

  // Update the help string to include the default value.
  flag.help += help.size() > 0 && help.find_last_of("\n\r") != help.size() - 1
    ? " (default: " // On same line, add space.
    : "(default: "; // On newline.
  if (t2 != nullptr) {
    flag.help += stringify(*t2);
  }
  flag.help += ")";

  add(flag);
}


template <typename Flags, typename T, typename F>
void FlagsBase::add(
    Option<T> Flags::*option,
    const Name& name,
    const Option<Name>& alias,
    const std::string& help,
    F validate)
{
  // Don't bother adding anything if the pointer is `nullptr`.
  if (option == nullptr) {
    return;
  }

  Flags* flags = dynamic_cast<Flags*>(this);
  if (flags == nullptr) {
    ABORT("Attempted to add flag '" + name.value +
          "' with incompatible type");
  }

  Flag flag;
  flag.name = name;
  flag.alias = alias;
  flag.help = help;
  flag.boolean = typeid(T) == typeid(bool);
  flag.required = false;

  // NOTE: See comment above in Flags::T* overload of FLagsBase::add
  // for why we need to pass FlagsBase* (or const FlagsBase&) as a
  // parameter.

  flag.load =
    [option](FlagsBase* base, const std::string& value) -> Try<Nothing> {
    Flags* flags = dynamic_cast<Flags*>(base);
    if (flags != nullptr) {
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
    if (flags != nullptr) {
      if ((flags->*option).isSome()) {
        return stringify((flags->*option).get());
      }
    }
    return None();
  };

  flag.validate = [option, validate](const FlagsBase& base) -> Option<Error> {
    const Flags* flags = dynamic_cast<const Flags*>(&base);
    if (flags != nullptr) {
      return validate(flags->*option);
    }
    return None();
  };

  add(flag);
}


inline void FlagsBase::add(const Flag& flag)
{
  // Check if the name and alias of the flag are valid.
  std::vector<Name> names = {flag.name};
  if (flag.alias.isSome()) {
    if (flag.alias.get() == flag.name) {
      EXIT(EXIT_FAILURE)
        << "Attempted to add flag '" << flag.name.value << "' with an alias"
        << " that is same as the flag name";
    }

    names.push_back(flag.alias.get());
  }

  foreach (const Name& name, names) {
    if (flags_.count(name.value) > 0) {
      EXIT(EXIT_FAILURE)
        << "Attempted to add duplicate flag '" << name.value << "'";
    } else if (name.value.find("no-") == 0) {
      EXIT(EXIT_FAILURE)
        << "Attempted to add flag '" << name.value
        << "' that starts with the reserved 'no-' prefix";
    }
  }

  flags_[flag.name.value] = flag;
  if (flag.alias.isSome()) {
    aliases[flag.alias.get().value] = flag.name.value;
  }
}


// Extract environment variable "flags" with the specified prefix.
inline std::map<std::string, Option<std::string>> FlagsBase::extract(
    const std::string& prefix) const
{
  std::map<std::string, Option<std::string>> values;

  foreachpair (const std::string& key,
               const std::string& value,
               os::environment()) {
    if (key.find(prefix) == 0) {
      std::string name = key.substr(prefix.size());
      name = strings::lower(name); // Allow PREFIX_NAME or PREFIX_name.

      // Only add if it's a known flag.
      // TODO(vinod): Reject flags with an unknown name if `unknowns` is false.
      // This will break backwards compatibility however!
      std::string flag_name = strings::remove(name, "no-", strings::PREFIX);
      if (flags_.count(flag_name) > 0 || aliases.count(flag_name) > 0) {
        values[name] = Some(value);
      }
    }
  }

  return values;
}


inline std::map<std::string, std::string> FlagsBase::buildEnvironment(
    const Option<std::string>& prefix) const
{
  std::map<std::string, std::string> result;

  foreachvalue (const Flag& flag, flags_) {
    Option<std::string> value = flag.stringify(*this);
    if (value.isSome()) {
      const std::string key = prefix.isSome()
        ? prefix.get() + strings::upper(flag.effective_name().value)
        : strings::upper(flag.effective_name().value);

      result[key] = value.get();
    }
  }

  return result;
}


inline Try<Warnings> FlagsBase::load(const std::string& prefix)
{
  return load(extract(prefix));
}


inline Try<Warnings> FlagsBase::load(
    const Option<std::string>& prefix,
    int argc,
    const char* const *argv,
    bool unknowns,
    bool duplicates)
{
  Multimap<std::string, Option<std::string>> values;

  // Grab the program name from argv[0].
  programName_ = argc > 0 ? Path(argv[0]).basename() : "";

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

    size_t eq = arg.find_first_of('=');
    if (eq == std::string::npos && arg.find("--no-") == 0) { // --no-name
      name = arg.substr(2);
    } else if (eq == std::string::npos) {                    // --name
      name = arg.substr(2);
    } else {                                                 // --name=value
      name = arg.substr(2, eq - 2);
      value = arg.substr(eq + 1);
    }

    name = strings::lower(name);

    values.put(name, value);
  }

  return load(values, unknowns, duplicates, prefix);
}


inline Try<Warnings> FlagsBase::load(
    const Option<std::string>& prefix,
    int* argc,
    char*** argv,
    bool unknowns,
    bool duplicates)
{
  Multimap<std::string, Option<std::string>> values;

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

    size_t eq = arg.find_first_of('=');
    if (eq == std::string::npos && arg.find("--no-") == 0) { // --no-name
      name = arg.substr(2);
    } else if (eq == std::string::npos) {                    // --name
      name = arg.substr(2);
    } else {                                                 // --name=value
      name = arg.substr(2, eq - 2);
      value = arg.substr(eq + 1);
    }

    name = strings::lower(name);

    values.put(name, value);
  }

  Try<Warnings> result = load(values, unknowns, duplicates, prefix);

  // Update 'argc' and 'argv' if we successfully loaded the flags.
  if (!result.isError()) {
    CHECK_LE(args.size(), (size_t) *argc);
    int i = 1; // Start at '1' to skip argv[0].
    foreach (char* arg, args) {
      (*argv)[i++] = arg;
    }

    *argc = i;

    // Now null terminate the array. Note that we'll "leak" the
    // arguments that were processed here but it's not like they would
    // have gotten deleted in normal operations anyway.
    (*argv)[i++] = nullptr;
  }

  return result;
}


inline Try<Warnings> FlagsBase::load(
    const std::map<std::string, Option<std::string>>& values,
    bool unknowns,
    const Option<std::string>& prefix)
{
  Multimap<std::string, Option<std::string>> values_;
  foreachpair (const std::string& name,
               const Option<std::string>& value,
               values) {
    values_.put(name, value);
  }
  return load(values_, unknowns, false, prefix);
}


inline Try<Warnings> FlagsBase::load(
    const std::map<std::string, std::string>& values,
    bool unknowns,
    const Option<std::string>& prefix)
{
  Multimap<std::string, Option<std::string>> values_;
  foreachpair (const std::string& name, const std::string& value, values) {
    values_.put(name, Some(value));
  }
  return load(values_, unknowns, false, prefix);
}


inline Try<Warnings> FlagsBase::load(
    Multimap<std::string, Option<std::string>>& values,
    bool unknowns,
    bool duplicates,
    const Option<std::string>& prefix)
{
  Warnings warnings;

  if (prefix.isSome()) {
    // Merge in flags from the environment. Values in the
    // map take precedence over environment flags.
    //
    // Other overloads parse command line flags into
    // the values map and pass them into this method.
    foreachpair (const std::string& name,
                 const Option<std::string>& value,
                 extract(prefix.get())) {
      if (!values.contains(name)) {
        values.put(name, value);
      }
    }
  }

  foreachpair (const std::string& name,
               const Option<std::string>& value,
               values) {
    bool is_negated = strings::startsWith(name, "no-");
    std::string flag_name = !is_negated ? name : name.substr(3);

    auto iter = aliases.count(flag_name)
      ? flags_.find(aliases[flag_name])
      : flags_.find(flag_name);

    if (iter == flags_.end()) {
      if (!unknowns) {
        return Error("Failed to load unknown flag '" + flag_name + "'" +
                     (!is_negated ? "" : " via '" + name + "'"));
      } else {
        continue;
      }
    }

    Flag* flag = &(iter->second);

    if (!duplicates && flag->loaded_name.isSome()) {
      return Error("Flag '" + flag_name + "' is already loaded via name '" +
                   flag->loaded_name->value + "'");
    }

    // Validate the flag value.
    std::string value_;
    if (!flag->boolean) {  // Non-boolean flag.
      if (is_negated) { // Non-boolean flag cannot be loaded with "no-" prefix.
        return Error("Failed to load non-boolean flag '" + flag_name +
                     "' via '" + name + "'");
      }

      if (value.isNone()) {
        return Error("Failed to load non-boolean flag '" + flag_name +
                     "': Missing value");
      }

      value_ = value.get();
    } else {  // Boolean flag.
      if (value.isNone() || value.get() == "") {
        value_ = !is_negated ? "true" : "false";
      } else if (!is_negated) {
        value_ = value.get();
      } else { // Boolean flag with "no-" prefix cannot have non-empty value.
        return Error(
            "Failed to load boolean flag '" + flag_name + "' via '" + name +
            "' with value '" + value.get() + "'");
      }
    }

    Try<Nothing> load = flag->load(this, value_);
    if (load.isError()) {
      return Error("Failed to load flag '" + flag_name + "': " + load.error());
    }

    // TODO(vinod): Move this logic inside `Flag::load()`.

    // Set `loaded_name` to the Name corresponding to `flag_name`.
    if (aliases.count(flag_name)) {
      CHECK_SOME(flag->alias);
      flag->loaded_name = flag->alias.get();
    } else {
      flag->loaded_name = flag->name;
    }

    if (flag->loaded_name->deprecated) {
      warnings.warnings.push_back(
          Warning("Loaded deprecated flag '" + flag_name + "'"));
    }
  }

  // Validate the flags value.
  //
  // TODO(benh): Consider validating all flags at the same time in
  // order to provide more feedback rather than requiring a user to
  // fix one at a time.
  foreachvalue (const Flag& flag, flags_) {
    if (flag.required && flag.loaded_name.isNone()) {
        return Error(
            "Flag '" + flag.name.value +
            "' is required, but it was not provided");
    }

    Option<Error> error = flag.validate(*this);
    if (error.isSome()) {
      return error.get();
    }
  }

  return warnings;
}


inline std::string FlagsBase::usage(const Option<std::string>& message) const
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
      col1[flag.name.value] += "  --[no-]" + flag.name.value;
      if (flag.alias.isSome()) {
        col1[flag.name.value] += ", --[no-]" + flag.alias->value;
      }
    } else {
      // TODO(vinod): Rename "=VALUE" to "=<VALUE>".
      col1[flag.name.value] += "  --" + flag.name.value + "=VALUE";
      if (flag.alias.isSome()) {
        col1[flag.name.value] += ", --" + flag.alias->value + "=VALUE";
      }
    }
    width = std::max(width, col1[flag.name.value].size());
  }

  // TODO(vinod): Print the help on the next line instead of on the same line as
  // the names.
  foreachvalue (const flags::Flag& flag, *this) {
    std::string line = col1[flag.name.value];

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
      _flags.push_back("--" + flag.effective_name().value + "=\"" +
                       value.get() + '"');
    }
  }

  return stream << strings::join(" ", _flags);
}

} // namespace flags {

#endif // __STOUT_FLAGS_FLAGS_HPP__
