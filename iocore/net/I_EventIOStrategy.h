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

#include "I_IOBuffer.h"
#include "I_Action.h"
#include "I_VIO.h"

class EventIOStrategy;
struct PollCont : public Continuation {
  EventIOStrategy *io_strategy;
  PollDescriptor *pollDescriptor;
  PollDescriptor *nextPollDescriptor;
  int poll_timeout;

  PollCont(Ptr<ProxyMutex> &m, int pt = net_config_poll_timeout);
  PollCont(Ptr<ProxyMutex> &m, EventIOStrategy *nh, int pt = net_config_poll_timeout);
  ~PollCont() override;
  int pollEvent(int, Event *);
  void do_poll(ink_hrtime timeout);
};
class InactivityCop;

class EventIOStrategy : public IOStrategy
{
public:
  EventIOStrategy(Ptr<ProxyMutex> &m);
  VIO *read(IOCompletionTarget *, int64_t nbytes, MIOBuffer *buf) override;
  VIO *write(IOCompletionTarget *, int64_t nbytes, MIOBuffer *buf) override;
  Action *accept(IOCompletionTarget *, int flags) override;
  Action *connect(IOCompletionTarget *, sockaddr const *target) override;

  void read_disable(VIO *) override;
  void write_disable(VIO *) override;

  int waitForActivity(ink_hrtime timeout) override;
  void signalActivity() override;
  void init_for_thread(EThread *thread);

  /**
    Start to handle read & write event on a NetEvent.
    Initial the socket fd of ne for polling system.
    Only be called when holding the mutex of this NetHandler.

  @param ne NetEvent to be managed by this NetHandler.
    @return 0 on success, ne->nh set to this NetHandler.
      -ERRNO on failure.
            */
  int startIO(NetEvent *ne);
  /**
    Stop to handle read & write event on a NetEvent.
    Remove the socket fd of ne from polling system.
    Only be called when holding the mutex of this NetHandler and must call stopCop(ne) first.

    @param ne NetEvent to be released.
    @return ne->nh set to nullptr.
   */
  void stopIO(NetEvent *ne);

  /**
    Start to handle active timeout and inactivity timeout on a NetEvent.
    Put the ne into open_list. All NetEvents in the open_list is checked for timeout by InactivityCop.
    Only be called when holding the mutex of this NetHandler and must call startIO(ne) first.

    @param ne NetEvent to be managed by InactivityCop
   */
  void startCop(NetEvent *ne);
  /**
    Stop to handle active timeout and inactivity on a NetEvent.
    Remove the ne from open_list and cop_list.
    Also remove the ne from keep_alive_queue and active_queue if its context is IN.
    Only be called when holding the mutex of this NetHandler.

    @param ne NetEvent to be released.
   */
  void stopCop(NetEvent *ne);

private:
  void process_enabled_list();
  void process_ready_list();
  void net_signal_hook_callback();

  void remove_from_keep_alive_queue(NetEvent *ne);
  void remove_from_active_queue(NetEvent *ne);
  void manage_keep_alive_queue();
  bool manage_active_queue(NetEvent *ne, bool ignore_queue_size = false);
  void add_to_keep_alive_queue(NetEvent *ne);
  bool add_to_active_queue(NetEvent *ne);
  void _close_ne(NetEvent *ne, ink_hrtime now, int &handle_event, int &closed, int &total_idle_time, int &total_idle_count);

  /**
    Release a ne and free it.

  @param ne NetEvent to be detached.
      */
  void free_netevent(NetEvent *ne);

  QueM(NetEvent, NetState, read, ready_link) read_ready_list;
  QueM(NetEvent, NetState, write, ready_link) write_ready_list;
  ASLLM(NetEvent, NetState, read, enable_link) read_enable_list;
  ASLLM(NetEvent, NetState, write, enable_link) write_enable_list;
  Que(NetEvent, open_link) open_list;
  DList(NetEvent, cop_link) cop_list;
  Que(NetEvent, keep_alive_queue_link) keep_alive_queue;
  uint32_t keep_alive_queue_size = 0;
  Que(NetEvent, active_queue_link) active_queue;
  uint32_t active_queue_size = 0;
#ifdef TS_USE_LINUX_IO_URING
  EventIO uring_evio;
#endif
  EThread *thread     = nullptr;
  PollCont *poll_cont = nullptr;

  // this is needed for the stat macros
  Ptr<ProxyMutex> mutex;

#if HAVE_EVENTFD
  int evfd = ts::NO_FD;
#else
  int evpipe[2];
#endif
  EventIO *ep = nullptr;

  friend class PollCont;
  friend class InactivityCop;
};

TS_INLINE int
EventIOStrategy::startIO(NetEvent *ne)
{
  ink_assert(this->mutex->thread_holding == this_ethread());
  ink_assert(ne->get_thread() == this_ethread());
  int res = 0;

  PollDescriptor *pd = this->poll_cont->pollDescriptor;
  if (ne->ep.start(pd, ne, EVENTIO_READ | EVENTIO_WRITE) < 0) {
    res = errno;
    // EEXIST should be ok, though it should have been cleared before we got back here
    if (errno != EEXIST) {
      Debug("iocore_net", "NetHandler::startIO : failed on EventIO::start, errno = [%d](%s)", errno, strerror(errno));
      return -res;
    }
  }

  if (ne->read.triggered == 1) {
    read_ready_list.enqueue(ne);
  }
  ne->ios = this;
  return res;
}

TS_INLINE void
EventIOStrategy::stopIO(NetEvent *ne)
{
  ink_release_assert(ne->ios == this);

  ne->ep.stop();

  read_ready_list.remove(ne);
  write_ready_list.remove(ne);
  if (ne->read.in_enabled_list) {
    read_enable_list.remove(ne);
    ne->read.in_enabled_list = 0;
  }
  if (ne->write.in_enabled_list) {
    write_enable_list.remove(ne);
    ne->write.in_enabled_list = 0;
  }

  ne->nh = nullptr;
}

TS_INLINE void
EventIOStrategy::startCop(NetEvent *ne)
{
  ink_assert(this->mutex->thread_holding == this_ethread());
  ink_release_assert(ne->ios == this);
  ink_assert(!open_list.in(ne));

  open_list.enqueue(ne);
}

TS_INLINE void
EventIOStrategy::stopCop(NetEvent *ne)
{
  ink_release_assert(ne->ios == this);

  open_list.remove(ne);
  cop_list.remove(ne);
  remove_from_keep_alive_queue(ne);
  remove_from_active_queue(ne);
}
