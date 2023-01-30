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
#include "I_IOStrategy.h"

struct RecRawStatBlock;
struct EventIOStrategyConfig {
  int poll_timeout               = 10;
  int inactivity_check_frequency = 1;       // proxy.config.net.inactivity_check_frequency
  RecRawStatBlock *stat_block    = nullptr; // currently stats from P_Net.h

  uint32_t max_connections_per_thread_in = 0;
  uint32_t max_requests_per_thread_in    = 0;
};

class EventIOStrategyImpl;
class EventIOStrategy : public IOStrategy
{
public:
  EventIOStrategy(Ptr<ProxyMutex> &m, const EventIOStrategyConfig &config);
  ~EventIOStrategy() override {}
  VIO *read(IOCompletionTarget *, int fd, int64_t nbytes, MIOBuffer *buf) override;
  VIO *write(IOCompletionTarget *, int fd, int64_t nbytes, MIOBuffer *buf) override;
  Action *accept(IOCompletionTarget *, int fd, int flags) override;
  Action *connect(IOCompletionTarget *, sockaddr const *target) override;

  void read_disable(VIO *) override;
  void write_disable(VIO *) override;

  int waitForActivity(ink_hrtime timeout) override;
  void signalActivity() override;
  void init_for_thread(EThread *thread) override;

#ifdef MOVE_TO_PRIVATE
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
#endif

private:
  void process_enabled_list();
  void process_ready_list();
  void net_signal_hook_callback();

  EventIOStrategyImpl *impl = nullptr;

  friend struct PollCont;
};
