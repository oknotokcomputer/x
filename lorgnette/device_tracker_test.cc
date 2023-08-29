// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "lorgnette/device_tracker.h"

#include <cstdint>
#include <memory>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <base/run_loop.h>
#include <base/files/file.h>
#include <base/files/scoped_file.h>
#include <base/files/scoped_temp_dir.h>
#include <base/test/bind.h>
#include <base/test/task_environment.h>

#include "lorgnette/firewall_manager.h"
#include "lorgnette/sane_client_fake.h"
#include "lorgnette/test_util.h"
#include "lorgnette/usb/libusb_wrapper_fake.h"
#include "lorgnette/usb/usb_device_fake.h"

using ::testing::_;
using ::testing::Each;
using ::testing::ElementsAre;
using ::testing::HasSubstr;
using ::testing::Not;
using ::testing::Pointee;
using ::testing::Property;
using ::testing::UnorderedElementsAre;

namespace lorgnette {

namespace {

class MockFirewallManager : public FirewallManager {
 public:
  explicit MockFirewallManager(const std::string& interface)
      : FirewallManager(interface) {}

  MOCK_METHOD(PortToken, RequestUdpPortAccess, (uint16_t), (override));
};

TEST(DeviceTrackerTest, CreateMultipleSessions) {
  base::test::SingleThreadTaskEnvironment task_environment;
  base::RunLoop run_loop;

  std::vector<std::string> closed_sessions;
  auto signal_handler = base::BindLambdaForTesting(
      [&closed_sessions](const ScannerListChangedSignal& signal) {
        if (signal.event_type() == ScannerListChangedSignal::SESSION_ENDING) {
          closed_sessions.push_back(signal.session_id());
        }
      });

  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());
  tracker->SetScannerListChangedSignalSender(signal_handler);

  EXPECT_EQ(tracker->NumActiveDiscoverySessions(), 0);

  StartScannerDiscoveryRequest start_request;
  start_request.set_client_id("client_1");
  StartScannerDiscoveryResponse response1 =
      tracker->StartScannerDiscovery(start_request);
  EXPECT_TRUE(response1.started());
  EXPECT_FALSE(response1.session_id().empty());
  EXPECT_EQ(tracker->NumActiveDiscoverySessions(), 1);

  start_request.set_client_id("client_2");
  StartScannerDiscoveryResponse response2 =
      tracker->StartScannerDiscovery(start_request);
  EXPECT_TRUE(response2.started());
  EXPECT_FALSE(response2.session_id().empty());
  EXPECT_NE(response1.session_id(), response2.session_id());
  EXPECT_EQ(tracker->NumActiveDiscoverySessions(), 2);

  StopScannerDiscoveryRequest stop_request;
  stop_request.set_session_id(response1.session_id());
  StopScannerDiscoveryResponse stop1 =
      tracker->StopScannerDiscovery(stop_request);
  EXPECT_TRUE(stop1.stopped());
  EXPECT_EQ(tracker->NumActiveDiscoverySessions(), 1);

  stop_request.set_session_id(response2.session_id());
  StopScannerDiscoveryResponse stop2 =
      tracker->StopScannerDiscovery(stop_request);
  EXPECT_TRUE(stop2.stopped());
  EXPECT_EQ(tracker->NumActiveDiscoverySessions(), 0);

  EXPECT_THAT(closed_sessions,
              ElementsAre(response1.session_id(), response2.session_id()));
}

TEST(DeviceTrackerTest, CreateDuplicateSessions) {
  base::test::SingleThreadTaskEnvironment task_environment;
  base::RunLoop run_loop;

  std::vector<std::string> closed_sessions;
  auto signal_handler = base::BindLambdaForTesting(
      [&closed_sessions](const ScannerListChangedSignal& signal) {
        if (signal.event_type() == ScannerListChangedSignal::SESSION_ENDING) {
          closed_sessions.push_back(signal.session_id());
        }
      });

  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());
  tracker->SetScannerListChangedSignalSender(signal_handler);

  EXPECT_EQ(tracker->NumActiveDiscoverySessions(), 0);

  StartScannerDiscoveryRequest start_request;
  start_request.set_client_id("client_1");
  StartScannerDiscoveryResponse response1 =
      tracker->StartScannerDiscovery(start_request);
  EXPECT_TRUE(response1.started());
  EXPECT_FALSE(response1.session_id().empty());
  EXPECT_EQ(tracker->NumActiveDiscoverySessions(), 1);

  start_request.set_client_id("client_1");
  StartScannerDiscoveryResponse response2 =
      tracker->StartScannerDiscovery(start_request);
  EXPECT_TRUE(response2.started());
  EXPECT_FALSE(response2.session_id().empty());
  EXPECT_EQ(response1.session_id(), response2.session_id());
  EXPECT_EQ(tracker->NumActiveDiscoverySessions(), 1);

