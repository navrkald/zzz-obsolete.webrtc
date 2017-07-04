/*
 *  Copyright 2004 The WebRTC Project Authors. All rights reserved.
 *
 *  Use of this source code is governed by a BSD-style license
 *  that can be found in the LICENSE file in the root of the source
 *  tree. An additional intellectual property rights grant can be found
 *  in the file PATENTS.  All contributing project authors may
 *  be found in the AUTHORS file in the root of the source tree.
 */

#include "webrtc/rtc_base/testclient.h"

#include "webrtc/rtc_base/gunit.h"
#include "webrtc/rtc_base/ptr_util.h"
#include "webrtc/rtc_base/thread.h"
#include "webrtc/rtc_base/timeutils.h"

namespace rtc {

// DESIGN: Each packet received is put it into a list of packets.
//         Callers can retrieve received packets from any thread by calling
//         NextPacket.

TestClient::TestClient(std::unique_ptr<AsyncPacketSocket> socket)
    : TestClient(std::move(socket), nullptr) {}

TestClient::TestClient(std::unique_ptr<AsyncPacketSocket> socket,
                       FakeClock* fake_clock)
    : fake_clock_(fake_clock),
      socket_(std::move(socket)),
      prev_packet_timestamp_(-1) {
  socket_->SignalReadPacket.connect(this, &TestClient::OnPacket);
  socket_->SignalReadyToSend.connect(this, &TestClient::OnReadyToSend);
}

TestClient::~TestClient() {}

bool TestClient::CheckConnState(AsyncPacketSocket::State state) {
  // Wait for our timeout value until the socket reaches the desired state.
  int64_t end = TimeAfter(kTimeoutMs);
  while (socket_->GetState() != state && TimeUntil(end) > 0) {
    AdvanceTime(1);
  }
  return (socket_->GetState() == state);
}

int TestClient::Send(const char* buf, size_t size) {
  rtc::PacketOptions options;
  return socket_->Send(buf, size, options);
}

int TestClient::SendTo(const char* buf, size_t size,
                       const SocketAddress& dest) {
  rtc::PacketOptions options;
  return socket_->SendTo(buf, size, dest, options);
}

std::unique_ptr<TestClient::Packet> TestClient::NextPacket(int timeout_ms) {
  // If no packets are currently available, we go into a get/dispatch loop for
  // at most timeout_ms.  If, during the loop, a packet arrives, then we can
  // stop early and return it.

  // Note that the case where no packet arrives is important.  We often want to
  // test that a packet does not arrive.

  // Note also that we only try to pump our current thread's message queue.
  // Pumping another thread's queue could lead to messages being dispatched from
  // the wrong thread to non-thread-safe objects.

  int64_t end = TimeAfter(timeout_ms);
  while (TimeUntil(end) > 0) {
    {
      CritScope cs(&crit_);
      if (packets_.size() != 0) {
        break;
      }
    }
    AdvanceTime(1);
  }

  // Return the first packet placed in the queue.
  std::unique_ptr<Packet> packet;
  CritScope cs(&crit_);
  if (packets_.size() > 0) {
    packet = std::move(packets_.front());
    packets_.erase(packets_.begin());
  }

  return packet;
}

bool TestClient::CheckNextPacket(const char* buf, size_t size,
                                 SocketAddress* addr) {
  bool res = false;
  std::unique_ptr<Packet> packet = NextPacket(kTimeoutMs);
  if (packet) {
    res = (packet->size == size && memcmp(packet->buf, buf, size) == 0 &&
           CheckTimestamp(packet->packet_time.timestamp));
    if (addr)
      *addr = packet->addr;
  }
  return res;
}

bool TestClient::CheckTimestamp(int64_t packet_timestamp) {
  bool res = true;
  if (packet_timestamp == -1) {
    res = false;
  }
  if (prev_packet_timestamp_ != -1) {
    if (packet_timestamp < prev_packet_timestamp_) {
      res = false;
    }
  }
  prev_packet_timestamp_ = packet_timestamp;
  return res;
}

void TestClient::AdvanceTime(int ms) {
  // If the test is using a fake clock, we must advance the fake clock to
  // advance time. Otherwise, ProcessMessages will work.
  if (fake_clock_) {
    SIMULATED_WAIT(false, ms, *fake_clock_);
  } else {
    Thread::Current()->ProcessMessages(1);
  }
}

bool TestClient::CheckNoPacket() {
  return NextPacket(kNoPacketTimeoutMs) == nullptr;
}

int TestClient::GetError() {
  return socket_->GetError();
}

int TestClient::SetOption(Socket::Option opt, int value) {
  return socket_->SetOption(opt, value);
}

void TestClient::OnPacket(AsyncPacketSocket* socket, const char* buf,
                          size_t size, const SocketAddress& remote_addr,
                          const PacketTime& packet_time) {
  CritScope cs(&crit_);
  packets_.push_back(MakeUnique<Packet>(remote_addr, buf, size, packet_time));
}

void TestClient::OnReadyToSend(AsyncPacketSocket* socket) {
  ++ready_to_send_count_;
}

TestClient::Packet::Packet(const SocketAddress& a,
                           const char* b,
                           size_t s,
                           const PacketTime& packet_time)
    : addr(a), buf(0), size(s), packet_time(packet_time) {
  buf = new char[size];
  memcpy(buf, b, size);
}

TestClient::Packet::Packet(const Packet& p)
    : addr(p.addr), buf(0), size(p.size), packet_time(p.packet_time) {
  buf = new char[size];
  memcpy(buf, p.buf, size);
}

TestClient::Packet::~Packet() {
  delete[] buf;
}

}  // namespace rtc