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

# pylint: disable=missing-docstring,protected-access,too-many-locals,too-many-arguments



from collections import namedtuple

import mock
import pytest
import requests.exceptions
import ujson

from mesos import http


def test_resource():
    # test initialization
    resource = http.Resource('http://some.domain/some/prefix')
    assert resource.url.geturl() == 'http://some.domain/some/prefix'

    # test subresource
    subresource = resource.subresource('some/subpath')
    assert subresource.url.geturl() == \
           'http://some.domain/some/prefix/some/subpath'


@pytest.mark.parametrize(
    ['default_tos',
     'override_tos',
     'expected_request_calls'],
    [(None,
      None,
      [mock.call(auth=None,
                 headers={'Content-Type': 'applicatioin/json',
                          'some-header-key': 'some-header-val'},
                 method='GET',
                 params=None,
                 somekey='someval',
                 timeout=None,
                 url='http://some.domain/some/prefix')]),
     (10,
      None,
      [mock.call(auth=None,
                 headers={'Content-Type': 'applicatioin/json',
                          'some-header-key': 'some-header-val'},
                 method='GET',
                 params=None,
                 somekey='someval',
                 timeout=10,
                 url='http://some.domain/some/prefix')]),
     (10,
      100,
      [mock.call(auth=None,
                 headers={'Content-Type': 'applicatioin/json',
                          'some-header-key': 'some-header-val'},
                 method='GET',
                 params=None,
                 somekey='someval',
                 timeout=100,
                 url='http://some.domain/some/prefix')])])
def test_resource_request(mock_mesos_http_request, default_tos, override_tos,
                          expected_request_calls):
    # test request
    mock_response = mock.Mock(status_code=200)
    mock_mesos_http_request.return_value = mock_response
    resource = http.Resource('http://some.domain/some/prefix',
                             default_timeout=default_tos,
                             default_headers={
                                 'Content-Type': 'applicatioin/json'})
    ret = resource.request(http.METHOD_GET,
                           additional_headers={
                               'some-header-key': 'some-header-val'
                           },
                           timeout=override_tos,
                           auth=None,
                           use_gzip_encoding=False,
                           max_attempts=1,
                           params=None,
                           somekey='someval')
    assert ret == mock_response
    assert mock_mesos_http_request.mock_calls == expected_request_calls


SomeModel = namedtuple('SomeModel', ['some'])

RequestJsonParams = namedtuple('RequestJsonParams',
                               [
                                   'default_tos',
                                   'override_tos',
                                   'json_payload',
                                   'obj_decoder',
                                   'request_exception',
                                   'resp_status',
                                   'json_exception',
                                   'expected_exception',
                                   'expected_additional_kwargs',
                               ])


@mock.patch('mesos.http.ujson.loads')
@pytest.mark.parametrize(
    RequestJsonParams._fields,
    [
        RequestJsonParams(
            default_tos=None,
            override_tos=None,
            json_payload={'some': 'payload'},
            obj_decoder=None,
            request_exception=None,
            resp_status=200,
            json_exception=None,
            expected_exception=None,
            expected_additional_kwargs=[{}]),
        RequestJsonParams(
            default_tos=None,
            override_tos=None,
            json_payload={'some': 'payload'},
            obj_decoder=lambda d: SomeModel(**d),
            request_exception=None,
            resp_status=200,
            json_exception=None,
            expected_exception=None,
            expected_additional_kwargs=[{}]),
        RequestJsonParams(
            default_tos=None,
            override_tos=None,
            json_payload={'some': 'payload'},
            obj_decoder=None,
            request_exception=requests.exceptions.SSLError,
            resp_status=200,
            json_exception=None,
            expected_exception=http.MesosException,
            expected_additional_kwargs=[{}]),
        RequestJsonParams(
            default_tos=None,
            override_tos=None,
            json_payload={'some': 'payload'},
            obj_decoder=None,
            request_exception=requests.exceptions.ConnectionError,
            resp_status=200,
            json_exception=None,
            expected_exception=http.MesosException,
            expected_additional_kwargs=[{}]),
        RequestJsonParams(
            default_tos=None,
            override_tos=None,
            json_payload={'some': 'payload'},
            obj_decoder=None,
            request_exception=requests.exceptions.Timeout,
            resp_status=200,
            json_exception=None,
            expected_exception=http.MesosException,
            expected_additional_kwargs=[{}]),
        RequestJsonParams(
            default_tos=None,
            override_tos=None,
            json_payload={'some': 'payload'},
            obj_decoder=None,
            request_exception=requests.exceptions.RequestException,
            resp_status=200,
            json_exception=None,
            expected_exception=http.MesosException,
            expected_additional_kwargs=[{}]),
        RequestJsonParams(
            default_tos=None,
            override_tos=None,
            json_payload={'some': 'payload'},
            obj_decoder=None,
            request_exception=None,
            resp_status=299,
            json_exception=None,
            expected_exception=None,
            expected_additional_kwargs=[{}]),
        RequestJsonParams(
            default_tos=None,
            override_tos=None,
            json_payload={'some': 'payload'},
            obj_decoder=None,
            request_exception=None,
            resp_status=200,
            json_exception=ValueError,
            expected_exception=http.MesosException,
            expected_additional_kwargs=[{}]),
        RequestJsonParams(
            default_tos=10,
            override_tos=None,
            json_payload={'some': 'payload'},
            obj_decoder=None,
            request_exception=None,
            resp_status=200,
            json_exception=None,
            expected_exception=None,
            expected_additional_kwargs=[dict(timeout=10)]),
        RequestJsonParams(
            default_tos=10,
            override_tos=100,
            json_payload={'some': 'payload'},
            obj_decoder=None,
            request_exception=None,
            resp_status=200,
            json_exception=None,
            expected_exception=None,
            expected_additional_kwargs=[dict(timeout=100)]),
        RequestJsonParams(
            default_tos=None,
            override_tos=None,
            json_payload={'some': 'payload'},
            obj_decoder=None,
            request_exception=None,
            resp_status=400,
            json_exception=None,
            expected_exception=http.Resource.ERROR_CODE_MAP[400],
            expected_additional_kwargs=[{}]),
        RequestJsonParams(
            default_tos=None,
            override_tos=None,
            json_payload={'some': 'payload'},
            obj_decoder=None,
            request_exception=None,
            resp_status=999,
            json_exception=None,
            expected_exception=http.MesosException,
            expected_additional_kwargs=[{}])])
