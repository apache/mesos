// Licensed to the Apache Software Foundation (ASF) under one
// or more contributor license agreements.  See the NOTICE file
// distributed with this work for additional information
// regarding copyright ownership.  The ASF licenses this file
// to you under the Apache License, Version 2.0 (the
// "License"); you may not use this file except in compliance
// with the License.  You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include <dlfcn.h>

#include <glog/logging.h>

#include <map>
#include <string>

#include <process/once.hpp>

#include <stout/error.hpp>
#include <stout/exit.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

#include <stout/posix/dynamiclibrary.hpp>

#include "logging/logging.hpp"

#include "slave/containerizer/mesos/isolators/gpu/nvml.hpp"

#ifndef ENABLE_NVML
// We provide dummy types and variables in case we do not use the NVML headers.
using nvmlReturn_t = int;
constexpr bool NVML_SUCCESS = true;
constexpr size_t NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE = 1;
constexpr nvmlReturn_t NVML_ERROR_INVALID_ARGUMENT{};
#endif // ENABLE_NVML

using process::Once;

using std::map;
using std::string;

namespace nvml {

static constexpr char LIBRARY_NAME[] = "libnvidia-ml.so.1";


struct NvidiaManagementLibrary
{
  NvidiaManagementLibrary(
      nvmlReturn_t (*_systemGetDriverVersion)(char *, unsigned int),
      nvmlReturn_t (*_deviceGetCount)(unsigned int*),
      nvmlReturn_t (*_deviceGetHandleByIndex)(unsigned int, nvmlDevice_t*),
      nvmlReturn_t (*_deviceGetMinorNumber)(nvmlDevice_t, unsigned int*),
      const char* (*_errorString)(nvmlReturn_t))
    : systemGetDriverVersion(_systemGetDriverVersion),
      deviceGetCount(_deviceGetCount),
      deviceGetHandleByIndex(_deviceGetHandleByIndex),
      deviceGetMinorNumber(_deviceGetMinorNumber),
      errorString(_errorString) {}

