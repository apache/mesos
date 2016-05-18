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

#include <fcntl.h>
#include <gelf.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <unistd.h>

#include <map>
#include <string>
#include <vector>

#include <stout/error.hpp>
#include <stout/foreach.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/stringify.hpp>
#include <stout/try.hpp>

namespace elf {

enum Class {
  CLASSNONE = ELFCLASSNONE,
  CLASS32 = ELFCLASS32,
  CLASS64 = ELFCLASS64,
};

enum class SectionType {
  DYNAMIC = SHT_DYNAMIC,
};

enum class DynamicTag {
  STRTAB = DT_STRTAB,
  SONAME = DT_SONAME,
  NEEDED = DT_NEEDED,
};

// This class is used to represent the contents of a standard ELF
// binary. The current implementation is incomplete.
//
// TODO(klueska): For now, we only include functionality to extract
// the SONAME from shared libraries and read their external library
// dependencies. In the future, we will expand this class to include
// helpers for full access to an ELF file's contents.
class File {
public:
  // Open an ELF binary. We do some preliminary parsing as part of
  // opening the ELF to get quick references to all of its internal
  // sections for all future calls.
  //
  // TODO(klueska): Consider adding a 'stout::Owned' type to avoid
  // returning a raw pointer here.
  static Try<File*> open(const std::string& path)
  {
    // Set the elf library operating version.
    if (elf_version(EV_CURRENT) == EV_NONE) {
      return Error("Failed to set ELF library version: " +
                   stringify(elf_errmsg(-1)));
    }

    File* file = new File();

    file->fd = ::open(path.c_str(), O_RDONLY);
    if (file->fd < 0) {
      delete file;
      return ErrnoError("Failed to open file");
    }

    file->elf = elf_begin(file->fd, ELF_C_READ, NULL);
    if (file->elf == NULL) {
      delete file;
      return Error("elf_begin() failed: " + stringify(elf_errmsg(-1)));
    }

    if (elf_kind(file->elf) != ELF_K_ELF) {
      delete file;
      return Error("File is not an ELF binary");
    }

    // Create the mapping from section type to section locations.
    Elf_Scn* section = NULL;
    while ((section = elf_nextscn(file->elf, section)) != NULL) {
      GElf_Shdr section_header;
      if (gelf_getshdr(section, &section_header) == NULL) {
        delete file;
        return Error("gelf_getshdr() failed: " + stringify(elf_errmsg(-1)));
      }

      SectionType section_type = (SectionType) section_header.sh_type;
      switch (section_type) {
        case SectionType::DYNAMIC:
          file->sections[section_type].push_back(section);
      }
    }

    return file;
  }

  void close()
  {
    if (elf != NULL) {
      elf_end(elf);
      elf = NULL;
    }

    if (fd >= 0) {
      ::close(fd);
      fd = -1;
    }
  }

  ~File()
  {
    close();
  };


  // Returns the ELF class of an
  // ELF file (CLASS32, or CLASS64).
  Try<Class> GetClass() const
  {
    Class c = (Class)gelf_getclass(elf);
    if (c == CLASSNONE) {
      return Error("gelf_getclass() failed: " + stringify(elf_errmsg(-1)));
    }
    return c;
  }


  // Extract the strings associated with the provided `DynamicTag`
  // from the DYNAMIC section of the ELF binary.
  Try<std::vector<std::string>> GetDynamicStrings(DynamicTag tag) const
  {
    if (sections.count(SectionType::DYNAMIC) == 0) {
      return Error("No DYNAMIC sections found in ELF");
    }

    if (sections.count(SectionType::DYNAMIC) != 1) {
      return Error("Multiple DYNAMIC sections found in ELF");
    }

    Elf_Scn* dynamic_section = sections.at(SectionType::DYNAMIC)[0];

    // Walk through the entries in the dynamic section and look for
    // entries with the provided tag. These entries contain offsets to
    // strings in the dynamic section's string table. Consequently, we
    // also have to look for an entry containing a pointer to the
    // dynamic section's string table so we can resolve the strings
    // associated with the provided tag later on.
    Elf_Data* dynamic_data = elf_getdata(dynamic_section, NULL);
    if (dynamic_data == NULL) {
      return Error("elf_getdata() failed: " + stringify(elf_errmsg(-1)));
    }

    Option<uintptr_t> strtab_pointer = None();
    std::vector<uintptr_t> strtab_offsets;

    for (size_t i = 0; i < dynamic_data->d_size / sizeof(GElf_Dyn); i++) {
      GElf_Dyn entry;
      if (gelf_getdyn(dynamic_data, i, &entry) == NULL) {
          return Error("gelf_getdyn() failed: " + stringify(elf_errmsg(-1)));
      }

      if ((DynamicTag)entry.d_tag == DynamicTag::STRTAB) {
        strtab_pointer = entry.d_un.d_ptr;
      }

      if ((DynamicTag)entry.d_tag == tag) {
        strtab_offsets.push_back(entry.d_un.d_ptr);
      }
    }

    if (strtab_offsets.empty()) {
      return std::vector<std::string>();
    }

    if (strtab_pointer.isNone()) {
      return Error("Failed to find string table");
    }

    // Get a reference to the actual string table so we can index into it.
    Elf_Scn* string_table_section = gelf_offscn(elf, strtab_pointer.get());
    if (string_table_section == NULL) {
      return Error("gelf_offscn() failed: " + stringify(elf_errmsg(-1)));
    }

    size_t strtab_index = elf_ndxscn(string_table_section);
    if (strtab_index == SHN_UNDEF) {
      return Error("elf_ndxscn() failed: " + stringify(elf_errmsg(-1)));
    }

    // Find the strings in the string table from their offsets and return them.
    std::vector<std::string> strings;
    foreach (uintptr_t offset, strtab_offsets) {
      char* string = elf_strptr(elf, strtab_index, offset);
      if (string == NULL) {
        return Error("elf_strptr() failed: " + stringify(elf_errmsg(-1)));
      }

      strings.push_back(string);
    }

    return strings;
  }

private:
  explicit File()
    : fd(-1),
      elf(NULL) {}

  int fd;
  Elf* elf;
  std::map<SectionType, std::vector<Elf_Scn*>> sections;
};

} // namespace elf {

#endif // __STOUT_ELF_HPP__
