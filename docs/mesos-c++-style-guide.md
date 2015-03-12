---
layout: documentation
---

# Mesos C++ Style Guide

The Mesos codebase follows the [Google C++ Style Guide](http://google-styleguide.googlecode.com/svn/trunk/cppguide.xml) with the following differences:

## Naming

### Variable Names
* We use [lowerCamelCase](http://en.wikipedia.org/wiki/CamelCase#Variations_and_synonyms) for variable names (Google uses snake_case, and their class member variables have trailing underscores).

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
* End each sentence with a period.
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

<pre>
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
</pre>

### Continuation
* Newline for an assignment statement: indent with 2 spaces.

<pre>
Try&lt;Duration&gt; failoverTimeout =
  Duration::create(FrameworkInfo().failover_timeout());
</pre>

## Empty Lines
* 1 blank line at the end of the file.
* Elements outside classes (classes, structs, global functions, etc.) should be spaced apart by 2 blank lines.
* Elements inside classes (member variables and functions) should not be spaced apart by more than 1 blank line.

## C++11

We still support older compilers. The whitelist of supported C++11 features is:

* Static assertions.
* Multiple right angle brackets.
* Type inference (`auto` and `decltype`). The main goal is to increase code readability. Here are several examples:

<pre>
// 1: OK.
const auto& i = values.find(keys.front());
// Compare with
const typename map::iterator& i = values.find(keys.front());

// 2: Don't use.
auto authorizer = LocalAuthorizer::create(acls);
// Compare with
Try&lt;Owned&lt;LocalAuthorizer>> authorizer = LocalAuthorizer::create();
</pre>

* Rvalue references.
* Variadic templates.
* Mutexes.
    * std::mutex.
    * std::lock_guard<std::mutex>.
    * std::unique_lock<std::mutex>.
* Shared from this.
    * class T : public std::enable_shared_from_this<T>.
    * shared_from_this().
