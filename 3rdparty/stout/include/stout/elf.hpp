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
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/result.hpp>
#include <stout/stringify.hpp>
#include <stout/strings.hpp>
#include <stout/try.hpp>
#include <stout/version.hpp>

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
  NOTE = SHT_NOTE,
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
      SectionType section_type = SectionType(section->get_type());
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
      return Error("No DYNAMIC sections found");
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
          return Error("Failed to get entry from DYNAMIC section");
        }

        if (tag == DynamicTag(entry_tag)) {
          strings.push_back(entry_string);
        }
      }
    }

    return strings;
  }

  // Get the ABI version of the ELF file by parsing the contents of
  // the `.note.ABI-tag` section. This section is Linux specific and
  // adheres to the format described in the links below.
  //
  // NOTE: Not all ELF files have a `.note.ABI-tag` section (even on
  // Linux), so we return a `Result` to allow the return value to be
  // `None()`.
  //
  // https://refspecs.linuxfoundation.org/LSB_1.2.0/gLSB/noteabitag.html
  // http://flint.cs.yale.edu/cs422/doc/ELF_Format.pdf
  Result<Version> get_abi_version() const
  {
    ELFIO::section* section = elf.sections[".note.ABI-tag"];

    if (section == nullptr) {
      return None();
    }

    if (SectionType(section->get_type()) != SectionType::NOTE) {
      return Error("Section '.note.ABI-tag' is not a NOTE");
    }

    auto accessor = ELFIO::note_section_accessor(elf, section);

    if (accessor.get_notes_num() != 1) {
      return Error("Section '.note.ABI-tag' does not have exactly one entry");
    }

    ELFIO::Elf_Word type;
    std::string name;
    void* descriptor;
    ELFIO::Elf_Word descriptor_size;

    if (!accessor.get_note(0, type, name, descriptor, descriptor_size)) {
      return Error("Failed to get entry from '.note.ABI-tag' section");
    }

    // The note in a `.note.ABI-tag` section must have `type == 1`.
    if (type != 1) {
      return Error("Corrupt tag type '" + stringify(type) + "' from"
                   " entry in '.note.ABI-tag' section");
    }

    // Linux mandates `name == GNU`.
    if (name != "GNU") {
      return Error("Corrupt label '" + name + "' from"
                   " entry in '.note.ABI-tag' section");
    }

    // The version array consists of 4 32-bit numbers, with the
    // first number fixed at 0 (meaning it is a Linux ELF file), and
    // the rest specifying the ABI version. For example, if the array
    // contains {0, 2, 3, 99}, this signifies a Linux ELF file
    // with an ABI version of 2.3.99.
    std::vector<uint32_t> version(
        (uint32_t*)descriptor,
        (uint32_t*)((char*)descriptor + descriptor_size));

    if (version.size() != 4 || version[0] != 0) {
      return Error("Corrupt version '" + stringify(version) + "'"
                   " from entry in '.note.ABI-tag' section");
    }

    return Version(version[1], version[2], version[3]);
  }

private:
  explicit File() {}

  ELFIO::elfio elf;
  std::map<SectionType, std::vector<ELFIO::section*>> sections_by_type;
};

} // namespace elf {

#endif // __STOUT_ELF_HPP__
