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

#ifndef __STOUT_OS_RAW_ENVIRONMENT_HPP__
#define __STOUT_OS_RAW_ENVIRONMENT_HPP__

#include <string.h>

#include <string>

#include <stout/foreach.hpp>
#include <stout/json.hpp>
#include <stout/stringify.hpp>

#ifdef __APPLE__
#include <crt_externs.h> // For _NSGetEnviron().
#elif !defined(__WINDOWS__)
// Need to declare 'environ' pointer for platforms that are not OS X or Windows.
extern char** environ;
#endif


// NOTE: the `os::raw` namespace contains a family of simple wrapper functions
// for getting environment data from either Windows or Unix machines. For
// example, `os::raw::environment` returns an "unstructured" `char**` that
// contains the raw environment variables of the executing process. Accessing
// "structured" version of this function, `os::environment`, returns a
// `map<string, string>` instead. This family of functions exists in the
// `os::raw` namespace because of the unstructured nature of their return
// values.
//
// WARNING: these functions are called `environment` and not `environ` because
// on Windows, `environ` is a macro, and not an `extern char**` as it is in the
// POSIX standard. The existance of this macro on Windows makes it impossible
// to use a function called `os::environ`.
namespace os {
namespace raw {

inline char** environment()
{
  // Accessing the list of environment variables is platform-specific.
  // On OS X, the 'environ' symbol isn't visible to shared libraries,
  // so we must use the _NSGetEnviron() function (see 'man environ' on
  // OS X). On other platforms, it's fine to access 'environ' from
  // shared libraries.
#ifdef __APPLE__
  return *_NSGetEnviron();
#else
  // NOTE: the correct style for this expression would be `::environ`, but we
  // leave it out because `environ` is a macro on Windows, and the `::` will
  // break the build.
  return environ;
#endif
}


// Returns the address of os::environment().
inline char*** environmentp()
{
  // Accessing the list of environment variables is platform-specific.
  // On OS X, the 'environ' symbol isn't visible to shared libraries,
  // so we must use the _NSGetEnviron() function (see 'man environ' on
  // OS X). On other platforms, it's fine to access 'environ' from
  // shared libraries.
#ifdef __APPLE__
  return _NSGetEnviron();
#else
  // NOTE: the correct style for this expression would be `environ`, but we
  // leave it out because `environ` is a macro on Windows, and the `::` will
  // break the build.
  return &environ;
#endif
}


// Represents the environment variable list expected by 'exec'
// routines. The environment variable list is an array of pointers
// that point to null-terminated strings. The array of pointers must
// be terminated by a nullptr. To use this abstraction, see the
// following example:
//
//   map<string, string> environment = {
//     {"key1", "value1"},
//     {"key2", "value2"}
//   };
//   os::raw::Envp envp(environment);
//   execle("/bin/sh", "sh", "-c", "echo hello", envp);
class Envp
{
public:
  Envp(Envp&& that)
    : envp(that.envp),
      size(that.size)
  {
    that.envp = nullptr;
    that.size = 0;
  }

  template <typename Map>
  explicit Envp(const Map& map)
  {
    size = map.size();

    // NOTE: We add 1 to the size for a `nullptr` terminator.
    envp = new char*[size + 1];
    size_t index = 0;

    for (auto it = map.begin(); it != map.end(); ++it) {
      std::string entry = stringify(it->first) + "=" + stringify(it->second);
      envp[index] = new char[entry.size() + 1];
      ::memcpy(envp[index], entry.c_str(), entry.size() + 1);
      ++index;
    }

    envp[index] = nullptr;
  }

  explicit Envp(const JSON::Object& object)
  {
    size = object.values.size();

    // NOTE: We add 1 to the size for a `nullptr` terminator.
    envp = new char*[size + 1];
    size_t index = 0;

    foreachpair (const std::string& key,
                 const JSON::Value& value,
                 object.values) {
      std::string entry = key + "=" + value.as<JSON::String>().value;
      envp[index] = new char[entry.size() + 1];
      ::memcpy(envp[index], entry.c_str(), entry.size() + 1);
      ++index;
    }

    envp[index] = nullptr;
  }

  ~Envp()
  {
    if (envp == nullptr) {
      return;
    }

    for (size_t i = 0; i < size; i++) {
      delete[] envp[i];
    }
    delete[] envp;
  }

  operator char**()
  {
    return envp;
  }

private:
  Envp(const Envp&) = delete;
  Envp& operator=(const Envp&) = delete;

  char **envp;
  size_t size;
};

} // namespace raw {
} // namespace os {

#endif // __STOUT_OS_RAW_ENVIRONMENT_HPP__
