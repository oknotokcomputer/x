// Copyright 2018 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/dhcp/dhcpv6_config.h"

#include <memory>
#include <string>

#include <base/bind.h>
#include <base/files/file_util.h>
#include <base/files/scoped_temp_dir.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/stringprintf.h>
#include <chromeos/dbus/service_constants.h>

#include "shill/dhcp/mock_dhcp_provider.h"
#include "shill/dhcp/mock_dhcp_proxy.h"
#include "shill/event_dispatcher.h"
#include "shill/mock_log.h"
#include "shill/mock_process_manager.h"
#include "shill/property_store_test.h"
#include "shill/testing.h"

using testing::_;
using testing::InvokeWithoutArgs;
using testing::Mock;
using testing::Return;

namespace shill {

namespace {
const char kDeviceName[] = "eth0";
const char kLeaseFileSuffix[] = "leasefilesuffix";
const bool kHasLeaseSuffix = true;
const char kIPAddress[] = "2001:db8:0:1::1";
const char kDelegatedPrefix[] = "2001:db8:0:100::";
}  // namespace

using DHCPv6ConfigRefPtr = scoped_refptr<DHCPv6Config>;

class DHCPv6ConfigTest : public PropertyStoreTest {
 public:
  DHCPv6ConfigTest()
      : proxy_(new MockDHCPProxy()),
        config_(new DHCPv6Config(control_interface(),
                                 dispatcher(),
                                 &provider_,
                                 kDeviceName,
                                 kLeaseFileSuffix)) {}

  void SetUp() override { config_->process_manager_ = &process_manager_; }

  bool StartInstance(DHCPv6ConfigRefPtr config) { return config->Start(); }

  void StopInstance() { config_->Stop("In test"); }

  DHCPv6ConfigRefPtr CreateMockMinijailConfig(const std::string& lease_suffix);
  DHCPv6ConfigRefPtr CreateRunningConfig(const std::string& lease_suffix);
  void StopRunningConfigAndExpect(DHCPv6ConfigRefPtr config,
                                  bool lease_file_exists);

 protected:
  static const int kPID;
  static const unsigned int kTag;

