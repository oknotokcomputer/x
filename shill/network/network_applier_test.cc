// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <gtest/gtest.h>
#include <gmock/gmock.h>

#include "shill/ipconfig.h"
#include "shill/mock_resolver.h"
#include "shill/mock_routing_policy_service.h"
#include "shill/net/mock_rtnl_handler.h"
#include "shill/network/mock_proc_fs_stub.h"
#include "shill/network/network_applier.h"
#include "shill/network/network_priority.h"
#include "shill/routing_policy_entry.h"
#include "shill/technology.h"

using testing::_;
using testing::Mock;
using testing::Return;
using testing::ReturnRef;
using testing::StrictMock;
using testing::Test;

namespace shill {

namespace {
MATCHER_P3(IsValidRoutingRule, family, priority, table, "") {
  return arg.family == family && arg.priority == priority && arg.table == table;
}

MATCHER_P4(IsValidFwMarkRule, family, priority, fwmark, table, "") {
  return arg.family == family && arg.priority == priority &&
         arg.fw_mark == fwmark && arg.table == table;
}

MATCHER_P4(IsValidIifRule, family, priority, iif, table, "") {
  return arg.family == family && arg.priority == priority &&
         arg.iif_name == iif && arg.table == table;
}

MATCHER_P4(IsValidOifRule, family, priority, oif, table, "") {
  return arg.family == family && arg.priority == priority &&
         arg.oif_name == oif && arg.table == table;
}

MATCHER_P4(IsValidSrcRule, family, priority, src, table, "") {
  return arg.family == family && arg.priority == priority && arg.src == src &&
         arg.table == table;
}

MATCHER_P4(IsValidDstRule, family, priority, dst, table, "") {
  return arg.family == family && arg.priority == priority && arg.dst == dst &&
         arg.table == table;
}

MATCHER_P4(IsValidUidRule, family, priority, uid, table, "") {
  return arg.family == family && arg.priority == priority && arg.uid_range &&
         arg.uid_range->start == uid && arg.uid_range->end == uid &&
         arg.table == table;
}

MATCHER_P5(IsValidFwMarkRuleWithUid, family, priority, fwmark, uid, table, "") {
  return arg.family == family && arg.priority == priority &&
         arg.fw_mark == fwmark && arg.uid_range &&
         arg.uid_range->start == uid && arg.uid_range->end == uid &&
         arg.table == table;
}

MATCHER_P5(IsValidIifRuleWithUid, family, priority, iif, uid, table, "") {
  return arg.family == family && arg.priority == priority &&
         arg.iif_name == iif && arg.uid_range && arg.uid_range->start == uid &&
         arg.uid_range->end == uid && arg.table == table;
}

MATCHER_P5(IsValidOifRuleWithUid, family, priority, oif, uid, table, "") {
  return arg.family == family && arg.priority == priority &&
         arg.oif_name == oif && arg.uid_range && arg.uid_range->start == uid &&
         arg.uid_range->end == uid && arg.table == table;
}

MATCHER_P5(IsValidSrcRuleWithUid, family, priority, src, uid, table, "") {
  return arg.family == family && arg.priority == priority && arg.src == src &&
         arg.uid_range && arg.uid_range->start == uid &&
         arg.uid_range->end == uid && arg.table == table;
}

}  // namespace

class NetworkApplierTest : public Test {
 public:
  NetworkApplierTest() {
    auto temp_proc_fs_ptr = std::make_unique<MockProcFsStub>("");
    proc_fs_ = temp_proc_fs_ptr.get();
    network_applier_ = NetworkApplier::CreateForTesting(
        &resolver_, &rule_table_, &rtnl_handler_, std::move(temp_proc_fs_ptr));
  }