  StopScannerDiscoveryRequest stop_request;
  stop_request.set_session_id(response1.session_id());
  StopScannerDiscoveryResponse stop1 =
      tracker->StopScannerDiscovery(stop_request);
  EXPECT_TRUE(stop1.stopped());
  EXPECT_EQ(tracker->NumActiveDiscoverySessions(), 0);

  stop_request.set_session_id(response2.session_id());
  StopScannerDiscoveryResponse stop2 =
      tracker->StopScannerDiscovery(stop_request);
  EXPECT_TRUE(stop2.stopped());
  EXPECT_EQ(tracker->NumActiveDiscoverySessions(), 0);

  // Session ID should get closed twice even though it doesn't exist the second
  // time.
  EXPECT_THAT(closed_sessions,
              ElementsAre(response1.session_id(), response1.session_id()));
}

TEST(DeviceTrackerTest, StartSessionMissingClient) {
  base::test::SingleThreadTaskEnvironment task_environment;
  base::RunLoop run_loop;

  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  StartScannerDiscoveryRequest start_request;
  start_request.set_client_id("");
  StartScannerDiscoveryResponse response =
      tracker->StartScannerDiscovery(start_request);
  EXPECT_FALSE(response.started());
  EXPECT_TRUE(response.session_id().empty());
  EXPECT_EQ(tracker->NumActiveDiscoverySessions(), 0);
}

TEST(DeviceTrackerTest, StopSessionMissingID) {
  base::test::SingleThreadTaskEnvironment task_environment;
  base::RunLoop run_loop;

  std::vector<std::string> closed_sessions;
  auto signal_handler = base::BindLambdaForTesting(
      [&closed_sessions](const ScannerListChangedSignal& signal) {
        if (signal.event_type() == ScannerListChangedSignal::SESSION_ENDING) {
          closed_sessions.push_back(signal.session_id());
        }
      });

  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());
  tracker->SetScannerListChangedSignalSender(signal_handler);

  StopScannerDiscoveryRequest stop_request;
  stop_request.set_session_id("");
  StopScannerDiscoveryResponse response =
      tracker->StopScannerDiscovery(stop_request);
  EXPECT_FALSE(response.stopped());
  EXPECT_TRUE(closed_sessions.empty());
  EXPECT_EQ(tracker->NumActiveDiscoverySessions(), 0);
}