  base::FilePath lease_file_;
  base::FilePath pid_file_;
  base::ScopedTempDir temp_dir_;
  std::unique_ptr<MockDHCPProxy> proxy_;
  MockProcessManager process_manager_;
  MockDHCPProvider provider_;
  DHCPv6ConfigRefPtr config_;
};

const int DHCPv6ConfigTest::kPID = 123456;
const unsigned int DHCPv6ConfigTest::kTag = 77;

DHCPv6ConfigRefPtr DHCPv6ConfigTest::CreateMockMinijailConfig(
    const std::string& lease_suffix) {
  DHCPv6ConfigRefPtr config(new DHCPv6Config(control_interface(), dispatcher(),
                                             &provider_, kDeviceName,
                                             lease_suffix));
  config->process_manager_ = &process_manager_;

  return config;
}

DHCPv6ConfigRefPtr DHCPv6ConfigTest::CreateRunningConfig(
    const std::string& lease_suffix) {
  DHCPv6ConfigRefPtr config(new DHCPv6Config(control_interface(), dispatcher(),
                                             &provider_, kDeviceName,
                                             lease_suffix));
  config->process_manager_ = &process_manager_;
  EXPECT_CALL(process_manager_,
              StartProcessInMinijail(_, _, _, _, _, _, _, _, _, _))
      .WillOnce(Return(kPID));
  EXPECT_CALL(provider_, BindPID(kPID, IsRefPtrTo(config)));
  EXPECT_TRUE(config->Start());
  EXPECT_EQ(kPID, config->pid_);

  EXPECT_TRUE(temp_dir_.CreateUniqueTempDir());
  config->root_ = temp_dir_.GetPath();
  base::FilePath varrun = temp_dir_.GetPath().Append("var/run/dhcpcd");
  EXPECT_TRUE(base::CreateDirectory(varrun));
  pid_file_ = varrun.Append(base::StringPrintf("dhcpcd-%s-6.pid", kDeviceName));
  base::FilePath varlib = temp_dir_.GetPath().Append("var/lib/dhcpcd");
  EXPECT_TRUE(base::CreateDirectory(varlib));
  lease_file_ =
      varlib.Append(base::StringPrintf("dhcpcd-%s.lease6", kDeviceName));
  EXPECT_EQ(0, base::WriteFile(pid_file_, "", 0));
  EXPECT_EQ(0, base::WriteFile(lease_file_, "", 0));
  EXPECT_TRUE(base::PathExists(pid_file_));
  EXPECT_TRUE(base::PathExists(lease_file_));

  return config;
}

void DHCPv6ConfigTest::StopRunningConfigAndExpect(DHCPv6ConfigRefPtr config,
                                                  bool lease_file_exists) {
  ScopedMockLog log;
  // We use a non-zero exit status so that we get the log message.
  EXPECT_CALL(log, Log(_, _, ::testing::EndsWith("status 10")));
  EXPECT_CALL(provider_, UnbindPID(kPID));
  config->OnProcessExited(10);

  EXPECT_FALSE(base::PathExists(pid_file_));
  EXPECT_EQ(lease_file_exists, base::PathExists(lease_file_));
}

TEST_F(DHCPv6ConfigTest, ParseConfiguration) {
  const char kConfigIPAddress[] = "2001:db8:0:1::129";
  const char kConfigDelegatedPrefix[] = "2001:db8:1:100::";
  const char kConfigNameServer[] = "fec8:0::1";
  const char kConfigDomainSearch[] = "example.domain";
  const uint32_t kConfigDelegatedPrefixLength = 56;
  const uint32_t kConfigIPAddressLeaseTime = 5;
  const uint32_t kConfigIPAddressPreferredLeaseTime = 4;
  const uint32_t kConfigDelegatedPrefixLeaseTime = 10;
  const uint32_t kConfigDelegatedPrefixPreferredLeaseTime = 3;

  // For building configuration strings.
  const std::string kOne = "1";

  KeyValueStore conf;
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyIPAddress + kOne,
                        kConfigIPAddress);
  conf.Set<uint32_t>(DHCPv6Config::kConfigurationKeyIPAddressLeaseTime + kOne,
                     kConfigIPAddressLeaseTime);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyIPAddressPreferredLeaseTime + kOne,
      kConfigIPAddressPreferredLeaseTime);
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyDelegatedPrefix + kOne,
                        kConfigDelegatedPrefix);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixLength + kOne,
      kConfigDelegatedPrefixLength);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixLeaseTime + kOne,
      kConfigDelegatedPrefixLeaseTime);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixPreferredLeaseTime + kOne,
      kConfigDelegatedPrefixPreferredLeaseTime);
  conf.Set<Strings>(DHCPv6Config::kConfigurationKeyDNS, {kConfigNameServer});
  conf.Set<Strings>(DHCPv6Config::kConfigurationKeyDomainSearch,
                    {kConfigDomainSearch});
  conf.Set<std::string>("UnknownKey", "UnknownValue");

  ASSERT_TRUE(config_->ParseConfiguration(conf));
  const Stringmaps kAddresses = {{
      {kDhcpv6AddressProperty, kConfigIPAddress},
      {kDhcpv6LengthProperty, "128"},
      {kDhcpv6LeaseDurationSecondsProperty,
       base::NumberToString(kConfigIPAddressLeaseTime)},
      {kDhcpv6PreferredLeaseDurationSecondsProperty,
       base::NumberToString(kConfigIPAddressPreferredLeaseTime)},
  }};
  EXPECT_EQ(kAddresses, config_->properties_.dhcpv6_addresses);
  const Stringmaps kDelegatedPrefixes = {{
      {kDhcpv6AddressProperty, kConfigDelegatedPrefix},
      {kDhcpv6LengthProperty,
       base::NumberToString(kConfigDelegatedPrefixLength)},
      {kDhcpv6LeaseDurationSecondsProperty,
       base::NumberToString(kConfigDelegatedPrefixLeaseTime)},
      {kDhcpv6PreferredLeaseDurationSecondsProperty,
       base::NumberToString(kConfigDelegatedPrefixPreferredLeaseTime)},
  }};
  EXPECT_EQ(kDelegatedPrefixes, config_->properties_.dhcpv6_delegated_prefixes);
  ASSERT_EQ(1, config_->properties_.dns_servers.size());
  EXPECT_EQ(kConfigNameServer, config_->properties_.dns_servers[0]);
  ASSERT_EQ(1, config_->properties_.domain_search.size());
  EXPECT_EQ(kConfigDomainSearch, config_->properties_.domain_search[0]);
  // Use IP address lease time since it is shorter.
  EXPECT_EQ(kConfigIPAddressLeaseTime,
            config_->properties_.lease_duration_seconds);
}