 protected:
  StrictMock<MockResolver> resolver_;
  StrictMock<MockRoutingPolicyService> rule_table_;
  MockRTNLHandler rtnl_handler_;
  MockProcFsStub* proc_fs_;  // owned by network_applier_;
  std::unique_ptr<NetworkApplier> network_applier_;
};

using NetworkApplierDNSTest = NetworkApplierTest;

TEST_F(NetworkApplierDNSTest, ApplyDNS) {
  NetworkPriority priority;
  priority.is_primary_for_dns = true;
  IPConfig::Properties ipv4_properties;
  ipv4_properties.dns_servers = {"8.8.8.8"};
  ipv4_properties.domain_search = {"domain1"};

  EXPECT_CALL(resolver_, SetDNSFromLists(ipv4_properties.dns_servers,
                                         ipv4_properties.domain_search));
  network_applier_->ApplyDNS(priority, &ipv4_properties, nullptr);

  priority.is_primary_for_dns = false;
  EXPECT_CALL(resolver_, SetDNSFromLists(_, _)).Times(0);
  network_applier_->ApplyDNS(priority, &ipv4_properties, nullptr);
}

TEST_F(NetworkApplierDNSTest, DomainAdded) {
  NetworkPriority priority;
  priority.is_primary_for_dns = true;
  IPConfig::Properties ipv4_properties;
  ipv4_properties.dns_servers = {"8.8.8.8"};
  const std::string kDomainName("chromium.org");
  ipv4_properties.domain_name = kDomainName;

  std::vector<std::string> expected_domain_search_list = {kDomainName + "."};
  EXPECT_CALL(resolver_, SetDNSFromLists(_, expected_domain_search_list));
  network_applier_->ApplyDNS(priority, &ipv4_properties, nullptr);
}

TEST_F(NetworkApplierDNSTest, DualStack) {
  NetworkPriority priority;
  priority.is_primary_for_dns = true;
  IPConfig::Properties ipv4_properties;
  ipv4_properties.dns_servers = {"8.8.8.8"};
  ipv4_properties.domain_search = {"domain1", "domain2"};
  IPConfig::Properties ipv6_properties;
  ipv6_properties.dns_servers = {"2001:4860:4860:0:0:0:0:8888"};
  ipv6_properties.domain_search = {"domain3", "domain4"};

  std::vector<std::string> expected_dns = {"2001:4860:4860:0:0:0:0:8888",
                                           "8.8.8.8"};
  std::vector<std::string> expected_dnssl = {"domain3", "domain4", "domain1",
                                             "domain2"};
  EXPECT_CALL(resolver_, SetDNSFromLists(expected_dns, expected_dnssl));
  network_applier_->ApplyDNS(priority, &ipv4_properties, &ipv6_properties);
}

TEST_F(NetworkApplierDNSTest, DualStackSearchListDedup) {
  NetworkPriority priority;
  priority.is_primary_for_dns = true;
  IPConfig::Properties ipv4_properties;
  ipv4_properties.dns_servers = {"8.8.8.8"};
  ipv4_properties.domain_search = {"domain1", "domain2"};
  IPConfig::Properties ipv6_properties;
  ipv6_properties.dns_servers = {"2001:4860:4860:0:0:0:0:8888"};
  ipv6_properties.domain_search = {"domain1", "domain2"};

  std::vector<std::string> expected_dnssl = {"domain1", "domain2"};
  EXPECT_CALL(resolver_, SetDNSFromLists(_, expected_dnssl));
  network_applier_->ApplyDNS(priority, &ipv4_properties, &ipv6_properties);
}

using NetworkApplierRoutingPolicyTest = NetworkApplierTest;

TEST_F(NetworkApplierRoutingPolicyTest, DefaultPhysical) {
  const int kInterfaceIndex = 3;
  const std::string kInterfaceName = "eth0";

  NetworkPriority priority;
  priority.is_primary_physical = true;
  priority.is_primary_logical = true;
  priority.ranking_order = 0;

  auto all_addresses = std::vector<IPAddress>{
      *IPAddress::CreateFromStringAndPrefix("198.51.100.101", 24),
      *IPAddress::CreateFromStringAndPrefix("2001:db8:0:1000::abcd", 64)};

  RoutingPolicyEntry::FwMark routing_fwmark;
  routing_fwmark.value = (1000 + kInterfaceIndex) << 16;
  routing_fwmark.mask = 0xffff0000;
  const uint32_t kExpectedTable = 1003u;
  EXPECT_CALL(rule_table_, GetShillUid());

  EXPECT_CALL(rule_table_, FlushRules(kInterfaceIndex));

  // IPv4 rules:
  // 1000:  from all lookup main
  EXPECT_CALL(rule_table_,
              AddRule(-1, IsValidRoutingRule(IPAddress::kFamilyIPv4, 1000u,
                                             RT_TABLE_MAIN)))
      .WillOnce(Return(true));
  // 1010:  from all fwmark 0x3eb0000/0xffff0000 lookup 1003
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidFwMarkRule(IPAddress::kFamilyIPv4, 1010u,
                                        routing_fwmark, kExpectedTable)))
      .WillOnce(Return(true));
  // 1010:  from all oif eth0 lookup 1003
  EXPECT_CALL(rule_table_, AddRule(kInterfaceIndex,
                                   IsValidOifRule(IPAddress::kFamilyIPv4, 1010u,
                                                  "eth0", kExpectedTable)))
      .WillOnce(Return(true));
  // 1010:  from 198.51.100.101/24 lookup 1003
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidSrcRule(IPAddress::kFamilyIPv4, 1010u,
                                     all_addresses[0], kExpectedTable)))
      .WillOnce(Return(true));
  // 1010:  from all iif eth0 lookup 1003
  EXPECT_CALL(rule_table_, AddRule(kInterfaceIndex,
                                   IsValidIifRule(IPAddress::kFamilyIPv4, 1010u,
                                                  "eth0", kExpectedTable)))
      .WillOnce(Return(true));
  // 32765: from all lookup 1003
  EXPECT_CALL(rule_table_, AddRule(kInterfaceIndex,
                                   IsValidRoutingRule(IPAddress::kFamilyIPv4,
                                                      32765u, kExpectedTable)))
      .WillOnce(Return(true));

  // IPv6 rules:
  // 1000:  from all lookup main
  EXPECT_CALL(rule_table_,
              AddRule(-1, IsValidRoutingRule(IPAddress::kFamilyIPv6, 1000u,
                                             RT_TABLE_MAIN)))
      .WillOnce(Return(true));
  // 1010:  from all fwmark 0x3eb0000/0xffff0000 lookup 1003
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidFwMarkRule(IPAddress::kFamilyIPv6, 1010u,
                                        routing_fwmark, kExpectedTable)))
      .WillOnce(Return(true));
  // 1010:  from all oif eth0 lookup 1003
  EXPECT_CALL(rule_table_, AddRule(kInterfaceIndex,
                                   IsValidOifRule(IPAddress::kFamilyIPv6, 1010u,
                                                  "eth0", kExpectedTable)))
      .WillOnce(Return(true));
  // 1010:  from 2001:db8:0:1000::abcd/64 lookup 1003
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidSrcRule(IPAddress::kFamilyIPv6, 1010u,
                                     all_addresses[1], kExpectedTable)))
      .WillOnce(Return(true));
  // 1010:  from all iif eth0 lookup 1003
  EXPECT_CALL(rule_table_, AddRule(kInterfaceIndex,
                                   IsValidIifRule(IPAddress::kFamilyIPv6, 1010u,
                                                  "eth0", kExpectedTable)))
      .WillOnce(Return(true));
  // 32765: from all lookup 1003
  EXPECT_CALL(rule_table_, AddRule(kInterfaceIndex,
                                   IsValidRoutingRule(IPAddress::kFamilyIPv6,
                                                      32765u, kExpectedTable)))
      .WillOnce(Return(true));

  EXPECT_CALL(*proc_fs_, FlushRoutingCache()).WillOnce(Return(true));
  network_applier_->ApplyRoutingPolicy(
      kInterfaceIndex, kInterfaceName, Technology::kEthernet, priority,
      all_addresses, std::vector<net_base::IPv4CIDR>());
}

