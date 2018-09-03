/** @file
 *
 *  A brief file description
 *
 *  @section license License
 *
 *  Licensed to the Apache Software Foundation (ASF) under one
 *  or more contributor license agreements.  See the NOTICE file
 *  distributed with this work for additional information
 *  regarding copyright ownership.  The ASF licenses this file
 *  to you under the Apache License, Version 2.0 (the
 *  "License"); you may not use this file except in compliance
 *  with the License.  You may obtain a copy of the License at
 *
 *      http://www.apache.org/licenses/LICENSE-2.0
 *
 *  Unless required by applicable law or agreed to in writing, software
 *  distributed under the License is distributed on an "AS IS" BASIS,
 *  WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 *  See the License for the specific language governing permissions and
 *  limitations under the License.
 */

#include "QUICFlowController.h"
#include "QUICFrame.h"

//
// QUICRateAnalyzer
//
void
QUICRateAnalyzer::update(QUICOffset offset)
{
  ink_hrtime now = Thread::get_hrtime();
  if (offset > 0 && now > this->_start_time) {
    this->_rate = static_cast<double>(offset) / (now - this->_start_time);
  }
}

uint64_t
QUICRateAnalyzer::expect_recv_bytes(ink_hrtime time)
{
  return static_cast<uint64_t>(time * this->_rate);
}

//
// QUICFlowController
//
uint64_t
QUICFlowController::credit() const
{
  return this->current_limit() - this->current_offset();
}

QUICOffset
QUICFlowController::current_offset() const
{
  return this->_offset;
}

QUICOffset
QUICFlowController::current_limit() const
{
  return this->_limit;
}

int
QUICFlowController::update(QUICOffset offset)
{
  if (this->_offset <= offset) {
    if (offset > this->_limit) {
      return -1;
    }
    this->_offset = offset;
  }

  return 0;
}

void
QUICFlowController::forward_limit(QUICOffset limit)
{
  // MAX_(STREAM_)DATA might be unorderd due to delay
  // Just ignore if the size was smaller than the last one
  if (this->_limit > limit) {
    return;
  }
  this->_limit = limit;
}

void
QUICFlowController::set_limit(QUICOffset limit)
{
  ink_assert(this->_limit == UINT64_MAX || this->_limit == limit);
  this->_limit = limit;
}

// For RemoteFlowController, caller of this function should also check QUICStreamManager::will_generate_frame()
bool
QUICFlowController::will_generate_frame(QUICEncryptionLevel level)
{
  if (!this->_is_level_matched(level)) {
    return false;
  }

  return this->_frame != nullptr;
}

QUICFrameUPtr
QUICFlowController::generate_frame(QUICEncryptionLevel level, uint64_t connection_credit, uint16_t maximum_frame_size)
{
  QUICFrameUPtr frame = QUICFrameFactory::create_null_frame();

  if (!this->_is_level_matched(level)) {
    return frame;
  }

  if (this->_frame && this->_frame->size() <= maximum_frame_size) {
    frame        = std::move(this->_frame);
    this->_frame = nullptr;
  }

  return frame;
}

//
// QUICRemoteFlowController
//
void
QUICRemoteFlowController::forward_limit(QUICOffset offset)
{
  QUICFlowController::forward_limit(offset);
  this->_blocked = false;
}

int
QUICRemoteFlowController::update(QUICOffset offset)
{
  int ret = QUICFlowController::update(offset);

  // Create BLOCKED(_STREAM) frame
  // The frame will be sent if stream has something to send.
  if (!this->_blocked && offset >= this->_limit) {
    this->_frame   = this->_create_frame();
    this->_blocked = true;
  }

  return ret;
}

//
// QUICLocalFlowController
//
QUICOffset
QUICLocalFlowController::current_limit() const
{
  return this->_advertized_limit;
}

void
QUICLocalFlowController::forward_limit(QUICOffset offset)
{
  QUICFlowController::forward_limit(offset);

  // Create MAX_(STREAM_)DATA frame. The frame will be sent on next WRITE_READY event on QUICNetVC
  if (this->_need_to_gen_frame()) {
    this->_frame            = this->_create_frame();
    this->_advertized_limit = this->_limit;
  }
}

int
QUICLocalFlowController::update(QUICOffset offset)
{
  if (this->_offset <= offset) {
    this->_analyzer.update(offset);
  }
  return QUICFlowController::update(offset);
}

void
QUICLocalFlowController::set_limit(QUICOffset limit)
{
  this->_advertized_limit = limit;
  QUICFlowController::set_limit(limit);
}

bool
QUICLocalFlowController::_need_to_gen_frame()
{
  QUICOffset threshold = this->_analyzer.expect_recv_bytes(2 * this->_rtt_provider->smoothed_rtt());
  if (this->_offset + threshold > this->_advertized_limit) {
    return true;
  }

  return false;
}

//
// QUIC[Remote|Local][Connection|Stream]FlowController
//
QUICFrameUPtr
QUICRemoteConnectionFlowController::_create_frame()
{
  return QUICFrameFactory::create_blocked_frame(this->_offset);
}

QUICFrameUPtr
QUICLocalConnectionFlowController::_create_frame()
{
  return QUICFrameFactory::create_max_data_frame(this->_limit);
}

QUICFrameUPtr
QUICRemoteStreamFlowController::_create_frame()
{
  return QUICFrameFactory::create_stream_blocked_frame(this->_stream_id, this->_offset);
}

QUICFrameUPtr
QUICLocalStreamFlowController::_create_frame()
{
  return QUICFrameFactory::create_max_stream_data_frame(this->_stream_id, this->_limit);
}
