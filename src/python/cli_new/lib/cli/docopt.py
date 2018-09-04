# Licensed to the Apache Software Foundation (ASF) under one
# or more contributor license agreements.  See the NOTICE file
# distributed with this work for additional information
# regarding copyright ownership.  The ASF licenses this file
# to you under the Apache License, Version 2.0 (the
# "License"); you may not use this file except in compliance
# with the License.  You may obtain a copy of the License at
#
#     http://www.apache.org/licenses/LICENSE-2.0
#
# Unless required by applicable law or agreed to in writing, software
# distributed under the License is distributed on an "AS IS" BASIS,
# WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
# See the License for the specific language governing permissions and
# limitations under the License.

"""
Unfortunately, docopt doesn't support multi-word commands.  This is
important for supporting things like:

mesos cluster ps <args>...

However, it looks like some plans are in place for supporting it in the
future: https://github.com/docopt/docopt/issues/41

The proposal is to add a "program" keyword argument to docopt to specify the
full set of words used to represent the command. Since this is not yet
supported officially, we include the hack below to make it work for us. We
essentially intercept the call to docopt and make it work with such a "program"
argument.

To make it work, we inspect the value of "program" and search and replace all
instances of it in the usage string with a transformed version of it to make it
a single word (i.e. we replace all spaces with dashes: echo $program | s/
/-/g). This essentially turns all multi-word commands in the usage string into
dash-separated single words (e.g., s/mesos cluster ps/mesos-cluster-ps/g). With
this in place, we then pass this usage string to the original docopt for
parsing.

Unfortunately, doing things this way means that docopt (by default) will print
the usage string containing the dashes. To avoid this, we intercept all paths
where docopt does the printing itself, and transform the usage string back to
its original form.

Hopefully we can remove this brutal hack at some point in the future
once docopt supports the "program" argument natively.
"""

import os
import sys

# pylint: disable=import-error
from docopt import docopt as real_docopt, DocoptExit


def docopt(usage, **keywords):
    """ A wrapper around the real docopt parser. """
    new_usage = usage

    if "program" in keywords:
        program = keywords.pop("program")
        new_usage = usage.replace(program, program.replace(" ", "-"))

    try:
        stdout = sys.stdout

        with open(os.devnull, 'w') as nullfile:
            sys.stdout = nullfile
            arguments = real_docopt(new_usage, **keywords)
            sys.stdout = stdout

        return arguments

    except DocoptExit:
        sys.stdout = stdout
        print(usage.strip(), file=sys.stderr)
        sys.exit(1)

    except SystemExit:
        sys.stdout = stdout

        if "argv" in keywords and any(h in ("-h", "--help")
                                      for h in keywords["argv"]):
            print(usage.strip())
        elif "version" in keywords and any(v in ("--version")
                                           for v in keywords["argv"]):
            print(keywords["version"].strip())

        sys.exit()
