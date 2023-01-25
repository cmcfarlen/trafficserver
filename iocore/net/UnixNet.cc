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

#include "P_Net.h"
#include "I_AIO.h"
#include "I_EventIOStrategy.h"

using namespace std::literals;

ink_hrtime last_throttle_warning;
ink_hrtime last_shedding_warning;
int net_connections_throttle;
bool net_memory_throttle = false;
int fds_throttle;
int fds_limit = 8000;
ink_hrtime last_transient_accept_error;

NetHandler::Config NetHandler::global_config;
std::bitset<std::numeric_limits<unsigned int>::digits> NetHandler::active_thread_types;
const std::bitset<NetHandler::CONFIG_ITEM_COUNT> NetHandler::config_value_affects_per_thread_value{0x3};

extern "C" void fd_reify(struct ev_loop *);

void
initialize_thread_for_net(EThread *thread)
{
  NetHandler *nh = get_NetHandler(thread);

  new (reinterpret_cast<ink_dummy_for_new *>(nh)) NetHandler();
  new (reinterpret_cast<ink_dummy_for_new *>(get_PollCont(thread))) PollCont(thread->mutex, nh);
  nh->mutex  = new_ProxyMutex();
  nh->thread = thread;
  // TODO(cmcfarlen): Select appropriate strategy
  // TODO(cmcfarlen): sharing the mutex ok?
  nh->io_strategy = new EventIOStrategy(nh->mutex);

  memcpy(&nh->config, &NetHandler::global_config, sizeof(NetHandler::global_config));
  nh->configure_per_thread_values();

  nh->io_strategy->init_for_thread(thread);

  thread->set_tail_handler(nh);
}

// NetHandler method definitions

NetHandler::NetHandler() : Continuation(nullptr)
{
  SET_HANDLER(&NetHandler::mainNetEvent);
}

int
NetHandler::update_nethandler_config(const char *str, RecDataT, RecData data, void *)
{
  uint32_t *updated_member = nullptr; // direct pointer to config member for update.
  std::string_view name{str};

  if (name == "proxy.config.net.max_connections_in"sv) {
    updated_member = &NetHandler::global_config.max_connections_in;
    Debug("net_queue", "proxy.config.net.max_connections_in updated to %" PRId64, data.rec_int);
  } else if (name == "proxy.config.net.max_requests_in"sv) {
    updated_member = &NetHandler::global_config.max_requests_in;
    Debug("net_queue", "proxy.config.net.max_requests_in updated to %" PRId64, data.rec_int);
  } else if (name == "proxy.config.net.inactive_threshold_in"sv) {
    updated_member = &NetHandler::global_config.inactive_threshold_in;
    Debug("net_queue", "proxy.config.net.inactive_threshold_in updated to %" PRId64, data.rec_int);
  } else if (name == "proxy.config.net.transaction_no_activity_timeout_in"sv) {
    updated_member = &NetHandler::global_config.transaction_no_activity_timeout_in;
    Debug("net_queue", "proxy.config.net.transaction_no_activity_timeout_in updated to %" PRId64, data.rec_int);
  } else if (name == "proxy.config.net.keep_alive_no_activity_timeout_in"sv) {
    updated_member = &NetHandler::global_config.keep_alive_no_activity_timeout_in;
    Debug("net_queue", "proxy.config.net.keep_alive_no_activity_timeout_in updated to %" PRId64, data.rec_int);
  } else if (name == "proxy.config.net.default_inactivity_timeout"sv) {
    updated_member = &NetHandler::global_config.default_inactivity_timeout;
    Debug("net_queue", "proxy.config.net.default_inactivity_timeout updated to %" PRId64, data.rec_int);
  }

  if (updated_member) {
    *updated_member = data.rec_int; // do the actual update.
    // portable form of the update, an index converted to <void*> so it can be passed as an event cookie.
    void *idx = reinterpret_cast<void *>(static_cast<intptr_t>(updated_member - &global_config[0]));
    // Signal the NetHandler instances, passing the index of the updated config value.
    for (int i = 0; i < eventProcessor.n_thread_groups; ++i) {
      if (!active_thread_types[i]) {
        continue;
      }
      for (EThread **tp    = eventProcessor.thread_group[i]._thread,
                   **limit = eventProcessor.thread_group[i]._thread + eventProcessor.thread_group[i]._count;
           tp < limit; ++tp) {
        NetHandler *nh = get_NetHandler(*tp);
        if (nh) {
          nh->thread->schedule_imm(nh, TS_EVENT_MGMT_UPDATE, idx);
        }
      }
    }
  }

  return REC_ERR_OKAY;
}

