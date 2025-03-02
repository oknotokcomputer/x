// Copyright 2018 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef NET_BASE_RTNL_MESSAGE_H_
#define NET_BASE_RTNL_MESSAGE_H_

#include <memory>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <base/containers/contains.h>
#include <base/containers/span.h>
#include <brillo/brillo_export.h>

#include "net-base/http_url.h"
#include "net-base/ip_address.h"
#include "net-base/ipv6_address.h"

struct rtattr;

namespace net_base {

struct RTNLHeader;

using RTNLAttrMap = std::unordered_map<uint16_t, std::vector<uint8_t>>;

// Helper class for processing rtnetlink messages. See uapi/linux/rtnetlink.h
// and rtnetlink manual page for details about the message binary encoding and
// meaning of struct fields populated by the kernel.
class BRILLO_EXPORT RTNLMessage {
 public:
  enum Type {
    kTypeUnknown,
    kTypeLink,
    kTypeAddress,
    kTypeRoute,
    kTypeRule,
    kTypeRdnss,
    kTypeDnssl,
    kTypeCaptivePortal,
    kTypeNeighbor,
    kTypeNdUserOption,  // Unknown ND user options that does not have own types.
  };

  enum Mode { kModeUnknown, kModeGet, kModeAdd, kModeDelete, kModeQuery };

  // Helper struct corresponding to struct ifinfomsg.
  struct LinkStatus {
    LinkStatus() : type(0), flags(0), change(0) {}
    LinkStatus(unsigned int in_type,
               unsigned int in_flags,
               unsigned int in_change,
               std::optional<std::string> kind = std::nullopt)
        : type(in_type), flags(in_flags), change(in_change), kind(kind) {}
    // Device type. Corresponds to ifi_type.
    unsigned int type;
    // Device flags. Corresponds to ifi_flags.
    unsigned int flags;
    // Change mask. Corresponds to ifi_mask.
    unsigned int change;
    // Device kind, as defined by the device driver. Corresponds to rtattr
    // IFLA_INFO_KIND nested inside rtattr IFLA_LINKINFO.
    std::optional<std::string> kind;
  };

  // Helper struct corresponding to struct ifaddrmsg.
  struct AddressStatus {
    AddressStatus() : prefix_len(0), flags(0), scope(0) {}
    AddressStatus(unsigned char prefix_len_in,
                  unsigned char flags_in,
                  unsigned char scope_in)
        : prefix_len(prefix_len_in), flags(flags_in), scope(scope_in) {}
    // Prefix length of the address. Corresponds to ifa_prefixlen.
    unsigned char prefix_len;
    // Address flags. Corresponds to ifa_flags.
    unsigned char flags;
    // Address scope. Corresponds to ifa_scope.
    unsigned char scope;
  };

  // Helper struct corresponding to struct rtmsg.
  struct RouteStatus {
    RouteStatus()
        : dst_prefix(0),
          src_prefix(0),
          table(0),
          protocol(0),
          scope(0),
          type(0),
          flags(0) {}
    RouteStatus(unsigned char dst_prefix_in,
                unsigned char src_prefix_in,
                unsigned char table_in,
                unsigned char protocol_in,
                unsigned char scope_in,
                unsigned char type_in,
                unsigned flags_in)
        : dst_prefix(dst_prefix_in),
          src_prefix(src_prefix_in),
          table(table_in),
          protocol(protocol_in),
          scope(scope_in),
          type(type_in),
          flags(flags_in) {}
    // Prefix length of the destination. Corresponds to rtm_dst_len.
    unsigned char dst_prefix;
    // Prefix length of the source. Corresponds to rtm_src_len.
    unsigned char src_prefix;
    // Legacy routing table id. Corresponds to rtm_table.
    // TODO(b/154500323) This cannot expose correctly the per-device routing
    // tables which starts with a +1000 offset. Instead this class should
    // expose the RTA_TABLE rtattr for kTypeRoute messages and the FRA_TABLE
    // rtattr for kTypeRule messages.
    unsigned char table;
    // Routing protocol. Corresponds to rtm_protocol.
    unsigned char protocol;
    // Distance to the destination. Corresponds to rtm_scope.
    unsigned char scope;
    // The type of route. Corresponds to rtm_type.
    unsigned char type;
    // Route flags. Corresponds to rtm_flags.
    unsigned flags;
  };

