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

# pylint: disable=too-many-arguments

"""
Classes and functions for interacting with the Mesos HTTP RESTful API
"""



from urllib.parse import urlparse
from copy import deepcopy

import requests
import tenacity
import ujson

from mesos.exceptions import MesosException
from mesos.exceptions import MesosHTTPException
from mesos.exceptions import MesosAuthenticationException
from mesos.exceptions import MesosAuthorizationException
from mesos.exceptions import MesosBadRequestException
from mesos.exceptions import MesosInternalServerErrorException
from mesos.exceptions import MesosServiceUnavailableException
from mesos.exceptions import MesosUnprocessableException

METHOD_HEAD = 'HEAD'
METHOD_GET = 'GET'
METHOD_POST = 'POST'
METHOD_PUT = 'PUT'
METHOD_PATCH = 'PATCH'
METHOD_DELETE = 'DELETE'
METHODS = {
    METHOD_HEAD,
    METHOD_GET,
    METHOD_POST,
    METHOD_PUT,
    METHOD_PATCH,
    METHOD_DELETE}

REQUEST_JSON_HEADERS = {'Accept': 'application/json'}
REQUEST_GZIP_HEADERS = {'Accept-Encoding': 'gzip'}

BASE_HEADERS = {}
DEFAULT_TIMEOUT = 30
DEFAULT_AUTH = None
DEFAULT_USE_GZIP_ENCODING = True
DEFAULT_MAX_ATTEMPTS = 3


def simple_urljoin(base, other):
    """
    Do a join by rstrip'ing / from base_url and lstrp'ing / from other.

    This is needed since urlparse.urljoin tries to be too smart
    and strips the subpath from base_url.

    :type base: str
    :type other: str
    :rtype: str
    """
    return '/'.join([base.rstrip('/'), other.lstrip('/')])


