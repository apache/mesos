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
#ifndef __STOUT_OS_ENVP_HPP__
#define __STOUT_OS_ENVP_HPP__

#include <map>

#include <stout/hashmap.hpp>
#include <stout/os.hpp>

namespace os {

// Used to build an environment suitable for execle()
class ExecEnv
{
public:
  explicit ExecEnv(const std::map<std::string, std::string>& environment);
  ~ExecEnv();

  ExecEnv(const ExecEnv&);

  char** operator () () const { return envp; }

private:
  // Not default constructable and not assignable.
  ExecEnv();
  ExecEnv& operator = (const ExecEnv&);

  char** envp;
  size_t size;
};


inline ExecEnv::ExecEnv(const std::map<std::string, std::string>& _environment)
  : envp(NULL),
    size(0)
{
  // Merge passed environment with OS environment, overriding where necessary.
  hashmap<std::string, std::string> environment = os::environment();

  foreachpair (const std::string& key, const std::string& value, _environment) {
    environment[key] = value;
  }

  size = environment.size();

  // Convert environment to internal representation.
  // Add 1 to the size for a NULL terminator.
  envp = new char*[size + 1];
  int index = 0;
  foreachpair (const std::string& key, const std::string& value, environment) {
    std::string entry = key + "=" + value;
    envp[index] = new char[entry.size() + 1];
    strncpy(envp[index], entry.c_str(), entry.size() + 1);
    ++index;
  }

  envp[index] = NULL;
}


inline ExecEnv::~ExecEnv()
{
  for (size_t i = 0; i < size; ++i) {
    delete[] envp[i];
  }
  delete[] envp;
  envp = NULL;
}


inline ExecEnv::ExecEnv(const ExecEnv& other)
{
  size = other.size;

  envp = new char*[size + 1];
  for (size_t i = 0; i < size; ++i) {
    envp[i] = new char[strlen(other.envp[i]) + 1];
    strncpy(envp[i], other.envp[i], strlen(other.envp[i]) + 1);
  }

  envp[size] = NULL;
}

}  // namespace os {

#endif // __STOUT_OS_ENVP_HPP__