  // Helper struct corresponding to struct ndmsg.
  struct NeighborStatus {
    NeighborStatus() : state(0), flags(0), type(0) {}
    NeighborStatus(uint16_t state_in, uint8_t flags_in, uint8_t type_in)
        : state(state_in), flags(flags_in), type(type_in) {}
    std::string ToString() const;
    // Neighbor state. Corresponds to ndm_state.
    uint16_t state;
    // Neighbor flags. Corresponds to ndm_flags.
    uint8_t flags;
    // Neighbor type. Corresponds to ndm_type.
    uint8_t type;
  };

  struct RdnssOption {
    RdnssOption() : lifetime(0) {}
    RdnssOption(uint32_t lifetime_in, std::vector<IPv6Address> addresses_in)
        : lifetime(lifetime_in), addresses(addresses_in) {}
    std::string ToString() const;
    uint32_t lifetime;
    std::vector<IPv6Address> addresses;
  };

  struct DnsslOption {
    DnsslOption() = default;
    std::string ToString() const;
    uint32_t lifetime;
    std::vector<std::string> domains;
  };

  struct NdUserOption {
    NdUserOption() = default;
    std::string ToString() const;
    uint8_t type;
    std::vector<uint8_t> option_bytes;  // Including header
  };

  // Packs the attribute map into bytes, with the proper alignment.
  static std::vector<uint8_t> PackAttrs(const RTNLAttrMap& attrs);

  // Parse an RTNL message.  Returns nullptr on failure.
  static std::unique_ptr<RTNLMessage> Decode(base::span<const uint8_t> data);

  // Build an RTNL message from arguments
  RTNLMessage(Type type,
              Mode mode,
              uint16_t flags,
              uint32_t seq,
              uint32_t pid,
              int32_t interface_index,
              sa_family_t family);
  RTNLMessage(const RTNLMessage&) = delete;
  RTNLMessage& operator=(const RTNLMessage&) = delete;

  // Encode an RTNL message.  Returns empty vector on failure.
  std::vector<uint8_t> Encode() const;

  // Getters and setters
  Type type() const { return type_; }
  Mode mode() const { return mode_; }
  uint16_t flags() const { return flags_; }
  uint32_t seq() const { return seq_; }
  void set_seq(uint32_t seq) { seq_ = seq; }
  uint32_t pid() const { return pid_; }
  int32_t interface_index() const { return interface_index_; }
  sa_family_t family() const { return family_; }

  static std::string ModeToString(Mode mode);
  static std::string TypeToString(Type type);
  std::string ToString() const;

