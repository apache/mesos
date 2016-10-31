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

#ifndef __STOUT_FLAGS_HPP__
#define __STOUT_FLAGS_HPP__

#include <stout/flags/flags.hpp>

// An abstraction for application/library "flags". An example is
// probably best:
//  -------------------------------------------------------------
// class MyFlags : public virtual FlagsBase // Use 'virtual' for composition!
// {
// public:
//   MyFlags()
//   {
//     add(&MyFlags::debug,
//         "debug",
//         "Help string for debug",
//         false);
//
//     add(&MyFlags::name,
//         "name",
//         "Help string for name");
//   }

//   bool debug;
//   Option<string> name;
// };
//
// ...
//
// map<string, Option<string>> values;
// values["no-debug"] = None();            // --no-debug
// values["debug"] = None();               // --debug
// values["debug"] = Some("true");         // --debug=true
// values["debug"] = Some("false");        // --debug=false
// values["name"] = Some("frank");         // --name=frank
//
// MyFlags flags;
// flags.load(values);
// flags.name.isSome() ...
// flags.debug ...
//  -------------------------------------------------------------
//
// You can also compose flags provided that each has used "virtual
// inheritance":
//  -------------------------------------------------------------
// class MyFlags : public virtual MyFlags1, public virtual MyFlags2 {};
//
// MyFlags flags;
// flags.add(...); // Any other flags you want to throw in there.
// flags.load(values);
// flags.flag_from_myflags1 ...
// flags.flag_from_myflags2 ...
//  -------------------------------------------------------------
//
// "Fail early, fail often":
//
// You cannot add duplicate flags, this is checked for you at compile
// time for composite flags (e.g., Flag<MyFlags1, MyFlags2>) and also
// checked at runtime for any other flags added via inheritance or
// Flags::add(...).
//
// Flags that cannot be loaded (e.g., attempting to use the 'no-'
// prefix for a flag that is not boolean) will print a message to
// standard error and abort the process.

// TODO(benh): Provide a boolean which specifies whether or not to
// abort on duplicates or load errors.

#endif // __STOUT_FLAGS_HPP__
