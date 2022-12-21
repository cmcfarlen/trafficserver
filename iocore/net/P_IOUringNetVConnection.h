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

#include "I_NetVConnection.h"
#include "IOUringConnection.h"
#include "IOUringNetAccept.h"
#include "I_IO_URING.h"

class IOUringNetVConnection;

class IOUringReader : public IOUringCompletionHandler
{
public:
  void handle_complete(io_uring_cqe *) override;

  std::string
  id() const override
  {
    return "read";
  }
  VIO vio;
  bool in_progress = false; // is there an outstanding read in the io_uring?
  IOUringNetVConnection *vc;
  IOVec tiovec[NET_MAX_IOV];
  struct msghdr msg;
};

class IOUringWriter : public IOUringCompletionHandler
{
public:
  void handle_complete(io_uring_cqe *) override;
  std::string
  id() const override
  {
    return "write";
  }
  VIO vio;
  bool in_progress = false; // is there an outstanding write in the io_uring?
  IOUringNetVConnection *vc;
  IOVec tiovec[NET_MAX_IOV];
  struct msghdr msg;
};

class IOUringNetVConnection : public NetVConnection
{
public:
  IOUringNetVConnection();
  ~IOUringNetVConnection() override;

  VIO *do_io_read(Continuation *c, int64_t nbytes, MIOBuffer *buf) override;
  VIO *do_io_write(Continuation *c, int64_t nbytes, IOBufferReader *buf, bool owner = false) override;
  void do_io_close(int lerrno = -1) override;
  void do_io_shutdown(ShutdownHowTo_t howto) override;
  void set_active_timeout(ink_hrtime timeout_in) override;
  void set_inactivity_timeout(ink_hrtime timeout_in) override;
  void set_default_inactivity_timeout(ink_hrtime timeout_in) override;
  bool is_default_inactivity_timeout() override;
  void cancel_active_timeout() override;
  void cancel_inactivity_timeout() override;
  void add_to_keep_alive_queue() override;
  void remove_from_keep_alive_queue() override;
  bool add_to_active_queue() override;
  ink_hrtime get_active_timeout() override;
  ink_hrtime get_inactivity_timeout() override;
  void apply_options() override;
  void reenable(VIO *vio) override;
  void reenable_re(VIO *vio) override;
  SOCKET get_socket() override;
  int set_tcp_congestion_control(int side) override;
  void set_local_addr() override;
  void set_remote_addr() override;
  void set_remote_addr(const sockaddr *) override;
  void set_mptcp_state() override;
  int connectUp(EThread *t, int fd);

  const int id;
  IOUringConnection con;
  Action action_;
  int recursion                   = 0;
  bool from_accept_thread         = false;
  IOUringNetAccept *accept_object = nullptr;
  int ops_in_flight               = 0;
  bool closing                    = false;

  int acceptEvent(int event, Event *e);

  // Make progress on reads and writes
  void prep_read();
  void prep_write();

private:
  int startEvent(int event, Event *e);
  int mainEvent(int event, Event *e);
  int write_signal_and_update(int event);
  void load_buffer_and_write(int64_t towrite, MIOBufferAccessor &buf);
  void _close();

  std::function<void(int)> _connectUp_handler;
  std::function<void(int)> _connectUp_handler_2;

  bool closed;
  IOUringReader read;
  IOUringWriter write;
};
