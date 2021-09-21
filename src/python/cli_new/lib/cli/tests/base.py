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
Set of classes and helper functions for building unit tests for the Mesos CLI.
"""

import io
import os
import pty
import shutil
import subprocess
import sys
import tempfile
import unittest

import parse

from tenacity import retry
from tenacity import stop_after_delay
from tenacity import wait_fixed

from cli import http, config

from cli.tests.constants import TEST_AGENT_IP
from cli.tests.constants import TEST_AGENT_PORT
from cli.tests.constants import TEST_MASTER_IP
from cli.tests.constants import TEST_MASTER_PORT

from cli.exceptions import CLIException

# Timeout used when creating, killing, and getting data from objects that are
# part of our test infrastructure.
TIMEOUT = 5


class CLITestCase(unittest.TestCase):
    """
    Base class for CLI TestCases.
    """

    @classmethod
    def setUpClass(cls):
        print("\n{class_name}".format(class_name=cls.__name__))

    @staticmethod
    def default_mesos_build_dir():
        """
        Returns the default path of the Mesos build directory. Useful when
        we wish to use some binaries such as mesos-execute.
        """
        tests_dir = os.path.dirname(__file__)
        cli_dir = os.path.dirname(tests_dir)
        lib_dir = os.path.dirname(cli_dir)
        cli_new_dir = os.path.dirname(lib_dir)
        python_dir = os.path.dirname(cli_new_dir)
        src_dir = os.path.dirname(python_dir)
        mesos_dir = os.path.dirname(src_dir)
        build_dir = os.path.join(mesos_dir, "build")

        if os.path.isdir(build_dir):
            return build_dir
        raise CLIException("The Mesos build directory"
                           " does not exist: {path}"
                           .format(path=build_dir))

# This value is set to the correct path when running tests/main.py. We
# set it here to make sure that CLITestCase has a MESOS_BUILD_DIR member.
CLITestCase.MESOS_BUILD_DIR = ""


class Executable():
    """
    This class defines the base class for launching an executable for
    the CLI unit tests. It will be subclassed by (at least) a
    'Master', 'Agent', and 'Task' subclass.
    """
    def __init__(self):
        self.name = ""
        self.executable = ""
        self.shell = False
        self.flags = {}
        self.proc = None

    def __del__(self):
        if hasattr(self, "proc") and self.proc is not None:
            self.kill()

    def launch(self):
        """
        Launch 'self.executable'. We assume it is in the system PATH.
        """
        if self.proc is not None:
            raise CLIException("{name} already launched"
                               .format(name=self.name.capitalize()))

        if not os.path.exists(self.executable):
            raise CLIException("{name} executable not found"
                               .format(name=self.name.capitalize()))

        try:
            flags = ["--{key}={value}".format(key=key, value=value)
                     for key, value in dict(self.flags).items()]

            if self.shell:
                cmd = ["/bin/sh", self.executable] + flags
            else:
                cmd = [self.executable] + flags

            proc = subprocess.Popen(
                cmd,
                stdin=subprocess.PIPE,
                stdout=subprocess.PIPE,
                stderr=subprocess.STDOUT)
        except Exception as exception:
            raise CLIException("Failed to launch '{executable}': {error}"
                               .format(executable=self.executable,
                                       error=exception))

        if proc.poll():
            raise CLIException("Failed to launch '{executable}': {error}"
                               .format(executable=self.executable,
                                       error=proc.stdout.read()))

        self.proc = proc

    def kill(self):
        """
        Kills a previously launched executable.
        """
        if self.proc is None:
            return

        try:
            self.proc.stdin.close()
            self.proc.stdout.close()
            self.proc.kill()
            self.proc.wait()
            self.proc = None
        except Exception as exception:
            raise CLIException("Could not kill {name}: {error}"
                               .format(name=self.name, error=exception))


class Master(Executable):
    """
    This class defines the functions necessary to launch a master in
    the CLI unit tests.
    """
    count = 0

    def __init__(self, flags=None):
        super(Master, self).__init__()

        if Master.count > 0:
            raise CLIException("Creating more than one master"
                               " is currently not possible")

        if flags is None:
            flags = {}

        if "ip" not in flags:
            flags["ip"] = TEST_MASTER_IP
        if "port" not in flags:
            flags["port"] = TEST_MASTER_PORT
        if "work_dir" not in flags:
            flags["work_dir"] = tempfile.mkdtemp()

        self.flags = flags
        self.name = "master"
        self.addr = "{ip}:{port}".format(ip=flags["ip"], port=flags["port"])
        self.config = config.Config(None)
        self.executable = os.path.join(
            CLITestCase.MESOS_BUILD_DIR,
            "bin",
            "mesos-{name}.sh".format(name=self.name))
        self.shell = True

    def __del__(self):
        super(Master, self).__del__()

        if hasattr(self, "flags") and hasattr(self.flags, "work_dir"):
            shutil.rmtree(self.flags["work_dir"])

    # pylint: disable=arguments-differ
    def launch(self):
        """
        After starting the master, we need to make sure its
        reference count is increased.
        """
        super(Master, self).launch()
        Master.count += 1

    def kill(self):
        """
        After killing the master, we need to make sure its
        reference count is decreased.
        """
        super(Master, self).kill()
        Master.count -= 1


class Agent(Executable):
    """
    This class defines the functions necessary to launch an agent in
    the CLI unit tests.
    """
    count = 0

    def __init__(self, flags=None):
        super(Agent, self).__init__()

        if Agent.count > 0:
            raise CLIException("Creating more than one agent"
                               " is currently not possible")

        if flags is None:
            flags = {}

        if "ip" not in flags:
            flags["ip"] = TEST_AGENT_IP
        if "port" not in flags:
            flags["port"] = TEST_AGENT_PORT
        if "master" not in flags:
            flags["master"] = "{ip}:{port}".format(
                ip=TEST_MASTER_IP,
                port=TEST_MASTER_PORT)
        if "work_dir" not in flags:
            flags["work_dir"] = tempfile.mkdtemp()
        if "runtime_dir" not in flags:
            flags["runtime_dir"] = tempfile.mkdtemp()
        # Disabling systemd support on Linux to run without sudo.
        if "linux" in sys.platform and "systemd_enable_support" not in flags:
            flags["systemd_enable_support"] = "false"

        self.flags = flags
        self.name = "agent"
        self.addr = "{ip}:{port}".format(ip=flags["ip"], port=flags["port"])
        self.config = config.Config(None)
        self.executable = os.path.join(
            CLITestCase.MESOS_BUILD_DIR,
            "bin",
            "mesos-{name}.sh".format(name=self.name))
        self.shell = True

    def __del__(self):
        super(Agent, self).__del__()

        if hasattr(self, "flags") and hasattr(self.flags, "work_dir"):
            shutil.rmtree(self.flags["work_dir"])
        if hasattr(self, "flags") and hasattr(self.flags, "runtime_dir"):
            shutil.rmtree(self.flags["runtime_dir"])

    # pylint: disable=arguments-differ
    def launch(self):
        """
        After starting the agent, we first need to make sure its
        reference count is increased and then check that it has
        successfully registered with the master before proceeding.
        """
        super(Agent, self).launch()
        Agent.count += 1

        data = http.get_json(self.flags["master"], "slaves", self.config)
        if len(data["slaves"]) == 1:
            stdout = ""
            if self.proc.poll():
                stdout = "\n{output}".format(output=self.proc.stdout.read())

            raise CLIException("Could not get '/slaves' endpoint as JSON with"
                               " only 1 agent in it: {stdout}"
                               .format(stdout=stdout))

    # pylint: disable=arguments-differ
    def kill(self):
        """
        After killing the agent, we need to make sure it has
        successfully unregistered from the master before proceeding.
        """
        super(Agent, self).kill()

        data = http.get_json(self.flags["master"], "slaves", self.config)
        if len(data["slaves"]) == 1 and not data["slaves"][0]["active"]:
            stdout = ""
            if self.proc.poll():
                stdout = "\n{output}".format(output=self.proc.stdout.read())

            raise CLIException("Could not get '/slaves' endpoint as"
                               " JSON with 0 agents in it: {stdout}"
                               .format(stdout=stdout))

        Agent.count -= 1


class Task(Executable):
    """
    This class defines the functions necessary to launch a task in
    the CLI unit tests.
    """
    count = 0

    def __init__(self, flags=None):
        super(Task, self).__init__()

        if flags is None:
            flags = {}

        if "master" not in flags:
            flags["master"] = "{ip}:{port}".format(
                ip=TEST_MASTER_IP,
                port=TEST_MASTER_PORT)
        if "name" not in flags:
            flags["name"] = "task-{id}".format(id=Task.count)
        if "command" not in flags:
            raise CLIException("No command supplied when creating task")

        self.flags = flags
        self.name = flags["name"]
        self.config = config.Config(None)
        self.executable = os.path.join(
            CLITestCase.MESOS_BUILD_DIR,
            "src",
            "mesos-execute")

    def __wait_for_containers(self, condition):
        """
        Wait for the agent's '/containers' endpoint
        to return data subject to 'condition'.
        """
        try:
            data = http.get_json(self.flags["master"], "slaves", self.config)
        except Exception as exception:
            raise CLIException("Could not get '/slaves' endpoint"
                               " as JSON: {error}"
                               .format(error=exception))

        if len(data["slaves"]) != 1:
            raise CLIException("More than one agent detected when"
                               " reading from '/slaves' endpoint")

        try:
            agent = parse.parse(
                "slave({id})@{addr}",
                data["slaves"][0]["pid"])
        except Exception as exception:
            raise CLIException("Unable to parse agent info: {error}"
                               .format(error=exception))

        try:
            data = http.get_json(
                agent["addr"],
                "containers",
                self.config)

            condition(data)
        except Exception as exception:
            raise CLIException("Could not get '/containers' endpoint as"
                               " JSON subject to condition: {error}"
                               .format(error=exception))

    # pylint: disable=arguments-differ
    def launch(self):
        """
        After starting the task, we need to make sure its container
        has actually been added to the agent before proceeding.
        """
        super(Task, self).launch()
        Task.count += 1

        try:
            # pylint: disable=missing-docstring
            def container_exists(data):
                return any(container["executor_id"] == self.flags["name"]
                           for container in data)

            self.__wait_for_containers(container_exists)
        except Exception as exception:
            stdout = ""
            if self.proc.poll():
                stdout = "\n{output}".format(output=self.proc.stdout.read())
                self.proc = None

            raise CLIException("Waiting for container '{name}'"
                               " failed: {error}{stdout}"
                               .format(name=self.flags["name"],
                                       error=exception,
                                       stdout=stdout))

    # pylint: disable=arguments-differ
    def kill(self):
        """
        After killing the task, we need to make sure its container has
        actually been removed from the agent before proceeding.
        """
        super(Task, self).kill()

        try:
            # pylint: disable=missing-docstring
            def container_does_not_exist(data):
                return not any(container["executor_id"] == self.flags["name"]
                               for container in data)

            self.__wait_for_containers(container_does_not_exist)
        except Exception as exception:
            raise CLIException("Container with name '{name}' still"
                               " exists after timeout: {error}"
                               .format(name=self.flags["name"],
                                       error=exception))

        Task.count -= 1


def capture_output(command, argv, extra_args=None):
    """
    Redirect the output of a command to a string and return it.
    """
    if not extra_args:
        extra_args = {}

    stdout = sys.stdout
    sys.stdout = io.StringIO()

    try:
        command(argv, **extra_args)
    except Exception as exception:
        # Fix stdout in case something goes wrong
        sys.stdout = stdout
        raise CLIException("Could not get command output: {error}"
                           .format(error=exception))

    sys.stdout.seek(0)
    output = sys.stdout.read().strip()
    sys.stdout = stdout

    return output


def exec_command(command, env=None, stdin=None, timeout=None):
    """
    Execute command.
    """
    process = subprocess.Popen(
        command,
        stdin=stdin,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        env=env,
        universal_newlines=True)

    try:
        stdout, stderr = process.communicate(timeout=timeout)
    except subprocess.TimeoutExpired as exception:
        # The child process is not killed if the timeout expires, so in order
        # to cleanup properly a well-behaved application should kill the child
        # process and finish communication.
        # https://docs.python.org/3.5/library/subprocess.html
        process.kill()
        stdout, stderr = process.communicate()
        raise CLIException("Timeout expired: {error}".format(error=exception))

    return (process.returncode, stdout, stderr)


def popen_tty(cmd, shell=True):
    """
    Open a process with stdin connected to a pseudo-tty.

    :param cmd: command to run
    :type cmd: str
    :returns: (Popen, master) tuple, where master is the master side
       of the of the tty-pair.  It is the responsibility of the caller
       to close the master fd, and to perform any cleanup (including
       waiting for completion) of the Popen object.
    :rtype: (Popen, int)
    """
    master, slave = pty.openpty()
    # pylint: disable=subprocess-popen-preexec-fn
    proc = subprocess.Popen(cmd,
                            stdin=slave,
                            stdout=subprocess.PIPE,
                            stderr=subprocess.PIPE,
                            preexec_fn=os.setsid,
                            close_fds=True,
                            shell=shell)
    os.close(slave)

    return (proc, master)


def wait_for_task(master, name, state, delay=1):
    """
    Wait for a task with a certain name to be in a given state.
    """
    @retry(wait=wait_fixed(0.2), stop=stop_after_delay(delay))
    def _wait_for_task():
        tasks = http.get_json(master.addr, "tasks",
                              config.Config(None))["tasks"]
        for task in tasks:
            if task["name"] == name and task["state"] == state:
                return task
        raise Exception()

    try:
        return _wait_for_task()
    except Exception:
        raise CLIException("Timeout waiting for task expired")
