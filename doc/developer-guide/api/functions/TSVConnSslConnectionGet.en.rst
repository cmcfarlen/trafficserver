.. Licensed to the Apache Software Foundation (ASF) under one or more
   contributor license agreements.  See the NOTICE file distributed
   with this work for additional information regarding copyright
   ownership.  The ASF licenses this file to you under the Apache
   License, Version 2.0 (the "License"); you may not use this file
   except in compliance with the License.  You may obtain a copy of
   the License at

   http://www.apache.org/licenses/LICENSE-2.0

   Unless required by applicable law or agreed to in writing, software
   distributed under the License is distributed on an "AS IS" BASIS,
   WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or
   implied.  See the License for the specific language governing
   permissions and limitations under the License.

.. include:: ../../../common.defs

.. default-domain:: cpp

TSVConnSslConnectionGet
***********************

Synopsis
========

.. code-block:: cpp

    #include <ts/ts.h>

.. function:: TSSslConnection TSVConnSslConnectionGet(TSVConn svc)

Description
===========

Get the SSL (per connection) object from the SSl connection :arg:`svc`.

Types
=====

.. type:: TSSslConnection

	The SSL (per connection) object. This is an opaque type that can be cast to the
	appropriate type (:code:`SSL *` for the OpenSSL library).
