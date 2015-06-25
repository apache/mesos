---
layout: documentation
---

# Mesos C++ Style Guide

The Mesos codebase follows the [Google C++ Style Guide](http://google-styleguide.googlecode.com/svn/trunk/cppguide.xml) with the following differences:

## Naming

### Variable Names
* We use [lowerCamelCase](http://en.wikipedia.org/wiki/CamelCase#Variations_and_synonyms) for variable names (Google uses snake_case, and their class member variables have trailing underscores).
* We prepend constructor and function arguments with a leading underscore to avoid ambiguity and / or shadowing:

~~~{.cpp}
Try(State _state, T* _t = NULL, const std::string& _message = "")
  : state(_state), t(_t), message(_message) {}
~~~

* Prefer trailing underscores for use as member fields (but not required). Some trailing underscores are used to distinguish between similar variables in the same scope (think prime symbols), *but this should be avoided as much as possible, including removing existing instances in the code base.*

* If you find yourself creating a copy of an argument passed by const reference, consider passing it by value instead (if you don't want to use a leading underscore and copy in the body of the function):

~~~{.cpp}
// You can pass-by-value in ProtobufProcess::install() handlers.
void Slave::statusUpdate(StatusUpdate update, const UPID& pid)
{
  ...
  update.mutable_status()->set_source(
      pid == UPID() ? TaskStatus::SOURCE_SLAVE : TaskStatus::SOURCE_EXECUTOR);
  ...
}
~~~

### Constant Names
* We use [SCREAMING_SNAKE_CASE](http://en.wikipedia.org/wiki/Letter_case#Special_case_styles) for constant names (Google uses a `k` followed by mixed case, e.g. `kDaysInAWeek`).

### Function Names
* We use [lowerCamelCase](http://en.wikipedia.org/wiki/CamelCase#Variations_and_synonyms) for function names (Google uses mixed case for regular functions; and their accessors and mutators match the name of the variable).
* Leave spaces around overloaded operators, e.g. `operator + (...);` rather than `operator+(...);`

### Namespace Names
* We do not use namespace aliases.

## Strings
* Strings used in log and error messages should end without a period.

## Comments
* End each sentence within a comment with a punctuation mark (please note that we generally prefer periods); this applies to incomplete sentences as well.
* At most 70 characters per line in comments.
* For trailing comments, leave one space.

## Breaks
* Break before braces on function, class, struct and union definitions. (Google attaches braces to the surrounding context)

## Indentation

### Class Format
* Access modifiers are not indented (Google uses one space indentation).
* Constructor initializers are indented by 2 spaces (Google indents by 4).

### Function Definition/Invocation
* Newline when calling or defining a function: indent with 4 spaces.
* We do not follow Google's style of wrapping on the open parenthesis, the general goal is to reduce visual "jaggedness" in the code. Prefer (1), (4), (5), sometimes (3), never (2):

~~~{.cpp}
// 1: OK.
allocator->resourcesRecovered(frameworkId, slaveId, resources, filters);

// 2: Don't use.
allocator->resourcesRecovered(frameworkId, slaveId,
                              resources, filters);

// 3: Don't use in this case due to "jaggedness".
allocator->resourcesRecovered(frameworkId,
                              slaveId,
                              resources,
                              filters);

// 3: In this case, 3 is OK.
foobar(someArgument,
       someOtherArgument,
       theLastArgument);

// 4: OK.
allocator->resourcesRecovered(
    frameworkId,
    slaveId,
    resources,
    filters);

// 5: OK.
allocator->resourcesRecovered(
    frameworkId, slaveId, resources, filters);
~~~

### Continuation
* Newline for an assignment statement: indent with 2 spaces.

~~~{.cpp}
Try<Duration> failoverTimeout =
  Duration::create(FrameworkInfo().failover_timeout());
~~~

## Empty Lines
* 1 blank line at the end of the file.
* Elements outside classes (classes, structs, global functions, etc.) should be spaced apart by 2 blank lines.
* Elements inside classes (member variables and functions) should not be spaced apart by more than 1 blank line.

## Capture by Reference

We disallow capturing **temporaries** by reference. See [MESOS-2629](https://issues.apache.org/jira/browse/MESOS-2629) for the rationale.

~~~{.cpp}
Future<Nothing> f() { return Nothing(); }
Future<bool> g() { return false; }

struct T
{
  T(const char* data) : data(data) {}
  const T& member() const { return *this; }
  const char* data;
};

// 1: Don't use.
const Future<Nothing>& future = f();

// 1: Instead use.
const Future<Nothing> future = f();

// 2: Don't use.
const Future<Nothing>& future = Future<Nothing>(Nothing());

// 2: Instead use.
const Future<Nothing> future = Future<Nothing>(Nothing());

// 3: Don't use.
const Future<bool>& future = f().then(lambda::bind(g));

// 3: Instead use.
const Future<bool> future = f().then(lambda::bind(g));

// 4: Don't use (since the T that got constructed is a temporary!).
const T& t = T("Hello").member();

// 4: Preferred alias pattern (see below).
const T t("Hello");
const T& t_ = t.member();

// 4: Can also use.
const T t = T("Hello").member();
~~~

We allow capturing non-temporaries by *constant reference* when the intent is to **alias**.

The goal is to make code more concise and improve readability. Use this if an expression referencing a field by `const` is:

* Used repeatedly.
* Would benefit from a concise name to provide context for readability.
* Will **not** be invalidated during the lifetime of the alias. Otherwise document this explicitly.

~~~{.cpp}
hashmap<string, hashset<int>> index;

// 1: Ok.
const hashset<int>& values = index[2];

// 2: Ok.
for (auto iterator = index.begin(); iterator != index.end(); ++iterator) {
  const hashset<int>& values = iterator->second;
}

// 3: Ok.
foreachpair (const string& key, const hashset<int>& values, index) {}
foreachvalue (const hashset<int>& values, index) {}
foreachkey (const string& key, index) {}

// 4: Avoid aliases in most circumstances as they can be dangerous.
//    This is an example of a dangling alias!
vector<string> strings{"hello"};

string& s = strings[0];

strings.erase(strings.begin());

s += "world"; // THIS IS A DANGLING REFERENCE!
~~~

## File Headers

* Mesos source files must contain the "ASF" header:

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


* Stout and libprocess source files must contain the "Apache License Version 2.0" header:

        /**
         * Licensed under the Apache License, Version 2.0 (the "License");
         * you may not use this file except in compliance with the License.
         * You may obtain a copy of the License at
         *
         *     http://www.apache.org/licenses/LICENSE-2.0
         *
         * Unless required by applicable law or agreed to in writing, software
         * distributed under the License is distributed on an "AS IS" BASIS,
         * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
         * See the License for the specific language governing permissions and
         * limitations under the License
         */

## C++11

We support C++11 and require GCC 4.8+ or Clang 3.5+ compilers. The whitelist of supported C++11 features is:

* Static assertions.
* Multiple right angle brackets.
* Type inference (`auto` and `decltype`). The main goal is to increase code readability. This is safely the case if the exact same type omitted on the left is already fully stated on the right. Here are several examples:

~~~{.cpp}
// 1: OK.
const auto& i = values.find(keys.front());
// Compare with
const typename map::iterator& i = values.find(keys.front());

// 2: OK.
auto names = shared_ptr<list<string>>(new list<string>());
// Compare with
shared_ptr<list<string>> names = shared_ptr<list<string>>(new list<string>());

// 3: Don't use.
auto authorizer = LocalAuthorizer::create(acls);
// Compare with
Try<Owned<LocalAuthorizer>> authorizer = LocalAuthorizer::create();
~~~

* Rvalue references.
* Explicitly-defaulted functions.
* Variadic templates.
* Delegating constructors.
* Mutexes.
  * `std::mutex`
  * `std::lock_guard<std::mutex>`
  * `std::unique_lock<std::mutex>`
* Shared from this.
  * `class T : public std::enable_shared_from_this<T>`
  * `shared_from_this()`
* Lambdas!
  * Don't put a space between the capture list and the parameter list:

~~~{.cpp}
// 1: OK.
[]() { ...; };

// 2: Don't use.
[] () { ...; };
~~~

  * Prefer default capture by value, explicit capture by value, then capture by reference. To avoid dangling-pointer bugs, *never* use default capture by reference:

~~~{.cpp}
// 1: OK.
[=]() { ... }; // Default capture by value.
[n]() { ... }; // Explicit capture by value.
[&n]() { ... }; // Explicit capture by reference.
[=, &n]() { ... }; // Default capture by value, explicit capture by reference.

// 2: Don't use.
[&]() { ... }; // Default capture by reference.
~~~

  * Use `mutable` only when absolutely necessary.

~~~{.cpp}
// 1: OK.
[]() mutable { ...; };
~~~

  * Feel free to ignore the return type by default, adding it as necessary to appease the compiler or be more explicit for the reader.

~~~{.cpp}
// 1: OK.
[]() { return true; };
[]() -> bool { return ambiguous(); };
~~~

  * Feel free to use `auto` when naming a lambda expression:

~~~{.cpp}
// 1: OK.
auto lambda = []() { ...; };
~~~

  * Format lambdas similar to how we format functions and methods. Feel free to let lambdas be one-liners:

~~~{.cpp}
// 1: OK.
auto lambda = []() {
  ...;
};

// 2: OK.
auto lambda = []() { ...; };
~~~

  * Feel free to inline lambdas within function arguments:

~~~{.cpp}
instance.method([]() {
  ...;
});
~~~

  * Chain function calls on a newline after the closing brace of the lambda and the closing parenthesis of function call:

~~~{.cpp}
// 1: OK.
instance
  .method([]() {
    ...;
  })
  .then([]() { ...; })
  .then([]() {
    ...;
  });

// 2: OK (when no chaining, compare to 1).
instance.method([]() {
  ...;
});

// 3: OK (if no 'instance.method').
function([]() {
  ...;
})
.then([]() { ...; })
.then([]() {
  ...;
});

// 3: OK (but prefer 1).
instance.method([]() {
  ...;
})
.then([]() { ...; })
.then([]() {
  ...;
});
~~~

  * Wrap capture lists indepedently of parameters, *use the same formatting as if the capture list were template parameters*:

~~~{.cpp}
// 1: OK.
function([&capture1, &capture2, &capture3](
    const T1& p1, const T2& p2, const T3& p3) {
  ...;
});

function(
    [&capture1, &capture2, &capture3](
        const T1& p1, const T2& p2, const T3& p3) {
  ...;
});

auto lambda = [&capture1, &capture2, &capture3](
    const T1& p1, const T2& p2, const T3& p3) {
  ...;
};

auto lambda =
  [&capture1, &capture2, &capture3](
      const T1& p1, const T2& p2, const T3& p3) {
  ...;
};

// 2: OK (when capture list is longer than 80 characters).
function([
    &capture1,
    &capture2,
    &capture3,
    &capture4](
        const T1& p1, const T2& p2) {
  ...;
});

auto lambda = [
    &capture1,
    &capture2,
    &capture3,
    &capture4](
        const T1& p1, const T2& p2) {
  ...;
};

// 3: OK (but prefer 2).
function([
    &capture1,
    &capture2,
    &capture3,
    &capture4](const T1& p1, const T2& t2) {
  ...;
});

auto lambda = [
    &capture1,
    &capture2,
    &capture3,
    &capture4](const T1& p1, const T2& p2) {
  ...;
};

// 3: Don't use.
function([&capture1,
          &capture2,
          &capture3,
          &capture4](const T1& p1, const T2& p2) {
  ...;
});

auto lambda = [&capture1,
               &capture2,
               &capture3,
               &capture4](const T1& p1, const T2& p2) {
  ...;
  };

// 4: Don't use.
function([&capture1,
           &capture2,
           &capture3,
           &capture4](
    const T1& p1, const T2& p2, const T3& p3) {
  ...;
});

auto lambda = [&capture1,
               &capture2,
               &capture3,
               &capture4](
    const T1& p1, const T2& p2, const T3& p3) {
  ...;
};

// 5: Don't use.
function([&capture1,
          &capture2,
          &capture3,
          &capture4](
    const T1& p1,
    const T2& p2,
    const T3& p3) {
  ...;
  });

auto lambda = [&capture1,
               &capture2,
               &capture3,
               &capture4](
    const T1& p1,
    const T2& p2,
    const T3& p3) {
  ...;
};

// 6: OK (parameter list longer than 80 characters).
function([&capture1, &capture2, &capture3](
    const T1& p1,
    const T2& p2,
    const T3& p3,
    const T4& p4) {
  ...;
});

auto lambda = [&capture1, &capture2, &capture3](
    const T1& p1,
    const T2& p2,
    const T3& p3,
    const T4& p4) {
  ...;
};

// 7: OK (capture and parameter lists longer than 80 characters).
function([
    &capture1,
    &capture2,
    &capture3,
    &capture4](
        const T1& p1,
        const T2& p2,
        const T3& p3,
        const T4& p4) {
  ...;
});

auto lambda = [
    &capture1,
    &capture2,
    &capture3,
    &capture4](
        const T1& p1,
        const T2& p2,
        const T3& p3,
        const T4& p4) {
  ...;
};
~~~

* Unrestricted Union.

  Like the pre-existing `union`, we can overlap storage allocation for objects that never exist simultaneously. However, with C++11 we are no longer *restricted to having only non-POD types in unions*. Adding non-POD types to unions complicates things, however, because we need to make sure to properly call constructors and destructors. Therefore, only use unrestricted unions (i.e., unions with non-POD types) when the union has only a single field. What does this buy us? Now we can avoid dynamic memory allocations for "container" like types, e.g., `Option`, `Try`, `Result`, etc. In effect, we treat the union like a dynamic allocation, calling *placement new*, `new (&t) T(...)` anyplace we would have just called `new T(...)` and the destructor `t.~T()` anyplace we would have called `delete t`.