// Test the whole flow with several fake USB devices.  Confirm that
// exactly and only the devices that fully match the checks and have a SANE
// backend have a signal emitted before shutting down the session.
TEST(DeviceTrackerTest, CompleteDiscoverySession) {
  // Scanner that supports eSCL over IPP-USB.
  auto ippusb_escl_device = std::make_unique<UsbDeviceFake>();

  libusb_device_descriptor device_desc = MakeMinimalDeviceDescriptor();
  device_desc.bDeviceClass = LIBUSB_CLASS_PER_INTERFACE;
  device_desc.bNumConfigurations = 1;
  device_desc.iManufacturer = 1;
  device_desc.iProduct = 2;
  ippusb_escl_device->SetStringDescriptors(
      {"", "GoogleTest", "eSCL Scanner 3000"});
  ippusb_escl_device->SetDeviceDescriptor(device_desc);

  // One altsetting with a printer class and the IPP-USB protocol.
  auto altsetting = MakeIppUsbInterfaceDescriptor();

  // One interface containing the altsetting.
  auto interface = std::make_unique<libusb_interface>();
  interface->num_altsetting = 1;
  interface->altsetting = altsetting.get();

  // One config descriptor containing the interface.
  libusb_config_descriptor descriptor;
  memset(&descriptor, 0, sizeof(descriptor));
  descriptor.bLength = sizeof(descriptor);
  descriptor.bDescriptorType = LIBUSB_DT_CONFIG;
  descriptor.wTotalLength = sizeof(descriptor);
  descriptor.bNumInterfaces = 1;
  descriptor.interface = interface.get();

  ippusb_escl_device->SetConfigDescriptors({descriptor});
  ippusb_escl_device->SetBusNumber(1);
  ippusb_escl_device->SetDeviceAddress(1);
  ippusb_escl_device->Init();

  // Printer that supports IPP-USB but not eSCL.
  auto ippusb_printer = UsbDeviceFake::Clone(*ippusb_escl_device.get());
  ippusb_printer->MutableDeviceDescriptor().idProduct = 0x6543;
  ippusb_printer->SetStringDescriptors(
      {"", "GoogleTest", "IPP-USB Printer 2000"});

  // Printer that doesn't support IPP-USB.
  auto printer_altsetting = MakeIppUsbInterfaceDescriptor();
  printer_altsetting->bInterfaceProtocol = 0;
  auto printer_interface = std::make_unique<libusb_interface>();
  printer_interface->num_altsetting = 1;
  printer_interface->altsetting = printer_altsetting.get();
  auto usb_printer = UsbDeviceFake::Clone(*ippusb_printer.get());
  usb_printer->MutableDeviceDescriptor().idProduct = 0x7654;
  usb_printer->MutableConfigDescriptor(0).interface = printer_interface.get();
  usb_printer->SetStringDescriptors({"", "GoogleTest", "USB Printer 1000"});

  // Not a printer at all.
  auto non_printer = UsbDeviceFake::Clone(*usb_printer.get());
  non_printer->MutableDeviceDescriptor().idProduct = 0x7654;
  non_printer->MutableDeviceDescriptor().bDeviceClass = LIBUSB_DT_HUB;
  non_printer->SetStringDescriptors({"", "GoogleTest", "USB Gadget 500"});

  std::vector<std::unique_ptr<UsbDevice>> device_list;
  device_list.emplace_back(std::move(non_printer));
  device_list.emplace_back(std::move(ippusb_escl_device));
  device_list.emplace_back(std::move(ippusb_printer));
  device_list.emplace_back(std::move(usb_printer));
  auto libusb = std::make_unique<LibusbWrapperFake>();
  libusb->SetDevices(std::move(device_list));

  base::test::SingleThreadTaskEnvironment task_environment;
  base::RunLoop run_loop;

  // A "socket" that can reach the fake IPP-USB scanner and the matching
  // fake SANE device to talk to it.
  auto ippusb_scanner = std::make_unique<SaneDeviceFake>();
  base::ScopedTempDir temp_dir;
  ASSERT_TRUE(temp_dir.CreateUniqueTempDir());
  auto sane_client = std::make_unique<SaneClientFake>();
  sane_client->SetIppUsbSocketDir(temp_dir.GetPath());
  base::FilePath ippusb_escl_path = temp_dir.GetPath().Append("1234-4321.sock");
  base::File ippusb_escl_socket(
      ippusb_escl_path, base::File::FLAG_CREATE | base::File::FLAG_WRITE);
  base::FilePath ippusb_path = temp_dir.GetPath().Append("1234-6543.sock");
  base::File ippusb_socket(ippusb_path,
                           base::File::FLAG_CREATE | base::File::FLAG_WRITE);
  sane_client->SetDeviceForName(
      "airscan:escl:GoogleTest eSCL Scanner 3000:unix://1234-4321.sock/eSCL/",
      std::move(ippusb_scanner));

  sane_client->SetListDevicesResult(true);
  // Duplicates of eSCL over ippusb that are filtered out.
  sane_client->AddDevice("pixma:12344321_12AF", "GoogleTest",
                         "eSCL Scanner 3001", "eSCL");
  auto pixma_scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("pixma:12344321_12AF",
                                std::move(pixma_scanner));
  sane_client->AddDevice("epson2:libusb:001:001", "GoogleTest",
                         "eSCL Scanner 3002", "eSCL");
  auto epson2_scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("epson2:libusb:001:001",
                                std::move(epson2_scanner));

  // Unique device without ippusb support that is added during SANE probing.
  sane_client->AddDevice("epsonds:libusb:001:002", "GoogleTest",
                         "SANE Scanner 4000", "USB");
  auto epsonds_scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("epsonds:libusb:001:002",
                                std::move(epsonds_scanner));

  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  MockFirewallManager firewall_manager(/*interface=*/"test");
  EXPECT_CALL(firewall_manager, RequestUdpPortAccess(8612))
      .WillOnce(testing::Return(PortToken(/*firewall_manager=*/nullptr,
                                          /*port=*/8612)));
  tracker->SetFirewallManager(&firewall_manager);

  // Signal handler that tracks all the events of interest.
  std::vector<std::string> closed_sessions;
  std::set<std::unique_ptr<ScannerInfo>> scanners;
  std::string session_id;
  auto signal_handler = base::BindLambdaForTesting(
      [&run_loop, &tracker, &closed_sessions, &scanners,
       &session_id](const ScannerListChangedSignal& signal) {
        if (signal.event_type() == ScannerListChangedSignal::ENUM_COMPLETE) {
          StopScannerDiscoveryRequest stop_request;
          stop_request.set_session_id(session_id);
          tracker->StopScannerDiscovery(stop_request);
        }
        if (signal.event_type() == ScannerListChangedSignal::SESSION_ENDING) {
          closed_sessions.push_back(signal.session_id());
          run_loop.Quit();
        }
        if (signal.event_type() == ScannerListChangedSignal::SCANNER_ADDED) {
          std::unique_ptr<ScannerInfo> info(signal.scanner().New());
          info->CopyFrom(signal.scanner());
          scanners.insert(std::move(info));
        }
      });
  tracker->SetScannerListChangedSignalSender(signal_handler);

  StartScannerDiscoveryRequest start_request;
  start_request.set_client_id("ippusb");
  start_request.set_preferred_only(true);
  StartScannerDiscoveryResponse response =
      tracker->StartScannerDiscovery(start_request);
  EXPECT_TRUE(response.started());
  EXPECT_FALSE(response.session_id().empty());
  session_id = response.session_id();

  run_loop.Run();

  EXPECT_THAT(closed_sessions, ElementsAre(response.session_id()));
  EXPECT_THAT(
      scanners,
      Each(Pointee(Property(&ScannerInfo::manufacturer, "GoogleTest"))));
  EXPECT_THAT(
      scanners,
      UnorderedElementsAre(
          Pointee(Property("model", &ScannerInfo::model, "eSCL Scanner 3000")),
          Pointee(
              Property("model", &ScannerInfo::model, "SANE Scanner 4000"))));
}