MATCHER_P(IsDHCPCDv6Args, has_lease_suffix, "") {
  if (arg[0] != "-B" || arg[1] != "-q" || arg[2] != "-6" || arg[3] != "-a") {
    return false;
  }

  int end_offset = 4;

  std::string device_arg = has_lease_suffix ? std::string(kDeviceName) + "=" +
                                                  std::string(kLeaseFileSuffix)
                                            : kDeviceName;
  return arg[end_offset] == device_arg;
}

TEST_F(DHCPv6ConfigTest, StartDhcpcd) {
  EXPECT_CALL(process_manager_,
              StartProcessInMinijail(_, _, IsDHCPCDv6Args(kHasLeaseSuffix), _,
                                     _, _, _, _, _, _))
      .WillOnce(Return(-1));
  EXPECT_FALSE(StartInstance(config_));
}

TEST_F(DHCPv6ConfigTest, ParseConfig) {
  const char kConfigIPAddress[] = "2001:db8:0:1::128";
  const char kConfigDelegatedPrefix[] = "2001:db8:1:101::";
  const char kConfigNameServer[] = "fec8:0::2";
  const char kConfigDomainSearch[] = "example.domain";
  const uint32_t kConfigDelegatedPrefixLength = 56;
  const uint32_t kConfigIPAddressLeaseTime = 5;
  const uint32_t kConfigIPAddressPreferredLeaseTime = 4;
  const uint32_t kConfigDelegatedPrefixLeaseTime = 10;
  const uint32_t kConfigDelegatedPrefixPreferredLeaseTime = 3;

  // For building configuration strings.
  const std::string kOne = "1";

  KeyValueStore conf;
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyIPAddress + kOne,
                        kConfigIPAddress);
  conf.Set<uint32_t>(DHCPv6Config::kConfigurationKeyIPAddressLeaseTime + kOne,
                     kConfigIPAddressLeaseTime);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyIPAddressPreferredLeaseTime + kOne,
      kConfigIPAddressPreferredLeaseTime);
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyDelegatedPrefix + kOne,
                        kConfigDelegatedPrefix);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixLength + kOne,
      kConfigDelegatedPrefixLength);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixLeaseTime + kOne,
      kConfigDelegatedPrefixLeaseTime);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixPreferredLeaseTime + kOne,
      kConfigDelegatedPrefixPreferredLeaseTime);
  conf.Set<Strings>(DHCPv6Config::kConfigurationKeyDNS, {kConfigNameServer});
  conf.Set<Strings>(DHCPv6Config::kConfigurationKeyDomainSearch,
                    {kConfigDomainSearch});
  conf.Set<std::string>("UnknownKey", "UnknownValue");

  ASSERT_TRUE(config_->ParseConfiguration(conf));
  const Stringmaps kAddresses = {{
      {kDhcpv6AddressProperty, kConfigIPAddress},
      {kDhcpv6LengthProperty, "128"},
      {kDhcpv6LeaseDurationSecondsProperty,
       base::NumberToString(kConfigIPAddressLeaseTime)},
      {kDhcpv6PreferredLeaseDurationSecondsProperty,
       base::NumberToString(kConfigIPAddressPreferredLeaseTime)},
  }};
  EXPECT_EQ(kAddresses, config_->properties_.dhcpv6_addresses);
  const Stringmaps kDelegatedPrefixes = {{
      {kDhcpv6AddressProperty, kConfigDelegatedPrefix},
      {kDhcpv6LengthProperty,
       base::NumberToString(kConfigDelegatedPrefixLength)},
      {kDhcpv6LeaseDurationSecondsProperty,
       base::NumberToString(kConfigDelegatedPrefixLeaseTime)},
      {kDhcpv6PreferredLeaseDurationSecondsProperty,
       base::NumberToString(kConfigDelegatedPrefixPreferredLeaseTime)},
  }};
  EXPECT_EQ(kDelegatedPrefixes, config_->properties_.dhcpv6_delegated_prefixes);
  ASSERT_EQ(1, config_->properties_.dns_servers.size());
  EXPECT_EQ(kConfigNameServer, config_->properties_.dns_servers[0]);
  ASSERT_EQ(1, config_->properties_.domain_search.size());
  EXPECT_EQ(kConfigDomainSearch, config_->properties_.domain_search[0]);
  // Use IP address lease time since it is shorter.
  EXPECT_EQ(kConfigIPAddressLeaseTime,
            config_->properties_.lease_duration_seconds);

  // Higher lease times
  const char kConfigIPAddress1[] = "2001:db8:0:1::128";
  const char kConfigDelegatedPrefix1[] = "2001:db8:1:101::";
  const char kConfigNameServer1[] = "fec8:0::2";
  const char kConfigDomainSearch1[] = "example.domain";
  const uint32_t kConfigDelegatedPrefixLength1 = 56;
  const uint32_t kConfigIPAddressLeaseTime1 = 500;
  const uint32_t kConfigIPAddressPreferredLeaseTime1 = 400;
  const uint32_t kConfigDelegatedPrefixLeaseTime1 = 100;
  const uint32_t kConfigDelegatedPrefixPreferredLeaseTime1 = 30;

  KeyValueStore conf1;
  conf1.Set<std::string>(DHCPv6Config::kConfigurationKeyIPAddress + kOne,
                        kConfigIPAddress1);
  conf1.Set<uint32_t>(DHCPv6Config::kConfigurationKeyIPAddressLeaseTime + kOne,
                     kConfigIPAddressLeaseTime1);
  conf1.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyIPAddressPreferredLeaseTime + kOne,
      kConfigIPAddressPreferredLeaseTime1);
  conf1.Set<std::string>(DHCPv6Config::kConfigurationKeyDelegatedPrefix + kOne,
                        kConfigDelegatedPrefix1);
  conf1.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixLength + kOne,
      kConfigDelegatedPrefixLength1);
  conf1.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixLeaseTime + kOne,
      kConfigDelegatedPrefixLeaseTime1);
  conf1.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixPreferredLeaseTime + kOne,
      kConfigDelegatedPrefixPreferredLeaseTime1);
  conf1.Set<Strings>(DHCPv6Config::kConfigurationKeyDNS, {kConfigNameServer1});
  conf1.Set<Strings>(DHCPv6Config::kConfigurationKeyDomainSearch,
                    {kConfigDomainSearch1});
  // Clear existing IP address and prefixes leases
  conf1.Set<uint32_t>(DHCPv6Config::kConfigurationKeyIPAddressIaid, 0);
  conf1.Set<uint32_t>(DHCPv6Config::kConfigurationKeyDelegatedPrefixIaid, 0);
  conf1.Set<std::string>("UnknownKey", "UnknownValue");

  ASSERT_TRUE(config_->ParseConfiguration(conf1));
  const Stringmaps kAddresses1 = {{
      {kDhcpv6AddressProperty, kConfigIPAddress1},
      {kDhcpv6LengthProperty, "128"},
      {kDhcpv6LeaseDurationSecondsProperty,
       base::NumberToString(kConfigIPAddressLeaseTime1)},
      {kDhcpv6PreferredLeaseDurationSecondsProperty,
       base::NumberToString(kConfigIPAddressPreferredLeaseTime1)},
  }};
  EXPECT_EQ(kAddresses1, config_->properties_.dhcpv6_addresses);
  const Stringmaps kDelegatedPrefixes1 = {{
      {kDhcpv6AddressProperty, kConfigDelegatedPrefix1},
      {kDhcpv6LengthProperty,
       base::NumberToString(kConfigDelegatedPrefixLength1)},
      {kDhcpv6LeaseDurationSecondsProperty,
       base::NumberToString(kConfigDelegatedPrefixLeaseTime1)},
      {kDhcpv6PreferredLeaseDurationSecondsProperty,
       base::NumberToString(kConfigDelegatedPrefixPreferredLeaseTime1)},
  }};
  EXPECT_EQ(kDelegatedPrefixes1,
            config_->properties_.dhcpv6_delegated_prefixes);
  ASSERT_EQ(1, config_->properties_.dns_servers.size());
  EXPECT_EQ(kConfigNameServer, config_->properties_.dns_servers[0]);
  ASSERT_EQ(1, config_->properties_.domain_search.size());
  EXPECT_EQ(kConfigDomainSearch, config_->properties_.domain_search[0]);
  // Use Delegate prefix lease time since it is shorter.
  EXPECT_EQ(kConfigDelegatedPrefixLeaseTime1,
            config_->properties_.lease_duration_seconds);

  //Lower lease
  const char kConfigIPAddress2[] = "2001:db8:0:1::128";
  const char kConfigDelegatedPrefix2[] = "2001:db8:1:101::";
  const char kConfigNameServer2[] = "fec8:0::2";
  const char kConfigDomainSearch2[] = "example.domain";
  const uint32_t kConfigDelegatedPrefixLength2 = 56;
  const uint32_t kConfigIPAddressLeaseTime2 = 50;
  const uint32_t kConfigIPAddressPreferredLeaseTime2 = 40;
  const uint32_t kConfigDelegatedPrefixLeaseTime2 = 30;
  const uint32_t kConfigDelegatedPrefixPreferredLeaseTime2 = 15;

  KeyValueStore conf2;
  conf2.Set<std::string>(DHCPv6Config::kConfigurationKeyIPAddress + kOne,
                        kConfigIPAddress2);
  conf2.Set<uint32_t>(DHCPv6Config::kConfigurationKeyIPAddressLeaseTime + kOne,
                     kConfigIPAddressLeaseTime2);
  conf2.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyIPAddressPreferredLeaseTime + kOne,
      kConfigIPAddressPreferredLeaseTime2);
  conf2.Set<std::string>(DHCPv6Config::kConfigurationKeyDelegatedPrefix + kOne,
                        kConfigDelegatedPrefix2);
  conf2.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixLength + kOne,
      kConfigDelegatedPrefixLength2);
  conf2.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixLeaseTime + kOne,
      kConfigDelegatedPrefixLeaseTime2);
  conf2.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixPreferredLeaseTime + kOne,
      kConfigDelegatedPrefixPreferredLeaseTime2);
  conf2.Set<Strings>(DHCPv6Config::kConfigurationKeyDNS, {kConfigNameServer2});
  conf2.Set<Strings>(DHCPv6Config::kConfigurationKeyDomainSearch,
                    {kConfigDomainSearch2});
  // Clear existing IP address and prefixes leases
  conf2.Set<uint32_t>(DHCPv6Config::kConfigurationKeyIPAddressIaid, 0);
  conf2.Set<uint32_t>(DHCPv6Config::kConfigurationKeyDelegatedPrefixIaid, 0);
  conf2.Set<std::string>("UnknownKey", "UnknownValue");

  ASSERT_TRUE(config_->ParseConfiguration(conf2));
  const Stringmaps kAddresses2 = {{
      {kDhcpv6AddressProperty, kConfigIPAddress2},
      {kDhcpv6LengthProperty, "128"},
      {kDhcpv6LeaseDurationSecondsProperty,
       base::NumberToString(kConfigIPAddressLeaseTime2)},
      {kDhcpv6PreferredLeaseDurationSecondsProperty,
       base::NumberToString(kConfigIPAddressPreferredLeaseTime2)},
  }};
  EXPECT_EQ(kAddresses2, config_->properties_.dhcpv6_addresses);
  const Stringmaps kDelegatedPrefixes2 = {{
      {kDhcpv6AddressProperty, kConfigDelegatedPrefix2},
      {kDhcpv6LengthProperty,
       base::NumberToString(kConfigDelegatedPrefixLength2)},
      {kDhcpv6LeaseDurationSecondsProperty,
       base::NumberToString(kConfigDelegatedPrefixLeaseTime2)},
      {kDhcpv6PreferredLeaseDurationSecondsProperty,
       base::NumberToString(kConfigDelegatedPrefixPreferredLeaseTime2)},
  }};
  EXPECT_EQ(kDelegatedPrefixes2,
            config_->properties_.dhcpv6_delegated_prefixes);
  ASSERT_EQ(1, config_->properties_.dns_servers.size());
  EXPECT_EQ(kConfigNameServer, config_->properties_.dns_servers[0]);
  ASSERT_EQ(1, config_->properties_.domain_search.size());
  EXPECT_EQ(kConfigDomainSearch, config_->properties_.domain_search[0]);
  // Use IP address lease time since it is shorter.
  EXPECT_EQ(kConfigDelegatedPrefixLeaseTime2,
            config_->properties_.lease_duration_seconds);
}

