// Copyright 2015 Mesosphere, Inc.
//
// This file has been modified from its original form by Mesosphere, Inc.
// All modifications made to this file by Mesosphere (the "Modifications")
// are copyright 2015 Mesosphere, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//  http://www.apache.org/licenses/LICENSE-2.0

// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// ---
//
// The original file on which the Modifications have been made were
// provided to Mesosphere pursuant to the following terms:
//
// Copyright (C) 2014 insane coder (http://insanecoding.blogspot.com/,
// http://asprintf.insanecoding.org/)
//
// Permission to use, copy, modify, and distribute this software for any
// purpose with or without fee is hereby granted, provided that the above
// copyright notice and this permission notice appear in all copies.
//
// THE SOFTWARE IS PROVIDED "AS IS" AND THE AUTHOR DISCLAIMS ALL WARRANTIES
// WITH REGARD TO THIS SOFTWARE INCLUDING ALL IMPLIED WARRANTIES OF
// MERCHANTABILITY AND FITNESS. IN NO EVENT SHALL THE AUTHOR BE LIABLE FOR
// ANY SPECIAL, DIRECT, INDIRECT, OR CONSEQUENTIAL DAMAGES OR ANY DAMAGES
// WHATSOEVER RESULTING FROM LOSS OF USE, DATA OR PROFITS, WHETHER IN AN
// ACTION OF CONTRACT, NEGLIGENCE OR OTHER TORTIOUS ACTION, ARISING OUT OF
// OR IN CONNECTION WITH THE USE OR PERFORMANCE OF THIS SOFTWARE.

#ifndef __STOUT_WINDOWS_FORMAT_HPP__
#define __STOUT_WINDOWS_FORMAT_HPP__

#include <stdio.h> // For '_vscprintf', 'vsnprintf'.
#include <stdlib.h> // For 'malloc', 'free'.
#include <limits.h> // For 'INT_MAX'.


inline int vasprintf(char** buffer, const char* format, va_list args)
{
  int result = -1;
  int size = _vscprintf(format, args) + 1;

  if (size >= 0 && size < INT_MAX) {
    *buffer = (char*) malloc(size);

    if (*buffer) {
      result = vsnprintf(*buffer, size, format, args);

      if (result < 0) {
        free(*buffer);
      }
    }
  }

  return result;
}


#endif // __STOUT_WINDOWS_FORMAT_HPP__