TEST(DeviceTrackerTest, OpenScannerEmptyDevice) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  OpenScannerRequest request;
  request.set_client_id("DeviceTrackerTest");
  auto response = tracker->OpenScanner(request);

  EXPECT_THAT(response.scanner_id(), EqualsProto(request.scanner_id()));
  EXPECT_EQ(response.result(), OPERATION_RESULT_INVALID);
  EXPECT_EQ(tracker->NumOpenScanners(), 0);
}

TEST(DeviceTrackerTest, OpenScannerEmptyString) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  OpenScannerRequest request;
  request.mutable_scanner_id()->set_connection_string("Test");
  OpenScannerResponse response = tracker->OpenScanner(request);

  EXPECT_THAT(response.scanner_id(), EqualsProto(request.scanner_id()));
  EXPECT_EQ(response.result(), OPERATION_RESULT_INVALID);
  EXPECT_EQ(tracker->NumOpenScanners(), 0);
}

TEST(DeviceTrackerTest, OpenScannerNoDevice) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  OpenScannerRequest request;
  request.mutable_scanner_id()->set_connection_string("Test");
  request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse response = tracker->OpenScanner(request);

  EXPECT_THAT(response.scanner_id(), EqualsProto(request.scanner_id()));
  EXPECT_NE(response.result(), OPERATION_RESULT_SUCCESS);
  EXPECT_EQ(tracker->NumOpenScanners(), 0);
}

TEST(DeviceTrackerTest, OpenScannerFirstClientSucceeds) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest request;
  request.mutable_scanner_id()->set_connection_string("Test");
  request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse response = tracker->OpenScanner(request);

  EXPECT_THAT(response.scanner_id(), EqualsProto(request.scanner_id()));
  EXPECT_EQ(response.result(), OPERATION_RESULT_SUCCESS);
  EXPECT_THAT(response.config().scanner(), Not(EqualsProto(ScannerHandle())));
  EXPECT_EQ(tracker->NumOpenScanners(), 1);
}

TEST(DeviceTrackerTest, OpenScannerSameClientSucceedsTwice) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest request;
  request.mutable_scanner_id()->set_connection_string("Test");
  request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse response1 = tracker->OpenScanner(request);

  // Re-insert the test device because the fake SANE client deletes it after one
  // connection.
  auto scanner2 = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner2));

  OpenScannerResponse response2 = tracker->OpenScanner(request);

  EXPECT_THAT(response1.scanner_id(), EqualsProto(request.scanner_id()));
  EXPECT_EQ(response1.result(), OPERATION_RESULT_SUCCESS);
  EXPECT_THAT(response1.config().scanner(), Not(EqualsProto(ScannerHandle())));

  EXPECT_THAT(response2.scanner_id(), EqualsProto(request.scanner_id()));
  EXPECT_EQ(response2.result(), OPERATION_RESULT_SUCCESS);
  EXPECT_THAT(response2.config().scanner(), Not(EqualsProto(ScannerHandle())));

  EXPECT_THAT(response2.config().scanner(),
              Not(EqualsProto(response1.config().scanner())));
  EXPECT_EQ(tracker->NumOpenScanners(), 1);
}

TEST(DeviceTrackerTest, OpenScannerSecondClientFails) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest request;
  request.mutable_scanner_id()->set_connection_string("Test");
  request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse response1 = tracker->OpenScanner(request);

  // Re-insert the test device because the fake SANE client deletes it after one
  // connection.
  auto scanner2 = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner2));

  request.set_client_id("DeviceTrackerTest2");
  OpenScannerResponse response2 = tracker->OpenScanner(request);

  EXPECT_THAT(response1.scanner_id(), EqualsProto(request.scanner_id()));
  EXPECT_EQ(response1.result(), OPERATION_RESULT_SUCCESS);
  EXPECT_THAT(response1.config().scanner(), Not(EqualsProto(ScannerHandle())));

  EXPECT_THAT(response2.scanner_id(), EqualsProto(request.scanner_id()));
  EXPECT_EQ(response2.result(), OPERATION_RESULT_DEVICE_BUSY);
  EXPECT_THAT(response2.config().scanner(), EqualsProto(ScannerHandle()));

  EXPECT_EQ(tracker->NumOpenScanners(), 1);
}

