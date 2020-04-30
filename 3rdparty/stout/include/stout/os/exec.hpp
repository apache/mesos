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

#ifndef __STOUT_OS_EXEC_HPP__
#define __STOUT_OS_EXEC_HPP__

// For readability, we minimize the number of #ifdef blocks in the code by
// splitting platform specific system calls into separate directories.
#ifdef __WINDOWS__
#include <stout/os/windows/exec.hpp>
#else
#include <stout/os/posix/exec.hpp>
#endif // __WINDOWS__

namespace os {

// Forks a subprocess and executes the provided file with the provided
// arguments.
//
// Blocks until the subprocess terminates and returns the exit code of
// the subprocess, or `None` if an error occurred (e.g., fork / exec /
// waitpid or the Windows equivalents failed).
//
// POSIX: this function is async signal safe. We return an
// `Option<int>` instead of a `Try<int>`, because although `Try`
// does not dynamically allocate, `Error` uses `std::string`,
// which is not async signal safe.
//
// Windows: Note that on Windows, processes are started using
// a string command line, and each process does its own parsing
// of that command line into arguments. This function will quote
// and escape the arguments compatible with any programs that
// use `CommandLineToArgvW` (this is the most common way, and any
// programs that use libc-style main with an arguments array will
// use this under the covers). However, some programs, notably
// cmd.exe have their own approach for parsing quotes / arguments
// that are not compatible with CommandLineToArgvW, and therefore
// should not be used with this function!
//
// TODO(bmahler): Add a windows only overload that takes a single
// string command line (to support cmd.exe and others with non-
// CommandLineToArgvW parsing). See MESOS-10093.
inline Option<int> spawn(
    const std::string& file,
    const std::vector<std::string>& arguments);


// This wrapper allows a caller to call `execvp` on both POSIX
// and Windows systems.
//
// Windows: In order to emulate `execvp`, this function forks
// another subprocess to execute with the provided arguments,
// and once the subprocess terminates, the parent process will
// in turn exit with the same exit code. Note that on Windows,
// processes are started using a string command line, and each
// process does its own parsing of that command line into
// arguments. This function will quote and escape the arguments
// compatible with any programs that use `CommandLineToArgvW`
// (this is the most common way, and any programs that use
// libc-style main with an arguments array will use this under
// the covers). However, some programs, notably cmd.exe have
// their own approach for parsing quotes / arguments that are
// not compatible with CommandLineToArgvW, and therefore should
// not be used with this function!
//
// TODO(bmahler): Probably we shouldn't provide this windows
// emulation and should instead have the caller use windows
// subprocess functions directly?
inline int execvp(const char* file, char* const argv[]);


// This function is a portable version of execvpe ('p' means searching
// executable from PATH and 'e' means setting environments). We add
// this function because it is not available on all POSIX systems.
//
// POSIX: This function is not thread safe. It is supposed to be used
// only after fork (when there is only one thread). This function is
// async signal safe.
//
// Windows: In order to emulate `execvpe`, this function forks
// another subprocess to execute with the provided arguments,
// and once the subprocess terminates, the parent process will
// in turn exit with the same exit code. Note that on Windows,
// processes are started using a string command line, and each
// process does its own parsing of that command line into
// arguments. This function will quote and escape the arguments
// compatible with any programs that use `CommandLineToArgvW`
// (this is the most common way, and any programs that use
// libc-style main with an arguments array will use this under
// the covers). However, some programs, notably cmd.exe have
// their own approach for parsing quotes / arguments that are
// not compatible with CommandLineToArgvW, and therefore should
// not be used with this function!
//
// NOTE: This function can accept `Argv` and `Envp` constructs
// via their implicit type conversions, but on Windows, it cannot
// accept the os::raw forms.
//
// TODO(bmahler): Probably we shouldn't provide this windows
// emulation and should instead have the caller use windows
// subprocess functions directly?
inline int execvpe(const char* file, char** argv, char** envp);

} // namespace os {

#endif // __STOUT_OS_EXEC_HPP__
