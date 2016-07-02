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

#ifndef __STOUT_ELF_HPP__
#define __STOUT_ELF_HPP__

#include <map>
#include <string>
#include <vector>

#include <elfio/elfio.hpp>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/try.hpp>

namespace elf {

enum Class
{
  CLASSNONE = ELFCLASSNONE,
  CLASS32 = ELFCLASS32,
  CLASS64 = ELFCLASS64,
};

enum class SectionType
{
  DYNAMIC = SHT_DYNAMIC,
};

enum class DynamicTag
{
  STRTAB = DT_STRTAB,
  SONAME = DT_SONAME,
  NEEDED = DT_NEEDED,
};


// This class is used to represent the contents of a standard ELF
// binary. The current implementation is incomplete.
//
// NOTE: We use ELFIO under the hood to do our elf parsing for us.
// Unfortunately, ELFIO reads the *entire* elf file into memory when
// it is first loaded instead of relying on something like `mmap` to
// only page in the parts of the file we are interested in. If this
// becomes a problem, we will have to use something like `libelf`
// instead (but note that libelf is not header-only and will
// therefore introduce a runtime library dependency).
class File
{
public:
  // TODO(klueska): Return a unique_ptr here.
  static Try<File*> load(const std::string& path)
  {
    File* file = new File();

    if (!file->elf.load(path.c_str())) {
      delete file;
      return Error("Unknown error during elfio::load");
    }

    // Create the mapping from section type to sections.
    foreach (ELFIO::section* section, file->elf.sections) {
      SectionType section_type = (SectionType) section->get_type();
      file->sections_by_type[section_type].push_back(section);
    }

    return file;
  }

  // Returns the ELF class of an ELF file (CLASS32, or CLASS64).
  Try<Class> get_class() const
  {
    Class c = Class(elf.get_class());
    if (c == CLASSNONE) {
      return Error("Unknown error");
    }
    return c;
  }


  // Extract the strings associated with the provided
  // `DynamicTag` from all DYNAMIC sections in the ELF binary.
  Try<std::vector<std::string>> get_dynamic_strings(DynamicTag tag) const
  {
    if (sections_by_type.count(SectionType::DYNAMIC) == 0) {
      return Error("No DYNAMIC sections found in ELF");
    }

    std::vector<std::string> strings;

    foreach (ELFIO::section* section,
             sections_by_type.at(SectionType::DYNAMIC)) {
      auto accessor = ELFIO::dynamic_section_accessor(elf, section);

      for (ELFIO::Elf_Xword i = 0; i < accessor.get_entries_num(); ++i) {
        ELFIO::Elf_Xword entry_tag;
        ELFIO::Elf_Xword entry_value;
        std::string entry_string;

        if (!accessor.get_entry(i, entry_tag, entry_value, entry_string)) {
          return Error("Failed to get entry from DYNAMIC section of elf");
        }

        if (tag == DynamicTag(entry_tag)) {
          strings.push_back(entry_string);
        }
      }
    }

    return strings;
  }

private:
  explicit File() {}

  ELFIO::elfio elf;
  std::map<SectionType, std::vector<ELFIO::section*>> sections_by_type;
};

} // namespace elf {

#endif // __STOUT_ELF_HPP__