TEST_F(DHCPv6ConfigTest, ParseConfigMultiplePD) {
  const char kConfigDelegatedPrefix[] = "2001:db8:1:101::";
  const uint32_t kConfigDelegatedPrefixLength = 56;
  const uint32_t kConfigDelegatedPrefixLeaseTime = 10;
  const uint32_t kConfigDelegatedPrefixPreferredLeaseTime = 3;
  const char kConfigDelegatedPrefix1[] = "2001:db8:1:102::";
  const uint32_t kConfigDelegatedPrefixLength1 = 60;
  const uint32_t kConfigDelegatedPrefixLeaseTime1 = 5;
  const uint32_t kConfigDelegatedPrefixPreferredLeaseTime1 = 2;
  const char kConfigNameServer[] = "fec8:0::2";
  const char kConfigDomainSearch[] = "example.domain";

  // For building configuration strings.
  const std::string kOne = "1";
  const std::string kTwo = "2";

  KeyValueStore conf;
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyDelegatedPrefix + kOne,
                        kConfigDelegatedPrefix);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixLength + kOne,
      kConfigDelegatedPrefixLength);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixLeaseTime + kOne,
      kConfigDelegatedPrefixLeaseTime);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixPreferredLeaseTime + kOne,
      kConfigDelegatedPrefixPreferredLeaseTime);
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyDelegatedPrefix + kTwo,
                        kConfigDelegatedPrefix1);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixLength + kTwo,
      kConfigDelegatedPrefixLength1);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixLeaseTime + kTwo,
      kConfigDelegatedPrefixLeaseTime1);
  conf.Set<uint32_t>(
      DHCPv6Config::kConfigurationKeyDelegatedPrefixPreferredLeaseTime + kTwo,
      kConfigDelegatedPrefixPreferredLeaseTime1);
  conf.Set<Strings>(DHCPv6Config::kConfigurationKeyDNS, {kConfigNameServer});
  conf.Set<Strings>(DHCPv6Config::kConfigurationKeyDomainSearch,
                    {kConfigDomainSearch});
  conf.Set<std::string>("UnknownKey", "UnknownValue");

  ASSERT_TRUE(config_->ParseConfiguration(conf));
  Stringmaps kDelegatedPrefixes;
  kDelegatedPrefixes.push_back({
      {kDhcpv6AddressProperty, kConfigDelegatedPrefix},
      {kDhcpv6LengthProperty,
       base::NumberToString(kConfigDelegatedPrefixLength)},
      {kDhcpv6LeaseDurationSecondsProperty,
       base::NumberToString(kConfigDelegatedPrefixLeaseTime)},
      {kDhcpv6PreferredLeaseDurationSecondsProperty,
       base::NumberToString(kConfigDelegatedPrefixPreferredLeaseTime)},
  });
  kDelegatedPrefixes.push_back({
      {kDhcpv6AddressProperty, kConfigDelegatedPrefix1},
      {kDhcpv6LengthProperty,
       base::NumberToString(kConfigDelegatedPrefixLength1)},
      {kDhcpv6LeaseDurationSecondsProperty,
       base::NumberToString(kConfigDelegatedPrefixLeaseTime1)},
      {kDhcpv6PreferredLeaseDurationSecondsProperty,
       base::NumberToString(kConfigDelegatedPrefixPreferredLeaseTime1)},
  });

  EXPECT_EQ(2, config_->properties_.dhcpv6_delegated_prefixes.size());
  EXPECT_EQ(kDelegatedPrefixes[0],
            config_->properties_.dhcpv6_delegated_prefixes[0]);
  EXPECT_EQ(kDelegatedPrefixes[1],
            config_->properties_.dhcpv6_delegated_prefixes[1]);
  ASSERT_EQ(1, config_->properties_.dns_servers.size());
  EXPECT_EQ(kConfigNameServer, config_->properties_.dns_servers[0]);
  ASSERT_EQ(1, config_->properties_.domain_search.size());
  EXPECT_EQ(kConfigDomainSearch, config_->properties_.domain_search[0]);
  // Use second prefix lease time since it is shorter.
  EXPECT_EQ(kConfigDelegatedPrefixLeaseTime1,
            config_->properties_.lease_duration_seconds);
}