TEST(DeviceTrackerTest, CloseScannerMissingHandle) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  CloseScannerRequest request;
  CloseScannerResponse response = tracker->CloseScanner(request);

  EXPECT_THAT(request.scanner(), EqualsProto(response.scanner()));
  EXPECT_EQ(response.result(), OPERATION_RESULT_INVALID);
  EXPECT_EQ(tracker->NumOpenScanners(), 0);
}

TEST(DeviceTrackerTest, CloseScannerInvalidHandle) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  CloseScannerRequest request;
  request.mutable_scanner()->set_token("NoSuchScanner");
  CloseScannerResponse response = tracker->CloseScanner(request);

  EXPECT_THAT(request.scanner(), EqualsProto(response.scanner()));
  EXPECT_EQ(response.result(), OPERATION_RESULT_MISSING);
  EXPECT_EQ(tracker->NumOpenScanners(), 0);
}

TEST(DeviceTrackerTest, CloseScannerSuccess) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest request1;
  request1.mutable_scanner_id()->set_connection_string("Test");
  request1.set_client_id("DeviceTrackerTest");
  OpenScannerResponse response1 = tracker->OpenScanner(request1);

  EXPECT_EQ(tracker->NumOpenScanners(), 1);

  CloseScannerRequest request2;
  *request2.mutable_scanner() = response1.config().scanner();
  CloseScannerResponse response2 = tracker->CloseScanner(request2);

  EXPECT_THAT(request2.scanner(), EqualsProto(response2.scanner()));
  EXPECT_EQ(response2.result(), OPERATION_RESULT_SUCCESS);
  EXPECT_EQ(tracker->NumOpenScanners(), 0);
}

TEST(DeviceTrackerTest, CloseScannerTwiceFails) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest request1;
  request1.mutable_scanner_id()->set_connection_string("Test");
  request1.set_client_id("DeviceTrackerTest");
  OpenScannerResponse response1 = tracker->OpenScanner(request1);

  EXPECT_EQ(tracker->NumOpenScanners(), 1);

  CloseScannerRequest request2;
  *request2.mutable_scanner() = response1.config().scanner();
  CloseScannerResponse response2 = tracker->CloseScanner(request2);
  CloseScannerResponse response3 = tracker->CloseScanner(request2);

  EXPECT_THAT(request2.scanner(), EqualsProto(response2.scanner()));
  EXPECT_EQ(response2.result(), OPERATION_RESULT_SUCCESS);
  EXPECT_THAT(request2.scanner(), EqualsProto(response3.scanner()));
  EXPECT_EQ(response3.result(), OPERATION_RESULT_MISSING);
  EXPECT_EQ(tracker->NumOpenScanners(), 0);
}

TEST(DeviceTrackerTest, CloseScannerFreesDevice) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner));

  // First client succeeds.
  OpenScannerRequest open_request;
  open_request.mutable_scanner_id()->set_connection_string("Test");
  open_request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse response1 = tracker->OpenScanner(open_request);
  EXPECT_EQ(tracker->NumOpenScanners(), 1);

  // Re-insert the test device because the fake SANE client deletes it after one
  // connection.
  auto scanner2 = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner2));

  // This will fail because the device is still open.
  open_request.set_client_id("DeviceTrackerTest2");
  OpenScannerResponse response2 = tracker->OpenScanner(open_request);
  EXPECT_EQ(tracker->NumOpenScanners(), 1);

  // Close first client's handle to free up the device.
  CloseScannerRequest close_request;
  *close_request.mutable_scanner() = response1.config().scanner();
  CloseScannerResponse response3 = tracker->CloseScanner(close_request);
  EXPECT_EQ(tracker->NumOpenScanners(), 0);

  // Now the second client can open the device.
  OpenScannerResponse response4 = tracker->OpenScanner(open_request);
  EXPECT_EQ(tracker->NumOpenScanners(), 1);

  EXPECT_THAT(response1.scanner_id(), EqualsProto(open_request.scanner_id()));
  EXPECT_EQ(response1.result(), OPERATION_RESULT_SUCCESS);
  EXPECT_THAT(response1.config().scanner(), Not(EqualsProto(ScannerHandle())));

  EXPECT_THAT(response2.scanner_id(), EqualsProto(open_request.scanner_id()));
  EXPECT_EQ(response2.result(), OPERATION_RESULT_DEVICE_BUSY);
  EXPECT_THAT(response2.config().scanner(), EqualsProto(ScannerHandle()));

  EXPECT_THAT(response3.scanner(), EqualsProto(close_request.scanner()));
  EXPECT_EQ(response3.result(), OPERATION_RESULT_SUCCESS);

  EXPECT_THAT(response4.scanner_id(), EqualsProto(open_request.scanner_id()));
  EXPECT_EQ(response4.result(), OPERATION_RESULT_SUCCESS);
  EXPECT_THAT(response4.config().scanner(), Not(EqualsProto(ScannerHandle())));
  EXPECT_THAT(response4.config().scanner(),
              Not(EqualsProto(response1.config().scanner())));
}

