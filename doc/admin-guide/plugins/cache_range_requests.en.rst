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


.. include:: ../../common.defs

.. _admin-plugins-cache-range-requests:


Cache Range Requests Plugin
***************************

Description
===========

Most origin servers support HTTP/1.1 range requests (rfc 7233).
ATS internally handles range request caching in one of 2 ways:

* Don't cache range requests.
* Only server range requests from a wholly cached object.

This plugin allows you to remap individual range requests so that they
are stored as individual objects in the ATS cache when subsequent range
requests are likely to use the same range.  This spreads range requests
over multiple stripes thereby reducing I/O wait and system load averages.

:program:`cache_range_requests` reads the range request header byte range
value and then creates a new ``cache key URL`` using the original request
url with the range value appended to it.  The range header is removed
where appropriate from the requests and the origin server response code
is changed from a 206 to a 200 to insure that the object is written to
cache using the new cache key url.  The response code sent to the client
will be changed back to a 206 and all requests to the origin server will
contain the range header so that the correct response is received.

The :program:`cache_range_requests` plugin by itself has no logic to
efficiently manage overlapping ranges.  It is best to use this plugin
in conjunction with a smart client that only requests predetermined
non overlapping cache ranges (request blocking) or as a helper for the
:program:`slice` plugin.

Only requests which contain the ``Range: <units>=`` GET header
will be served by the :program:`cache_range_requests` plugin.

If/when ATS implements partial object caching this plugin will
become deprecated.

*NOTE* Given a multi range request the :program:`cache_range_requests`
only processes the first range and ignores the rest.

How to run the plugin
=====================

The plugin can run as a global plugin (a single global instance configured
using :file:`plugin.config`) or as per-remap plugin (a separate instance
configured per remap rule in :file:`remap.config`).

Global instance
---------------

.. code::

  $ cat plugin.config
  cache_range_requests.so


Per-remap instance
------------------

.. code::

  $cat remap.config
  map http://www.example.com http://www.origin.com \
      @plugin=cache_range_requests.so


If both global and per-remap instance are used the per-remap configuration
would take precedence (per-remap configuration would be applied and the
global configuration ignored).

Plugin options
==============


Parent Selection as Cache Key
-----------------------------

.. option:: --ps-cachekey
.. option:: -p

Without this option parent selection is based solely on the hash of a
URL Path a URL is requested from the same upstream parent cache listed
in parent.config


With this option parent selection is based on the full ``cache key URL``
which includes information about the partial content range.  In this mode,
all requests (include partial content) will use consistent hashing method
for parent selection.

X-Crr-Ims header support
------------------------

.. option:: --consider-ims
.. option:: -c
.. option:: --ims-header=[header name] (default: X-Crr-Ims)
.. option:: -i

To support slice plugin self healing an option to force revalidation
after cache lookup complete was added.  This option is triggered by a
special header:

.. code::

    X-Crr-Ims: Tue, 19 Nov 2019 13:26:45 GMT

When this header is provided and a `cache hit fresh` is encountered the
``Date`` header of the object in cache is compared to this header date
value.  If the cache date is *less* than this IMS date then the object
is marked as STALE and an appropriate If-Modified-Since or If-Match
request along with this X-Crr-Ims header is passed up to the parent.

In order for this to properly work in a CDN each cache in the
chain *SHOULD* also contain a remap rule with the
:program:`cache_range_requests` plugin with this option set.

When used with the :program:`slice` plugin its `--crr-ims-header`
option must have the same value (or not be defined) in order to work.

Presence of the `--ims-header` automatically sets the `--consider-ims` option.

X-Crr-Ident header support
--------------------------

.. option:: --consider-ident
.. option:: -d
.. option:: --ident-header=[header name] (default: X-Crr-Ident)
.. option:: -j

This supports the slice plugin which makes multiple adjacent range
requests. The slice plugin will record the identifier of the first range
request (Etag, or Last-Modified in that order) and will add the value
to this header.

.. code::

    X-Crr-Ident: Etag: "foo"
    X-Crr-Ident: Last-Modified: Tue, 19 Nov 2019 13:26:45 GMT