  const LinkStatus& link_status() const { return link_status_; }
  void set_link_status(const LinkStatus& link_status) {
    link_status_ = link_status;
  }
  const AddressStatus& address_status() const { return address_status_; }
  void set_address_status(const AddressStatus& address_status) {
    address_status_ = address_status;
  }
  const RouteStatus& route_status() const { return route_status_; }
  void set_route_status(const RouteStatus& route_status) {
    route_status_ = route_status;
  }
  const RdnssOption& rdnss_option() const { return rdnss_option_; }
  void set_rdnss_option(const RdnssOption& rdnss_option) {
    rdnss_option_ = rdnss_option;
  }
  const DnsslOption& dnssl_option() const { return dnssl_option_; }
  void set_dnssl_option(const DnsslOption& dnssl_option) {
    dnssl_option_ = dnssl_option;
  }
  const HttpUrl& captive_portal_uri() const { return captive_portal_uri_; }
  void set_captive_portal_uri(const HttpUrl& captive_portal_uri) {
    captive_portal_uri_ = captive_portal_uri;
  }
  const NdUserOption& nd_user_option() const { return nd_user_option_; }
  const NeighborStatus& neighbor_status() const { return neighbor_status_; }
  void set_neighbor_status(const NeighborStatus& neighbor_status) {
    neighbor_status_ = neighbor_status;
  }
  // GLint hates "unsigned short", and I don't blame it, but that's the
  // type that's used in the system headers.  Use uint16_t instead and hope
  // that the conversion never ends up truncating on some strange platform.
  bool HasAttribute(uint16_t attr) const {
    return base::Contains(attributes_, attr);
  }
  std::vector<uint8_t> GetAttribute(uint16_t attr) const {
    return HasAttribute(attr) ? attributes_.find(attr)->second
                              : std::vector<uint8_t>();
  }
  void SetAttribute(uint16_t attr, base::span<const uint8_t> val) {
    attributes_[attr] = {val.data(), val.data() + val.size()};
  }
  // Return the value of an rtattr attribute of type uint32_t.
  uint32_t GetUint32Attribute(uint16_t attr) const;
  // Returns the value of an rtattr attribute of type string. String attributes
  // serialized by the kernel with nla_put_string() are null terminated and the
  // null terminator is included in the underlying std::vector<uint8_t>. In case
  // the std::vector<uint8_t> does not contain any terminator, all the bytes of
  // contained in the std::vector<uint8_t> are copied into the standard string.
  std::string GetStringAttribute(uint16_t attr) const;
  // returns the IFLA_IFNAME attribute as standard string. This should only be
  // used for RTNLMessages of type kTypeLink.
  std::string GetIflaIfname() const;
  // Returns the local address. IFA_LOCAL will be looked up at first, and if it
  // does not exist, value of IFA_ADDRESS will be used. This should only be used
  // for RTNLMessages of type kTypeAddress.
  std::optional<IPCIDR> GetAddress() const;
  // Returns the routing table id of RTNLMessages with type kTypeRoute.
  uint32_t GetRtaTable() const;
  // Returns the RTA_DST attribute for RTNLMessages of type kTypeRoute.
  std::optional<IPCIDR> GetRtaDst() const;
  // Returns the RTA_SRC attribute for RTNLMessages of type kTypeRoute.
  std::optional<IPCIDR> GetRtaSrc() const;
  // Returns the RTA_GATEWAY attribute for RTNLMessages of type kTypeRoute.
  std::optional<IPAddress> GetRtaGateway() const;
  // Returns the RTA_OIF output interface attribute as an interface index
  // name for RTNLMessages of type kTypeRoute.
  uint32_t GetRtaOif() const;
  // Returns the RTA_OIF output interface attribute translated as an interface
  // name for RTNLMessages of type kTypeRoute.
  std::string GetRtaOifname() const;
  // Returns the RTA_PRIORITY attribute for RTNLMessages of type kTypeRoute.
  uint32_t GetRtaPriority() const;
  // Returns the lookup routing table id of RTNLMessages with type kTypeRule.
  uint32_t GetFraTable() const;
  // Returns the input interface name of RTNLMessages with type kTypeRule.
  std::string GetFraOifname() const;
  // Returns the output interface name of RTNLMessages with type kTypeRule.
  std::string GetFraIifname() const;
  // Returns the fwmark value of RTNLMessages with type kTypeRule.
  uint32_t GetFraFwmark() const;
  // Returns the fwmask value of RTNLMessages with type kTypeRule.
  uint32_t GetFraFwmask() const;
  // Returns the FRA_PRIORITY attribute for RTNLMessages of type kTypeRule.
  uint32_t GetFraPriority() const;
  // Returns the FRA_SRC attribute for RTNLMessages of type kTypeRule.
  std::optional<IPCIDR> GetFraSrc() const;
  // Returns the FRA_DST attribute for RTNLMessages of type kTypeRule.
  std::optional<IPCIDR> GetFraDst() const;