  nvmlReturn_t (*systemGetDriverVersion)(char *, unsigned int);
  nvmlReturn_t (*deviceGetCount)(unsigned int*);
  nvmlReturn_t (*deviceGetHandleByIndex)(unsigned int, nvmlDevice_t*);
  nvmlReturn_t (*deviceGetMinorNumber)(nvmlDevice_t, unsigned int*);
  const char* (*errorString)(nvmlReturn_t);
};


static const NvidiaManagementLibrary* nvml = nullptr;


static Once* initialized = new Once();
static Option<Error>* error = new Option<Error>();
static DynamicLibrary* library = new DynamicLibrary();


Try<Nothing> initialize()
{
  if (initialized->once()) {
    if (error->isSome()) {
      return error->get();
    }
    return Nothing();
  }

  Try<Nothing> open = library->open(LIBRARY_NAME);
  if (open.isError()) {
    *error = Error("Failed to open '" + stringify(LIBRARY_NAME) + "': " +
                   open.error());
    initialized->done();
    return error->get();
  }

  // Load the needed symbols.
  //
  // NOTE: NVML maps nvmlInit to nvmlInit_v2 via a macro.
  // This means that we have to explicitly load nvmlInit_v2.
  //
  // TODO(klueska): Use nvmlDeviceGetCount_v2 and nvmlDeviceGetHandleByIndex_v2.
  map<string, void*> symbols = {
      { "nvmlInit_v2", nullptr },
      { "nvmlSystemGetDriverVersion", nullptr },
      { "nvmlDeviceGetCount", nullptr },
      { "nvmlDeviceGetHandleByIndex", nullptr },
      { "nvmlDeviceGetMinorNumber", nullptr },
      { "nvmlErrorString", nullptr },
  };

  foreachkey (const string& name, symbols) {
    Try<void*> symbol = library->loadSymbol(name);
    if (symbol.isError()) {
      *error = Error("Failed to load symbol '" + name + "': " + symbol.error());
      initialized->done();
      return error->get();
    }
    symbols.at(name) = symbol.get();
  }

  auto nvmlInit =
    (nvmlReturn_t (*)()) symbols.at("nvmlInit_v2");

  auto nvmlErrorString =
    (const char* (*)(nvmlReturn_t)) symbols.at("nvmlErrorString");

  nvmlReturn_t result = nvmlInit();
  if (result != NVML_SUCCESS) {
    *error = Error("nvmlInit failed: " + stringify(nvmlErrorString(result)));
    initialized->done();
    return error->get();
  }

  nvml = new NvidiaManagementLibrary(
      (nvmlReturn_t (*)(char*, unsigned int))
          symbols.at("nvmlSystemGetDriverVersion"),
      (nvmlReturn_t (*)(unsigned int*))
          symbols.at("nvmlDeviceGetCount"),
      (nvmlReturn_t (*)(unsigned int, nvmlDevice_t*))
          symbols.at("nvmlDeviceGetHandleByIndex"),
      (nvmlReturn_t (*)(nvmlDevice_t, unsigned int*))
          symbols.at("nvmlDeviceGetMinorNumber"),
      (const char* (*)(nvmlReturn_t))
          symbols.at("nvmlErrorString"));

  initialized->done();

  return Nothing();
}


bool isAvailable()
{
#ifndef ENABLE_NVML
  return false;
#endif // ENABLE_NVML

  // Unfortunately, there is no function available in `glibc` to check
  // if a dynamic library is available to open with `dlopen()`.
  // Instead, availability is determined by attempting to open a
  // library with `dlopen()` and if this call fails, assuming the
  // library is unavailable. The problem with using this method to
  // implement a generic `isAvailable()` function is knowing if we
  // should call `dlclose()` on the library once we've determined that
  // the library is in fact available (because some other code path
  // may have already opened the library and we don't want to close it
  // out from under them). Fortunately, calls to `dlopen()` are reference
  // counted, so that subsequent calls to `dlclose()` simply down the
  // reference count and only actually close the library when this
  // reference count hits zero. Because of this, we can
  // unconditionally call `dlclose()` and trust that glibc will take
  // care to do the right thing. Additionally, calling `dlopen()` with
  // `RTLD_LAZY` is the preferred method here because it is faster in
  // cases where the library is not yet opened, and having previously
  // opened it with `RTLD_NOW` will always take precedence.
  void* open = ::dlopen(LIBRARY_NAME, RTLD_LAZY);
  if (open == nullptr) {
    return false;
  }

  CHECK_EQ(0, ::dlclose(open))
    << "dlcose failed: " << dlerror();

  return true;
}


Try<string> systemGetDriverVersion()
{
  if (nvml == nullptr) {
    return Error("NVML has not been initialized");
  }

  // The NVML manual specifies that the version string will never
  // exceed 80 characters.
  char version[NVML_SYSTEM_DRIVER_VERSION_BUFFER_SIZE];
  nvmlReturn_t result = nvml->systemGetDriverVersion(version, sizeof(version));
  if (result != NVML_SUCCESS) {
    return Error(nvml->errorString(result));
  }
  return string(version);
}


Try<unsigned int> deviceGetCount()
{
  if (nvml == nullptr) {
    return Error("NVML has not been initialized");
  }

  unsigned int count;
  nvmlReturn_t result = nvml->deviceGetCount(&count);
  if (result != NVML_SUCCESS) {
    return Error(nvml->errorString(result));
  }
  return count;
}


Try<nvmlDevice_t> deviceGetHandleByIndex(unsigned int index)
{
  if (nvml == nullptr) {
    return Error("NVML has not been initialized");
  }

  nvmlDevice_t handle;
  nvmlReturn_t result = nvml->deviceGetHandleByIndex(index, &handle);
  if (result == NVML_ERROR_INVALID_ARGUMENT) {
    return Error("GPU device not found");
  }
  if (result != NVML_SUCCESS) {
    return Error(nvml->errorString(result));
  }
  return handle;
}


Try<unsigned int> deviceGetMinorNumber(nvmlDevice_t handle)
{
  if (nvml == nullptr) {
    return Error("NVML has not been initialized");
  }

  unsigned int minor;
  nvmlReturn_t result = nvml->deviceGetMinorNumber(handle, &minor);
  if (result != NVML_SUCCESS) {
    return Error(nvml->errorString(result));
  }
  return minor;
}

} // namespace nvml {