void
NetHandler::init_for_process()
{
  // read configuration values and setup callbacks for when they change
  REC_ReadConfigInt32(global_config.max_connections_in, "proxy.config.net.max_connections_in");
  REC_ReadConfigInt32(global_config.max_requests_in, "proxy.config.net.max_requests_in");
  REC_ReadConfigInt32(global_config.inactive_threshold_in, "proxy.config.net.inactive_threshold_in");
  REC_ReadConfigInt32(global_config.transaction_no_activity_timeout_in, "proxy.config.net.transaction_no_activity_timeout_in");
  REC_ReadConfigInt32(global_config.keep_alive_no_activity_timeout_in, "proxy.config.net.keep_alive_no_activity_timeout_in");
  REC_ReadConfigInt32(global_config.default_inactivity_timeout, "proxy.config.net.default_inactivity_timeout");

  RecRegisterConfigUpdateCb("proxy.config.net.max_connections_in", update_nethandler_config, nullptr);
  RecRegisterConfigUpdateCb("proxy.config.net.max_requests_in", update_nethandler_config, nullptr);
  RecRegisterConfigUpdateCb("proxy.config.net.inactive_threshold_in", update_nethandler_config, nullptr);
  RecRegisterConfigUpdateCb("proxy.config.net.transaction_no_activity_timeout_in", update_nethandler_config, nullptr);
  RecRegisterConfigUpdateCb("proxy.config.net.keep_alive_no_activity_timeout_in", update_nethandler_config, nullptr);
  RecRegisterConfigUpdateCb("proxy.config.net.default_inactivity_timeout", update_nethandler_config, nullptr);

  Debug("net_queue", "proxy.config.net.max_connections_in updated to %d", global_config.max_connections_in);
  Debug("net_queue", "proxy.config.net.max_requests_in updated to %d", global_config.max_requests_in);
  Debug("net_queue", "proxy.config.net.inactive_threshold_in updated to %d", global_config.inactive_threshold_in);
  Debug("net_queue", "proxy.config.net.transaction_no_activity_timeout_in updated to %d",
        global_config.transaction_no_activity_timeout_in);
  Debug("net_queue", "proxy.config.net.keep_alive_no_activity_timeout_in updated to %d",
        global_config.keep_alive_no_activity_timeout_in);
  Debug("net_queue", "proxy.config.net.default_inactivity_timeout updated to %d", global_config.default_inactivity_timeout);
}

//
// The main event for NetHandler
int
NetHandler::mainNetEvent(int event, Event *e)
{
  if (TS_EVENT_MGMT_UPDATE == event) {
    intptr_t idx = reinterpret_cast<intptr_t>(e->cookie);
    // Copy to the same offset in the instance struct.
    config[idx] = global_config[idx];
    if (config_value_affects_per_thread_value[idx]) {
      this->configure_per_thread_values();
    }
    return EVENT_CONT;
  } else {
    ink_assert(trigger_event == e && (event == EVENT_INTERVAL || event == EVENT_POLL));
    return this->waitForActivity(-1);
  }
}

int
NetHandler::waitForActivity(ink_hrtime timeout)
{
  NET_INCREMENT_DYN_STAT(net_handler_run_stat);
  SCOPED_MUTEX_LOCK(lock, mutex, this->thread);

  return io_strategy->waitForActivity(timeout);
}

void
NetHandler::signalActivity()
{
  io_strategy->signalActivity();
}

void
NetHandler::configure_per_thread_values()
{
  // figure out the number of threads and calculate the number of connections per thread
  int threads                   = eventProcessor.thread_group[ET_NET]._count;
  max_connections_per_thread_in = config.max_connections_in / threads;
  max_requests_per_thread_in    = config.max_requests_in / threads;
  Debug("net_queue", "max_connections_per_thread_in updated to %d threads: %d", max_connections_per_thread_in, threads);
  Debug("net_queue", "max_requests_per_thread_in updated to %d threads: %d", max_requests_per_thread_in, threads);
}