  // Sets the IFLA_INFO_KIND attribute which is nested in IFLA_LINKINFO (and
  // thus it is hard to be set via SetAttribute() directly). This attribute will
  // be used as the type string of a link when creating a new link. This
  // function should be used only for RTNLMessages of type kTypeLink. The second
  // optional parameter |info_data| will be used as the value of IFLA_INFO_DATA,
  // which is kind-specific. Leave it empty if there is no addtiional data
  // needed for |link_kind|.
  void SetIflaInfoKind(const std::string& link_kind,
                       base::span<const uint8_t> info_data);

 private:
  // Decodes different kind of NL messages. |payload| points to the remaining
  // data after the `struct nlmsghdr`.
  static std::unique_ptr<RTNLMessage> DecodeLink(
      Mode mode, base::span<const uint8_t> payload);
  static std::unique_ptr<RTNLMessage> DecodeAddress(
      Mode mode, base::span<const uint8_t> payload);
  static std::unique_ptr<RTNLMessage> DecodeRoute(
      Mode mode, base::span<const uint8_t> payload);
  static std::unique_ptr<RTNLMessage> DecodeRule(
      Mode mode, base::span<const uint8_t> payload);
  static std::unique_ptr<RTNLMessage> DecodeNdUserOption(
      Mode mode, base::span<const uint8_t> payload);
  static std::unique_ptr<RTNLMessage> DecodeNeighbor(
      Mode mode, base::span<const uint8_t> payload);

  void SetNdUserOptionBytes(base::span<const uint8_t> data);
  bool ParseDnsslOption(base::span<const uint8_t> data);
  bool ParseRdnssOption(base::span<const uint8_t> data);
  bool ParseCaptivePortalOption(base::span<const uint8_t> data);
  bool EncodeLink(RTNLHeader* hdr) const;
  bool EncodeAddress(RTNLHeader* hdr) const;
  bool EncodeRoute(RTNLHeader* hdr) const;
  bool EncodeNeighbor(RTNLHeader* hdr) const;

  // Type and mode of the message, corresponding to a subset of the RTM_* enum
  // defined in uapi/linux/rtnetlink.h
  Type type_;
  Mode mode_;
  // Netlink request flags. Corresponds to nlmsg_flags in struct nlmsghdr.
  uint16_t flags_;
  // Arbitrary msg id used for response correlation. Corresponds to nlmsg_seq in
  // struct nlmsghdr.
  uint32_t seq_;
  // The sender id. Corresponds to nlmsg_pid in struct nlmsghdr.
  uint32_t pid_;
  // Corresponds to ifi_index (kTypeLink), ifa_index (kTypeAddress), ndm_ifindex
  // (kTypeNeighbor).
  int32_t interface_index_;
  // Corresponds to ifi_family (kTypeLink), ifa_family (kTypeAddress),
  // rtm_family (kTypeRoute and kTypeRule), ndm_family (kTypeNeighbor). Always
  // IPv6 for neighbor discovery options (kTypeRdnss, kTypeDnssl,
  // kTypeNdUserOption).
  sa_family_t family_;
  // Details specific to a message type.
  LinkStatus link_status_;
  AddressStatus address_status_;
  RouteStatus route_status_;
  NeighborStatus neighbor_status_;
  RdnssOption rdnss_option_;
  DnsslOption dnssl_option_;
  HttpUrl captive_portal_uri_;
  NdUserOption nd_user_option_;
  // Additional rtattr contained in the message.
  RTNLAttrMap attributes_;
  // NOTE: Update Reset() accordingly when adding a new member field.
};

}  // namespace net_base
#endif  // NET_BASE_RTNL_MESSAGE_H_
