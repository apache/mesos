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

#ifndef __STOUT_OS_RAW_ARGV_HPP__
#define __STOUT_OS_RAW_ARGV_HPP__

#include <string.h>

#include <string>
#include <vector>

#include <stout/foreach.hpp>

namespace os {
namespace raw {

/**
 * Represent the argument list expected by `execv` routines. The
 * argument list is an array of pointers that point to null-terminated
 * strings. The array of pointers must be terminated by a nullptr. To
 * use this abstraction, see the following example:
 *
 *   vector<string> args = {"arg0", "arg1"};
 *   os::raw::Argv argv(args);
 *   execvp("my_binary", argv);
 */
class Argv
{
public:
  template <typename Iterable>
  explicit Argv(const Iterable& iterable)
  {
    foreach (const std::string& arg, iterable) {
      args.emplace_back(arg);
    }

    argv = new char*[args.size() + 1];
    for (size_t i = 0; i < args.size(); i++) {
      argv[i] = const_cast<char*>(args[i].c_str());
    }

    argv[args.size()] = nullptr;
  }

  ~Argv()
  {
    delete[] argv;
  }

  operator char**() const
  {
    return argv;
  }

  operator std::vector<std::string>() const
  {
    return args;
  }

private:
  std::vector<std::string> args;

  // NOTE: This points to strings in the vector `args`.
  char** argv;
};

} // namespace raw {
} // namespace os {

#endif // __STOUT_OS_RAW_ARGV_HPP__