namespace {

class DHCPv6ConfigCallbackTest : public DHCPv6ConfigTest {
 public:
  void SetUp() override {
    DHCPv6ConfigTest::SetUp();
    config_->RegisterUpdateCallback(base::Bind(
        &DHCPv6ConfigCallbackTest::SuccessCallback, base::Unretained(this)));
    config_->RegisterFailureCallback(base::Bind(
        &DHCPv6ConfigCallbackTest::FailureCallback, base::Unretained(this)));
    ip_config_ = config_;
  }

  MOCK_METHOD(void, SuccessCallback, (const IPConfigRefPtr&, bool));
  MOCK_METHOD(void, FailureCallback, (const IPConfigRefPtr&));

  // The mock methods above take IPConfigRefPtr because this is the type
  // that the registered callbacks take.  This conversion of the DHCP
  // config ref pointer eases our work in setting up expectations.
  const IPConfigRefPtr& ConfigRef() { return ip_config_; }

 private:
  IPConfigRefPtr ip_config_;
};

}  // namespace

TEST_F(DHCPv6ConfigCallbackTest, ProcessEventSignalFail) {
  KeyValueStore conf;
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyIPAddress, kIPAddress);
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyDelegatedPrefix,
                        kDelegatedPrefix);
  EXPECT_CALL(*this, SuccessCallback(_, _)).Times(0);
  EXPECT_CALL(*this, FailureCallback(ConfigRef()));
  config_->ProcessEventSignal(DHCPv6Config::kReasonFail, conf);
  Mock::VerifyAndClearExpectations(this);
  EXPECT_TRUE(config_->properties().address.empty());
}

