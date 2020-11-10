// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include <net/if.h>

#include <fuzzer/FuzzedDataProvider.h>
#include <string>
#include <vector>

#include <base/at_exit.h>
#include <base/bind.h>
#include <base/bind_helpers.h>
#include <base/logging.h>

#include "patchpanel/datapath.h"
#include "patchpanel/firewall.h"
#include "patchpanel/minijailed_process_runner.h"
#include "patchpanel/multicast_forwarder.h"
#include "patchpanel/net_util.h"

namespace patchpanel {

// Always succeeds
int ioctl_stub(int fd, unsigned long req, ...) {
  return 0;
}

class RandomProcessRunner : public MinijailedProcessRunner {
 public:
  explicit RandomProcessRunner(FuzzedDataProvider* data_provider)
      : data_provider_{data_provider} {}
  RandomProcessRunner(const RandomProcessRunner&) = delete;
  RandomProcessRunner& operator=(const RandomProcessRunner&) = delete;
  ~RandomProcessRunner() = default;

  int Run(const std::vector<std::string>& argv, bool log_failures) override {
    return data_provider_->ConsumeBool();
  }

 private:
  FuzzedDataProvider* data_provider_;
};

namespace {

constexpr pid_t kTestPID = -2;

class Environment {
 public:
  Environment() {
    logging::SetMinLogLevel(logging::LOG_FATAL);  // <- DISABLE LOGGING.
  }
  base::AtExitManager at_exit;
};

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
  static Environment env;

  FuzzedDataProvider provider(data, size);
  RandomProcessRunner runner(&provider);
  Firewall firewall;
  Datapath datapath(&runner, &firewall, ioctl_stub);

  while (provider.remaining_bytes() > 0) {
    uint32_t pid = provider.ConsumeIntegral<uint32_t>();
    std::string netns_name = provider.ConsumeRandomLengthString(10);
    std::string ifname = provider.ConsumeRandomLengthString(IFNAMSIZ - 1);
    std::string ifname2 = provider.ConsumeRandomLengthString(IFNAMSIZ - 1);
    std::string bridge = provider.ConsumeRandomLengthString(IFNAMSIZ - 1);
    uint32_t addr = provider.ConsumeIntegral<uint32_t>();
    uint32_t addr2 = provider.ConsumeIntegral<uint32_t>();
    uint32_t addr3 = provider.ConsumeIntegral<uint32_t>();
    std::string addr_str = IPv4AddressToString(addr);
    uint32_t prefix_len = provider.ConsumeIntegralInRange<uint32_t>(0, 31);
    Subnet subnet(provider.ConsumeIntegral<int32_t>(), prefix_len,
                  base::DoNothing());
    std::unique_ptr<SubnetAddress> subnet_addr = subnet.AllocateAtOffset(0);
    MacAddress mac;
    std::vector<uint8_t> bytes = provider.ConsumeBytes<uint8_t>(mac.size());
    std::copy(std::begin(bytes), std::begin(bytes), std::begin(mac));

    datapath.Start();
    datapath.Stop();
    datapath.AddBridge(ifname, addr, prefix_len);
    datapath.RemoveBridge(ifname);
    datapath.StartRoutingDevice(ifname, ifname2, addr, TrafficSource::UNKNOWN);
    datapath.StopRoutingDevice(ifname, ifname2, addr, TrafficSource::UNKNOWN);
    datapath.StartRoutingNamespace(kTestPID, netns_name, ifname, ifname2, addr,
                                   prefix_len, addr2, addr3, mac);
    datapath.StopRoutingNamespace(netns_name, ifname, addr, prefix_len, addr2);
    datapath.ConnectVethPair(pid, netns_name, ifname, ifname2, mac, addr,
                             prefix_len, provider.ConsumeBool());
    datapath.RemoveInterface(ifname);
    datapath.AddTAP(ifname, &mac, subnet_addr.get(), "");
    datapath.RemoveTAP(ifname);
    datapath.AddIPv4Route(provider.ConsumeIntegral<uint32_t>(),
                          provider.ConsumeIntegral<uint32_t>(),
                          provider.ConsumeIntegral<uint32_t>());
  }

  return 0;
}

}  // namespace
}  // namespace patchpanel
