// Copyright 2018 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/connection.h"

#include <arpa/inet.h>
#include <linux/rtnetlink.h>

#include <memory>
#include <optional>
#include <string>
#include <vector>

#include <gtest/gtest.h>
#include <gmock/gmock.h>
#include <net-base/ip_address.h>

#include "shill/ipconfig.h"
#include "shill/mock_control.h"
#include "shill/mock_device.h"
#include "shill/mock_manager.h"
#include "shill/mock_routing_table.h"
#include "shill/network/address_service.h"
#include "shill/routing_policy_entry.h"
#include "shill/routing_table_entry.h"

using testing::_;
using testing::AnyNumber;
using testing::Eq;
using testing::Mock;
using testing::Return;
using testing::StrictMock;
using testing::Test;
using testing::WithArg;

namespace shill {

namespace {
const int kDeviceInterfaceIndexBase = 100;

const char kIPAddress0[] = "192.168.1.1";
const char kIPAddress1[] = "192.168.1.101";
const char kGatewayAddress0[] = "192.168.1.254";
const char kBroadcastAddress0[] = "192.168.1.255";
const char kNameServer0[] = "8.8.8.8";
const char kNameServer1[] = "8.8.9.9";
const int32_t kPrefix0 = 24;
const int32_t kPrefix1 = 31;
const char kSearchDomain0[] = "chromium.org";
const char kSearchDomain1[] = "google.com";
const char kIPv6Address[] = "2001:db8::1";
const char kIPv6GatewayAddress[] = "::";
const char kIPv6NameServer0[] = "2001:db9::1";
const char kIPv6NameServer1[] = "2001:db9::2";

MATCHER_P2(IsIPAddress, address, prefix, "") {
  IPAddress match_address(address);
  match_address.set_prefix(prefix);
  return match_address.Equals(arg);
}

MATCHER_P(IsIPv6Address, address, "") {
  IPAddress match_address(address);
  return match_address.Equals(arg);
}

MATCHER(IsDefaultAddress, "") {
  IPAddress match_address(arg);
  return match_address.IsDefault();
}

MATCHER_P(IsValidRoutingTableEntry, dst, "") {
  return dst.Equals(arg.dst);
}

MATCHER_P(IsValidThrowRoute, dst, "") {
  return dst.Equals(arg.dst) && arg.type == RTN_THROW;
}

MATCHER_P(IsLinkRouteTo, dst, "") {
  return dst.HasSameAddressAs(arg.dst) &&
         arg.dst.prefix() ==
             IPAddress::GetMaxPrefixLength(IPAddress::kFamilyIPv4) &&
         !arg.src.IsValid() && !arg.gateway.IsValid() &&
         arg.scope == RT_SCOPE_LINK;
}

net_base::IPCIDR CreateAndUnwrapIPCIDR(const std::string& addr_str,
                                       int prefix_length) {
  const auto ret =
      net_base::IPCIDR::CreateFromStringAndPrefix(addr_str, prefix_length);
  CHECK(ret.has_value()) << addr_str << "is not a valid IP";
  return *ret;
}

net_base::IPv4Address CreateAndUnwrapIPv4Address(const std::string& addr_str) {
  const auto ret = net_base::IPv4Address::CreateFromString(addr_str);
  CHECK(ret.has_value()) << addr_str << "is not a valid IP";
  return *ret;
}

IPAddress CreateAndUnwrapIPAddress(const std::string& addr_str) {
  const auto ret = IPAddress::CreateFromString(addr_str);
  CHECK(ret.has_value()) << addr_str << "is not a valid IP";
  return *ret;
}

}  // namespace

class MockAddressService : public AddressService {
 public:
  MockAddressService() = default;
  MockAddressService(const MockAddressService&) = delete;
  MockAddressService& operator=(const MockAddressService&) = delete;

  ~MockAddressService() override = default;

  MOCK_METHOD(void, FlushAddress, (int), (override));

  MOCK_METHOD(bool,
              RemoveAddressOtherThan,
              (int, const net_base::IPCIDR&),
              (override));

