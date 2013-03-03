We migrated the Mesos build system on Jan 19th 2012 to using Autotools (SVN commit #1233580, which is equivalent to Git-SVN commit #ebaf069611abf23266b009c3516da4b3cccccb8d). If you are using a version of Mesos from before that commit, checked out from Apache SVN (possibly via git-svn), then you probably need to follow these build instructions:

<b><font size="4">1) Run one of the configure template scripts</font></b>

**NOTE:** do not simply run `./configure` without arguments. If you do, your build will fail due to a known issue (see [[MESOS-103|https://issues.apache.org/jira/browse/MESOS-103]] for more details).

We recommend you use one of the configure.template scripts in the root directory, which will call the more general `configure` script and pass it appropriate arguments. E.g. if you are using OS X, run ./configure.template.macosx.

These configure template scripts contain guesses for the Java and Python paths for distribution indicated in their names (e.g. Mac OS X and several Linux distributions). They assume that you have already installed the packages (i.e. `python-dev` and a JDK). You should double check the configure template script you use (they are just shell scripts, i.e. text files) to make sure the paths it is using for Python and Java match what you have installed. for example, make sure that if you have installed Sun's Java 1.6, your configure template script is not setting JAVA_HOME to be for openjdk.

Advanced users may wish to run `./configure` directly with their own combination of flag options (see [[Mesos Configure Command Flag Options]]).

<b><font size="4">2) Run `make`</font></b>

#### NOTES:
* If you get errors with `pushd` not working on Ubuntu, this is because /bin/sh is a link to /bin/dash, not /bin/bash. To fix, do: `sudo ln -fs /bin/bash /bin/sh` (this bug has been fixed in [MESOS-50](https://issues.apache.org/jira/browse/MESOS-50), so if you are seeing it, consider upgrading to a newer version of Mesos)