TEST_F(DHCPv6ConfigCallbackTest, ProcessEventSignalSuccess) {
  const std::string kOne = "1";
  for (const auto& reason :
       {DHCPv6Config::kReasonBound, DHCPv6Config::kReasonRebind,
        DHCPv6Config::kReasonReboot, DHCPv6Config::kReasonRenew}) {
    KeyValueStore conf;
    conf.Set<std::string>(DHCPv6Config::kConfigurationKeyIPAddress + kOne,
                          kIPAddress);
    const uint32_t kLeaseTime = 1;
    conf.Set<uint32_t>(DHCPv6Config::kConfigurationKeyIPAddressLeaseTime + kOne,
                       kLeaseTime);
    conf.Set<uint32_t>(
        DHCPv6Config::kConfigurationKeyIPAddressPreferredLeaseTime + kOne,
        kLeaseTime);
    conf.Set<uint32_t>(DHCPv6Config::kConfigurationKeyIPAddressIaid, 0);

    EXPECT_CALL(*this, SuccessCallback(ConfigRef(), true));
    EXPECT_CALL(*this, FailureCallback(_)).Times(0);
    config_->ProcessEventSignal(reason, conf);
    std::string failure_message = std::string(reason) + " failed";
    EXPECT_TRUE(Mock::VerifyAndClearExpectations(this)) << failure_message;
    ASSERT_EQ(1, config_->properties().dhcpv6_addresses.size());
    auto it =
        config_->properties().dhcpv6_addresses[0].find(kDhcpv6AddressProperty);
    ASSERT_TRUE(it != config_->properties().dhcpv6_addresses[0].end())
        << failure_message;
    EXPECT_EQ("2001:db8:0:1::1", it->second) << failure_message;
  }
}

