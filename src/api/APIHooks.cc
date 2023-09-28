/** @file

  Implements the Traffic Server C API functions.

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

#include "api/InkAPIInternal.h"

static ClassAllocator<APIHook> apiHookAllocator("apiHookAllocator");

APIHook *
APIHook::next() const
{
  return m_link.next;
}

APIHook *
APIHook::prev() const
{
  return m_link.prev;
}

int
APIHook::invoke(int event, void *edata) const
{
  if (event == EVENT_IMMEDIATE || event == EVENT_INTERVAL || event == TS_EVENT_HTTP_TXN_CLOSE) {
    if (ink_atomic_increment((int *)&m_cont->m_event_count, 1) < 0) {
      ink_assert(!"not reached");
    }
  }
  WEAK_MUTEX_TRY_LOCK(lock, m_cont->mutex, this_ethread());
  if (!lock.is_locked()) {
    // If we cannot get the lock, the caller needs to restructure to handle rescheduling
    ink_release_assert(0);
  }
  return m_cont->handleEvent(event, edata);
}

int
APIHook::blocking_invoke(int event, void *edata) const
{
  if (event == EVENT_IMMEDIATE || event == EVENT_INTERVAL || event == TS_EVENT_HTTP_TXN_CLOSE) {
    if (ink_atomic_increment((int *)&m_cont->m_event_count, 1) < 0) {
      ink_assert(!"not reached");
    }
  }

  WEAK_SCOPED_MUTEX_LOCK(lock, m_cont->mutex, this_ethread());

  return m_cont->handleEvent(event, edata);
}

APIHook *
APIHooks::head() const
{
  return m_hooks.head;
}

void
APIHooks::append(INKContInternal *cont)
{
  APIHook *api_hook;

  api_hook         = THREAD_ALLOC(apiHookAllocator, this_thread());
  api_hook->m_cont = cont;

  m_hooks.enqueue(api_hook);
}

void
APIHooks::clear()
{
  APIHook *hook;
  while (nullptr != (hook = m_hooks.pop())) {
    THREAD_FREE(hook, apiHookAllocator, this_thread());
  }
}