TEST_F(NetworkApplierRoutingPolicyTest, DefaultVPN) {
  const int kInterfaceIndex = 11;
  const std::string kInterfaceName = "tun0";

  NetworkPriority priority;
  priority.is_primary_logical = true;
  priority.ranking_order = 0;

  auto all_addresses = std::vector<IPAddress>{
      *IPAddress::CreateFromStringAndPrefix("198.51.100.101", 24),
      *IPAddress::CreateFromStringAndPrefix("2001:db8:0:1000::abcd", 64)};

  RoutingPolicyEntry::FwMark routing_fwmark;
  routing_fwmark.value = (1000 + kInterfaceIndex) << 16;
  routing_fwmark.mask = 0xffff0000;
  const uint32_t kExpectedTable = 1011u;
  EXPECT_CALL(rule_table_, GetShillUid());

  auto user_uids = std::vector<uint32_t>{100u};
  EXPECT_CALL(rule_table_, GetUserTrafficUids()).WillOnce(ReturnRef(user_uids));

  EXPECT_CALL(rule_table_, FlushRules(kInterfaceIndex));

  // IPv4 rules:
  // 10:    from all fwmark 0x3f30000/0xffff0000 lookup 1011
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidFwMarkRule(IPAddress::kFamilyIPv4, 10u,
                                        routing_fwmark, kExpectedTable)))
      .WillOnce(Return(true));
  // 10:    from all oif tun0 lookup 1011
  EXPECT_CALL(rule_table_, AddRule(kInterfaceIndex,
                                   IsValidOifRule(IPAddress::kFamilyIPv4, 10u,
                                                  "tun0", kExpectedTable)))
      .WillOnce(Return(true));
  // 32764:  from all uidrange ()-() lookup 1003
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex, IsValidUidRule(IPAddress::kFamilyIPv4, 32764u,
                                              100u, kExpectedTable)))
      .WillOnce(Return(true));

  // IPv6 rules:
  // 10:    from all fwmark 0x3f30000/0xffff0000 lookup 1011
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidFwMarkRule(IPAddress::kFamilyIPv6, 10u,
                                        routing_fwmark, kExpectedTable)))
      .WillOnce(Return(true));
  // 10:    from all oif tun0 lookup 1011
  EXPECT_CALL(rule_table_, AddRule(kInterfaceIndex,
                                   IsValidOifRule(IPAddress::kFamilyIPv6, 10u,
                                                  "tun0", kExpectedTable)))
      .WillOnce(Return(true));
  // 32764:  from all uidrange ()-() lookup 1003
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex, IsValidUidRule(IPAddress::kFamilyIPv6, 32764u,
                                              100u, kExpectedTable)))
      .WillOnce(Return(true));

  EXPECT_CALL(*proc_fs_, FlushRoutingCache()).WillOnce(Return(true));
  network_applier_->ApplyRoutingPolicy(
      kInterfaceIndex, kInterfaceName, Technology::kVPN, priority,
      all_addresses, std::vector<net_base::IPv4CIDR>());
}

