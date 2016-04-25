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

#ifndef __STOUT_SUBCOMMAND_HPP__
#define __STOUT_SUBCOMMAND_HPP__

#include <iostream>
#include <sstream>
#include <string>
#include <vector>

#include <stout/flags.hpp>
#include <stout/foreach.hpp>
#include <stout/hashset.hpp>
#include <stout/option.hpp>
#include <stout/preprocessor.hpp>

// Subcommand is an abstraction for creating command binaries that
// encompass many subcommands. For example:
//
//  $ ./runner start --arg1=1 --arg2=2
//  $ ./runner stop --arg3=3 --arg4=4
//
// Here, the 'runner' command contains two subcommand implementations:
// StartCommand and StopCommand. Each subcommand needs to define a
// name, implement an 'execute' function, and provide the address of a
// flags where the command line arguments will be parsed to. To
// simplify creating command binaries that encompass many subcommands,
// we provide a 'dispatch' function which will look at argv[1] to
// decide which subcommand to execute (based on its name) and then
// parse the command line flags for you.
class Subcommand
{
public:
  // This function is supposed to be called by the main function of
  // the command binary. A user needs to register at least one
  // subcommand. Here is a typical example of the main function of the
  // command binary:
  //
  // int main(int argc, char** argv)
  // {
  //   return Subcommand::dispatch(
  //     None(),
  //     argc,
  //     argv,
  //     new Subcommand1(),
  //     new Subcommand2(),
  //     new Subcommand3());
  // }
#define INSERT(z, N, _) subcommands.push_back( c ## N );
#define TEMPLATE(Z, N, DATA)                            \
  static int dispatch(                                  \
      const Option<std::string>& prefix,                \
      int argc,                                         \
      char** argv,                                      \
      ENUM_PARAMS(N, Subcommand* c))                    \
  {                                                     \
    std::vector<Subcommand*> subcommands;               \
    REPEAT_FROM_TO(0, N, INSERT, _)                     \
    return dispatch(prefix, argc, argv, subcommands);   \
  }

  REPEAT_FROM_TO(1, 11, TEMPLATE, _) // Args C1 -> C11.
#undef TEMPLATE
#undef INSERT

  explicit Subcommand(const std::string& _name) : name_(_name) {}
  virtual ~Subcommand() {}

  std::string name() const { return name_; }

protected:
  // Defines the main function of this subcommand. The return value
  // will be used as the exit code.
  // TODO(jieyu): Consider passing in argc and argv as some users
  // might want to access the remaining command line arguments.
  virtual int execute() = 0;

  // Returns the pointer to the flags that will be used for this
  // subcommand. If the user does not provide an override, the default
  // empty flags will be used.
  virtual flags::FlagsBase* getFlags() { return &flags_; }

private:
  // Returns the usage by listing all the registered subcommands.
  static std::string usage(
      const std::string& argv0,
      const std::vector<Subcommand*>& subcommands);

  static int dispatch(
    const Option<std::string>& prefix,
    int argc,
    char** argv,
    const std::vector<Subcommand*>& subcommands);

  // The name of this subcommand.
  std::string name_;

  // The default flags which is empty.
  flags::FlagsBase flags_;
};


inline std::string Subcommand::usage(
    const std::string& argv0,
    const std::vector<Subcommand*>& subcommands)
{
  std::ostringstream stream;

  stream << "Usage: " << argv0 << " <subcommand> [OPTIONS]\n\n"
         << "Available subcommands:\n"
         << "    help\n";

  // Get a list of available subcommands.
  foreach (Subcommand* subcommand, subcommands) {
    stream << "    " << subcommand->name() << "\n";
  }

  return stream.str();
}


inline int Subcommand::dispatch(
    const Option<std::string>& prefix,
    int argc,
    char** argv,
    const std::vector<Subcommand*>& subcommands)
{
  if (subcommands.empty()) {
    std::cerr << "No subcommand is found" << std::endl;
    return 1;
  }

  // Check for duplicated subcommand names.
  hashset<std::string> names;
  foreach (Subcommand* subcommand, subcommands) {
    if (names.contains(subcommand->name())) {
      std::cerr << "Multiple subcommands have name '"
                << subcommand->name() << "'" << std::endl;
      return 1;
    }
    names.insert(subcommand->name());
  }

  if (argc < 2) {
    std::cerr << usage(argv[0], subcommands) << std::endl;
    return 1;
  }

  if (std::string(argv[1]) == "help") {
    if (argc == 2) {
      std::cout << usage(argv[0], subcommands) << std::endl;
      return 0;
    }

    // 'argv[0] help subcommand' => 'argv[0] subcommand --help'
    argv[1] = argv[2];
    argv[2] = (char*) "--help";
  }

  foreach (Subcommand* subcommand, subcommands) {
    if (subcommand->name() == argv[1]) {
      flags::FlagsBase* flags = subcommand->getFlags();

      Try<flags::Warnings> load = flags->load(prefix, argc - 1, argv + 1);
      if (load.isError()) {
        std::cerr << "Failed to parse the flags: " << load.error() << std::endl;
        return 1;
      }

      return subcommand->execute();
    }
  }

  std::cerr << "Subcommand '" << argv[1] << "' is not available\n"
            << usage(argv[0], subcommands) << std::endl;
  return 1;
}

#endif // __STOUT_SUBCOMMAND_HPP__