During the cache lookup hook if a range request is considered STALE
the identifier from this header will be compared to the stale cache
identifier. If the values match the response will be changed to FRESH,
preventing the transaction from contacting a parent.

When used with the :program:`slice` plugin its `--crr-ident-header`
option must have the same value (or not be defined) in order to work.

Presence of the `--ident-header` automatically sets the `--consider-ident`
option.

Don't modify the Cache Key
--------------------------

.. option:: --no-modify-cachekey
.. option:: -n

With each transaction TSCacheUrlSet may only be called once.  When
using the `cache_range_requests` plugin in conjunction with the
`cachekey` plugin the option `--include-headers=Range` should be
added as a `cachekey` parameter with this option.  Configuring this
incorrectly *WILL* result in cache poisoning.

.. code::

       map http://ats/ http://parent/ \
           @plugin=cachekey.so @pparam=--include-headers=Range \
           @plugin=cache_range_requests.so @pparam=--no-modify-cachekey

*Without this `cache_range_requests` plugin option*

*IF* the TSCacheUrlSet call in cache_range_requests fails, an error is
generated in the logs and the cache_range_requests plugin will disable
transaction caching in order to avoid cache poisoning.

Verify Cacheability
-------------------

.. option:: --verify-cacheability
.. option:: -v

This option causes the plugin to verify whether the requested object is
cacheable.

By default, an object's cacheability is not verified after
the plugin changes the response code of the upstream response from 206
to 200 to force the object into cache. When this option is enabled,
cacheability is considered, and if the object is not cacheable, the
status code is reset back to 206, which leads to the object not being cached.

This option is useful when used with other plugins, such as Cache Promote.

Cache Complete Responses
------------------------

.. option:: --cache-complete-responses
.. option:: -r

This option causes the plugin to cache complete responses (200 OK). By default,
only 206 Partial Content responses are cached by this plugin; without this flag,
any 200 OK observed will be marked as not cacheable.

This option is intended to cover the case when an origin responds with a 200 OK
when the requested range exceeds the size of the object. For example, if an object
is 500 bytes, and the requested range is for bytes 0-5000, some origins will
respond with a 206 and a `Content-Range` header, while others may respond with a
200 OK and no `Content-Range` header. The same origin that responds with a 200 OK
when the requested range exceeds the object size will serve 206s when the range is
smaller than or within the bytes of the object.

**NOTE:** This option *should be used carefully* with full knowledge of how
cache keys are set for a given remap rule that relies on this behavior and origin
response mechanics. For example, when this option is the sole argument to
`cache_range_requests.so` and no other plugins are in use, the behavior could be
abused, especially if the origin always responds with 200 OKs. This is because
the plugin will automatically include the requested `Range` in the cache key.
This means that arbitrary ranges can be used to pollute the cache with different
combinations of ranges, which will lead to many copies of the same complete object
stored under different cache keys.

For this reason, if the plugin is instructed to cache complete responses, `Range`
request headers coming into the remap should ideally be normalized. Normalization
can be accomplished by using the slice plugin *without* the `--ref-relative` argument
which is disabled by default. The cache key plugin can also be used to tightly control
the construction of the cache key itself.

The preferred means of using this plugin option is with the following plugins:
- slice to normalize the requested ranges, *without* the `--ref-relative` option
- cachekey to control the cache key, including the `Range` header normalized by slice
- cache range requests with `--no-modify-cachekey` and `--cache-complete-responses`

Configuration examples
======================

Global plugin
-------------

.. code::

    cache_range_requests.so --ps-cachekey --consider-ims --no-modify-cachekey

or

.. code::

    cache_range_requests.so -p -c -n

Remap plugin
------------

.. code::

    map http://ats http://parent @plugin=cache_range_requests.so @pparam=--ps-cachekey @pparam=--consider-ims @pparam=--no-modify-cachekey

or

.. code::

    map http://ats http://parent @plugin=cache_range_requests.so @pparam=-p @pparam=-c @pparam=-n
