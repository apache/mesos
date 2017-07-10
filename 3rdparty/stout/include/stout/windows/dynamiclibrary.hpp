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

#ifndef __STOUT_WINDOWS_DYNAMICLIBRARY_HPP__
#define __STOUT_WINDOWS_DYNAMICLIBRARY_HPP__

#include <string>

#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/windows/error.hpp>

/**
 * DynamicLibrary is a very simple wrapper around the programming interface
 * to the dynamic linking loader.
 */
class DynamicLibrary
{
public:
  DynamicLibrary() : handle_(nullptr) { }

  // Since this class manages a naked handle it cannot be copy- or
  // move-constructed.
  // TODO(bbannier): Allow for move-construction.
  DynamicLibrary(const DynamicLibrary&) = delete;
  DynamicLibrary(DynamicLibrary&&) = delete;

  virtual ~DynamicLibrary()
  {
    if (handle_ != nullptr) {
      close();
    }
  }

  Try<Nothing> open(const std::string& path)
  {
    // Check if we've already opened a library.
    if (handle_ != nullptr) {
      return Error("Library already opened");
    }

    handle_ = ::LoadLibraryW(wide_stringify(path).data());

    if (handle_ == nullptr) {
      return WindowsError("Could not load library '" + path + "'");
    }

    path_ = path;

    return Nothing();
  }

  Try<Nothing> close()
  {
    if (handle_ == nullptr) {
      return Error("Could not close library; handle was already `nullptr`");
    }

    if (!::FreeLibrary(handle_)) {
      return WindowsError(
          "Could not close library '" + (path_.isSome() ? path_.get() : ""));
    }

    handle_ = nullptr;
    path_ = None();

    return Nothing();
  }

  Try<void*> loadSymbol(const std::string& name)
  {
    if (handle_ == nullptr) {
      return Error(
          "Could not get symbol '" + name + "'; library handle was `nullptr`");
    }

    void* symbol = ::GetProcAddress(handle_, name.c_str());

    if (symbol == nullptr) {
      return WindowsError(
          "Error looking up symbol '" + name + "' in '" +
          (path_.isSome() ? path_.get() : ""));
    }

    return symbol;
  }

private:
  HMODULE handle_;
  Option<std::string> path_;
};

#endif // __STOUT_WINDOWS_DYNAMICLIBRARY_HPP__