  MOCK_METHOD(void,
              AddAddress,
              (int,
               const net_base::IPCIDR&,
               const std::optional<net_base::IPv4Address>&),
              (override));
};

class ConnectionTest : public Test {
 public:
  ConnectionTest()
      : manager_(&control_, nullptr, nullptr),
        connection_(nullptr),
        local_address_(CreateAndUnwrapIPCIDR(kIPAddress0, kPrefix0)),
        broadcast_address_(CreateAndUnwrapIPv4Address(kBroadcastAddress0)),
        gateway_ipv4_address_(CreateAndUnwrapIPAddress(kGatewayAddress0)),
        gateway_ipv6_address_(CreateAndUnwrapIPAddress(kIPv6GatewayAddress)),
        default_address_(
            IPAddress::CreateFromFamily_Deprecated(IPAddress::kFamilyIPv4)),
        local_ipv6_address_(CreateAndUnwrapIPCIDR(kIPv6Address, 0)) {}

  void SetUp() override {
    ipv4_properties_.address = kIPAddress0;
    ipv4_properties_.subnet_prefix = kPrefix0;
    ipv4_properties_.gateway = kGatewayAddress0;
    ipv4_properties_.broadcast_address = kBroadcastAddress0;
    ipv4_properties_.dns_servers = {kNameServer0, kNameServer1};
    ipv4_properties_.domain_search = {kSearchDomain0, kSearchDomain1};
    ipv4_properties_.address_family = IPAddress::kFamilyIPv4;

    ipv6_properties_.address = kIPv6Address;
    ipv6_properties_.gateway = kIPv6GatewayAddress;
    ipv6_properties_.dns_servers = {kIPv6NameServer0, kIPv6NameServer1};
    ipv6_properties_.address_family = IPAddress::kFamilyIPv6;
  }

  void TearDown() override {
    if (connection_) {
      AddDestructorExpectations();
      connection_ = nullptr;
    }
  }

  bool FixGatewayReachability(const IPAddress& local,
                              const std::optional<IPAddress>& gateway) {
    return connection_->FixGatewayReachability(local, gateway);
  }

  void SetLocal(const IPAddress& local) { connection_->local_ = local; }

  scoped_refptr<MockDevice> CreateDevice(Technology technology) {
    scoped_refptr<MockDevice> device = new StrictMock<MockDevice>(
        &manager_, "test_" + TechnologyName(technology), std::string(),
        kDeviceInterfaceIndexBase + static_cast<int>(technology));
    EXPECT_CALL(*device, technology()).WillRepeatedly(Return(technology));
    return device;
  }

 protected:
  void AddDestructorExpectations() {
    ASSERT_NE(connection_, nullptr);
    EXPECT_CALL(routing_table_, FlushRoutes(connection_->interface_index_));
    EXPECT_CALL(routing_table_,
                FlushRoutesWithTag(connection_->interface_index_));
  }

  void AddIncludedRoutes(const std::vector<std::string>& included_routes) {
    ipv4_properties_.inclusion_list = included_routes;

    // Add expectations for the added routes.
    auto address_family = ipv4_properties_.address_family;
    for (const auto& prefix_cidr : included_routes) {
      const auto destination_address =
          IPAddress::CreateFromPrefixString(prefix_cidr);
      CHECK(destination_address.has_value()) << prefix_cidr;
      // Left as default.
      const auto source_address(
          IPAddress::CreateFromFamily_Deprecated(address_family));
      EXPECT_CALL(routing_table_,
                  AddRoute(connection_->interface_index_,
                           RoutingTableEntry::Create(*destination_address,
                                                     source_address,
                                                     gateway_ipv4_address_)
                               .SetTable(connection_->table_id_)
                               .SetTag(connection_->interface_index_)))
          .WillOnce(Return(true));
    }
  }

  void AddDHCPClasslessStaticRoutes(
      const std::vector<IPConfig::Route>& routes) {
    ipv4_properties_.dhcp_classless_static_routes = routes;

    dhcp_classless_static_route_dsts_.clear();
    // Add expectations for the added routes.
    auto address_family = ipv4_properties_.address_family;
    for (const auto& route : routes) {
      IPAddress destination_address = CreateAndUnwrapIPAddress(route.host);
      destination_address.set_prefix(route.prefix);

      // Left as default.
      const auto source_address =
          IPAddress::CreateFromFamily_Deprecated(address_family);
      IPAddress gateway_address = CreateAndUnwrapIPAddress(route.gateway);

      EXPECT_CALL(
          routing_table_,
          AddRoute(connection_->interface_index_,
                   RoutingTableEntry::Create(destination_address,
                                             source_address, gateway_address)
                       .SetTable(connection_->table_id_)
                       .SetTag(connection_->interface_index_)))
          .WillOnce(Return(true));
      dhcp_classless_static_route_dsts_.push_back(destination_address);
    }
  }

