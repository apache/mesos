---
title: Apache Mesos - Markdown Style Guide
layout: documentation
---

# Mesos Markdown Style Guide

This guide introduces a consistent documentation style to be used across the entire non-code documentation.
User guides and non-code technical documentation are stored in markdown files in the `docs/` folder. These files get rendered for the [online documentation](http://mesos.apache.org/documentation/latest/).

**NOTE:** As of right now this is work in progress and the existing documentation might not yet comply to this style.


## What to Document?

Any new substantial feature should be documented in its own markdown file.
If the link between source code and documentation is not obvious, consider inserting a short code comment stating that there is non-code documentation that needs to be kept in sync and indicating where it is located.


## Keep Documentation and Style Guides in Sync with Code.

When changing code consider whether you need to update the documentation.
This is especially relevant when introducing new or updating existing command line flags.
These should be reflected in `configuration.md`!


## Section Headings

Use [title case](https://en.wikipedia.org/wiki/Capitalization#Title_case) for
section headings, preferably the
[APA style](http://blog.apastyle.org/apastyle/headings/) variant:

* Capitalize the first word of any heading, including title, subtitle,
  subheading.
* Capitalize all "major" words (nouns, verbs, adjectives, adverbs, and pronouns)
  in any heading, including the second part of hyphenated major words (e.g.,
  Self-Report not Self-report).
* Capitalize all words of five letters or more.

Effectively, only "minor" words of four letters or fewer, namely, conjunctions
(words like __and__, __or__, __nor__, and __but__), articles (the words __a__,
__an__, and __the__), and prepositions (words like __as__, __at__, __by__,
__for__, __from__, __in__, __of__, __on__, __per__, __to__, __with__), are
lowercased in any heading, as long as they aren't the first word.


## Code Examples

Code examples should be specified as follows:

    ~~~{.cpp}
    int main(int argc, char** argv)
    {
      ....
    }
    ~~~

**NOTE:** Because of shortcomings of Doxygen's markdown parser we currently use indentation for wrapping all non C++ code blocks.


## Notes/Emphasis

Notes are used to highlight important parts of the text and should be specified as follows.

~~~{.txt}
**NOTE:**  Short note.
Continued longer note.
~~~

We use single backticks to highlight individual words in a sentence such as certain identifiers:

~~~{.txt}
Use the default `HierarchicalDRF` allocator....
~~~


## Commands

We use single backticks to highlight sample commands as follows:

~~~{.txt}
`mesos-master --help`
~~~


## Files/Paths

Files and path references should be specified as follows:

~~~{.txt}
Remember you can also use the `file:///path/to/file` or `/path/to/file`
~~~


## Tables

In order to avoid problems with markdown formatting we should specify tables in html directly:

    <table class="table table-striped">
      <thead>
        <tr>
          <th width="30%">
            Flag
          </th>
          <th>
            Explanation
          </th>
      </thead>
      <tr>
        <td>
          --ip=VALUE
        </td>
        <td>
          IP address to listen on
        </td>
      </tr>
      <tr>
        <td>
          --[no-]help
        </td>
        <td>
          Prints this help message (default: false)
        </td>
      </tr>
    </table>


## Indentation and Whitespace

We use no extra indentation in markdown files.
We have one new line after section headings and two blank lines
in between sections.

~~~{.txt}
... end of previous section.


## New Section

Beginning of new section ....
~~~
