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
Tests that test the RecordIO encoder and decoder.
"""

import json

from mesos import recordio
from mesos.exceptions import MesosException


def test_encode():
    """
    Test encoding of a message into 'RecordIO' format.
    """
    try:
        encoder = recordio.Encoder(lambda s: bytes(json.dumps(s), "UTF-8"))
    except Exception as exception:
        raise MesosException("Error instantiating 'RecordIO' encoder: {error}"
                             .format(error=exception))

    try:
        message = {
            "type": "ATTACH_CONTAINER_OUTPUT",
            "containerId": "123456789"
        }

        encoded = encoder.encode(message)

    except Exception as exception:
        raise MesosException("Error encoding 'RecordIO' message: {error}"
                             .format(error=exception))

    string = json.dumps(message)
    assert encoded == bytes(str(len(string)) + "\n" + string, "UTF-8")


def test_encode_decode():
    """
    Test encoding/decoding of a message and records into 'RecordIO' format.
    """
    total_messages = 10

    try:
        encoder = recordio.Encoder(lambda s: bytes(json.dumps(s), "UTF-8"))
    except Exception as exception:
        raise MesosException("Error instantiating 'RecordIO' encoder: {error}"
                             .format(error=exception))

    try:
        decoder = recordio.Decoder(lambda s: json.loads(s.decode("UTF-8")))
    except Exception as exception:
        raise MesosException("Error instantiating 'RecordIO' decoder: {error}"
                             .format(error=exception))

    try:
        message = {
            "type": "ATTACH_CONTAINER_OUTPUT",
            "containerId": "123456789"
        }

        encoded = b""
        for _ in range(total_messages):
            encoded += encoder.encode(message)

    except Exception as exception:
        raise MesosException("Error encoding 'RecordIO' message: {error}"
                             .format(error=exception))

    try:
        all_records = []
        offset = 0
        chunk_size = 5
        while offset < len(encoded):
            records = decoder.decode(encoded[offset:offset + chunk_size])
            all_records.extend(records)
            offset += chunk_size

        assert len(all_records) == total_messages

        for record in all_records:
            assert record == message
    except Exception as exception:
        raise MesosException("Error decoding 'RecordIO' messages: {error}"
                             .format(error=exception))