  std::unique_ptr<Connection> CreateConnection(DeviceRefPtr device,
                                               bool fixed_ip_params = false) {
    auto connection = std::make_unique<Connection>(
        device->interface_index(), device->link_name(), fixed_ip_params,
        device->technology());
    connection->routing_table_ = &routing_table_;
    connection->address_service_ = &address_service_;
    return connection;
  }

  MockControl control_;
  MockManager manager_;
  std::unique_ptr<Connection> connection_;
  IPConfig::Properties ipv4_properties_;
  IPConfig::Properties ipv6_properties_;
  const net_base::IPCIDR local_address_;
  const net_base::IPv4Address broadcast_address_;
  const IPAddress gateway_ipv4_address_;
  const IPAddress gateway_ipv6_address_;
  const IPAddress default_address_;
  const net_base::IPCIDR local_ipv6_address_;
  std::vector<IPAddress> dhcp_classless_static_route_dsts_;
  StrictMock<MockRoutingTable> routing_table_;
  MockAddressService address_service_;
};

TEST_F(ConnectionTest, InitState) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  EXPECT_EQ(device->link_name(), connection_->interface_name());
}

TEST_F(ConnectionTest, AddNonPhysicalDeviceConfig) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  const auto table_id =
      RoutingTable::GetInterfaceTableId(device->interface_index());

  EXPECT_CALL(
      address_service_,
      AddAddress(device->interface_index(), local_address_,
                 net_base::IPv4Address::CreateFromString(kBroadcastAddress0)));
  EXPECT_CALL(routing_table_,
              SetDefaultRoute(device->interface_index(),
                              IsIPAddress(gateway_ipv4_address_, 0), table_id));
  connection_->UpdateFromIPConfig(ipv4_properties_);

  IPAddress test_local_address(local_address_);
  test_local_address.set_prefix(kPrefix0);
  EXPECT_FALSE(connection_->IsIPv6());

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, AddNonPhysicalDeviceConfigIncludedRoutes) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  const auto table_id =
      RoutingTable::GetInterfaceTableId(device->interface_index());

  EXPECT_CALL(
      address_service_,
      AddAddress(device->interface_index(), local_address_,
                 net_base::IPv4Address::CreateFromString(kBroadcastAddress0)));
  EXPECT_CALL(routing_table_,
              SetDefaultRoute(device->interface_index(),
                              IsIPAddress(gateway_ipv4_address_, 0), table_id));
  AddIncludedRoutes({"1.1.1.1/10", "3.3.3.3/5"});
  connection_->UpdateFromIPConfig(ipv4_properties_);

  IPAddress test_local_address(local_address_);
  test_local_address.set_prefix(kPrefix0);
  EXPECT_FALSE(connection_->IsIPv6());

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, AddPhysicalDeviceConfig) {
  auto device = CreateDevice(Technology::kEthernet);
  connection_ = CreateConnection(device);

  const auto table_id =
      RoutingTable::GetInterfaceTableId(device->interface_index());

  EXPECT_CALL(
      address_service_,
      AddAddress(device->interface_index(), local_address_,
                 net_base::IPv4Address::CreateFromString(kBroadcastAddress0)));
  EXPECT_CALL(routing_table_,
              SetDefaultRoute(device->interface_index(),
                              IsIPAddress(gateway_ipv4_address_, 0), table_id));
  connection_->UpdateFromIPConfig(ipv4_properties_);

  IPAddress test_local_address(local_address_);
  test_local_address.set_prefix(kPrefix0);
  EXPECT_FALSE(connection_->IsIPv6());

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, AddPhysicalDeviceConfigIncludedRoutes) {
  auto device = CreateDevice(Technology::kEthernet);
  connection_ = CreateConnection(device);

  const auto table_id =
      RoutingTable::GetInterfaceTableId(device->interface_index());

  EXPECT_CALL(
      address_service_,
      AddAddress(device->interface_index(), local_address_,
                 net_base::IPv4Address::CreateFromString(kBroadcastAddress0)));
  EXPECT_CALL(routing_table_,
              SetDefaultRoute(device->interface_index(),
                              IsIPAddress(gateway_ipv4_address_, 0), table_id));
  AddIncludedRoutes({"1.1.1.1/10"});
  connection_->UpdateFromIPConfig(ipv4_properties_);

  IPAddress test_local_address(local_address_);
  test_local_address.set_prefix(kPrefix0);
  EXPECT_FALSE(connection_->IsIPv6());

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, AddConfigWithDHCPClasslessStaticRoutes) {
  auto device = CreateDevice(Technology::kEthernet);
  connection_ = CreateConnection(device, true);

  const auto table_id =
      RoutingTable::GetInterfaceTableId(device->interface_index());

  AddIncludedRoutes({"1.1.1.1/10"});
  AddDHCPClasslessStaticRoutes(
      {{"2.2.2.2", 24, "3.3.3.3"}, {"4.4.4.4", 16, "5.5.5.5"}});
  EXPECT_CALL(routing_table_,
              SetDefaultRoute(device->interface_index(),
                              IsIPAddress(gateway_ipv4_address_, 0), table_id));

  connection_->UpdateFromIPConfig(ipv4_properties_);
  Mock::VerifyAndClearExpectations(&routing_table_);
}

