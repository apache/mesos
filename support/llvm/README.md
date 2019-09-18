# Mesos LLVM Tools

This is an overview of the Mesos-flavored LLVM Tools we maintain.
  - MesosFormat: [mesos/clang]
  - MesosTidy: [mesos/clang-tools-extra]

[mesos/clang]: https://github.com/mesos/clang
[mesos/clang-tools-extra]: https://github.com/mesos/clang-tools-extra

Statically-linked pre-built binaries are
  - __hosted__ on [Bintray][bintray]
  - __installed__ by [install.sh] in `<MESOS_DIR>/.git/llvm/<VERSION>`
  - __built__ by scripts at [mesos/llvm]

[bintray]: https://bintray.com/apache/mesos/llvm
[install.sh]: install.sh
[mesos/llvm]: https://github.com/mesos/llvm

## MesosFormat

[ClangFormat] is a Clang-based formatting tool. While the provided configuration
is close to the Mesos C++ Style Guide, we are not yet at a point where we take
the result produced by [ClangFormat] to be the final definitive answer.

The current expected workflow is to integrate it into your favorite editor
and format selected regions of code.

### Configuration

By default, [ClangFormat] uses the configuration defined in a `.clang-format`
file located in the nearest parent directory of the input file. The
`support/setup-dev.sh` script creates a `.clang-format` symlink at
`<MESOS_DIR>` which points to `support/clang-format` for [ClangFormat] to find.

### Integration

#### Vim

Add the following to your `.vimrc`:

```
let g:clang_format_path = "<MESOS_DIR>/.git/llvm/active/bin/clang-format"

map <C-K> :pyf <MESOS_DIR>/.git/llvm/active/share/clang/clang-format.py<CR>
imap <C-K> <C-O>:pyf <MESOS_DIR>/.git/llvm/active/share/clang/clang-format.py<CR>
```

Refer to https://clang.llvm.org/docs/ClangFormat.html#vim-integration

[ClangFormat]: https://clang.llvm.org/docs/ClangFormat.html

## MesosTidy

[ClangTidy] is a Clang-based linter tool.

### Configuration

By default, [ClangTidy] uses the configuration defined in a `.clang-tidy` file
located in the nearest parent directory of the input file. The
`./support/setup-dev.sh` script creates a `.clang-tidy` symlink at
`<MESOS_DIR>` which points to `support/clang-tidy` for [ClangTidy] to find.

### Invocation

The script `support/mesos-tidy.py` is a slightly modified version of
[`run-clang-tidy.py`][run-clang-tidy], a tool that runs `clang-tidy`
over all files in a compilation database.

[run-clang-tidy]: https://github.com/llvm-mirror/clang-tools-extra/blob/release_60/clang-tidy/tool/run-clang-tidy.py

Run the checks with a temporary compilation database:

```bash
./support/mesos-tidy.py 1> clang-tidy.log
```

Run the checks with an existing compilation database:

```bash
./support/mesos-tidy.py -p build 1> clang-tidy.log
```

Run the checks with an existing compilation database,
apply fix-its, and reformat the code after:

```
./support/mesos-tidy.py -p build -fix -format
```

[ClangTidy]: http://clang.llvm.org/extra/clang-tidy/
