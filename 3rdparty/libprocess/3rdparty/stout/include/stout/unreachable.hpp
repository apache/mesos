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
#ifndef __STOUT_UNREACHABLE_HPP__
#define __STOUT_UNREACHABLE_HPP__

#include <iostream>

#define UNREACHABLE() Unreachable(__FILE__, __LINE__)

struct Unreachable
{
  Unreachable(const std::string& _file, int _line)
    : file(_file), line(_line) {}

  template <typename T>
  operator T () const
  {
    // TODO(benh): Print stack trace too.
    std::cerr << "Reached unreachable statement at "
              << file << ":" << line << std::endl;
    abort();

    // Note that dereference a T* since T might not be default
    // constructable and can't just 'return T()'.
    return *((T*) NULL);
  }

private:
  const std::string file;
  const int line;
};

#endif // __STOUT_UNREACHABLE_HPP__
