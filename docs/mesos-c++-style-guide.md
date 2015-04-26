---
layout: documentation
---

# Mesos C++ Style Guide

The Mesos codebase follows the [Google C++ Style Guide](http://google-styleguide.googlecode.com/svn/trunk/cppguide.xml) with the following differences:

## Naming

### Variable Names
* We use [lowerCamelCase](http://en.wikipedia.org/wiki/CamelCase#Variations_and_synonyms) for variable names (Google uses snake_case, and their class member variables have trailing underscores).
* We prepend constructor and function arguments with a leading underscore to avoid ambiguity and / or shadowing:

```
Try(State _state, T* _t = NULL, const std::string& _message = "")
  : state(_state), t(_t), message(_message) {}
```

* Prefer trailing underscores for use as member fields (but not required). Some trailing underscores are used to distinguish between similar variables in the same scope (think prime symbols), *but this should be avoided as much as possible, including removing existing instances in the code base.*

* If you find yourself creating a copy of an argument passed by const reference, consider passing it by value instead (if you don't want to use a leading underscore and copy in the body of the function):

```
// You can pass-by-value in ProtobufProcess::install() handlers.
void Slave::statusUpdate(StatusUpdate update, const UPID& pid)
{
  ...
  update.mutable_status()->set_source(
      pid == UPID() ? TaskStatus::SOURCE_SLAVE : TaskStatus::SOURCE_EXECUTOR);
  ...
}
```


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

```
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
```

### Continuation
* Newline for an assignment statement: indent with 2 spaces.

```
Try&lt;Duration&gt; failoverTimeout =
  Duration::create(FrameworkInfo().failover_timeout());
```

## Empty Lines
* 1 blank line at the end of the file.
* Elements outside classes (classes, structs, global functions, etc.) should be spaced apart by 2 blank lines.
* Elements inside classes (member variables and functions) should not be spaced apart by more than 1 blank line.

## C++11

We still support older compilers. The whitelist of supported C++11 features is:

* Static assertions.
* Multiple right angle brackets.
* Type inference (`auto` and `decltype`). The main goal is to increase code readability. This is safely the case if the exact same type omitted on the left is already fully stated on the right. Here are several examples:

```
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
Try&lt;Owned&lt;LocalAuthorizer>> authorizer = LocalAuthorizer::create();
```

* Rvalue references.
* Variadic templates.
* Mutexes.
  * `std::mutex`
  * `std::lock_guard<std::mutex>`
  * `std::unique_lock<std::mutex>`
* Shared from this.
  * `class T : public std::enable_shared_from_this<T>`
  * `shared_from_this()`
