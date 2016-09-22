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
    std::vector<char*> _argv;
    foreach (const std::string& arg, iterable) {
      char* _arg = new char[arg.size() + 1];
      ::memcpy(_arg, arg.c_str(), arg.size() + 1);
      _argv.emplace_back(_arg);
    }

    size = _argv.size();
    argv = new char*[size + 1];
    for (size_t i = 0; i < size; i++) {
      argv[i] = _argv[i];
    }
    argv[size] = nullptr;
  }

  ~Argv()
  {
    for (size_t i = 0; i < size; i++) {
      delete[] argv[i];
    }
    delete[] argv;
  }

  operator char**()
  {
    return argv;
  }

private:
  char** argv;
  size_t size;
};

} // namespace raw {
} // namespace os {

#endif // __STOUT_OS_RAW_ARGV_HPP__
