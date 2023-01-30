/** @file

A brief file description

  @section license License

    Licensed to the Apache Software Foundation (ASF) under one
  or more contributor license agreements.  See the NOTICE file
  distributed with this work for additional information
  regarding copyright ownership.  The ASF licenses this file
  to you under the Apache License, Version 2.0 (the
  "License"); you may not use this file except in compliance
  with the License.  You may obtain a copy of the License at

      http://www.apache.org/licenses/LICENSE-2.0

  Unless required by applicable law or agreed to in writing, software
  distributed under the License is distributed on an "AS IS" BASIS,
  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
  See the License for the specific language governing permissions and
  limitations under the License.

 */

#pragma once
#define I_IOStrategy_h

#include <cstdint>

#include "I_EThread.h"
#include "I_IOBuffer.h"
#include "I_VIO.h"

#include "tscore/ink_hrtime.h"
#include "tscore/ink_inet.h"

class IOStrategy;
class IOCompletionTarget
{
public:
  // return true to continue, false to stop
  virtual bool progress(VIO *vio, int res) = 0;
  virtual bool complete(VIO *vio, int res) = 0;
};

// Just signals the continuation for the callbacks
class ContinuationVIOCompletionTarget : public IOCompletionTarget
{
public:
  ContinuationVIOCompletionTarget(Continuation *);
};

class IOStrategy
{
public:
  virtual ~IOStrategy() {}
  virtual VIO *read(IOCompletionTarget *, int fd, int64_t nbytes, MIOBuffer *buf)  = 0;
  virtual VIO *write(IOCompletionTarget *, int fd, int64_t nbytes, MIOBuffer *buf) = 0;
  virtual Action *accept(IOCompletionTarget *, int fd, int flags)                  = 0;
  virtual Action *connect(IOCompletionTarget *, sockaddr const *target)            = 0;

  virtual void read_disable(VIO *)  = 0;
  virtual void write_disable(VIO *) = 0;

  // NetHandler will need these to defer to
  virtual int waitForActivity(ink_hrtime timeout) = 0;
  virtual void signalActivity()                   = 0;
  virtual void init_for_thread(EThread *)         = 0;
};
