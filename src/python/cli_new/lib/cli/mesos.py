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
Functions to handle agents.
"""

import base64
import json
import os
import platform
import signal
import sys
import threading
import time
import tty
import uuid

from functools import partial
from queue import Queue

import termios

from cli import http
from cli import util
from cli.exceptions import CLIException

import mesos.http
from mesos import recordio
from mesos.exceptions import MesosException
from mesos.exceptions import MesosHTTPException


def get_agent_address(agent_id, master, config):
    """
    Given a master and an agent id, return the agent address
    by checking the /slaves endpoint of the master.
    """
    agents = get_agents(master, config)

    for agent in agents:
        if agent["id"] == agent_id:
            return agent["pid"].split("@")[1]
    raise CLIException("Unable to find agent '{id}'".format(id=agent_id))


def get_agents(master, config):
    """
    Get the agents in a Mesos cluster.
    """
    endpoint = "slaves"
    key = "slaves"
    return get_key_endpoint(key, endpoint, master, config)

def get_framework_address(framework_id, master, config):
    """
    Given a master and an framework id, return the framework address
    by checking the /master/frameworks endpoint of the master.
    """
    frameworks = get_frameworks(master, config)

    for framework in frameworks:
        if framework["id"] == framework_id:
            return framework["webui_url"]
    raise CLIException("Unable to find framework '{id}'"
                       .format(id=framework_id))


def get_frameworks(master, config):
    """
    Get the frameworks in a Mesos cluster.
    """
    endpoint = "master/frameworks/"
    key = "frameworks"
    return get_key_endpoint(key, endpoint, master, config)


def get_key_endpoint(key, endpoint, master, config):
    """
    Get the json key of the given endpoint
    """
    try:
        data = http.get_json(master, endpoint, config)
    except Exception as exception:
        raise CLIException(
            "Could not open '/{endpoint}' on master: {error}"
            .format(endpoint=endpoint, error=exception))

    if not key in data:
        raise CLIException(
            "Missing '{key}' key in data retrieved"
            " from master on '/{endpoint}'"
            .format(key=key, endpoint=endpoint))

    return data[key]


def get_container_id(task):
    """
    Get the container ID of a task.
    """

    if 'statuses' not in task:
        raise CLIException("Unable to obtain status information for task")

    statuses = task['statuses']
    if not statuses:
        raise CLIException("No status updates available for task")

    # It doesn't matter which status we use to get the `container_id`, if the
    # `container_id` has been set for the task, all statuses will contain it.
    if not 'container_status' in statuses[0]:
        raise CLIException("Task status does not contain container information")

    container_status = statuses[0]['container_status']
    if 'container_id' in container_status:
        container_id = container_status['container_id']
        if 'value' in container_id:
            return container_id

    raise CLIException(
        "No container found for the specified task."
        " It might still be spinning up."
        " Please try again.")


def get_tasks(master, config, query=None):
    """
    Get the tasks in a Mesos cluster.
    """
    endpoint = "tasks"
    key = "tasks"

    if query is None:
        query = {'order':'asc', 'limit':'-1'}

    try:
        data = http.get_json(master, endpoint, config, query=query)
    except Exception as exception:
        raise CLIException(
            "Could not open '/{endpoint}' with query parameters: {query}"
            "on master: {error}"
            .format(endpoint=endpoint, query=query, error=exception))

    if not key in data:
        raise CLIException(
            "Missing '{key}' key in data retrieved"
            " from master on '/{endpoint}' with query parameters: {query}"
            .format(key=key, endpoint=endpoint, query=query))

    return data[key]

class TaskIO():
    """
    Object used to stream I/O between a
    running Mesos task and the local terminal.

    :param task: task ID
    :type task: str
    :param cmd: a command to launch inside the task's container
    :type cmd: str
    :param args: Additional arguments for the command
    :type args: str
    :param interactive: whether to attach STDIN of the current
                        terminal to the new command being launched
    :type interactive: bool
    :param tty: whether to allocate a tty for this command and attach
                the local terminal to it
    :type tty: bool
    """
    # pylint: disable=too-many-instance-attributes

    # The interval to send heartbeat messages to
    # keep persistent connections alive.
    HEARTBEAT_INTERVAL = 30
    HEARTBEAT_INTERVAL_NANOSECONDS = HEARTBEAT_INTERVAL * 1000000000

    def __init__(self, master, config, task_id):
        # Get the task and make sure its container was launched by the UCR.
        # Since task's containers are launched by the UCR by default, we want
        # to allow most tasks to pass through unchecked. The only exception is
        # when a task has an explicit container specified and it is not of type
        # "MESOS". Having a type of "MESOS" implies that it was launched by the
        # UCR -- all other types imply it was not.
        try:
            tasks = get_tasks(master, config, query={'task_id': task_id})
        except Exception as exception:
            raise CLIException("Unable to get task with ID {task_id}"
                               " from leading master '{master}': {error}"
                               .format(task_id=task_id, master=master,
                                       error=exception))

        running_tasks = [t for t in tasks if t["state"] == "TASK_RUNNING"]
        matching_tasks = [t for t in running_tasks if t["id"] == task_id]

        if not matching_tasks:
            raise CLIException("Unable to find running task '{task_id}'"
                               " from leading master '{master}'"
                               .format(task_id=task_id, master=master))

        if len(matching_tasks) > 1:
            raise CLIException("More than one task matching id '{id}'"
                               .format(id=task_id))


        task_obj = matching_tasks[0]

        if "container" in task_obj:
            if "type" in task_obj["container"]:
                if task_obj["container"]["type"] != "MESOS":
                    raise CLIException(
                        "This command is only supported for tasks"
                        " launched by the Universal Container Runtime (UCR).")

        # Get the scheme of the agent
        scheme = "https://" if config.agent_ssl() else "http://"

        # Get the URL to the agent running the task.
        agent_addr = util.sanitize_address(
            scheme + get_agent_address(task_obj["slave_id"], master, config))
        self.agent_url = mesos.http.simple_urljoin(agent_addr, "api/v1")
        # Get the agent's task path by checking the `state` endpoint.
        try:
            self.container_id = get_container_id(task_obj)
        except CLIException as exception:
            raise CLIException("Could not get container ID of task '{id}'"
                               " from agent '{addr}': {error}"
                               .format(id=task_id, addr=agent_addr,
                                       error=exception))

        # Set up a recordio encoder and decoder
        # for any incoming and outgoing messages.
        self.encoder = recordio.Encoder(
            lambda s: bytes(json.dumps(s, ensure_ascii=False), "UTF-8"))
        self.decoder = recordio.Decoder(
            lambda s: json.loads(s.decode("UTF-8")))

        # Set up queues to send messages between threads used for
        # reading/writing to STDIN/STDOUT/STDERR and threads
        # sending/receiving data over the network.
        self.input_queue = Queue()
        self.output_queue = Queue()

        # Set up an event to block attaching
        # input until attaching output is complete.
        self.attach_input_event = threading.Event()
        self.attach_input_event.clear()

        # Set up an event to block printing the output
        # until an attach input event has successfully
        # been established.
        self.print_output_event = threading.Event()
        self.print_output_event.clear()

        # Set up an event to block the main thread
        # from exiting until signaled to do so.
        self.exit_event = threading.Event()
        self.exit_event.clear()

        # Use a class variable to store exceptions thrown on
        # other threads and raise them on the main thread before
        # exiting.
        self.exception = None

        # Default values for the TaskIO.
        self.cmd = None
        self.args = None
        self.interactive = False
        self.tty = False
        self.output_thread_entry_point = None
        self.config = config

        # Allow an exit sequence to be used to break the CLIs attachment to
        # the remote task. Depending on the call, this may be disabled, or
        # the exit sequence to be used may be overwritten.
        self.supports_exit_sequence = False
        self.exit_sequence = b'\x10\x11'  # Ctrl-p, Ctrl-q
        self.exit_sequence_detected = False

    def attach(self, _no_stdin=False):
        """
        Attach the stdin/stdout/stderr of the CLI to the
        STDIN/STDOUT/STDERR of a running task.

        As of now, we can only attach to tasks launched with a remote TTY
        already set up for them. If we try to attach to a task that was
        launched without a remote TTY attached, this command will fail.

        :param task: task ID pattern to match
        :type task: str
        :param no_stdin: True if we should *not* attach stdin,
                         False if we should
        :type no_stdin: bool
        """

        # Store relevant parameters of the call for later.
        self.interactive = not _no_stdin
        self.tty = True

        # Set the entry point of the output thread to be a call to
        # _attach_container_output.
        self.output_thread_entry_point = self._attach_container_output

        self._run()

        if not self.exit_sequence_detected:
            # We are only able to get the 'exit_status' of tasks launched via
            # the default executor (i.e. as pods rather than via the command
            # executor). In the future, mesos will deprecate the command
            # executor in favor of the default executor, so this check will
            # be able to go away. In the meantime, we will always return '0'
            # for tasks launched via the command executor.
            if "parent" in self.container_id:
                return self._wait()

        return 0

    def exec(self, _cmd, _args=None, _interactive=False, _tty=False):
        """
        Execute a new process inside of a given task by redirecting
        STDIN/STDOUT/STDERR between the CLI and the Mesos Agent API.

        If a tty is requested, we take over the current terminal and
        put it into raw mode. We make sure to reset the terminal back
        to its original settings before exiting.

        :param cmd: The command to launch inside the task's container
        :type args: cmd
        :param args: Additional arguments for the command
        :type args: list
        :param interactive: attach stdin
        :type interactive: bool
        :param tty: attach a tty
        :type tty: bool
        """

        # Store relevant parameters of the call for later.
        self.cmd = _cmd
        self.args = _args
        self.interactive = _interactive
        self.tty = _tty
        # Override the container ID with the current container ID as the
        # parent, and generate a new UUID for the nested container used to
        # run commands passed to `task exec`.
        self.container_id = {
            'parent': self.container_id,
            'value': str(uuid.uuid4())
        }

        # Set the entry point of the output thread to be a call to
        # _launch_nested_container_session.
        self.output_thread_entry_point = self._launch_nested_container_session

        self._run()

        return self._wait()

    def _run(self):
        """
        Run the helper threads in this class which enable streaming
        of STDIN/STDOUT/STDERR between the CLI and the Mesos Agent API.

        If a tty is requested, we take over the current terminal and
        put it into raw mode. We make sure to reset the terminal back
        to its original settings before exiting.
        """

        # Without a TTY.
        if not self.tty:
            try:
                self._start_threads()
                self.exit_event.wait()
            except Exception as e:
                self.exception = e

            if self.exception:
                # Known Pylint issue: https://github.com/PyCQA/pylint/issues/157
                # pylint: disable=raising-bad-type
                raise self.exception
            return

        # With a TTY.
        if platform.system() == "Windows":
            raise CLIException(
                "Running with the '--tty' flag is not supported on windows")

        if not sys.stdin.isatty():
            raise CLIException(
                "Must be running in a tty to pass the '--tty flag'")

        fd = sys.stdin.fileno()
        oldtermios = termios.tcgetattr(fd)

        try:
            if self.interactive:
                self.supports_exit_sequence = True
                tty.setraw(fd, when=termios.TCSANOW)
                # To force a redraw of the remote terminal, we first resize it
                # to 0 before setting it to the actual size of our local
                # terminal. After that, all terminal resizing is handled in our
                # SIGWINCH handler.
                self._window_resize(signal.SIGWINCH, dimensions=[0, 0])
                self._window_resize(signal.SIGWINCH)
                signal.signal(signal.SIGWINCH, self._window_resize)

            self._start_threads()
            self.exit_event.wait()
        except Exception as e:
            self.exception = e

        termios.tcsetattr(
            sys.stdin.fileno(),
            termios.TCSAFLUSH,
            oldtermios)

        if self.exception:
            # Known Pylint issue: https://github.com/PyCQA/pylint/issues/157
            # pylint: disable=raising-bad-type
            raise self.exception

    def _wait(self):
        """
        Wait for the container associated with this class (through
        'container_id') to exit and return its exit status.
        """
        message = {
            'type': 'WAIT_CONTAINER',
            'wait_container': {
                'container_id': self.container_id}}
        req_extra_args = {
            'verify': self.config.agent_ssl_verify(),
            'additional_headers': {
                'Content-Type': 'application/json',
                'Accept': 'application/json'}}
        try:
            resource = mesos.http.Resource(self.agent_url)
            response = resource.request(
                mesos.http.METHOD_POST,
                data=json.dumps(message),
                auth=self.config.authentication_header(),
                retry=False,
                timeout=None,
                **req_extra_args)
        except MesosException as exception:
            raise CLIException(
                "Error waiting for command to complete: {error}"
                .format(error=exception))

        exit_status = response.json()["wait_container"]["exit_status"]
        if os.WIFSIGNALED(exit_status):
            return os.WTERMSIG(exit_status) + 128
        return os.WEXITSTATUS(exit_status)

    def _thread_wrapper(self, func):
        """
        A wrapper around all threads used in this class.

        If a thread throws an exception, it will unblock the main
        thread and save the exception in a class variable. The main
        thread will then rethrow the exception before exiting.

        :param func: The start function for the thread
        :type func: function
        """
        try:
            func()
        except Exception as e:
            self.exception = e
            self.exit_event.set()

    def _start_threads(self):
        """
        Start all threads associated with this class.
        """
        if self.interactive:
            # Collects input from STDIN and puts
            # it in the input_queue as data messages.
            thread = threading.Thread(
                target=self._thread_wrapper,
                args=(self._input_thread,))
            thread.daemon = True
            thread.start()

            # Prepares heartbeat control messages and
            # puts them in the input queueaat a specific
            # heartbeat interval.
            thread = threading.Thread(
                target=self._thread_wrapper,
                args=(self._heartbeat_thread,))
            thread.daemon = True
            thread.start()

            # Opens a persistent connection with the mesos agent and
            # feeds it both control and data messages from the input
            # queue via ATTACH_CONTAINER_INPUT messages.
            thread = threading.Thread(
                target=self._thread_wrapper,
                args=(self._attach_container_input,))
            thread.daemon = True
            thread.start()

        # Opens a persistent connection with a mesos agent, reads
        # data messages from it and feeds them to an output_queue.
        thread = threading.Thread(
            target=self._thread_wrapper,
            args=(self.output_thread_entry_point,))
        thread.daemon = True
        thread.start()

        # Collects data messages from the output queue and writes
        # their content to STDOUT and STDERR.
        thread = threading.Thread(
            target=self._thread_wrapper,
            args=(self._output_thread,))
        thread.daemon = True
        thread.start()

    def _attach_container_output(self):
        """
        Streams all output data (e.g. STDOUT/STDERR) to the
        client from the agent.
        """

        message = {
            'type': 'ATTACH_CONTAINER_OUTPUT',
            'attach_container_output': {
                'container_id': self.container_id}}

        req_extra_args = {
            'stream': True,
            'verify': self.config.agent_ssl_verify(),
            'additional_headers': {
                'Content-Type': 'application/json',
                'Accept': 'application/recordio',
                'Message-Accept': 'application/json'}}

        try:
            resource = mesos.http.Resource(self.agent_url)
            response = resource.request(
                mesos.http.METHOD_POST,
                data=json.dumps(message),
                retry=False,
                timeout=None,
                auth=self.config.authentication_header(),
                **req_extra_args)
        except MesosHTTPException as e:
            text = "I/O switchboard server was disabled for this container"
            if e.response.status_code == 500 and e.response.text == text:
                raise CLIException("Unable to attach to a task"
                                   " launched without a TTY")
            raise e

        self._process_output_stream(response)

    def _launch_nested_container_session(self):
        """
        Sends a request to the Mesos Agent to launch a new
        nested container and attach to its output stream.
        The output stream is then sent back in the response.
        """

        message = {
            'type': "LAUNCH_NESTED_CONTAINER_SESSION",
            'launch_nested_container_session': {
                'container_id': self.container_id,
                'command': {
                    'value': self.cmd,
                    'arguments': [self.cmd] + self.args,
                    'shell': False}}}

        if self.tty:
            message[
                'launch_nested_container_session'][
                    'container'] = {
                        'type': 'MESOS',
                        'tty_info': {}}

        req_extra_args = {
            'stream': True,
            'verify': self.config.agent_ssl_verify(),
            'additional_headers': {
                'Content-Type': 'application/json',
                'Accept': 'application/recordio',
                'Message-Accept': 'application/json'}}
        resource = mesos.http.Resource(self.agent_url)
        try:
            response = resource.request(
                mesos.http.METHOD_POST,
                data=json.dumps(message),
                retry=False,
                timeout=None,
                auth=self.config.authentication_header(),
                **req_extra_args)
        except MesosException as exception:
            raise CLIException("{error}".format(error=exception))

        self._process_output_stream(response)

    def _process_output_stream(self, response):
        """
        Gets data streamed over the given response and places the
        returned messages into our output_queue. Only expects to
        receive data messages.

        :param response: Response from an http post
        :type response: requests.models.Response
        """

        # Now that we are ready to process the output stream (meaning
        # our output connection has been established), allow the input
        # stream to be attached by setting an event.
        self.attach_input_event.set()

        # If we are running in interactive mode, wait to make sure that
        # our input connection succeeds before pushing any output to the
        # output queue.
        if self.interactive:
            self.print_output_event.wait()

        try:
            for chunk in response.iter_content(chunk_size=None):
                records = self.decoder.decode(chunk)

                for r in records:
                    if r.get('type') and r['type'] == 'DATA':
                        self.output_queue.put(r['data'])
        except Exception as e:
            raise CLIException(
                "Error parsing output stream: {error}".format(error=e))

        self.output_queue.join()
        self.exit_event.set()

    def _attach_container_input(self):
        """
        Streams all input data (e.g. STDIN) from the client to the agent.
        """

        def _initial_input_streamer():
            """
            Generator function yielding the initial ATTACH_CONTAINER_INPUT
            message for streaming. We have a separate generator for this so
            that we can attempt the connection once before committing to a
            persistent connection where we stream the rest of the input.

            :returns: A RecordIO encoded message
            """

            message = {
                'type': 'ATTACH_CONTAINER_INPUT',
                'attach_container_input': {
                    'type': 'CONTAINER_ID',
                    'container_id': self.container_id}}

            yield self.encoder.encode(message)

        def _input_streamer():
            """
            Generator function yielding ATTACH_CONTAINER_INPUT messages for
            streaming. It yields the _intitial_input_streamer() message,
            followed by messages from the input_queue on each subsequent call.

            :returns: A RecordIO encoded message
            """
            yield next(_initial_input_streamer(), None)

            while True:
                record = self.input_queue.get()
                if not record:
                    if self.exit_sequence_detected:
                        sys.stdout.write("\r\n")
                        sys.stdout.flush()
                        self.exit_event.set()
                    break
                yield record

        req_extra_args = {
            'verify': self.config.agent_ssl_verify(),
            'additional_headers': {
                'Content-Type': 'application/recordio',
                'Message-Content-Type': 'application/json',
                'Accept': 'application/json',
                'Connection': 'close',
                'Transfer-Encoding': 'chunked'
            }
        }

        # Ensure we don't try to attach our input to a container that isn't
        # fully up and running by waiting until the
        # `_process_output_stream` function signals us that it's ready.
        self.attach_input_event.wait()

        # Send an intial "Test" message to ensure that we are able to
        # establish a connection with the agent. If we aren't we will throw
        # an exception and break out of this thread. However, in cases where
        # we receive a 500 response from the agent, we actually want to
        # continue without throwing an exception. A 500 error indicates that
        # we can't connect to the container because it has already finished
        # running. In that case we continue running to allow the output queue
        # to be flushed.
        resource = mesos.http.Resource(self.agent_url)
        try:
            resource.request(
                mesos.http.METHOD_POST,
                data=_initial_input_streamer(),
                retry=False,
                auth=self.config.authentication_header(),
                **req_extra_args)
        except MesosHTTPException as e:
            if not e.response.status_code == 500:
                raise e

        # If we succeeded with that connection, unblock process_output_stream()
        # from sending output data to the output thread.
        self.print_output_event.set()

        # Begin streaming the input.
        resource = mesos.http.Resource(self.agent_url)
        resource.request(
            mesos.http.METHOD_POST,
            data=_input_streamer(),
            retry=False,
            timeout=None,
            auth=self.config.authentication_header(),
            **req_extra_args)

    def _detect_exit_sequence(self, chunk):
        """
        Detects if 'self.exit_sequence' is present in 'chunk'.

        If a partial exit sequence is detected at the end of 'chunk', then
        more characters are read from 'stdin' and appended to 'chunk' in
        search of the full sequence. Since python cannot pass variables by
        reference, we return a modified 'chunk' with the extra characters
        read if necessary.

        If the exit sequence is found, the class variable
        'exit_sequence_detected' is set to True.

        :param chunk: a byte array to search for the exit sequence in
        :type chunk: byte array
        :returns: a modified byte array containing the original 'chunk' plus
                  any extra characters read in search of the exit sequence
        :rtype: byte array
        """
        if not self.supports_exit_sequence:
            return chunk

        if chunk.find(self.exit_sequence) != -1:
            self.exit_sequence_detected = True
            return chunk

        for i in reversed(range(1, len(self.exit_sequence))):
            if self.exit_sequence[:-i] == chunk[len(chunk)-i:]:
                chunk += os.read(sys.stdin.fileno(), 1)
                return self._detect_exit_sequence(chunk)

        return chunk

    def _input_thread(self):
        """
        Reads from STDIN and places a message
        with that data onto the input_queue.
        """

        message = {
            'type': 'ATTACH_CONTAINER_INPUT',
            'attach_container_input': {
                'type': 'PROCESS_IO',
                'process_io': {
                    'type': 'DATA',
                    'data': {
                        'type': 'STDIN',
                        'data': ''}}}}

        for chunk in iter(partial(os.read, sys.stdin.fileno(), 1024), b''):
            chunk = self._detect_exit_sequence(chunk)
            if self.exit_sequence_detected:
                break

            message[
                'attach_container_input'][
                    'process_io'][
                        'data'][
                            'data'] = base64.b64encode(chunk).decode('utf-8')

            self.input_queue.put(self.encoder.encode(message))

        # Push an empty string to indicate EOF to the server and push
        # 'None' to signal that we are done processing input.
        message['attach_container_input']['process_io']['data']['data'] = ''
        self.input_queue.put(self.encoder.encode(message))
        self.input_queue.put(None)

    def _output_thread(self):
        """
        Reads from the output_queue and writes the data
        to the appropriate STDOUT or STDERR.
        """

        while True:
            # Get a message from the output queue and decode it.
            # Then write the data to the appropriate stdout or stderr.
            output = self.output_queue.get()
            if not output.get('data'):
                raise CLIException("Error no 'data' field in output message")

            data = output['data']
            data = base64.b64decode(data.encode('utf-8'))

            if output.get('type') and output['type'] == 'STDOUT':
                sys.stdout.buffer.write(data)
                sys.stdout.flush()
            elif output.get('type') and output['type'] == 'STDERR':
                sys.stderr.buffer.write(data)
                sys.stderr.flush()
            else:
                raise CLIException("Unsupported data type in output stream")

            self.output_queue.task_done()

    def _heartbeat_thread(self):
        """
        Generates a heartbeat message to send over the ATTACH_CONTAINER_INPUT
        stream every `interval` seconds and inserts it in the input queue.
        """

        interval = self.HEARTBEAT_INTERVAL
        nanoseconds = self.HEARTBEAT_INTERVAL_NANOSECONDS

        message = {
            'type': 'ATTACH_CONTAINER_INPUT',
            'attach_container_input': {
                'type': 'PROCESS_IO',
                'process_io': {
                    'type': 'CONTROL',
                    'control': {
                        'type': 'HEARTBEAT',
                        'heartbeat': {
                            'interval': {
                                'nanoseconds': nanoseconds}}}}}}

        while True:
            self.input_queue.put(self.encoder.encode(message))
            time.sleep(interval)

    def _window_resize(self, signum=None, frame=None, dimensions=None):
        # pylint: disable=unused-argument
        """
        Signal handler for SIGWINCH.

        Generates a message with the current dimensions of the
        terminal and puts it in the input_queue.

        :param signum: the signal number being handled
        :type signum: int
        :param frame: current stack frame
        :type frame: frame
        """

        # Determine the size of our terminal, and create the message to be sent
        if dimensions:
            rows, columns = dimensions
        else:
            rows, columns = os.popen('stty size', 'r').read().split()

        message = {
            'type': 'ATTACH_CONTAINER_INPUT',
            'attach_container_input': {
                'type': 'PROCESS_IO',
                'process_io': {
                    'type': 'CONTROL',
                    'control': {
                        'type': 'TTY_INFO',
                        'tty_info': {
                            'window_size': {
                                'rows': int(rows),
                                'columns': int(columns)}}}}}}

        self.input_queue.put(self.encoder.encode(message))