TEST_F(ConnectionTest, AddNonPhysicalDeviceConfigUserTrafficOnly) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  const std::string kExcludeAddress1 = "192.0.1.0/24";
  const std::string kExcludeAddress2 = "192.0.2.0/24";
  IPAddress address1 = *IPAddress::CreateFromPrefixString(kExcludeAddress1);
  IPAddress address2 = *IPAddress::CreateFromPrefixString(kExcludeAddress2);

  ipv4_properties_.default_route = false;
  ipv4_properties_.exclusion_list = {kExcludeAddress1, kExcludeAddress2};

  EXPECT_CALL(
      address_service_,
      AddAddress(device->interface_index(), local_address_,
                 net_base::IPv4Address::CreateFromString(kBroadcastAddress0)));

  // SetupExcludedRoutes should create RTN_THROW entries for both networks.
  EXPECT_CALL(routing_table_,
              AddRoute(device->interface_index(), IsValidThrowRoute(address1)))
      .WillOnce(Return(true));
  EXPECT_CALL(routing_table_,
              AddRoute(device->interface_index(), IsValidThrowRoute(address2)))
      .WillOnce(Return(true));

  connection_->UpdateFromIPConfig(ipv4_properties_);

  IPAddress test_local_address(local_address_);
  test_local_address.set_prefix(kPrefix0);
  EXPECT_FALSE(connection_->IsIPv6());

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, AddNonPhysicalDeviceConfigIPv6) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  EXPECT_CALL(address_service_,
              AddAddress(device->interface_index(), local_ipv6_address_,
                         Eq(std::nullopt)));
  connection_->UpdateFromIPConfig(ipv6_properties_);

  IPAddress test_local_address(local_ipv6_address_);
  EXPECT_TRUE(connection_->IsIPv6());

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, AddPhysicalDeviceConfigIPv6) {
  auto device = CreateDevice(Technology::kEthernet);
  connection_ = CreateConnection(device);

  EXPECT_CALL(address_service_,
              AddAddress(device->interface_index(), local_ipv6_address_,
                         Eq(std::nullopt)));
  connection_->UpdateFromIPConfig(ipv6_properties_);

  IPAddress test_local_address(local_ipv6_address_);
  EXPECT_TRUE(connection_->IsIPv6());

  // Destruct cleanup
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, AddConfigWithPeer) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  const std::string kPeerAddress("192.168.1.222");
  IPAddress peer_address = CreateAndUnwrapIPAddress(kPeerAddress);
  ipv4_properties_.peer_address = kPeerAddress;
  ipv4_properties_.gateway = std::string();
  EXPECT_CALL(
      address_service_,
      AddAddress(device->interface_index(), local_address_,
                 net_base::IPv4Address::CreateFromString(kBroadcastAddress0)));
  EXPECT_CALL(routing_table_, SetDefaultRoute(_, _, _)).Times(1);
  connection_->UpdateFromIPConfig(ipv4_properties_);

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, AddConfigWithBrokenNetmask) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  // Assign a prefix that makes the gateway unreachable.
  ipv4_properties_.subnet_prefix = kPrefix1;

  const auto table_id =
      RoutingTable::GetInterfaceTableId(device->interface_index());

  // Connection should add a link route which will allow the
  // gateway to be reachable.
  IPAddress gateway_address = CreateAndUnwrapIPAddress(kGatewayAddress0);
  EXPECT_CALL(routing_table_, AddRoute(device->interface_index(),
                                       IsLinkRouteTo(gateway_address)))
      .WillOnce(Return(true));
  EXPECT_CALL(
      address_service_,
      AddAddress(
          device->interface_index(),
          *net_base::IPCIDR::CreateFromStringAndPrefix(kIPAddress0, kPrefix1),
          net_base::IPv4Address::CreateFromString(kBroadcastAddress0)));
  EXPECT_CALL(routing_table_,
              SetDefaultRoute(device->interface_index(),
                              IsIPAddress(gateway_ipv4_address_, 0), table_id));

  connection_->UpdateFromIPConfig(ipv4_properties_);

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, AddConfigReverse) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  const auto table_id =
      RoutingTable::GetInterfaceTableId(device->interface_index());

  EXPECT_CALL(
      address_service_,
      AddAddress(device->interface_index(), local_address_,
                 net_base::IPv4Address::CreateFromString(kBroadcastAddress0)));
  EXPECT_CALL(routing_table_,
              SetDefaultRoute(device->interface_index(),
                              IsIPAddress(gateway_ipv4_address_, 0), table_id));
  connection_->UpdateFromIPConfig(ipv4_properties_);

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, AddConfigWithFixedIpParams) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device, true);

  // Initial setup: routes but no IP configuration.
  EXPECT_CALL(address_service_, AddAddress(_, _, _)).Times(0);
  EXPECT_CALL(routing_table_, SetDefaultRoute(_, _, _));
  connection_->UpdateFromIPConfig(ipv4_properties_);
  Mock::VerifyAndClearExpectations(&routing_table_);
  Mock::VerifyAndClearExpectations(&address_service_);

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(_)).Times(0);
}