TEST_F(DHCPv6ConfigCallbackTest, StoppedDuringFailureCallback) {
  const std::string kOne = "1";
  KeyValueStore conf;
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyIPAddress + kOne,
                        kIPAddress);
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyDelegatedPrefix + kOne,
                        kDelegatedPrefix);
  // Stop the DHCP config while it is calling the failure callback.  We
  // need to ensure that no callbacks are left running inadvertently as
  // a result.
  EXPECT_CALL(*this, FailureCallback(ConfigRef()))
      .WillOnce(InvokeWithoutArgs(this, &DHCPv6ConfigTest::StopInstance));
  config_->ProcessEventSignal(DHCPv6Config::kReasonFail, conf);
  EXPECT_TRUE(Mock::VerifyAndClearExpectations(this));
}

TEST_F(DHCPv6ConfigCallbackTest, StoppedDuringSuccessCallback) {
  const std::string kOne = "1";
  KeyValueStore conf;
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyIPAddress + kOne,
                        kIPAddress);
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyDelegatedPrefix + kOne,
                        kDelegatedPrefix);
  const uint32_t kLeaseTime = 1;
  conf.Set<uint32_t>(DHCPv6Config::kConfigurationKeyIPAddressLeaseTime,
                     kLeaseTime);
  // Stop the DHCP config while it is calling the success callback.  This
  // can happen if the device has a static IP configuration and releases
  // the lease after accepting other network parameters from the DHCP
  // IPConfig properties.  We need to ensure that no callbacks are left
  // running inadvertently as a result.
  EXPECT_CALL(*this, SuccessCallback(ConfigRef(), true))
      .WillOnce(InvokeWithoutArgs(this, &DHCPv6ConfigTest::StopInstance));
  config_->ProcessEventSignal(DHCPv6Config::kReasonBound, conf);
  EXPECT_TRUE(Mock::VerifyAndClearExpectations(this));
}