def test_resource_request_json(
        mock_ujson_loads,
        mock_mesos_http_request,
        default_tos,
        override_tos,
        json_payload,
        obj_decoder,
        request_exception,
        resp_status,
        json_exception,
        expected_exception,
        expected_additional_kwargs):
    call_kwargs = dict(method=http.METHOD_POST,
                       json={'some': 'payload'},
                       timeout=None,
                       url='http://some.domain/some/prefix',
                       auth=None,
                       headers={'Accept': 'application/json',
                                'Accept-Encoding': 'gzip'},
                       params=None)
    mock_calls = []
    for kwargs in expected_additional_kwargs:
        call_kwargs.update(kwargs)
        mock_calls.append(mock.call(**call_kwargs))

    def json_side_effect(_):
        if json_exception is None:
            return {'some': 'return_value'}
        raise json_exception

    def request_side_effect(*_, **__):
        if request_exception is None:
            return mock.Mock(status_code=resp_status)
        raise request_exception

    mock_mesos_http_request.side_effect = request_side_effect
    mock_ujson_loads.side_effect = json_side_effect

    resource = http.Resource('http://some.domain/some/prefix',
                             default_timeout=default_tos,
                             default_auth=None,
                             default_max_attempts=1,
                             default_use_gzip_encoding=True)

    if expected_exception is None:
        ret = resource.request_json(http.METHOD_POST,
                                    timeout=override_tos,
                                    payload=json_payload,
                                    decoder=obj_decoder)
        expected_ret = {'some': 'return_value'}
        if obj_decoder is None:
            assert ret == expected_ret
        else:
            assert ret == SomeModel(**expected_ret)
    else:
        with pytest.raises(expected_exception):
            resource.request_json(http.METHOD_POST,
                                  timeout=override_tos,
                                  payload=json_payload)

    assert mock_mesos_http_request.mock_calls == mock_calls


def test_resource_get_json(mock_mesos_http_request):
    mock_mesos_http_request.return_value = mock.Mock(
        status_code=200,
        text=ujson.dumps({'hello': 'world'}),
    )
    mock_auth = mock.Mock()
    resource = http.Resource('http://some.domain/some/prefix',
                             default_timeout=100,
                             default_auth=mock_auth,
                             default_max_attempts=1,
                             default_use_gzip_encoding=True)
    ret = resource.get_json()
    assert mock_mesos_http_request.mock_calls == [
        mock.call(
            json=None,
            method='GET',
            url='http://some.domain/some/prefix',
            auth=mock_auth,
            headers={'Accept-Encoding': 'gzip',
                     'Accept': 'application/json'},
            params=None,
            timeout=100,
        )
    ]
    assert ret == {'hello': 'world'}


def test_resource_post_json(mock_mesos_http_request):
    mock_mesos_http_request.return_value = mock.Mock(
        status_code=200,
        text=ujson.dumps({'hello': 'world'}),
    )
    mock_auth = mock.Mock()
    resource = http.Resource('http://some.domain/some/prefix',
                             default_timeout=100,
                             default_auth=mock_auth,
                             default_max_attempts=1,
                             default_use_gzip_encoding=True)
    ret = resource.post_json(payload={'somekey': 'someval'})
    assert mock_mesos_http_request.mock_calls == [
        mock.call(json={'somekey': 'someval'},
                  method='POST',
                  url='http://some.domain/some/prefix',
                  auth=mock_auth,
                  headers={'Accept-Encoding': 'gzip',
                           'Accept': 'application/json'},
                  params=None,
                  timeout=100)
    ]
    assert ret == {'hello': 'world'}