TEST_F(ConnectionTest, HasOtherAddress) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  // Config with first address.
  const auto table_id =
      RoutingTable::GetInterfaceTableId(device->interface_index());
  EXPECT_CALL(
      address_service_,
      AddAddress(device->interface_index(), local_address_,
                 net_base::IPv4Address::CreateFromString(kBroadcastAddress0)));
  EXPECT_CALL(routing_table_,
              SetDefaultRoute(device->interface_index(),
                              IsIPAddress(gateway_ipv4_address_, 0), table_id));
  connection_->UpdateFromIPConfig(ipv4_properties_);

  // Config with a different address should cause address and route flush.
  EXPECT_CALL(routing_table_, FlushRoutesWithTag(device->interface_index()));

  EXPECT_CALL(
      address_service_,
      RemoveAddressOtherThan(
          device->interface_index(),
          *net_base::IPCIDR::CreateFromStringAndPrefix(kIPAddress1, kPrefix0)))
      .WillOnce(Return(true));
  EXPECT_CALL(
      address_service_,
      AddAddress(
          device->interface_index(),
          *net_base::IPCIDR::CreateFromStringAndPrefix(kIPAddress1, kPrefix0),
          net_base::IPv4Address::CreateFromString(kBroadcastAddress0)));
  // EXPECT_CALL(rtnl_handler_,
  //             AddInterfaceAddress(
  //                 device->interface_index(),
  //                 *IPAddress::CreateFromStringAndPrefix(kIPAddress1,
  //                 kPrefix0), IsIPAddress(broadcast_address_, 0)));
  // EXPECT_CALL(rtnl_handler_,
  //             RemoveInterfaceAddress(device->interface_index(),
  //                                    IsIPAddress(local_address_, kPrefix0)));
  EXPECT_CALL(routing_table_,
              SetDefaultRoute(device->interface_index(),
                              IsIPAddress(gateway_ipv4_address_, 0), table_id));
  ipv4_properties_.address = kIPAddress1;
  connection_->UpdateFromIPConfig(ipv4_properties_);

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, BlackholeIPv6) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  const auto table_id =
      RoutingTable::GetInterfaceTableId(device->interface_index());
  ipv4_properties_.blackhole_ipv6 = true;
  EXPECT_CALL(address_service_, AddAddress(_, _, _));
  EXPECT_CALL(routing_table_, SetDefaultRoute(_, _, _));
  EXPECT_CALL(routing_table_,
              CreateBlackholeRoute(device->interface_index(),
                                   IPAddress::kFamilyIPv6, 0, table_id))
      .WillOnce(Return(true));
  connection_->UpdateFromIPConfig(ipv4_properties_);

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, PointToPointNetwork) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  // If this is a peer-to-peer interface, the gateway address should be modified
  // to allow routing to work correctly.
  static const char kLocal[] = "10.242.2.13";
  static const char kRemote[] = "10.242.2.14";
  IPConfig::Properties properties(ipv4_properties_);
  properties.peer_address = kRemote;
  properties.address = kLocal;
  EXPECT_CALL(address_service_, AddAddress(_, _, _));
  EXPECT_CALL(routing_table_, SetDefaultRoute(_, IsDefaultAddress(), _));
  connection_->UpdateFromIPConfig(properties);

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

