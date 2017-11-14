## Stout Developer Guide

Stout is a header-only C++ library. Simply add the `include` folder to your include path (i.e., `-I/path/to/stout/include`) during compilation (eventually we plan to support installation).

> NOTE: Depending on which headers you'd like to use, you may require the following third party libraries:
>
>  - Boost
>  - Google's glog (this dependency will be removed in the future)
>  - Google's protobuf (version 3.3.x - 3.5.x is required to run tests)
>  - Google's gmock/gtest

There are a handful of data structures provided within the library (including some collections), as well as some namespaced and miscellaneous utilities. Also included are abstractions for [command line flags](#flags).

Stout provides and heavily leverages some monadic structures including [Option](#option) and [Try](#try).

Note that the library is designed to completely avoid exceptions. See [exceptions](#exceptions) for further discussion.

* <a href="#option">Option, Some, and None</a>
* <a href="#try">Try, Result, and Error</a>
* <a href="#nothing">Nothing</a>
* <a href="#fs">fs::</a>
* <a href="#gzip">gzip::</a>
* <a href="#json">JSON::</a>
* <a href="#jsonify">`jsonify`</a>
* <a href="#lambda">lambda::</a>
* <a href="#net">net::</a>
* <a href="#os">os::</a>
* <a href="#path">path::</a>
* <a href="#proc">proc::</a>
* <a href="#protobuf">protobuf::</a>
* <a href="#strings">strings::</a>
* <a href="#flags">Command Line Flags</a>
* <a href="#collections">Collections</a>
* <a href="#miscellaneous">Miscellaneous</a>
* <a href="#testing">Testing</a>
* <a href="#philosophy">Philosophy</a>


<a href="option"></a>

## `Option`, `Some`, and `None`

The `Option` type provides a safe alternative to using `nullptr`. An `Option` can be constructed explicitly or implicitly:

~~~{.cpp}
    Option<bool> o(true);
    Option<bool> o = true;
~~~

You can check if a value is present using `Option::isSome()` and `Option::isNone()` and retrieve the value using `Option::get()`:

~~~{.cpp}
    if (!o.isNone()) {
      ... o.get() ...
    }
~~~

Note that the current implementation *copies* the underlying values (see [Philosophy](#philosophy) for more discussion). Nothing prevents you from using pointers, however, *the pointer will not be deleted when the Option is destructed*:

~~~{.cpp}
    Option<std::string*> o = new std::string("hello world");
~~~

The `None` type acts as "syntactic sugar" to make using [Option](#option) less verbose. For example:

~~~{.cpp}
    Option<T> foo(const Option<T>& o)
    {
      return None(); // Can use 'None' here.
    }

    ...

    foo(None()); // Or here.

    ...

    Option<int> o = None(); // Or here.
~~~

Similar to `None`, the `Some` type can be used to construct an `Option` as well. In most circumstances `Some` is unnecessary due to the implicit `Option` constructor, however, it can still be useful to remove any ambiguities as well as when embedded within collections:

~~~{.cpp}
    Option<Option<std::string>> o = Some("42");

    std::map<std::string, Option<std::string>> values;
    values["value1"] = None();
    values["value2"] = Some("42");
~~~


<a href="try"></a>

## `Try`, `Result`, and `Error`

A `Try` provides a mechanism to return a value or an error without throwing exceptions. Like `Option`, you can explicitly or implicitly construct a `Try`:

~~~{.cpp}
    Try<bool> t(true);
    Try<bool> t = true;
~~~

You can check if a value is present using `Try::isSome()` and `Try::isError()` and retrieve the value using `Try::get()` or the error via `Try::error`:

~~~{.cpp}
    if (!t.isError()) {
      ... t.get() ...
    } else {
      ... t.error() ...
    }
~~~

A `Result` is a combination of a `Try` and an `Option`; you can think of a `Result` as semantically being equivalent to `Try<Option<T>>`. In addition to `isSome()` and `isError()` a `Result` includes `isNone()`.

The `Error` type acts as "syntactic sugar" for implicitly constructing a `Try` or `Result`. For example:

~~~{.cpp}
    Try<bool> parse(const std::string& s)
    {
      if (s == "true") {
        return true;
      } else if (s == "false") {
        return false;
      } else {
        return Error("Failed to parse string as boolean");
      }
    }

    Try<bool> t = parse("false");
~~~

You can also use `None` and `Some` with `Result` just like with `Option`:

~~~{.cpp}
    Result<bool> r = None();
    Result<bool> r = Some(true);
~~~




<a href="nothing"></a>

## `Nothing`

A lot of functions that return `void` can also "return" an
error. Since we don't use exceptions (see [Exceptions](#exceptions))
we capture this pattern using `Try<Nothing>` (see [Try](#try)).



<a href="fs"></a>

## `fs::`

A collection of utilities for working with a filesystem. Currently we provide `fs::size`, `fs::usage`, and `fs::symlink`.


<a href="gzip"></a>

## `gzip::`

A collection of utilities for doing gzip compression and decompression using `gzip::compress` and `gzip::decompress`.

~~~{.cpp}
  gzip::decompress(gzip::compress("hello world"));
~~~


<a href="json"></a>

## `JSON::`

*Requires Boost.*

Provides structures and rendering of the JavaScript Object Notation (JSON) grammar using `boost::variant`. A JSON "value" (`JSON::Value`) is one of (i.e., a variant of) `JSON::String`, `JSON::Number`, `JSON::Object`, `JSON::Array`, `JSON::True`, `JSON::False`, and `JSON::Null`.

We explicitly define 'true' (`JSON::True`), 'false' (`JSON::False`), and 'null' (`JSON::Null`), for clarity and also because `boost::variant` "picks" the wrong type when we try and use `std::string`, `long` (or `int`), `double` (or `float`), and `bool` all in the same variant.

We could have avoided using `JSON::String` or `JSON::Number` and just used `std::string` and `double` respectively, but we choose to include them for completeness (although, this does slow compilation due to the extra template instantiations). That being said, you can use the native types in place of constructing the `JSON` wrapper. For example:

~~~{.cpp}
  // {
  //   "foo": "value1",
  //   "bar": "value2",
  //   "baz": 42.0,
  //   "bam": 0.42,
  // }
  JSON::Object object;
  object.values["foo"] = JSON::String("value1");
  object.values["bar"] = "value2";
  object.values["baz"] = JSON::Number(42.0);
  object.values["bam"] = 0.42;
~~~

Of course, nesting of a `JSON::Value` is also permitted as per the JSON specification:

~~~{.cpp}
  // An array of objects:
  // [
  //   { "first": "Benjamin", "last": "Hindman" },
  //   { "first": "Michael", "last": "Hindman" }
  // ]
  JSON::Array array;

  JSON::Object object1;
  object1.values["first"] = "Benjamin";
  object1.values["last"] = "Hindman";

  array.values.push_back(object1);

  JSON::Object object2;
  object2.values["first"] = "Michael";
  object2.values["last"] = "Hindman";

  array.values.push_back(object2);
~~~

You can "render" a JSON value using `std::ostream operator<<` (or by using `stringify` (see [here](#stringify)).


<a href="jsonify"></a>

## `jsonify`

`jsonify` takes an instance of a C++ object and returns a representation of `JSON` string captured in a light-weight proxy object. The proxy object can either be implicitly converted to a `std::string`, or directly inserted into an output stream.

`jsonify(const T&)` is implemented by calling the function `json`. We perform unqualified function call so that it can detect overloads via argument dependent lookup. That is, we will search for, and use a free function named `json` in the same namespace as `T`.

> NOTE: This relationship is similar to `boost::hash` and `hash_value`.

`json` takes two parameters: a pointer to a writer type, and the type of the object. The following are the available writers and their member functions:

* `BooleanWriter` -- `set(value)`
* `NumberWriter`  -- `set(value)`
* `StringWriter`  -- `append(value)`
* `ArrayWriter`   -- `element(value)`
* `ObjectWriter`  -- `field(key, value)`

~~~{.cpp}
namespace store {

struct Customer
{
  std::string first_name;
  std::string last_name;
  int age;
};

void json(JSON::ObjectWriter* writer, const Customer& customer)
{
  writer->field("first name", customer.first_name);
  writer->field("last name", customer.last_name);
  writer->field("age", customer.age);
}

} // namespace store {

store::Customer customer{"michael", "park", 25};
std::cout << jsonify(customer);
// prints: {"first name":"michael","last name":"park","age":25}
~~~

`jsonify(const F&)` overload takes a function object `F` that takes a pointer to writer. This is useful in cases where we don't want to define a public `json` function out-of-line.

~~~{.cpp}
namespace store {

struct Customer
{
  std::string first_name;
  std::string last_name;
  int age;
};

} // namespace store {

store::Customer customer{"michael", "park", 25};
std::cout << jsonify([&customer](JSON::ObjectWriter* writer) {
  writer->field("first name", customer.first_name);
  writer->field("last name", customer.last_name);
  writer->field("age", customer.age);
});
// prints: {"first name":"michael","last name":"park","age":25}
~~~

<a href="lambda"></a>

## `lambda::`

To help deal with compatibility issues between C++98/03 and C++11 we wrap some of the TR1 types from `functional` in `lambda`. That way, when using `lambda::bind` you'll get `std::tr1::bind` where TR1 is available and `std::bind` when C++11 is available.


<a href="net"></a>

## `net::`

A collection of utilities for working with the networking subsystem. Currently we provide `net::download` and `net::getHostname`.


<a href="os"></a>

## `os::`

The `os` namespace provides numerous utilities for working with the operating system. Most of these utilities are simple wrappers around C-style functions that don't cleanly take or return C++ types, for example `os::getenv`, `os::setenv`, `os::realpath`, etc. The library also includes a handful of "shell familiars", such as `os::chown`, `os::touch`, `os::ls`, `os::find`, `os::su`, `os::glob`, etc. We call out a few of the special abstractions below.

#### Reading and Writing `std::string`

To make reading and writing from files easier there are implementations of `os::read` and `os::write` that take and return `std::string`.

#### Files

Most of the ways to get or set information about files requires complicated calls using the `stat` family of functions. We provide simple wrappers around `stat` via things like `os::exists`, `os::stat::isdir`, `os::stat::isfile`, and `os::stat::islink`.

#### Processes

There are some fairly extensive abstractions around getting process information from the operating system. There is an `os::Process` type that gets provided by utilities like `os::processes`. You can construct an arbitrary fork/exec process tree by constructing an `os::Fork`, get all the processes in a process tree using `os::pstree`, and kill all the processes in a process tree using `os::killtree` (the latter is similar to the shell command `kill` but strictly more powerful in that it can walk a process tree, following groups and sessions if requested).

#### `sysctl`

There is an `os::sysctl` abstraction for getting and setting kernel state on OS X.

#### Signals

There are a handful of wrappers that make working with signals easier, including `os::signals::pending`, `os::signals::block`, and `os::signals::unblock`. In addition, there is a suppression abstraction that enables executing a block of code while blocking a signal. Consider writing to a pipe which may raise a SIGPIPE if the pipe has been closed. Using `suppress` you can do:

~~~{.cpp}
    suppress (SIGPIPE) {
      write(fd, data, size);
      if (errno == EPIPE) {
        // The pipe has been closed!
      }
    }
~~~


<a href="path"></a>

## `path::`

The `path` namespace provides the `path::join` function for joining together filesystem paths. Additionally to the `path` namespace there also exists the class `Path`. `Path` provides `basename()` and `dirname()` as thread safe replacements for standard `::basename()` and `::dirname()`.


<a href="proc"></a>

## `proc::`

*Requires Linux.*

The `proc` namespace provides some abstractions for working with the Linux `proc` filesystem. The key abstractions are `proc::ProcessStatus` which models the data provided in `/proc/[pid]/stat` and `proc::SystemStatus` which models the data provided in `/proc/stat`.


<a href="protobuf"></a>

## `protobuf::`

*Requires protobuf.*

Helpers for reading and writing protobufs from files and file descriptors are provided via `protobuf::read` and `protobuf::write`. These assume a "recordio" format where the length of the serialized data is read/written first followed by the actual data.

There is also a protobuf to JSON converter via `JSON::protobuf` that enables serializing a protobuf into JSON:

~~~{.cpp}
    google::protobuf::Message message;
    ... fill in message ...
    JSON::Object o = JSON::protobuf(message);
~~~


<a href="strings"></a>

## `strings::`

Utilities for inspecting and manipulating strings are available in the `strings` namespace. This includes varying tokenization techniques via `strings::tokenize`, `strings::split`, and `strings::pairs`.

String formatting is provided via `strings::format`. The `strings::format` functions produces strings based on the `printf` family of functions. Except, unlike the `printf` family of functions, the `strings::format` routines attempt to "stringify" (see [here](#stringify)) any arguments that are not POD types (i.e., "plain old data": primitives, pointers, certain structs/classes and unions, etc). This enables passing structs/classes to `strings::format` provided there is a definition/specialization of `std::ostream operator<<` available for that type. Note that the `%s` format specifier is expected for each argument that gets passed. A specialization for `std::string` is also provided so that `std::string::c_str` is not necessary (but again, `%s` is expected as the format specifier).



<a href="flags"></a>

## Command Line Flags

One frustration with existing command line flags libraries was the burden they put on writing tests that attempted to have many different instantiations of the flags. For example, running two instances of the same component within a test where each instance was started with different command line flags. To solve this, we provide a command line flags abstraction called `Flags` (in the `flags` namespace) that you can extend to define your own flags:

~~~{.cpp}
    struct MyFlags : virtual flags::Flags // Use `virtual` for composition!
    {
      MyFlags()
      {
        // A flag with a default value.
        add(&MyFlags::foo,
            "foo",
            "Some information about foo",
            DEFAULT_VALUE_FOR_FOO);

        // A flag without a default value,
        // defined below with an `Option`.
        add(&MyFlags::bar,
            "bar",
            "Some information about bar");
      }

      int foo;
      Option<std::string> bar;
    };
~~~

You can then load the flags via `argc` and `argv` via:

~~~{.cpp}
    MyFlags flags;
    Try<flags::Warnings> load = flags.load(None(), argc, argv);

    if (load.isError()) { ... }

    ... flags.foo ...
    ... flags.bar.isSome() ... flags.bar.get() ...
~~~

You can load flags from the environment in addition to `argc` and `argv` by specifying a prefix to use when looking for flags:

~~~{.cpp}
    MyFlags flags;
    Try<flags::Warnings> load = flags.load("PREFIX_", argc, argv);
~~~

Then both PREFIX_foo and PREFIX_bar will be loaded from the environment as well as possibly from on the command line.

There are various ways to deal with unknown flags (i.e., `--baz` in our example above) and duplicates (i.e., `--foo` on the command line twice or once in the environment and once on the command line). See the header files for the various `load` overloads.



<a href="collections"></a>

## Collections

Many of the collections and containers provided either by Boost or the C++ standard are cumbersome to use or yield brittle, hard to maintain code. For many of the standard data structures we provide wrappers with modified interfaces often simplified or enhanced using types like `Try` and `Option`. These wrappers include `hashmap`, `hashset`, `multihashmap`, and `multimap`.

> NOTE: The collections are not namespaced.

`LinkedHashMap` is a hashmap that maintains the order in which the keys have been inserted. This allows both constant-time access to a particular key-value pair, as well as iteration over key-value pairs according to the insertion order.

There is also a `Cache` implementation that provides a templated implementation of a least-recently used (LRU) cache. Note that the key type must be compatible with `std::unordered_map`.

Finally, we provide some overloaded operators for doing set union (`|`), set intersection (`&`), and set appending (`+`) using `std::set`.

<a href="miscellaneous"></a>

## Miscellaneous

There are a handful of types and utilities that fall into the miscellaneous category. Note that like the collections _these are not namespaced_.


#### `Bytes`

Used to represent some magnitude of bytes, i.e., kilobytes, megabytes, gigabytes, etc. The main way to construct a `Bytes` is to invoke `Bytes::parse` which expects a string made up of a number and a unit, i.e., `42B`, `42MB`, `42GB`, `42TB`. For each of the supported units there are associated types: `Megabytes`, `Gigabytes`, `Terabytes`. Each of these types inherit from `Bytes` and can be used anywhere a `Bytes` is expected, for example:

~~~{.cpp}
    Try<Bytes> bytes = Bytes::parse("32MB");

    Bytes bytes = Megabytes(10);
~~~

There are operators for comparing (equal to, greater than or less than, etc) and manipulating (addition, subtraction, etc) `Bytes` objects, as well as a `std::ostream operator<<` overload (thus making them stringifiable, see [here](#stringify)). For example:

~~~{.cpp}
    Try<Bytes> bytes = Bytes::parse("32MB");

    Bytes tengb = Gigabytes(10);
    Bytes onegb = Megabytes(1024);

    stringify(tengb + onegb); // Yields "11GB".
~~~

#### `Duration`

Used to represent some duration of time. The main way to construct a `Duration` is to invoke `Duration::parse` which expects a string made up of a number and a unit, i.e., `42ns`, `42us`, `42ms`, `42secs`, `42mins`, `42hrs`, `42days`, `42weeks`. For each of the supported units there are associated types: `Nanoseconds`, `Microseconds`, `Milliseconds`, `Seconds`, `Minutes`, `Hours`, `Days`, `Weeks`. Each of these types inherit from `Duration` and can be used anywhere a `Duration` is expected, for example:

~~~{.cpp}
    Duration d = Seconds(5);
~~~

There are operators for comparing (equal to, greater than or less than, etc) and manipulating (addition, subtraction, etc) `Duration` objects, as well as a `std::ostream operator<<` overload (thus making them stringifiable, see [here](#stringify)). Note that the `std::ostream operator<<` overload formats the output (including the unit) based on the magnitude, for example:

~~~{.cpp}
    stringify(Seconds(42)); // Yields "42secs".
    stringify(Seconds(120)); // Yields "2mins".
~~~


#### `Stopwatch`

A data structure for recording elapsed time (according to the underlying operating system clock):

~~~{.cpp}
    Stopwatch stopwatch;
    stopwatch.start();

    Duration elapsed = stopwatch.elapsed();

    stopwatch.stop();

    assert(elapsed <= stopwatch.elapsed());
~~~


#### `UUID`

*Requires Boost.*

A wrapper around `boost::uuid` with a simpler interface.


#### `EXIT`

A macro for exiting an application without generating a signal (such as from `assert`) or a stack trace (such as from Google logging's `CHECK` family of macros). This is useful if you want to exit the program with an error message:

~~~{.cpp}
    EXIT(42) << "You've provided us bad input ...";
~~~

Note that a newline is automatically appended and the processes exit status is set to 42.


#### `foreach`

*Requires Boost.*

Macros for looping over collections:

~~~{.cpp}
    std::list<std::string> l;

    foreach (std::string s, l) {}
    foreach (const std::string& s, l) {}

    std::map<std::string, int> m;

    foreachpair (std::string s, int i, m) {}
    foreachpair (const std::string& s, int i, m) {}

    foreachkey (const std::string& s, m) {}
    foreachvalue (int i, m) {}
~~~


#### `numify`

*Requires Boost.*

Wraps `boost::lexical_cast` for converting strings to numbers but returns a `Try` rather than throwing exceptions.


<a href="stringify"></a>

#### `stringify`

Converts arbitrary types into strings by attempting to use an overloaded `std::ostream operator<<` (otherwise compilation fails). Note that `stringify` aborts the program if the stringification process fails.


#### `ThreadLocal`

*Requires pthreads.*

You can give every thread its own copy of some data using the `ThreadLocal` abstraction:

~~~{.cpp}
    ThreadLocal<std::string> local;
    local = new std::string("hello");
    local->append(" world");
    std::string* s = local;
    assert(*s == "hello world");
~~~


<a href="testing"></a>

## Testing


There are some macros provided for integration with gtest that make the tests less verbose while providing better messages when tests do fail:

~~~{.cpp}
    Try<int> t = foo();

    // Rather than:
    ASSERT(t.isSome()) << "Error: " << t.error();

    // Just do:
    ASSERT_SOME(t);
~~~

There available macros include `ASSERT_SOME`, `EXPECT_SOME`, `ASSERT_NONE`, `EXPECT_NONE`, `ASSERT_ERROR`, `EXPECT_ERROR`, `ASSERT_SOME_EQ`, `EXPECT_SOME_EQ`, `ASSERT_SOME_TRUE`, `EXPECT_SOME_TRUE`, `ASSERT_SOME_FALSE`, `EXPECT_SOME_FALSE`.


<a href="philosophy"></a>

## Philosophy

*"Premature optimization is the root of all evil."*

You'll notice that the library is designed in a way that can lead to a
lot of copying. This decision was deliberate. Capturing the semantics
of pointer ownership is hard to enforce programmatically unless you
copy, and in many instances these copies can be elided by an
optimizing compiler. We've chosen safety rather than premature
optimizations.

Note, however, that we plan to liberally augment the library as we add
C++11 support. In particular, we plan to use rvalue references and
std::unique_ptr (although, likely wrapped as Owned) in order to
explicitly express ownership semantics. Until then, it's unlikely that
the performance overhead incurred via any extra copying is your
bottleneck, and if it is we'd love to hear from you!


<a href="exceptions"></a>

### Exceptions

The library WILL NEVER throw exceptions and will attempt to capture
any exceptions thrown by underlying C++ functions and convert them
into an [Error](#error).
