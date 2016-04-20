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

#ifndef __STOUT_OS_OS_HPP__
#define __STOUT_OS_OS_HPP__

#include <string>

#include <stout/bytes.hpp>


namespace os {

// Structure returned by loadavg(). Encodes system load average
// for the last 1, 5 and 15 minutes.
struct Load {
  double one;
  double five;
  double fifteen;
};


// Structure returned by memory() containing the total size of main
// and free memory.
struct Memory
{
  Bytes total;
  Bytes free;
  Bytes totalSwap;
  Bytes freeSwap;
};


// The structure returned by uname describing the currently running system.
struct UTSInfo
{
  std::string sysname;    // Operating system name (e.g. Linux).
  std::string nodename;   // Network name of this machine.
  std::string release;    // Release level of the operating system.
  std::string version;    // Version level of the operating system.
  std::string machine;    // Machine hardware platform.
};


} // namespace os {

#endif // __STOUT_OS_OS_HPP__