TEST_F(ConnectionTest, FixGatewayReachability) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  static const char kLocal[] = "10.242.2.13";
  IPAddress local = CreateAndUnwrapIPAddress(kLocal);
  const int kPrefix = 24;
  local.set_prefix(kPrefix);

  // Should fail because no gateway is set
  EXPECT_FALSE(FixGatewayReachability(local, std::nullopt));
  EXPECT_EQ(kPrefix, local.prefix());

  // Should succeed because with the given prefix, this gateway is reachable.
  static const char kReachableGateway[] = "10.242.2.14";
  IPAddress gateway = *IPAddress::CreateFromString(kReachableGateway);
  IPAddress gateway_backup(gateway);
  EXPECT_TRUE(FixGatewayReachability(local, gateway));
  // Prefix should remain unchanged.
  EXPECT_EQ(kPrefix, local.prefix());
  // Gateway should remain unchanged.
  EXPECT_TRUE(gateway_backup.Equals(gateway));

  // Should succeed because we created a link route to the gateway.
  static const char kRemoteGateway[] = "10.242.3.14";
  gateway = *IPAddress::CreateFromString(kRemoteGateway);
  gateway_backup = gateway;
  gateway_backup.SetAddressToDefault();
  EXPECT_CALL(routing_table_,
              AddRoute(device->interface_index(), IsLinkRouteTo(gateway)))
      .WillOnce(Return(true));
  EXPECT_TRUE(FixGatewayReachability(local, gateway));

  // Gateway should not be set to default.
  EXPECT_FALSE(gateway_backup.Equals(gateway));

  // Should fail if AddRoute() fails.
  EXPECT_CALL(routing_table_,
              AddRoute(device->interface_index(), IsLinkRouteTo(gateway)))
      .WillOnce(Return(false));
  EXPECT_FALSE(FixGatewayReachability(local, gateway));
}

TEST_F(ConnectionTest, SetIPv6DefaultRoute) {
  auto device = CreateDevice(Technology::kUnknown);
  connection_ = CreateConnection(device);

  // IPv6 default route should be added by shill if default_route is set to
  // true.
  const auto table_id =
      RoutingTable::GetInterfaceTableId(device->interface_index());
  ipv6_properties_.default_route = true;
  ipv6_properties_.method = kTypeVPN;
  EXPECT_CALL(address_service_, AddAddress(_, _, _));
  EXPECT_CALL(routing_table_,
              SetDefaultRoute(device->interface_index(),
                              IsIPAddress(gateway_ipv6_address_, 0), table_id));
  connection_->UpdateFromIPConfig(ipv6_properties_);

  // Default route should not be added if default_route is false.
  ipv6_properties_.default_route = false;
  ipv6_properties_.method = kTypeVPN;
  EXPECT_CALL(address_service_, AddAddress(_, _, _));
  EXPECT_CALL(routing_table_,
              SetDefaultRoute(device->interface_index(),
                              IsIPAddress(gateway_ipv6_address_, 0), table_id))
      .Times(0);
  connection_->UpdateFromIPConfig(ipv6_properties_);

  // IPv6 default route should not be added by shill if Flimflam type is
  // ethernet.
  ipv6_properties_.default_route = true;
  ipv6_properties_.method = kTypeEthernet;
  EXPECT_CALL(address_service_, AddAddress(_, _, _));
  EXPECT_CALL(routing_table_,
              SetDefaultRoute(device->interface_index(),
                              IsIPAddress(gateway_ipv6_address_, 0), table_id))
      .Times(0);
  connection_->UpdateFromIPConfig(ipv6_properties_);

  // Destruct cleanup.
  EXPECT_CALL(address_service_, FlushAddress(device->interface_index()));
}

}  // namespace shill
