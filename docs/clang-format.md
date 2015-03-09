---
layout: documentation
---

# ClangFormat

[ClangFormat](http://llvm.org/releases/3.5.0/tools/clang/docs/ClangFormat.html) is an automatic source code formatting tool which help us focus on the code rather than the formatting.

> The provided configurations try to honor the [Mesos C++ Style Guide](http://mesos.apache.org/documentation/latest/mesos-c++-style-guide/) as much as possible, but there are some limitations. Even with these limitations however, ClangFormat will likely be extremely useful for your workflow!

## Quick Start

If you already have an installation of `clang-format` version >= 3.5, follow your editor's [integration instructions](#integrate-with-your-editor) then start formatting!

## Setup

### Install clang 3.5

#### Ubuntu 14.04

```bash
# Ensure apt-get is up to date.
sudo apt-get update

# Install clang-format-3.5
sudo apt-get install clang-format-3.5
```

#### OS X Yosemite

```bash
# Download and extract the clang-3.5 source.
$ wget http://llvm.org/releases/3.5.0/cfe-3.5.0.src.tar.xz
$ tar -xzf cfe-3.5.0.src.tar.xz

# Download and extract the clang-3.5 pre-built binaries.
$ wget http://llvm.org/releases/3.5.0/clang+llvm-3.5.0-macosx-apple-darwin.tar.xz
$ tar -xzf clang+llvm-3.5.0-macosx-apple-darwin.tar.xz

# Create a directory for clang.
$ mkdir `brew --cellar`/clang

# Install the pre-built binaries.
$ mv clang+llvm-3.5.0-macosx-apple-darwin `brew --cellar`/clang/3.5

# Install the clang-format tools.
$ mkdir `brew --cellar`/clang/3.5/share/clang
$ cp cfe-3.5.0.src/tools/clang-format/clang-format* `brew --cellar`/clang/3.5/share/clang

# Link!
$ brew link clang
Linking /usr/local/Cellar/clang/3.5... 491 symlinks created

# You can delete cleanly by running `brew uninstall clang`
```

### Integrate with your editor

#### Vim

Add the following to your `.vimrc`:

Ubuntu:

```
map <C-K> :pyf /usr/share/vim/addons/syntax/clang-format-3.5.py<cr>
imap <C-K> <c-o>:pyf /usr/share/vim/addons/syntax/clang-format-3.5.py<cr>
```

OS X:

```
map <C-K> :pyf /usr/local/share/clang/clang-format.py<cr>
imap <C-K> <c-o>:pyf /usr/local/share/clang/clang-format.py<cr>
```

The first line enables clang-format for `NORMAL` and `VISUAL` mode, the second line adds support for `INSERT` mode. Change `C-K` to another binding if you need clang-format on a different key (`C-K` stands for `Ctrl+k`).

With this integration you can press the bound key and clang-format will format the current line in `NORMAL` and `INSERT` mode or the selected region in `VISUAL` mode. The line or region is extended to the next bigger syntactic entity.

It operates on the current, potentially unsaved buffer and does not create or save any files. To revert a formatting, just undo.

> Source: http://llvm.org/releases/3.5.0/tools/clang/docs/ClangFormat.html

#### Emacs

Add the following to your `.emacs`:

Ubuntu:

```
(load "/usr/share/emacs/site-lisp/clang-format-3.5/clang-format.el")
(global-set-key [C-M-tab] 'clang-format-region)
```

OS X:

```
(load "/usr/local/share/clang/clang-format.el")
(global-set-key [C-M-tab] 'clang-format-region)
```

This binds the function `clang-format-region` to `C-M-tab`, which then formats the current line or selected region.

> Source: http://llvm.org/releases/3.5.0/tools/clang/docs/ClangFormat.html

## Known Limitations

* The braces after `namespace`s should not be wrapped.
* The braces after `struct`s and `union`s should be wrapped.
* Parameters and arguments should be indented by 4 spaces rather than 2.
* Overloaded operators should be padded with spaces.
  (e.g. `Foo operator + (...);`)
* Should not follow Google's style of wrapping on open parentheses, we should
  try to reduce "jaggedness" in the code.