TEST(DeviceTrackerTest, StartPreparedScanMissingHandleFails) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  StartPreparedScanRequest request;
  StartPreparedScanResponse response = tracker->StartPreparedScan(request);
  EXPECT_THAT(response.scanner(), EqualsProto(request.scanner()));
  EXPECT_EQ(response.result(), OPERATION_RESULT_INVALID);
  EXPECT_FALSE(response.has_job_handle());
}

TEST(DeviceTrackerTest, StartPreparedScanEmptyHandleFails) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  StartPreparedScanRequest request;
  request.mutable_scanner()->set_token("");
  StartPreparedScanResponse response = tracker->StartPreparedScan(request);
  EXPECT_THAT(response.scanner(), EqualsProto(request.scanner()));
  EXPECT_EQ(response.result(), OPERATION_RESULT_INVALID);
  EXPECT_FALSE(response.has_job_handle());
}

TEST(DeviceTrackerTest, StartPreparedScanInvalidHandleFails) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  StartPreparedScanRequest request;
  request.mutable_scanner()->set_token("NoSuchScanner");
  StartPreparedScanResponse response = tracker->StartPreparedScan(request);
  EXPECT_THAT(response.scanner(), EqualsProto(request.scanner()));
  EXPECT_EQ(response.result(), OPERATION_RESULT_MISSING);
  EXPECT_FALSE(response.has_job_handle());
}

TEST(DeviceTrackerTest, StartPreparedScanMissingImageFormatFails) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest open_request;
  open_request.mutable_scanner_id()->set_connection_string("Test");
  open_request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse open_response = tracker->OpenScanner(open_request);
  ASSERT_EQ(open_response.result(), OPERATION_RESULT_SUCCESS);

  StartPreparedScanRequest sps_request;
  *sps_request.mutable_scanner() = open_response.config().scanner();
  StartPreparedScanResponse sps_response =
      tracker->StartPreparedScan(sps_request);
  EXPECT_THAT(sps_response.scanner(), EqualsProto(sps_request.scanner()));
  EXPECT_EQ(sps_response.result(), OPERATION_RESULT_INVALID);
  EXPECT_FALSE(sps_response.has_job_handle());
}

TEST(DeviceTrackerTest, StartPreparedScanDeviceStartFails) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  scanner->SetStartScanResult(SANE_STATUS_JAMMED);
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest open_request;
  open_request.mutable_scanner_id()->set_connection_string("Test");
  open_request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse open_response = tracker->OpenScanner(open_request);
  ASSERT_EQ(open_response.result(), OPERATION_RESULT_SUCCESS);

  StartPreparedScanRequest sps_request;
  *sps_request.mutable_scanner() = open_response.config().scanner();
  sps_request.set_image_format("image/jpeg");
  StartPreparedScanResponse sps_response =
      tracker->StartPreparedScan(sps_request);
  EXPECT_THAT(sps_response.scanner(), EqualsProto(sps_request.scanner()));
  EXPECT_EQ(sps_response.result(), OPERATION_RESULT_ADF_JAMMED);
  EXPECT_FALSE(sps_response.has_job_handle());
}

TEST(DeviceTrackerTest, StartPreparedScanDeviceMissingJob) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  scanner->SetCallStartJob(false);
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest open_request;
  open_request.mutable_scanner_id()->set_connection_string("Test");
  open_request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse open_response = tracker->OpenScanner(open_request);
  ASSERT_EQ(open_response.result(), OPERATION_RESULT_SUCCESS);

  StartPreparedScanRequest sps_request;
  *sps_request.mutable_scanner() = open_response.config().scanner();
  sps_request.set_image_format("image/jpeg");
  StartPreparedScanResponse sps_response =
      tracker->StartPreparedScan(sps_request);
  EXPECT_THAT(sps_response.scanner(), EqualsProto(sps_request.scanner()));
  EXPECT_EQ(sps_response.result(), OPERATION_RESULT_INTERNAL_ERROR);
  EXPECT_FALSE(sps_response.has_job_handle());
}

