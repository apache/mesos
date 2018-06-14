---
title: Apache Mesos - RecordIO Data Format
layout: documentation
---

# RecordIO Data Format

## Overview

RecordIO is the name for a set of binary data exchange formats.
The basic idea is to divide the data into individual chunks, called 'records',
and then to prepend to every record its length in bytes, followed by the data.

Since there is no formal specification of the RecordIO format, there tend
to be slight incompatibilities between RecordIO implementations. Common
differences include, for example, a magic value at the beginning of the
stream to indicate the stream format, encoding the record length using
fixed-size fields with native integers or extra fields to suppord compressed
record content.

Therefore, when using a third-party library, one needs to verify that
the implementation matches the RecordIO format used by Mesos, as described
below.


## Mesos RecordIO Format

The [BNF grammar](http://www.w3.org/Protocols/rfc2616/rfc2616-sec2.html#sec2.1)
for a RecordIO-encoded streaming response is:

```
    records         = *record

    record          = record-size LF record-data

    record-size     = 1*DIGIT
    record-data     = record-size(OCTET)
```

`record-size` should be interpreted as an unsigned 64-bit integer (`uint64`).

For example, a stream may look like:

```
128\n
{"type": "SUBSCRIBED","subscribed": {"framework_id": {"value":"12220-3440-12532-2345"},"heartbeat_interval_seconds":15.0}20\n
{"type":"HEARTBEAT"}675\n
...
```

In pseudo-code, this could be parsed with something like the following:

```
  while (true) {
    do {
      lengthBytes = readline()
    } while (lengthBytes.length < 1)

    messageLength = parseInt(lengthBytes);
    messageBytes = read(messageLength);
    process(messageBytes);
  }
```

Network intermediaries (e.g., proxies) are free to change the chunk
boundaries; this should not have any effect on the recipient application.


## Streaming HTTP requests

Within Mesos, RecordIO is used as data format for API endpoints
that support streaming responses. The implementation expects the
client to be aware of the following non-standard HTTP header fields
when using such endpoints:

The `Message-Accept` header must be set by clients that request a
streaming response from the server by setting the `Accept` header to
`application/recordio`. It specifies which media types the client
accepts encoded in the RecordIO stream, and its content must have the
same format as the standard HTTP `Accept` header.

The `Message-Content-Type` is set by the server when streaming the
response to an API call. It specifies the content type of the data
encoded in the RecordIO stream and has the same format as the
standard HTTP `Content-Type` header.

Currently, the only supported MIME types for both header fields are
`application/json` and `application/x-protobuf`.
