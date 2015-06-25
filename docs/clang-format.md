---
layout: documentation
---

# ClangFormat

[ClangFormat](http://llvm.org/releases/3.5.1/tools/clang/docs/ClangFormat.html) is an automatic source code formatting tool which helps us focus on the code rather than the formatting.

> The provided configurations try to honor the [Mesos C++ Style Guide](http://mesos.apache.org/documentation/latest/mesos-c++-style-guide/) as much as possible, but there are some limitations which require manual attention. Even with these limitations however, ClangFormat will be extremely useful for your workflow!

## Setup

### Install `clang-format-3.5`

#### Ubuntu 14.04


    # Ensure apt-get is up to date.
    sudo apt-get update

    # Install clang-format-3.5
    sudo apt-get install clang-format-3.5


#### OS X Yosemite


    # Install clang-3.5. The binaries are suffixed with '-3.5', e.g. 'clang++-3.5'.
    $ brew install llvm35 --with-clang

    # Download and install the clang-format scripts.
    $ wget http://llvm.org/releases/3.5.1/cfe-3.5.1.src.tar.xz
    $ tar -xzf cfe-3.5.1.src.tar.xz
    $ cp cfe-3.5.1.src/tools/clang-format/clang-format* `brew --cellar`/llvm35/3.5.1/share/clang-3.5


### Formatting Configuration

By default, ClangFormat uses the configuration defined in a `.clang-format` or
`_clang-format` file located in the nearest parent directory of the input file.
The `bootstrap` script creates a `.clang-format` symlink at the top-level
directory which points to `support/clang-format` for ClangFormat to find.

> NOTE: If you're using `clang-format-3.6`, you'll need to add a new option
`BinPackArguments: false` to the `.clang-format` file!

### Editor Integration

#### Vim

Add the following to your `.vimrc`:

Ubuntu:

    map <C-K> :pyf /usr/share/vim/addons/syntax/clang-format-3.5.py<cr>
    imap <C-K> <c-o>:pyf /usr/share/vim/addons/syntax/clang-format-3.5.py<cr>

OS X:

    map <C-K> :pyf /usr/local/share/clang-3.5/clang-format.py<cr>
    imap <C-K> <c-o>:pyf /usr/local/share/clang-3.5/clang-format.py<cr>

The first line enables clang-format for `NORMAL` and `VISUAL` mode, the second line adds support for `INSERT` mode. Change `C-K` to another binding if you need clang-format on a different key (`C-K` stands for `Ctrl+k`).

With this integration you can press the bound key and clang-format will format the current line in `NORMAL` and `INSERT` mode or the selected region in `VISUAL` mode. The line or region is extended to the next bigger syntactic entity.

It operates on the current, potentially unsaved buffer and does not create or save any files. To revert a formatting, just undo.

> Source: http://llvm.org/releases/3.5.1/tools/clang/docs/ClangFormat.html

#### Emacs

Add the following to your `.emacs`:

Ubuntu:

    (load "/usr/share/emacs/site-lisp/clang-format-3.5/clang-format.el")
    (global-set-key [C-M-tab] 'clang-format-region)

OS X:

    (load "/usr/local/share/clang-3.5/clang-format.el")
    (global-set-key [C-M-tab] 'clang-format-region)

This binds the function `clang-format-region` to `C-M-tab`, which then formats the current line or selected region.

> Source: http://llvm.org/releases/3.5.1/tools/clang/docs/ClangFormat.html

## Known Limitations

* The braces after `namespace`s should not be wrapped.
* The braces after `struct`s and `union`s should be wrapped.
* Parameters and arguments should be indented by 4 spaces rather than 2.
* Overloaded operators should be padded with spaces.
  (e.g. `Foo operator + (...);`)
* Should not follow Google's style of wrapping on open parentheses, we should
  try to reduce "jaggedness" in the code.