TEST(DeviceTrackerTest, StartPreparedScanCreatesJob) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest open_request;
  open_request.mutable_scanner_id()->set_connection_string("Test");
  open_request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse open_response = tracker->OpenScanner(open_request);
  ASSERT_EQ(open_response.result(), OPERATION_RESULT_SUCCESS);

  StartPreparedScanRequest sps_request;
  *sps_request.mutable_scanner() = open_response.config().scanner();
  sps_request.set_image_format("image/jpeg");
  StartPreparedScanResponse sps_response =
      tracker->StartPreparedScan(sps_request);
  EXPECT_THAT(sps_response.scanner(), EqualsProto(sps_request.scanner()));
  EXPECT_EQ(sps_response.result(), OPERATION_RESULT_SUCCESS);
  EXPECT_TRUE(sps_response.has_job_handle());
}

// CancelScan with a scan_uuid is supposed to be handled by Manager::CancelScan,
// not DeviceTracker::CancelScan.
TEST(DeviceTrackerTest, CancelScanByUuidIsBlocked) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  CancelScanRequest request;
  request.set_scan_uuid("12345");
  ASSERT_DEATH(tracker->CancelScan(std::move(request)), "Manager::CancelScan");
}

TEST(DeviceTrackerTest, CancelScanRequiresJobHandle) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  CancelScanRequest request;
  request.mutable_job_handle()->set_token("");
  CancelScanResponse response = tracker->CancelScan(request);
  EXPECT_FALSE(response.success());
  EXPECT_NE(response.failure_reason(), "");
  EXPECT_EQ(response.result(), OPERATION_RESULT_INVALID);
  EXPECT_THAT(response.job_handle(), EqualsProto(request.job_handle()));
}

TEST(DeviceTrackerTest, CancelScanInvalidJobHandle) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  CancelScanRequest request;
  request.mutable_job_handle()->set_token("bad_handle");
  request.set_scan_uuid("bad_uuid");
  CancelScanResponse response = tracker->CancelScan(request);
  EXPECT_FALSE(response.success());
  EXPECT_THAT(response.failure_reason(), HasSubstr("bad_handle"));
  EXPECT_THAT(response.failure_reason(), Not(HasSubstr("bad_uuid")));
  EXPECT_EQ(response.result(), OPERATION_RESULT_INVALID);
  EXPECT_THAT(response.job_handle(), EqualsProto(request.job_handle()));
}

TEST(DeviceTrackerTest, CancelScanClosedScanner) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest open_request;
  open_request.mutable_scanner_id()->set_connection_string("Test");
  open_request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse open_response = tracker->OpenScanner(open_request);
  ASSERT_EQ(open_response.result(), OPERATION_RESULT_SUCCESS);

  StartPreparedScanRequest sps_request;
  *sps_request.mutable_scanner() = open_response.config().scanner();
  sps_request.set_image_format("image/jpeg");
  StartPreparedScanResponse sps_response =
      tracker->StartPreparedScan(sps_request);
  ASSERT_EQ(sps_response.result(), OPERATION_RESULT_SUCCESS);

  // Close device, leaving a dangling job handle.
  CloseScannerRequest close_request;
  *close_request.mutable_scanner() = open_response.config().scanner();
  CloseScannerResponse close_response = tracker->CloseScanner(close_request);
  ASSERT_EQ(close_response.result(), OPERATION_RESULT_SUCCESS);

  CancelScanRequest cancel_request;
  *cancel_request.mutable_job_handle() = sps_response.job_handle();
  CancelScanResponse cancel_response = tracker->CancelScan(cancel_request);
  EXPECT_FALSE(cancel_response.success());
  EXPECT_NE(cancel_response.failure_reason(), "");
  EXPECT_EQ(cancel_response.result(), OPERATION_RESULT_MISSING);
  EXPECT_THAT(cancel_response.job_handle(),
              EqualsProto(cancel_request.job_handle()));

  // Job handle itself is no longer valid.
  cancel_response = tracker->CancelScan(cancel_request);
  EXPECT_FALSE(cancel_response.success());
  EXPECT_NE(cancel_response.failure_reason(), "");
  EXPECT_EQ(cancel_response.result(), OPERATION_RESULT_INVALID);
  EXPECT_THAT(cancel_response.job_handle(),
              EqualsProto(cancel_request.job_handle()));
}

