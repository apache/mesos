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

#ifndef __STOUT_BITS_HPP__
#define __STOUT_BITS_HPP__

#include <stdint.h>

// Provides efficient bit operations.
// More details can be found at:
// http://graphics.stanford.edu/~seander/bithacks.html
namespace bits {

// Counts set bits from a 32 bit unsigned integer using Hamming weight.
inline int countSetBits(uint32_t value)
{
  int count = 0;
  value = value - ((value >> 1) & 0x55555555);
  value = (value & 0x33333333) + ((value >> 2) & 0x33333333);
  count = (((value + (value >> 4)) & 0x0F0F0F0F) * 0x01010101) >> 24;

  return count;
}

} // namespace bits {

#endif // __STOUT_BITS_HPP__
