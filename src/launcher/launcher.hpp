/**
 * Licensed to the Apache Software Foundation (ASF) under one
 * or more contributor license agreements.  See the NOTICE file
 * distributed with this work for additional information
 * regarding copyright ownership.  The ASF licenses this file
 * to you under the Apache License, Version 2.0 (the
 * "License"); you may not use this file except in compliance
 * with the License.  You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */

#ifndef __LAUNCHER_LAUNCHER_HPP__
#define __LAUNCHER_LAUNCHER_HPP__

#include <string>

#include <process/future.hpp>
#include <process/owned.hpp>

#include <stout/flags.hpp>
#include <stout/none.hpp>
#include <stout/nothing.hpp>
#include <stout/option.hpp>
#include <stout/try.hpp>

namespace mesos {
namespace internal {
namespace launcher {

// The default executable used by the launcher.
extern const std::string DEFAULT_EXECUTABLE;


// Represents an operation to be executed by a subprocess.
class Operation
{
public:
  // Launches this operation in a subprocess. The user may choose to
  // specify the executable and the path in which to search for the
  // executable. If not specified, the default executable and the
  // default path will be used.
  process::Future<Option<int> > launch(
      const Option<int>& stdout = None(),
      const Option<int>& stderr = None(),
      const std::string& executable = DEFAULT_EXECUTABLE,
      const Option<std::string>& path = None());

protected:
  // Returns the name of this operation.
  virtual std::string name() const = 0;

  // Defines the operation that will be executed by a subprocess. The
  // return value will be the exit code of the subprocess.
  virtual int execute() = 0;

  // Returns the pointer to the flags that will be used for this
  // operation. By default, the flags is empty.
  virtual flags::FlagsBase* getFlags() { return &flags; }

private:
  friend void add(const process::Owned<Operation>& operation);
  friend int main(int argc, char** argv);

  // The default flags which is empty.
  flags::FlagsBase flags;
};


// Tell the launcher which directory to search for the executable by
// default if it is not specified by the user. When launching an
// operation, if the user does not specify the 'path' and no default
// 'path' is set, the 'launch' will fail.
void setDefaultPath(const std::string& path);


// Register an operation. This is supposed to be called in the main
// function of the subprocess.
void add(const process::Owned<Operation>& operation);


// Syntactic sugar for registering an operation. For example, the
// following code shows a typical main function of the subprocess.
//
// int main(int argc, char** argv)
// {
//   launcher::add<Operation1>();
//   launcher::add<OPeration2>();
//
//   return launcher::main(argc, argv);
// }
template <typename T>
void add()
{
  add(process::Owned<Operation>(new T()));
}


// The main entry of the subprocess.
int main(int argc, char** argv);


// An operation which takes a shell command and executes it. This is
// mainly used for testing.
class ShellOperation : public Operation
{
public:
  struct Flags : public flags::FlagsBase
  {
    Flags();

    Option<std::string> command;
  };

  Flags flags;

protected:
  virtual std::string name() const { return "shell"; }
  virtual int execute();
  virtual flags::FlagsBase* getFlags() { return &flags; }
};

} // namespace launcher {
} // namespace internal {
} // namespace mesos {

#endif // __LAUNCHER_LAUNCHER_HPP__