TEST(DeviceTrackerTest, CancelScanNotCurrentJob) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  SaneDeviceFake* raw_scanner = scanner.get();
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest open_request;
  open_request.mutable_scanner_id()->set_connection_string("Test");
  open_request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse open_response = tracker->OpenScanner(open_request);
  ASSERT_EQ(open_response.result(), OPERATION_RESULT_SUCCESS);

  StartPreparedScanRequest sps_request1;
  *sps_request1.mutable_scanner() = open_response.config().scanner();
  sps_request1.set_image_format("image/jpeg");
  StartPreparedScanResponse sps_response1 =
      tracker->StartPreparedScan(sps_request1);
  ASSERT_EQ(sps_response1.result(), OPERATION_RESULT_SUCCESS);

  // Simulate finishing the first job by clearing it out.
  raw_scanner->ClearScanJob();

  StartPreparedScanRequest sps_request2;
  *sps_request2.mutable_scanner() = open_response.config().scanner();
  sps_request2.set_image_format("image/jpeg");
  StartPreparedScanResponse sps_response2 =
      tracker->StartPreparedScan(sps_request2);
  ASSERT_EQ(sps_response2.result(), OPERATION_RESULT_SUCCESS);

  // Cancelling original job should fail.
  CancelScanRequest cancel_request;
  *cancel_request.mutable_job_handle() = sps_response1.job_handle();
  CancelScanResponse cancel_response = tracker->CancelScan(cancel_request);
  EXPECT_FALSE(cancel_response.success());
  EXPECT_NE(cancel_response.failure_reason(), "");
  EXPECT_EQ(cancel_response.result(), OPERATION_RESULT_CANCELLED);
  EXPECT_THAT(cancel_response.job_handle(),
              EqualsProto(cancel_request.job_handle()));

  // Cancelling second/current job should still succeed.
  *cancel_request.mutable_job_handle() = sps_response2.job_handle();
  cancel_response = tracker->CancelScan(cancel_request);
  EXPECT_TRUE(cancel_response.success());
  EXPECT_EQ(cancel_response.failure_reason(), "");
  EXPECT_EQ(cancel_response.result(), OPERATION_RESULT_SUCCESS);
  EXPECT_THAT(cancel_response.job_handle(),
              EqualsProto(cancel_request.job_handle()));
}

TEST(DeviceTrackerTest, CancelScanDeviceCancelFails) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  scanner->SetCancelScanResult(false);
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest open_request;
  open_request.mutable_scanner_id()->set_connection_string("Test");
  open_request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse open_response = tracker->OpenScanner(open_request);
  ASSERT_EQ(open_response.result(), OPERATION_RESULT_SUCCESS);

  StartPreparedScanRequest sps_request;
  *sps_request.mutable_scanner() = open_response.config().scanner();
  sps_request.set_image_format("image/jpeg");
  StartPreparedScanResponse sps_response =
      tracker->StartPreparedScan(sps_request);
  ASSERT_EQ(sps_response.result(), OPERATION_RESULT_SUCCESS);

  CancelScanRequest cancel_request;
  *cancel_request.mutable_job_handle() = sps_response.job_handle();
  CancelScanResponse cancel_response = tracker->CancelScan(cancel_request);
  EXPECT_FALSE(cancel_response.success());
  EXPECT_NE(cancel_response.failure_reason(), "");
  EXPECT_EQ(cancel_response.result(), OPERATION_RESULT_INTERNAL_ERROR);
  EXPECT_THAT(cancel_response.job_handle(),
              EqualsProto(cancel_request.job_handle()));
}

TEST(DeviceTrackerTest, CancelScanNoErrors) {
  auto sane_client = std::make_unique<SaneClientFake>();
  auto libusb = std::make_unique<LibusbWrapperFake>();
  auto tracker =
      std::make_unique<DeviceTracker>(sane_client.get(), libusb.get());

  auto scanner = std::make_unique<SaneDeviceFake>();
  sane_client->SetDeviceForName("Test", std::move(scanner));

  OpenScannerRequest open_request;
  open_request.mutable_scanner_id()->set_connection_string("Test");
  open_request.set_client_id("DeviceTrackerTest");
  OpenScannerResponse open_response = tracker->OpenScanner(open_request);
  ASSERT_EQ(open_response.result(), OPERATION_RESULT_SUCCESS);

  StartPreparedScanRequest sps_request;
  *sps_request.mutable_scanner() = open_response.config().scanner();
  sps_request.set_image_format("image/jpeg");
  StartPreparedScanResponse sps_response =
      tracker->StartPreparedScan(sps_request);
  ASSERT_EQ(sps_response.result(), OPERATION_RESULT_SUCCESS);

  CancelScanRequest cancel_request;
  *cancel_request.mutable_job_handle() = sps_response.job_handle();
  CancelScanResponse cancel_response = tracker->CancelScan(cancel_request);
  EXPECT_TRUE(cancel_response.success());
  EXPECT_EQ(cancel_response.failure_reason(), "");
  EXPECT_EQ(cancel_response.result(), OPERATION_RESULT_SUCCESS);
  EXPECT_THAT(cancel_response.job_handle(),
              EqualsProto(cancel_request.job_handle()));

  // Job handle is no longer valid.
  cancel_response = tracker->CancelScan(cancel_request);
  EXPECT_FALSE(cancel_response.success());
  EXPECT_NE(cancel_response.failure_reason(), "");
  EXPECT_EQ(cancel_response.result(), OPERATION_RESULT_INVALID);
  EXPECT_THAT(cancel_response.job_handle(),
              EqualsProto(cancel_request.job_handle()));
}

}  // namespace
}  // namespace lorgnette
