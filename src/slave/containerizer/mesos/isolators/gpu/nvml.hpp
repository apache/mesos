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

#ifndef __NVIDIA_NVML_HPP__
#define __NVIDIA_NVML_HPP__

#ifdef ENABLE_NVML
#include <nvidia/gdk/nvml.h>
#else
// We provide dummy types in case we do not use the NVML headers.
using nvmlDevice_t = int;
#endif // ENABLE_NVML

#include <string>

#include <stout/nothing.hpp>
#include <stout/try.hpp>

// The `nvml` namespace acts as a higher-level wrapper on top of the
// NVIDIA Management Library (NVML). By providing this higher-level
// API, we eliminate the build-time dependency on `libnvidia-ml` in
// favor of a runtime dependency. The caller may probe to see if the
// library is available, and may take different code paths accordingly.
// This is advantageous for deploying a common `libmesos` on machines
// both with and without GPUs installed. Only those machines that have
// GPUs need to have the `libnvidia-ml` library installed on their
// system.
//
// TODO(klueska): Add unit tests and provide finalization support.
namespace nvml {

// Returns whether the NVML library is installed on the system
// and can be loaded at runtime.
bool isAvailable();

// Load and initialize NVML. This must be called prior to making
// use of the NVML wrapper functions below.
Try<Nothing> initialize();

// NVML wrapper functions. May be called after initializing
// the library.
Try<std::string> systemGetDriverVersion();
Try<unsigned int> deviceGetCount();
Try<nvmlDevice_t> deviceGetHandleByIndex(unsigned int index);
Try<unsigned int> deviceGetMinorNumber(nvmlDevice_t handle);

} // namespace nvml {

#endif // __NVIDIA_NVML_HPP__
