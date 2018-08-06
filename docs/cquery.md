---
title: Apache Mesos - Using cquery for Code Navigation
layout: documentation
---

# Using cquery for Code Navigation

Instead of using `grep` and other tools to find your way through the
Mesos codebase, you can use [cquery](https://github.com/cquery-project/cquery)
on Windows, Linux, and macOS!

Tested and designed for large code bases like Chromium, cquery
provides accurate and fast semantic analysis for any editor that
supports the [Language Server Protocol](https://microsoft.github.io/language-server-protocol/).

Using cquery provides IDE features like these (and more):

* Find definitions and references
* Contextual completion candidates
* On-the-fly syntax checking
* Preprocessor skipped regions
* Symbol documentation
* Hover information

Although the cquery wiki provides a
[getting started guide](https://github.com/cquery-project/cquery/wiki/Getting-started)
for building from source, generating the compilation information for
your project, and setting up your editor, this guide covers setup
specifically for Mesos using CMake. Feel free to refer to the wiki for
further information, but what follows is the recommended setup.

NOTE: *Do not* use the released binaries as the latest, v20180302, is
still built Clang 5 instead of Clang 6, which is buggy with Mesos.

## Building cquery from source

The cquery project is currently switching to CMake, but the guide
still uses the `waf` build tool. Since we need to use CMake for Mesos
too, it easier to use it for both. More information can be found
[on the wiki](https://github.com/cquery-project/cquery/wiki/Build-%28CMake%29):

1. Install CMake following the instructions [here](cmake.md).
2. Install [Ninja](https://ninja-build.org/) by downloading the latest
   release for your platform and placing it in your path (optional for
   non-Windows platforms, but highly recommended).
3. If you're on Windows, make sure build in an "x64 Native Tools
   Command Prompt for VS 2017".

```sh
git clone --recursive https://github.com/cquery-project/cquery
cd cquery && mkdir build && cd build
cmake .. -GNinja -DCMAKE_BUILD_TYPE=Release
ninja
```

There should now exist a binary `cquery` (or `cquery.exe` on Windows)
in the build folder of the cquery repo. When configuring your editor,
you need to make sure this can either be found automatically via your
`PATH`, or point the editor's plugin toward it.

## Generating compilation information for Mesos

The next step is to generate a `compile_commands.json` file for Mesos.
Fortunately, this can be done automatically using CMake. In fact, the
instructions are (almost) identical to the instructions above. Once
generated, either symlink or copy it to the root of the Mesos
repository.

```sh
git clone https://gitbox.apache.org/repos/asf/mesos.git
cd mesos && mkdir build && cd build
cmake .. -GNinja -DCMAKE_EXPORT_COMPILE_COMMANDS=YES
ninja
cd ..
ln -s build/compile_commands.json .
```

The CMake option
[`CMAKE_EXPORT_COMPILE_COMMANDS`](https://cmake.org/cmake/help/latest/variable/CMAKE_EXPORT_COMPILE_COMMANDS.html)
also supports the Makefile generator on Linux, if you wish to use it
instead of Ninja, but the author has not tested this scenario, and
recommends Ninja anyway as it builds faster. On Windows, Ninja is the
only generator which supports exporting the compilation commands.

Note that for cquery to work properly, an initial build must be
completed because of our third-party dependencies, otherwise many of
the project's required headers will be missing.

## Setting up your editor

Finally, your editor's cquery / LSP plugin needs to be set up.
Information for other editors can be found at
[Langserver.org](https://langserver.org/). Once you have LSP setup, it
can be used for other languages too by switching out the cquery
backend with one specific for the language.

### Emacs

For Emacs, the packages [lsp-mode](https://github.com/emacs-lsp/lsp-mode) and
[lsp-ui](https://github.com/emacs-lsp/lsp-ui) are recommended.

A sample (but complete) Emacs configuration which sets up syntax
checking and auto-completions with LSP and cquery looks like this:

```elisp
;; Generic Emacs package repo setup
(require 'package)
(customize-set-variable
 'package-archives
 '(("melpa" . "https://melpa.org/packages/")
   ("gnu" . "https://elpa.gnu.org/packages/")))
(package-initialize)

;; Used to install and configure Emacs packages.
;; I forgot the old way since using this.
;; https://github.com/jwiegley/use-package
(unless (package-installed-p 'use-package)
  (package-refresh-contents)
  (package-install 'use-package))

(eval-when-compile
  (require 'use-package))

;; Syntax checking.
;; Should be automatic.nn
;; http://www.flycheck.org/en/latest/
(use-package flycheck
  :ensure t
  :config
  (global-flycheck-mode))

;; Auto-completions.
;; There's also `C-M-i`, but this is async.
;; Also look at `company-flx` for better sorting.
;; https://company-mode.github.io/
(use-package company
  :ensure t
  :config
  (global-company-mode))

;; Language Server Protocol Plugin.
;; The actual plugin used to communicate with cquery.
;; https://github.com/emacs-lsp/lsp-mode
(use-package lsp-mode
  :ensure t)

;; Flycheck and other IDE-feature support for LSP.
;; This has the "fancy features" and should be customized.
;; Personally, I turned the symbol highlighting off.
;; https://github.com/emacs-lsp/lsp-ui
(use-package lsp-ui
  :ensure t
  :config
  (add-hook 'lsp-mode-hook #'lsp-ui-mode))

;; LSP backend for Company.
;; https://github.com/tigersoldier/company-lsp
(use-package company-lsp
n  :ensure t
  :config
  (setq company-lsp-enable-recompletion t)
  (add-to-list 'company-backends 'company-lsp))

;; Client to configure and auto-start cquery.
;; https://github.com/cquery-project/emacs-cquery
(use-package cquery
  :ensure t
  :config
  (add-hook 'c-mode-common-hook #'lsp-cquery-enable)
  (setq cquery-executable "/path/to/cquery/build/cquery")
  (setq cquery-extra-init-params '(:completion (:detailedLabel t))))
```

Being Emacs, feel free to customize to your liking. The author's
configurations can be found [here](https://github.com/andschwa/.emacs.d).
While auto completion, syntax checking, and other UI improvements are
automatic, you should also be aware of `M-.` for
`xref-find-definitions`; `M-?` for `xref-find-references`; `M-,` to
pop back before using `xref`; `imenu` to list an index of functions,
namespaces, etc.; and the customization options of
[lsp-ui](https://github.com/emacs-lsp/lsp-ui), as it will
automatically turn on a sideline and symbol highlighting, which can be
noisy.

### Vim

An [LSP plugin](https://github.com/prabirshrestha/vim-lsp) exists; if
you set it up, please add the steps here.

### Visual Studio Code

A [cquery specific extension](https://marketplace.visualstudio.com/items?itemName=cquery-project.cquery)
exists; if you set it up, please add the steps here.

### Sublime Text

An [LSP package](https://github.com/tomv564/LSP) exists; if you set it
up, please add the steps here.
