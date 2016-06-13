---
title: Apache Mesos - Documentation Guide
layout: documentation
---

# Mesos Documentation Guide

Documentation is an integral part of every good feature. It describes the intended usage and enables new users to start using and understanding the feature.

We have three different kinds of documentation:

1. [MarkDown User Guides](markdown-style-guide.md)

  User guides and non-code technical documentation are stored in markdown files in the `docs/` folder. These files get rendered for the [online documentation](http://mesos.apache.org/documentation/latest/).

  We will accept small documentation changes on [Github via a pull request](https://github.com/apache/mesos). Larger documentation changes should go through the [reviewboard](https://reviews.apache.org/groups/mesos/).

2. [Doxygen API Documentation and Developer Guides as part of source code](doxygen-style-guide.md)

  Doxygen API documentation needs only to be applied to source code parts that
  constitute an interface for which we want to generate Mesos API documentation
  files. Implementation code that does not participate in this should still be
  enhanced by source code comments as appropriate, but these comments should not follow the doxygen style.

  Substantial libraries, components, and subcomponents of the Mesos system such as
  stout, libprocess, master, agent, containerizer, allocator, and others
  should have an overview page in markdown format that explains their
  purpose, overall structure, and general use. This can even be a complete developer guide.

3. Regular source code documentation

  All other source code comments must follow the [Google Style Guide](https://google.github.io/styleguide/cppguide.html#Comments).


## Conventions

We follow the [IETF RFC 2119](https://www.ietf.org/rfc/rfc2119.txt)
on how to use words such as "must", "should", "can",
and other requirement-related notions.