class Resource():
    """
    Encapsulate the context for an HTTP resource.

    Context for an HTTP resource may include properties such as the URL,
    default timeout for connections, default headers to be included in each
    request, and auth.
    """
    SUCCESS_CODES = frozenset(range(200, 300))
    ERROR_CODE_MAP = {c.STATUS_CODE: c for c in (
        MesosBadRequestException,
        MesosAuthenticationException,
        MesosAuthorizationException,
        MesosUnprocessableException,
        MesosInternalServerErrorException,
        MesosServiceUnavailableException)}

    def __init__(self,
                 url,
                 default_headers=None,
                 default_timeout=DEFAULT_TIMEOUT,
                 default_auth=DEFAULT_AUTH,
                 default_use_gzip_encoding=DEFAULT_USE_GZIP_ENCODING,
                 default_max_attempts=DEFAULT_MAX_ATTEMPTS):
        """
        :param url: URL identifying the resource
        :type url: str
        :param default_headers: headers to attache to requests
        :type default_headers: dict[str, str]
        :param default_timeout: timeout in seconds
        :type default_timeout: float
        :param default_auth: auth scheme
        :type default_auth: requests.auth.AuthBase
        :param default_use_gzip_encoding: use gzip encoding by default or not
        :type default_use_gzip_encoding: bool
        :param default_max_attempts: max number of attempts when retrying
        :type default_max_attempts: int
        """
        self.url = urlparse(url)
        self.default_timeout = default_timeout
        self.default_auth = default_auth
        self.default_use_gzip_encoding = default_use_gzip_encoding
        self.default_max_attempts = default_max_attempts

        if default_headers is None:
            self._default_headers = {}
        else:
            self._default_headers = deepcopy(default_headers)

    def default_headers(self):
        """
        Return a copy of the default headers.

        :rtype: dict[str, str]
        """
        return deepcopy(self._default_headers)

    def subresource(self, subpath):
        """
        Return a new Resource object at a subpath of the current resource's URL.

        :param subpath: subpath of the resource
        :type subpath: str
        :return: Resource at subpath
        :rtype: Resource
        """
        return self.__class__(
            url=simple_urljoin(self.url.geturl(), subpath),
            default_headers=self.default_headers(),
            default_timeout=self.default_timeout,
            default_auth=self.default_auth,
            default_use_gzip_encoding=self.default_use_gzip_encoding,
            default_max_attempts=self.default_max_attempts,
        )

    def _request(self,
                 method,
                 additional_headers=None,
                 timeout=None,
                 auth=None,
                 use_gzip_encoding=None,
                 params=None,
                 **kwargs):
        """
        Make an HTTP request with given method and an optional timeout.

        :param method: request method
        :type method: str
        :param additional_headers: additional headers to include in the request
        :type additional_headers: dict[str, str]
        :param timeout: timeout in seconds
        :type timeout: float
        :param auth: auth scheme for request
        :type auth: requests.auth.AuthBase
        :param use_gzip_encoding: boolean indicating whether to
                                  pass gzip encoding in the request
                                  headers or not
        :type use_gzip_encoding: boolean
        :param params: additional params to include in the request
        :type params: str | dict[str, T]
        :param kwargs: additional arguments to pass to requests.request
        :type kwargs: dict[str, T]
        :return: HTTP response
        :rtype: requests.Response
        """
        headers = self.default_headers()
        if additional_headers is not None:
            headers.update(additional_headers)

        if timeout is None:
            timeout = self.default_timeout

        if auth is None:
            auth = self.default_auth

        if use_gzip_encoding is None:
            use_gzip_encoding = self.default_use_gzip_encoding

        if headers and use_gzip_encoding:
            headers.update(REQUEST_GZIP_HEADERS)

        kwargs.update(dict(
            url=self.url.geturl(),
            method=method,
            headers=headers,
            timeout=timeout,
            auth=auth,
            params=params,
        ))

        # Here we call request without a try..except block since all exceptions
        # raised here will be used to determine whether or not a retry is
        # necessary in self.request.
        response = requests.request(**kwargs)

        if response.status_code in self.SUCCESS_CODES:
            return response

        known_exception = self.ERROR_CODE_MAP.get(response.status_code)
        if known_exception:
            raise known_exception(response)

        raise MesosHTTPException(response)

    def request(self,
                method,
                additional_headers=None,
                retry=True,
                timeout=None,
                auth=None,
                use_gzip_encoding=None,
                params=None,
                max_attempts=None,
                **kwargs):
        """
        Make an HTTP request by calling self._request with backoff retry.

        :param method: request method
        :type method: str
        :param additional_headers: additional headers to include in the request
        :type additional_headers: dict[str, str]
        :param retry: boolean indicating whether to retry if the request fails
        :type retry: boolean
        :param timeout: timeout in seconds, overrides default_timeout_secs
        :type timeout: float
        :param timeout: timeout in seconds
        :type timeout: float
        :param auth: auth scheme for the request
        :type auth: requests.auth.AuthBase
        :param use_gzip_encoding: boolean indicating whether to pass gzip
                                  encoding in the request headers or not
        :type use_gzip_encoding: boolean | None
        :param params: additional params to include in the request
        :type params: str | dict[str, T] | None
        :param max_attempts: maximum number of attempts to try for any request
        :type max_attempts: int
        :param kwargs: additional arguments to pass to requests.request
        :type kwargs: dict[str, T]
        :return: HTTP response
        :rtype: requests.Response
        """
        request = self._request

        if retry:
            if max_attempts is None:
                max_attempts = self.default_max_attempts

            # We retry only when it makes sense: either due to a network
            # partition (e.g. connection errors) or if the request failed
            # due to a server error such as 500s, timeouts, and so on.
            request = tenacity.retry(
                stop=tenacity.stop_after_attempt(max_attempts),
                wait=tenacity.wait_exponential(),
                retry=tenacity.retry_if_exception_type((
                    requests.exceptions.Timeout,
                    requests.exceptions.ConnectionError,
                    MesosServiceUnavailableException,
                    MesosInternalServerErrorException,
                )),
                reraise=True,
            )(request)

        try:
            return request(
                method=method,
                additional_headers=additional_headers,
                timeout=timeout,
                auth=auth,
                use_gzip_encoding=use_gzip_encoding,
                params=params,
                **kwargs
            )
        # If the request itself failed, an exception subclassed from
        # RequestException will be raised. Catch this and reraise as
        # MesosException since we want the caller to be able to catch
        # and handle this.
        except requests.exceptions.RequestException as err:
            raise MesosException('Request failed', err)

    def request_json(self,
                     method,
                     timeout=None,
                     auth=None,
                     payload=None,
                     decoder=None,
                     params=None,
                     **kwargs):
        """
        Make an HTTP request and deserialize the response as JSON. Optionally
        decode the deserialized json dict into a decoded object.

        :param method: request method
        :type method: str
        :param timeout: timeout in seconds
        :type timeout: float
        :param auth: auth scheme for the request
        :type auth: requests.auth.AuthBase
        :param payload: json payload in the request
        :type payload: dict[str, T] | str
        :param decoder: decoder for json response
        :type decoder: (dict) -> T
        :param params: additional params to include in the request
        :type params: str | dict[str, T]
        :param kwargs: additional arguments to pass to requests.request
        :type kwargs: dict[str, T]
        :return: JSON response
        :rtype: dict[str, T]
        """
        resp = self.request(method=method,
                            timeout=timeout,
                            auth=auth,
                            json=payload,
                            additional_headers=REQUEST_JSON_HEADERS,
                            params=params,
                            **kwargs)

        try:
            json_dict = ujson.loads(resp.text)
        except ValueError as exception:
            raise MesosException(
                'could not load JSON from "{data}"'.format(data=resp.text),
                exception)

        if decoder is not None:
            return decoder(json_dict)

        return json_dict

    def get_json(self,
                 timeout=None,
                 auth=None,
                 decoder=None,
                 params=None):
        """
        Send a GET request.

        :param timeout: timeout in seconds
        :type  timeout: float
        :param auth: auth scheme for the request
        :type auth: requests.auth.AuthBase
        :param decoder: decoder for json response
        :type decoder: (dict) -> T
        :param params: additional params to include in the request
        :type params: str | dict[str, U]
        :rtype: dict[str, U]
        """
        return self.request_json(METHOD_GET,
                                 timeout=timeout,
                                 auth=auth,
                                 decoder=decoder,
                                 params=params)

    def post_json(self,
                  timeout=None,
                  auth=None,
                  payload=None,
                  decoder=None,
                  params=None):
        """
        Sends a POST request.

        :param timeout: timeout in seconds
        :type  timeout: float
        :param auth: auth scheme for the request
        :type auth: requests.auth.AuthBase
        :param payload: post data
        :type  payload: dict[str, T] | str
        :param decoder: decoder for json response
        :type decoder: (dict) -> T
        :param params: additional params to include in the request
        :type params: str | dict[str, T]
        :rtype: dict[str, T]
        """
        return self.request_json(METHOD_POST,
                                 timeout=timeout,
                                 auth=auth,
                                 payload=payload,
                                 decoder=decoder,
                                 params=params)
