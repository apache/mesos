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

#ifndef __LINUX_LDD_HPP__
#define __LINUX_LDD_HPP__

#include <string>
#include <vector>

#include <stout/hashset.hpp>
#include <stout/try.hpp>

#include "linux/ldcache.hpp"

// Given an ldcache, recursively expand the ELF link dependencies
// for the given path.
Try<hashset<std::string>> ldd(
    const std::string& path,
    const std::vector<ldcache::Entry>& cache);

#endif // __LINUX_LDD_HPP__