TEST_F(NetworkApplierTest,
       ApplyRoutingPolicy_NonDefaultPhysicalWithClasslessStaticRoute) {
  const int kInterfaceIndex = 4;
  const std::string kInterfaceName = "wlan0";

  NetworkPriority priority;
  priority.ranking_order = 1;

  auto all_addresses = std::vector<IPAddress>{
      *IPAddress::CreateFromStringAndPrefix("198.51.100.101", 24),
      *IPAddress::CreateFromStringAndPrefix("2001:db8:0:1000::abcd", 64)};
  auto rfc3442_dsts = std::vector<net_base::IPv4CIDR>{
      *net_base::IPv4CIDR::CreateFromStringAndPrefix("203.0.113.0", 26),
      *net_base::IPv4CIDR::CreateFromStringAndPrefix("203.0.113.128", 26),
  };

  RoutingPolicyEntry::FwMark routing_fwmark;
  routing_fwmark.value = (1000 + kInterfaceIndex) << 16;
  routing_fwmark.mask = 0xffff0000;
  const uint32_t kExpectedTable = 1004u;
  EXPECT_CALL(rule_table_, GetShillUid());

  EXPECT_CALL(rule_table_, FlushRules(kInterfaceIndex));

  // IPv4 rules:
  // 1020:  from all fwmark 0x3ec0000/0xffff0000 lookup 1004
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidFwMarkRule(IPAddress::kFamilyIPv4, 1020u,
                                        routing_fwmark, kExpectedTable)))
      .WillOnce(Return(true));
  // 1020:  from all oif wlan0 lookup 1004
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex, IsValidOifRule(IPAddress::kFamilyIPv4, 1020u,
                                              kInterfaceName, kExpectedTable)))
      .WillOnce(Return(true));
  // 1020:  from 198.51.100.101/24 lookup 1004
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidSrcRule(IPAddress::kFamilyIPv4, 1020u,
                                     all_addresses[0], kExpectedTable)))
      .WillOnce(Return(true));
  // 1020:  from all iif wlan0 lookup 1004
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex, IsValidIifRule(IPAddress::kFamilyIPv4, 1020u,
                                              kInterfaceName, kExpectedTable)))
      .WillOnce(Return(true));
  // 32763:  from all to 203.0.113.0/26 lookup 1004
  // 32763:  from all to 203.0.113.128/26 lookup 1004
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex,
              IsValidDstRule(IPAddress::kFamilyIPv4, 32763u,
                             IPAddress(rfc3442_dsts[0]), kExpectedTable)))
      .WillOnce(Return(true));
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex,
              IsValidDstRule(IPAddress::kFamilyIPv4, 32763u,
                             IPAddress(rfc3442_dsts[1]), kExpectedTable)))
      .WillOnce(Return(true));

  // IPv6 rules:
  // 1020:  from all fwmark 0x3ec0000/0xffff0000 lookup 1004
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidFwMarkRule(IPAddress::kFamilyIPv6, 1020u,
                                        routing_fwmark, kExpectedTable)))
      .WillOnce(Return(true));
  // 1020:  from all oif wlan0 lookup 1004
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex, IsValidOifRule(IPAddress::kFamilyIPv6, 1020u,
                                              kInterfaceName, kExpectedTable)))
      .WillOnce(Return(true));
  // 1020:  from 198.51.100.101/24 lookup 1004
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidSrcRule(IPAddress::kFamilyIPv6, 1020u,
                                     all_addresses[1], kExpectedTable)))
      .WillOnce(Return(true));
  // 1020:  from all iif wlan0 lookup 1004
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex, IsValidIifRule(IPAddress::kFamilyIPv6, 1020u,
                                              kInterfaceName, kExpectedTable)))
      .WillOnce(Return(true));

  EXPECT_CALL(*proc_fs_, FlushRoutingCache()).WillOnce(Return(true));
  network_applier_->ApplyRoutingPolicy(kInterfaceIndex, kInterfaceName,
                                       Technology::kWiFi, priority,
                                       all_addresses, rfc3442_dsts);
}