TEST_F(DHCPv6ConfigCallbackTest, ProcessEventSignalUnknown) {
  const std::string kOne = "1";
  KeyValueStore conf;
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyIPAddress + kOne,
                        kIPAddress);
  conf.Set<std::string>(DHCPv6Config::kConfigurationKeyDelegatedPrefix + kOne,
                        kDelegatedPrefix);
  static const char kReasonUnknown[] = "UNKNOWN_REASON";
  EXPECT_CALL(*this, SuccessCallback(_, _)).Times(0);
  EXPECT_CALL(*this, FailureCallback(_)).Times(0);
  config_->ProcessEventSignal(kReasonUnknown, conf);
  Mock::VerifyAndClearExpectations(this);
  EXPECT_TRUE(config_->properties().dhcpv6_addresses.empty());
}

TEST_F(DHCPv6ConfigTest, StartSuccessEphemeral) {
  DHCPv6ConfigRefPtr config = CreateRunningConfig(kDeviceName);
  StopRunningConfigAndExpect(config, false);
}

TEST_F(DHCPv6ConfigTest, StartSuccessPersistent) {
  DHCPv6ConfigRefPtr config = CreateRunningConfig(kLeaseFileSuffix);
  StopRunningConfigAndExpect(config, true);
}

}  // namespace shill
