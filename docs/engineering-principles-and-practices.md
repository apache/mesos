---
title: Apache Mesos - Engineering Principles And Practices
layout: documentation
---

# Engineering Principles and Practices

This document is an attempt to capture a shared set of values for the project.
Many companies rely on Mesos as a foundational layer of their software
infrastructure and it is imperative that we ship **robust, high quality**
code. We aim to foster a culture where we can trust and rely upon the work of
the community.

The following are some of the aspirational principles and practices that
guide us:

1. We value **craftsmanship**: code should be easy to read and understand,
   should be written with high attention to detail, and should be consistent
   in its style. Code is written for humans to read and maintain!
2. We value **resiliency**: our system must be highly-available, stable, and
   backwards-compatible. We must consider the implications of failures.
3. We value **accountability**: we own and support our software, and are
   accountable for improving it, fixing issues, and learning from our mistakes.
4. We value **design and code review**: review helps us maintain a high
   quality system architecture and high quality code, it also helps us mentor
   new contributors, learn to collaborate more effectively, and reduce the
   amount of mistakes.
5. We value **automated testing**: automated testing allows us to iterate and
   refactor in our large codebase, while verifying correctness and preventing
   regression.
6. We value **benchmarking**: benchmarking allows us to identify the right
   locations to target performance improvements. It also allows us to iterate
   and refactor in our large codebase, while observing the performance
   implications.