TEST_F(NetworkApplierRoutingPolicyTest, NonDefaultCellularShouldHaveNoIPv6) {
  const int kInterfaceIndex = 5;
  const std::string kInterfaceName = "wwan0";

  NetworkPriority priority;
  priority.ranking_order = 2;

  auto all_addresses = std::vector<IPAddress>{
      *IPAddress::CreateFromStringAndPrefix("198.51.100.101", 24),
      *IPAddress::CreateFromStringAndPrefix("2001:db8:0:1000::abcd", 64)};

  RoutingPolicyEntry::FwMark routing_fwmark;
  routing_fwmark.value = (1000 + kInterfaceIndex) << 16;
  routing_fwmark.mask = 0xffff0000;
  const uint32_t kExpectedTable = 1005u;

  const uint32_t shill_uid = 22000u;
  EXPECT_CALL(rule_table_, GetShillUid()).WillOnce(Return(shill_uid));

  EXPECT_CALL(rule_table_, FlushRules(kInterfaceIndex));

  // IPv4 rules:
  // 1030:  from all fwmark 0x3ed0000/0xffff0000 lookup 1005
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidFwMarkRule(IPAddress::kFamilyIPv4, 1030u,
                                        routing_fwmark, kExpectedTable)))
      .WillOnce(Return(true));
  // 1030:  from all oif wwan0 lookup 1005
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex, IsValidOifRule(IPAddress::kFamilyIPv4, 1030u,
                                              kInterfaceName, kExpectedTable)))
      .WillOnce(Return(true));
  // 1030:  from 198.51.100.101/24 lookup 1005
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidSrcRule(IPAddress::kFamilyIPv4, 1030u,
                                     all_addresses[0], kExpectedTable)))
      .WillOnce(Return(true));
  // 1030:  from all iif wwan0 lookup 1005
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex, IsValidIifRule(IPAddress::kFamilyIPv4, 1030u,
                                              kInterfaceName, kExpectedTable)))
      .WillOnce(Return(true));

  // IPv6 rules:
  // 1030:  from all fwmark 0x3ed0000/0xffff0000 uidrange (shill) lookup 1005
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex, IsValidFwMarkRuleWithUid(
                                   IPAddress::kFamilyIPv6, 1030u,
                                   routing_fwmark, shill_uid, kExpectedTable)))
      .WillOnce(Return(true));
  // 1030:  from all oif wlan0 lookup 1005
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex,
              IsValidOifRuleWithUid(IPAddress::kFamilyIPv6, 1030u,
                                    kInterfaceName, shill_uid, kExpectedTable)))
      .WillOnce(Return(true));
  // 1030:  from 198.51.100.101/24 lookup 1005
  EXPECT_CALL(rule_table_,
              AddRule(kInterfaceIndex,
                      IsValidSrcRuleWithUid(IPAddress::kFamilyIPv6, 1030u,
                                            all_addresses[1], shill_uid,
                                            kExpectedTable)))
      .WillOnce(Return(true));
  // 1030:  from all iif wwan0 lookup 1005
  EXPECT_CALL(
      rule_table_,
      AddRule(kInterfaceIndex,
              IsValidIifRuleWithUid(IPAddress::kFamilyIPv6, 1030u,
                                    kInterfaceName, shill_uid, kExpectedTable)))
      .WillOnce(Return(true));

  EXPECT_CALL(*proc_fs_, FlushRoutingCache()).WillOnce(Return(true));
  network_applier_->ApplyRoutingPolicy(
      kInterfaceIndex, kInterfaceName, Technology::kCellular, priority,
      all_addresses, std::vector<net_base::IPv4CIDR>());
}

}  // namespace shill
