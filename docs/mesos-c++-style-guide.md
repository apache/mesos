---
layout: documentation
---

# Mesos C++ Style Guide

The Mesos codebase follows the [Google C++ Style Guide](http://google-styleguide.googlecode.com/svn/trunk/cppguide.xml) with the following differences:

## Naming

### Variable Names
* We use [lowerCamelCase](http://en.wikipedia.org/wiki/CamelCase#Variations_and_synonyms) for variable names (Google uses snake_case, and their class member variables have trailing underscores).

### Constant Names
* We use lowerCamelCase for constant names (Google uses a `k` followed by mixed case, e.g. `kDaysInAWeek`).

### Function Names
* We use lowerCamelCase for function names (Google uses mixed case for regular functions; and their accessors and mutators match the name of the variable).

## Strings
* Strings used in log and error messages should end without a period.

## Comments
* End each sentence with a period.
* At most 70 characters per line in comments.

## Indentation
* Newline when calling or defining a function: indent with 4 spaces.
* We do not follow Google's style of wrapping on the open parenthesis, the general goal is to reduce visual "jaggedness" in the code. Prefer (1), (4), (5), sometimes (3), never (2):

<pre>
// 1: OK.
allocator->resourcesUnused(frameworkId, slaveId, resources, filters);

// 2: Don't use.
allocator->resourcesUnused(frameworkId, slaveId,
                           resources, filters);

// 3: Don't use in this case due to "jaggedness".
allocator->resourcesUnused(frameworkId,
                           slaveId,
                           resources,
                           filters);

// 3: In this case, 3 is OK.
foobar(someArgument,
       someOtherArgument,
       theLastArgument);

// 4: OK.
allocator->resourcesUnused(
    frameworkId,
    slaveId,
    resources,
    filters);

// 5: OK.
allocator->resourcesUnused(
    frameworkId, slaveId, resources, filters);
</pre>

* Newline for an assignment statement: indent with 2 spaces.

<pre>
Try&lt;Duration&gt; failoverTimeout =
  Duration::create(FrameworkInfo().failover_timeout());
</pre>

## New Lines
* 1 blank line at the end of the file.
* Elements outside classes (classes, structs, global functions, etc.) should be spaced apart by 2 blank lines.
* Elements inside classes (member variables and functions) should not be spaced apart by more than 1 blank line.