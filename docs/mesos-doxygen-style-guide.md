# Apache Mesos Doxygen Style Guide

This guide introduces a consistent style
for [documenting Mesos source code](http://mesos.apache.org/api/latest/c++)
using [Doxygen](http://www.doxygen.org).
There is an ongoing, incremental effort with the goal to document all public Mesos, libprocess, and stout APIs this way.
For now, existing code may not follow these guidelines, but new code should.

## Preliminaries

We follow the [IETF RFC2119](https://www.ietf.org/rfc/rfc2119.txt)
on how to use words such as "must", "should", "can",
and other requirement-related notions.


## Building Doxygen Documentation
As of right now, the Doxygen documentation should be built from the *build* subdirectory using *doxygen ../Doxyfile* . The documentation will then be generated into the *./doxygen* subdirectory.
Todo: We should create a regular make target.


## Doxygen Tags
*When following these links be aware that the doxygen documentation is using another syntax in that @param is explained as \param.*

* [@param](http://doxygen.org/manual/commands.html#cmdparam) Describes function parameters.
* [@return](http://doxygen.org/manual/commands.html#cmdreturn) Describes return values.
* [@see](http://doxygen.org/manual/commands.html#cmdsa) Describes a cross-reference to classes, functions, methods, variables, files or URL.
* [@file](http://doxygen.org/manual/commands.html#cmdfile) Describes a refence to a file. It is required when documenting global functions, variables, typedefs, or enums in separate files.
* [@link](http://doxygen.org/manual/commands.html#cmdlink) and [@endlink](http://doxygen.org/manual/commands.html#cmdendlink) Describes a link to a file, class, or member.
* [@example](http://doxygen.org/manual/commands.html#cmdexample) Describes source code examples.
* [@todo](http://doxygen.org/manual/commands.html#cmdtodo) Describes a TODO item.
* [@image](http://doxygen.org/manual/commands.html#cmdimage) Describes an image.

## Wrapping
We wrap long descriptions using 4 spaces on the next line.
~~~
@param uncompressed The input string that requires
    a very long description and an even longer
    description on this line as well.
~~~


## Outside Source Code

### Library and Component Overview Pages and User Guides

Substantial libraries, components, and subcomponents of the Mesos system such as
stout, libprocess, master, slave, containerizer, allocator, and others
should have an overview page in markdown format that explains their
purpose, overall structure, and general use. This can even be a complete user guide.

This page must be located in the top directory of the library/component and named "REAMDE.md".

The first line in such a document must be a section heading bearing the title which will appear in the generated Doxygen index.
Example: "# Libprocess User Guide"

## In Source Code

Doxygen documentation needs only to be applied to source code parts that
constitute an interface for which we want to generate Mesos API documentation
files. Implementation code that does not participate in this should still be
enhanced by source code comments as appropriate, but these comments should not follow the doxygen style.

We follow the [Javadoc syntax](http://en.wikipedia.org/wiki/Javadoc) to mark comment blocks.
These have the general form:

    /**
     * Brief summary.
     *
     * Detailed description. More detail.
     * @see Some reference
     *
     * @param <name> Parameter description.
     * @return Return value description.
     */

Example:

    /**
     * Returns a compressed version of a string.
     *
     * Compresses an input string using the foobar algorithm.
     *
     * @param uncompressed The input string.
     * @return A compressed version of the input string.
     */
     std::string compress(const std::string& uncompressed);

### Constants and Variables

### Functions

### Classes

#### Methods

#### Fields

### Templates

### Macros

### Global declarations outside classes
