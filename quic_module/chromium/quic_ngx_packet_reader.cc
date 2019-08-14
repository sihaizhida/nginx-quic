#include "quic_ngx_packet_reader.h"

#include <errno.h>
#ifndef __APPLE__
// This is a GNU header that is not present on Apple platforms
#include <features.h>
#endif
#include <string.h>
#include <sys/socket.h>

#include "net/third_party/quiche/src/quic/core/quic_packets.h"
#include "net/third_party/quiche/src/quic/core/quic_process_packet_interface.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_arraysize.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_bug_tracker.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_flag_utils.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_flags.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_logging.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_server_stats.h"
#include "net/third_party/quiche/src/quic/platform/api/quic_socket_address.h"
#include "net/quic/platform/impl/quic_socket_utils.h"

#ifndef SO_RXQ_OVFL
#define SO_RXQ_OVFL 40
#endif

// #define QUIC_FLAG(type, flag, value) type flag = value;
// QUIC_FLAG(bool, FLAGS_quic_reloadable_flag_quic_use_quic_time_for_received_timestamp, true)
// #undef QUIC_FLAG

namespace quic {
  
QuicNgxPacketReader::QuicNgxPacketReader() {
#if !MMSG_MORE
  // Zero initialize uninitialized memory.
  memset(mmsg_hdr_, 0, sizeof(mmsg_hdr_));

  for (int i = 0; i < kNumPacketsPerReadMmsgCall; ++i) {
    packets_[i].iov.iov_base = packets_[i].buf;
    packets_[i].iov.iov_len = sizeof(packets_[i].buf);
    memset(&packets_[i].raw_address, 0, sizeof(packets_[i].raw_address));
    memset(packets_[i].cbuf, 0, sizeof(packets_[i].cbuf));
    memset(packets_[i].buf, 0, sizeof(packets_[i].buf));

    msghdr* hdr = &mmsg_hdr_[i].msg_hdr;
    hdr->msg_name = &packets_[i].raw_address;
    hdr->msg_namelen = sizeof(sockaddr_storage);
    hdr->msg_iov = &packets_[i].iov;
    hdr->msg_iovlen = 1;

    hdr->msg_control = packets_[i].cbuf;
    hdr->msg_controllen = kCmsgSpaceForReadPacket;
  }
#endif
}

QuicNgxPacketReader::~QuicNgxPacketReader() = default;

bool QuicNgxPacketReader::ReadAndDispatchPackets(
    int fd,
    int port,
    const QuicClock& clock,
    ProcessPacketInterface* processor,
    QuicPacketCount* packets_dropped) {
#if MMSG_MORE_NO_ANDROID
  return QuicPacketReader::ReadAndDispatchPackets(
                  fd, port, clock, processor, packets_dropped);
#else
  // Re-set the length fields in case recvmmsg has changed them.
  for (int i = 0; i < kNumPacketsPerReadMmsgCall; ++i) {
    // DCHECK_LE(kMaxPacketSize, packets_[i].iov.iov_len);
    msghdr* hdr = &mmsg_hdr_[i].msg_hdr;
    hdr->msg_namelen = sizeof(sockaddr_storage);
    DCHECK_EQ(1, static_cast<const int>(hdr->msg_iovlen));
    hdr->msg_controllen = kCmsgSpaceForReadPacket;
    hdr->msg_flags = 0;
  }

  int packets_read =
      recvmmsg(fd, mmsg_hdr_, kNumPacketsPerReadMmsgCall, MSG_TRUNC, nullptr);

  if (packets_read <= 0) {
    return false;  // recvmmsg failed.
  }

  // bool use_quic_time =
  //     GetQuicReloadableFlag(quic_use_quic_time_for_received_timestamp);
  bool use_quic_time = true;
  QuicTime fallback_timestamp(QuicTime::Zero());
  QuicWallTime fallback_walltimestamp = QuicWallTime::Zero();
  for (int i = 0; i < packets_read; ++i) {
    if (mmsg_hdr_[i].msg_len == 0) {
      continue;
    }

    if (QUIC_PREDICT_FALSE(mmsg_hdr_[i].msg_hdr.msg_flags & MSG_CTRUNC)) {
      QUIC_BUG << "Incorrectly set control length: "
               << mmsg_hdr_[i].msg_hdr.msg_controllen << ", expected "
               << kCmsgSpaceForReadPacket;
      continue;
    }

    if (QUIC_PREDICT_FALSE(mmsg_hdr_[i].msg_hdr.msg_flags & MSG_TRUNC)) {
      QUIC_LOG_FIRST_N(WARNING, 100)
          << "Dropping truncated QUIC packet: buffer size:"
          << packets_[i].iov.iov_len << " packet size:" << mmsg_hdr_[i].msg_len;
      QUIC_SERVER_HISTOGRAM_COUNTS(
          "QuicNgxPacketReader.DroppedPacketSize", mmsg_hdr_[i].msg_len, 1, 10000,
          20, "In QuicNgxPacketReader, the size of big packets that are dropped.");
      continue;
    }

    QuicSocketAddress peer_address(packets_[i].raw_address);
    QuicIpAddress self_ip;
    QuicWallTime packet_walltimestamp = QuicWallTime::Zero();
    QuicSocketUtils::GetAddressAndTimestampFromMsghdr(
        &mmsg_hdr_[i].msg_hdr, &self_ip, &packet_walltimestamp);
    if (!self_ip.IsInitialized()) {
      QUIC_BUG << "Unable to get self IP address.";
      continue;
    }

    // This isn't particularly desirable, but not all platforms support socket
    // timestamping.
    QuicTime timestamp(QuicTime::Zero());
    if (!use_quic_time) {
      if (packet_walltimestamp.IsZero()) {
        if (fallback_walltimestamp.IsZero()) {
          fallback_walltimestamp = clock.WallNow();
        }
        packet_walltimestamp = fallback_walltimestamp;
      }
      timestamp = clock.ConvertWallTimeToQuicTime(packet_walltimestamp);

    } else {
      // QUIC_RELOADABLE_FLAG_COUNT(quic_use_quic_time_for_received_timestamp);
      if (packet_walltimestamp.IsZero()) {
        if (!fallback_timestamp.IsInitialized()) {
          fallback_timestamp = clock.Now();
        }
        timestamp = fallback_timestamp;
      } else {
        timestamp = clock.ConvertWallTimeToQuicTime(packet_walltimestamp);
      }
    }
    int ttl = 0;
    bool has_ttl =
        QuicSocketUtils::GetTtlFromMsghdr(&mmsg_hdr_[i].msg_hdr, &ttl);
    // Because QuicSocketUtils::GetPacketHeadersFromMsghdr is not implemented
    // char* headers = nullptr;
    // size_t headers_length = 0;
    // QuicSocketUtils::GetPacketHeadersFromMsghdr(&mmsg_hdr_[i].msg_hdr, &headers,
    //                                             &headers_length);
    // QuicReceivedPacket packet(reinterpret_cast<char*>(packets_[i].iov.iov_base),
    //                           mmsg_hdr_[i].msg_len, timestamp, false, ttl,
    //                           has_ttl, headers, headers_length, false);
    
    QuicReceivedPacket packet(reinterpret_cast<char*>(packets_[i].iov.iov_base),
                              mmsg_hdr_[i].msg_len, timestamp, false, ttl,
                              has_ttl, nullptr, 0, false);    
    QuicSocketAddress self_address(self_ip, port);
    processor->ProcessPacket(self_address, peer_address, packet);
  }

  if (packets_dropped != nullptr) {
    QuicSocketUtils::GetOverflowFromMsghdr(&mmsg_hdr_[0].msg_hdr,
                                           packets_dropped);
  }

  // We may not have read all of the packets available on the socket.
  return packets_read == kNumPacketsPerReadMmsgCall;
#endif // MMSG_MORE_NO_ANDROID
}

}  // namespace quic
