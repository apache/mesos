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
Provides facilities for "Record-IO" encoding of data.
"Record-IO" encoding allows one to encode a sequence
of variable-length records by prefixing each record
with its size in bytes:

5\n
hello
6\n
world!

Note that this currently only supports record lengths
encoded as base 10 integer values with newlines as a
delimiter. This is to provide better language
portability: parsing a base 10 integer is simple. Most
other "Record-IO" implementations use a fixed-size
header of 4 bytes to directly encode an unsigned 32 bit
length.
"""

from mesos.exceptions import MesosException


class Encoder():
    """
    Encode an arbitray message type into a 'RecordIO' message.

    This class encapsulates the process of encoding an
    arbitrary message into a 'RecordIO' message. Its
    constructor takes a serialization function of the form
    'serialize(message)'. This serialization function is
    responsible for knowing how to take whatever message type
    is passed to 'encode()' and serializing it to a 'UTF-8'
    encoded byte array.

    Once 'encode(message)' is called, it will use the
    serialization function to convert 'message' into a 'UTF-8'
    encoded byte array, wrap it in a 'RecordIO' frame,
    and return it.

    :param serialize: a function to serialize any message
                        passed to 'encode()' into a 'UTF-8'
                        encoded byte array
    :type serialize: function
    """

    def __init__(self, serialize):
        self.serialize = serialize

    def encode(self, message):
        """
        Encode a message into 'RecordIO' format.

        :param message: a message to serialize and then wrap in
                        a 'RecordIO' frame.
        :type message: object
        :returns: a serialized message wrapped in a 'RecordIO' frame
        :rtype: bytes
        """

        s = self.serialize(message)

        if not isinstance(s, bytes):
            raise MesosException("Calling 'serialize(message)' must"
                                 " return a 'bytes' object")

        return bytes(str(len(s)) + "\n", "UTF-8") + s


class Decoder():
    """
    Decode a 'RecordIO' message back to an arbitrary message type.

    This class encapsulates the process of decoding a message
    previously encoded with 'RecordIO' back to an arbitrary
    message type. Its constructor takes a deserialization
    function of the form 'deserialize(data)'. This
    deserialization function is responsible for knowing how to
    take a fully constructed 'RecordIO' message containing a
    'UTF-8' encoded byte array and deserialize it back into the
    original message type.

    The 'decode(data)' message takes a 'UTF-8' encoded byte array
    as input and buffers it across subsequent calls to
    construct a set of fully constructed 'RecordIO' messages that
    are decoded and returned in a list.

    :param deserialize: a function to deserialize from 'RecordIO'
                        messages built up by subsequent calls
                        to 'decode(data)'
    :type deserialize: function
    """

    HEADER = 0
    RECORD = 1
    FAILED = 2

    def __init__(self, deserialize):
        self.deserialize = deserialize
        self.state = self.HEADER
        self.buffer = bytes("", "UTF-8")
        self.length = 0

    def decode(self, data):
        """
        Decode a 'RecordIO' formatted message to its original type.

        :param data: an array of 'UTF-8' encoded bytes that make up a
                      partial 'RecordIO' message. Subsequent calls to this
                      function maintain state to build up a full 'RecordIO'
                      message and decode it
        :type data: bytes
        :returns: a list of deserialized messages
        :rtype: list
        """

        if not isinstance(data, bytes):
            raise MesosException("Parameter 'data' must of of type 'bytes'")

        if self.state == self.FAILED:
            raise MesosException("Decoder is in a FAILED state")

        records = []

        for c in data:
            if self.state == self.HEADER:
                if c != ord('\n'):
                    self.buffer += bytes([c])
                    continue

                try:
                    self.length = int(self.buffer.decode("UTF-8"))
                except Exception as exception:
                    self.state = self.FAILED
                    raise MesosException("Failed to decode length"
                                         "'{buffer}': {error}"
                                         .format(buffer=self.buffer,
                                                 error=exception))

                self.buffer = bytes("", "UTF-8")
                self.state = self.RECORD

                # Note that for 0 length records, we immediately decode.
                if self.length <= 0:
                    records.append(self.deserialize(self.buffer))
                    self.state = self.HEADER

            elif self.state == self.RECORD:
                assert self.length
                assert len(self.buffer) < self.length

                self.buffer += bytes([c])

                if len(self.buffer) == self.length:
                    records.append(self.deserialize(self.buffer))
                    self.buffer = bytes("", "UTF-8")
                    self.state = self.HEADER

        return records
