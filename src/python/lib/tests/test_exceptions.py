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

# pylint: disable=redefined-outer-name,missing-docstring

import mock
import pytest

from mesos.exceptions import MesosHTTPException
from mesos.exceptions import MesosAuthenticationException
from mesos.exceptions import MesosAuthorizationException
from mesos.exceptions import MesosBadRequestException
from mesos.exceptions import MesosUnprocessableException


@pytest.mark.parametrize([
    'exception',
    'status_code',
    'expected_string'
], [
    (MesosHTTPException,
     400,
     "The url 'http://some.url' returned HTTP 400: something bad happened"),
    (MesosAuthenticationException,
     401,
     "The url 'http://some.url' returned HTTP 401: something bad happened"),
    (MesosAuthorizationException,
     403,
     "The url 'http://some.url' returned HTTP 403: something bad happened"),
    (MesosBadRequestException,
     400,
     "The url 'http://some.url' returned HTTP 400: something bad happened"),
    (MesosUnprocessableException,
     422,
     "The url 'http://some.url' returned HTTP 422: something bad happened"),
])
def test_exceptions(exception, status_code, expected_string):
    """
    Test exceptions
    """
    mock_resp = mock.Mock()
    mock_resp.status_code = status_code
    mock_resp.reason = 'some_reason'
    mock_resp.request.url = 'http://some.url'
    mock_resp.text = 'something bad happened'

    # Test MesosHTTPException
    err = exception(mock_resp)
    assert str(err) == expected_string
    assert err.status() == status_code
