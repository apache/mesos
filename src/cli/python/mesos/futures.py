#!/usr/bin/env python

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

try:
    from concurrent.futures import *
except ImportError:
    import threading
    import time

    from Queue import Queue
    from Queue import Empty

    class TimeoutError(Exception):
        """The operation timed out"""

    class Future(object):
        def __init__(self):
            self._lock = threading.RLock()
            self._condition = threading.Condition(self._lock)
            self._done = False
            self._result = None
            self._exception = None
            self._exc_info = None
            self._callbacks = []

        def cancel(self):
            # These futures are possibly backed by threads which can
            # not (easily or portably) be interrupted so instead we
            # simply don't let people cancel.

            # TODO(benh): If more use cases come about that want to
            # differentiate a started versus a
            raise False

        def cancelled(self):
            return False

        def running(self):
            return not self.done()

        def done(self):
            with self._lock:
                return self._done

        def result(self, timeout=None):
            with self._lock:
                self._await(timeout)
                if self._exception:
                    raise self._exception
                return self._result

        def exception(self, timeout=None):
            with self._lock:
                self._await(timeout)
                if self._exception:
                    return self._exception
                return None

        def add_done_callback(self, fn):
            run = False
            with self._lock:
                if self._done:
                    run = True
                else:
                    self._callbacks.append(fn)
            if run:
                try:
                    fn(self)
                except:
                    # TODO(benh): Log if Exception, but semantics tell
                    # us to ignore regardless.
                    pass

        def set_result(self, result):
            with self._lock:
                self._result = result
                self._finish()

        def set_exception(self, exception):
            with self._lock:
                self._exception = exception
                self._finish()

        def _await(self, timeout):
            with self._lock:
                if not self._done:
                    self._condition.wait(timeout)
                    if not self._done:
                        raise TimeoutError()

        def _finish(self):
            callbacks = None
            with self._lock:
                self._done = True
                callbacks = self._callbacks
                self._callbacks = None
            for cb in callbacks:
                try:
                    cb(self)
                except:
                    # TODO(benh): Log if Exception, but semantics tell
                    # us to ignore regardless.
                    pass


    class Executor(object):
        def __enter__(self):
            return self

        def __exit__(self, type, value, traceback):
            self.shutdown()


    def as_completed(futures, timeout=None):
        # Record the start time in order to determine the remaining
        # timeout as futures are completed.
        start = time.time()

        # Use a queue to collect the done futures.
        queue = Queue()

        # Define a helper for the future "done callback".
        def done(future):
            queue.put(future)

        # Add a "done callback" for each future.
        for future in futures:
            future.add_done_callback(done)

        # Helper to determine the remaining timeout.
        def remaining():
            if timeout is None:
                return None
            end = start + timeout
            remaining = end - time.time()
            return remaining if remaining >= 0 else 0

        # Now wait until all the futures have completed or we timeout.
        finished = 0
        while finished < len(futures):
            try:
                yield queue.get(timeout=remaining())
            except Empty:
                raise TimeoutError()
            else:
                finished += 1


class ThreadingExecutor(Executor):
    def __init__(self):
        self._threads = []

    def submit(self, fn, *args, **kwargs):
        future = Future()
        def run():
            try:
                future.set_result(fn(*args, **kwargs))
            except Exception as e:
                future.set_exception(e)
        thread = threading.Thread(target=run)
        thread.start()
        self._threads.append(thread)
        return future

    def map(self, func, iterables, timeout=None):
        # TODO(benh): Implement!
        raise NotImplementedError()

    def shutdown(self, wait=True):
        if wait:
            for thread in self._threads:
                thread.join()
        self._threads = []
