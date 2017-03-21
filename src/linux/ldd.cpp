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

#include "linux/ldd.hpp"

#include <process/owned.hpp>

#include <stout/elf.hpp>
#include <stout/nothing.hpp>

using std::string;
using std::vector;

using process::Owned;

Try<hashset<string>> ldd(
    const string& path,
    const vector<ldcache::Entry>& cache)
{
  hashset<string> dependencies;

  // Keep a queue of paths that are candidates to have their
  // ELF dependencies scanned. Once we actually scan something,
  // we push it into the dependencies set. This lets us avoid
  // scanning paths more than once.
  vector<string> candidates;

  // The first candidate is the input path.
  candidates.push_back(path);

  while (!candidates.empty()) {
    // Take the next candidate from the back of the
    // queue. There's no special significance to this, other
    // than it avoids unnecessary copies that would occur if
    // we popped the front.
    const string candidate = candidates.back();
    candidates.pop_back();

    // If we already have this path, don't scan it again.
    if (dependencies.contains(candidate)) {
      continue;
    }

    Try<elf::File*> load = elf::File::load(candidate);
    if (load.isError()) {
      return Error(load.error());
    }

    Owned<elf::File> elf(load.get());

    Try<vector<string>> _dependencies =
      elf->get_dynamic_strings(elf::DynamicTag::NEEDED);
    if (_dependencies.isError()) {
      return Error(_dependencies.error());
    }

    // Collect the ELF dependencies of this path into the needed
    // list, scanning the ld.so cache to find the actual path of
    // each needed library.
    foreach (const string& dependency, _dependencies.get()) {
      auto entry = std::find_if(
          cache.begin(),
          cache.end(),
          [&dependency](const ldcache::Entry& e) {
            return e.name == dependency;
          });

      if (entry == cache.end()) {
        return Error("'" + dependency + "' is not in the ld.so cache");
      }

      // If this ELF object has an interpreter (e.g. ld-linux-x86-64.so.2),
      // inspect that too. We need both to be able to run an executable program.
      // NOTE: The same object may be specified as a dependency too but on some
      // systems (e.g. Ubuntu), the exact path of the two can differ even though
      // they link to the the same file (see MESOS-7060). We however need the
      // exact path specified in the interpreter section to run the executable.
      Result<string> interpreter = elf->get_interpreter();
      if (interpreter.isSome()) {
        candidates.push_back(interpreter.get());
      }

      candidates.push_back(entry->path);
    }

    dependencies.insert(candidate);
  }

  // Since we are only finding the dependencies, we should
  // not include the initial file itself.
  dependencies.erase(path);

  return dependencies;
}
