.. Licensed to the Apache Software Foundation (ASF) under one
   or more contributor license agreements.  See the NOTICE file
   distributed with this work for additional information
   regarding copyright ownership.  The ASF licenses this file
   to you under the Apache License, Version 2.0 (the
   "License"); you may not use this file except in compliance
   with the License.  You may obtain a copy of the License at

   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing,
   software distributed under the License is distributed on an
   "AS IS" BASIS, WITHOUT WARRANTIES OR CONDITIONS OF ANY
   KIND, either express or implied.  See the License for the
   specific language governing permissions and limitations
   under the License.

.. include:: ../../../common.defs

.. default-domain:: cpp

TSUrlHostGet
************

Traffic Server URL component retrieval API.

Synopsis
========

.. code-block:: cpp

    #include <ts/ts.h>

.. function:: const char * TSUrlHostGet(TSMBuffer bufp, TSMLoc offset, int * length)
.. function:: const char * TSUrlSchemeGet(TSMBuffer bufp, TSMLoc offset, int * length)
.. function:: const char * TSUrlRawSchemeGet(TSMBuffer bufp, TSMLoc offset, int * length)
.. function:: const char * TSUrlUserGet(TSMBuffer bufp, TSMLoc offset, int * length)
.. function:: const char * TSUrlPasswordGet(TSMBuffer bufp, TSMLoc offset, int* length)
.. function:: int TSUrlPortGet(TSMBuffer bufp, TSMLoc offset)
.. function:: int TSUrlRawPortGet(TSMBuffer bufp, TSMLoc offset)
.. function:: const char * TSUrlPathGet(TSMBuffer bufp, TSMLoc offset, int * length)
.. function:: const char * TSUrlHttpQueryGet(TSMBuffer bufp, TSMLoc offset, int * length)
.. function:: const char * TSUrlHttpFragmentGet(TSMBuffer bufp, TSMLoc offset, int * length)

Description
===========

The URL data structure is a parsed version of a standard internet URL. The
Traffic Server URL API provides access to URL data stored in marshal
buffers. The URL functions can create, copy, retrieve or delete entire URLs,
and retrieve or modify parts of URLs, such as their host, port or scheme
information.

:func:`TSUrlSchemeGet`, :func:`TSUrlRawSchemeGet`, :func:`TSUrlUserGet`, :func:`TSUrlPasswordGet`,
:func:`TSUrlHostGet`, :func:`TSUrlPathGet`, :func:`TSUrlHttpQueryGet`
and :func:`TSUrlHttpFragmentGet` each retrieve an internal pointer to the
specified portion of the URL from the marshall buffer :arg:`bufp`. The length
of the returned string is placed in :arg:`length` and a pointer to the URL
portion is returned.

If a request URL does not have a explicit scheme, :func:`TSUrlRawSchemeGet` will return null and
set :arg:`length` to zero.  :func:`TSUrlSchemeGet`, will return the scheme corresponding to the
URL type (HTTP or HTTPS) if there is no explicit scheme.

:func:`TSUrlPortGet` retrieves the port number portion of the URL located at
:arg:`offset` within the marshal buffer :arg:`bufp`. If there is no explicit
port number in the URL, a canonicalized valued is returned based on the URL
scheme.

:func:`TSUrlRawPortGet` also retrieves the port number portion of the URL located at
:arg:`offset` within the marshal buffer :arg:`bufp`. If there is no explicit
port number in the URL, zero is returned.

Return Values
=============

All APIs except :func:`TSUrlPortGet` and :func:`TSUrlRawPortGet` return a string, which is
not guaranteed to be null-terminated. You must therefore always use the :arg:`length` value
to determine the actual length of the returned string.

:func:`TSUrlPortGet` simply returns the port number as an integer, possibly
canonicalized with :literal:`80` for HTTP and :literal:`443` for HTTPS schemes. If
there is neither port nor scheme information available in the URL, :literal:`0`
is returned. :func:`TSUrlRawPortGet`, by contrast, returns 0 in all cases where the
port is not explicitly present in the URL.

See Also
========

:manpage:`TSAPI(3ts)`,
:manpage:`TSUrlCreate(3ts)`,
:manpage:`TSHttpHdrUrlGet(3ts)`,
:manpage:`TSUrlHostSet(3ts)`,
:manpage:`TSUrlStringGet(3ts)`,
:manpage:`TSUrlPercentEncode(3ts)`
