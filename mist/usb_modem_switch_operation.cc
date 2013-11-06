// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mist/usb_modem_switch_operation.h"

#include <vector>

#include <base/bind.h>
#include <base/string_number_conversions.h>
#include <base/string_util.h>
#include <base/stringprintf.h>

#include "mist/context.h"
#include "mist/event_dispatcher.h"
#include "mist/proto_bindings/usb_modem_info.pb.h"
#include "mist/usb_bulk_transfer.h"
#include "mist/usb_config_descriptor.h"
#include "mist/usb_constants.h"
#include "mist/usb_device.h"
#include "mist/usb_device_descriptor.h"
#include "mist/usb_device_event_notifier.h"
#include "mist/usb_endpoint_descriptor.h"
#include "mist/usb_interface.h"
#include "mist/usb_interface_descriptor.h"
#include "mist/usb_manager.h"
#include "mist/usb_modem_switch_context.h"

using base::Bind;
using base::CancelableClosure;
using base::StringPrintf;
using base::Unretained;
using std::string;
using std::vector;

namespace mist {

namespace {

const int kDefaultUsbInterfaceIndex = 0;
const int kDefaultUsbInterfaceAlternateSettingIndex = 0;

// Expected response length observed in experiments.
const int kExpectedResponseLength = 13;

// TODO(benchan): To be conservative, use large timeout values for now. Add UMA
// metrics to determine appropriate timeout values.
const int64 kReconnectTimeoutMilliseconds = 15000;
const int64 kUsbMessageTransferTimeoutMilliseconds = 8000;

}  // namespace

// TODO(benchan): Add unit tests for UsbModemSwitchOperation.

UsbModemSwitchOperation::UsbModemSwitchOperation(
    Context* context,
    UsbModemSwitchContext* switch_context)
    : context_(context),
      switch_context_(switch_context),
      interface_claimed_(false),
      interface_number_(0),
      in_endpoint_address_(0),
      out_endpoint_address_(0),
      message_index_(0),
      num_usb_messages_(0) {
  CHECK(context_);
  CHECK(switch_context_);
  CHECK(!switch_context_->sys_path().empty());
  CHECK(switch_context_->modem_info());
}

UsbModemSwitchOperation::~UsbModemSwitchOperation() {
  Cancel();
  CloseDevice();

  // If the USB bulk transfer is being cancelled, the UsbBulkTransfer object
  // held by |bulk_transfer_| still need to survive until libusb notifies the
  // cancellation of the underlying transfer via a callback as we have no way to
  // cancel that calback. This should only happen when mist is about to
  // terminate while the transfer is being cancelled. To avoid deferring the
  // termination of mist, we intentionally leak the UsbBulkTransfer object held
  // by |bulk_transfer_| and hope that either the callback is invoked (with an
  // invalidated weak pointer to this object) before mist terminates or is
  // discarded after mist terminates.
  if (bulk_transfer_ && bulk_transfer_->state() == UsbTransfer::kCancelling)
    ignore_result(bulk_transfer_.release());
}

void UsbModemSwitchOperation::Start(
    const CompletionCallback& completion_callback) {
  CHECK(!completion_callback.is_null());

  completion_callback_ = completion_callback;
  VLOG(1) << "Start modem switch operation for device '"
          << switch_context_->sys_path() << "'.";

  // Defer the execution of the first task as multiple UsbModemSwitchOperation
  // objects may be created and started in a tight loop.
  ScheduleTask(
      &UsbModemSwitchOperation::OpenDeviceAndClaimMassStorageInterface);
}

void UsbModemSwitchOperation::Cancel() {
  pending_task_.Cancel();
  reconnect_timeout_callback_.Cancel();
  context_->usb_device_event_notifier()->RemoveObserver(this);

  if (bulk_transfer_)
    bulk_transfer_->Cancel();
}

void UsbModemSwitchOperation::ScheduleTask(Task task) {
  pending_task_.Reset(Bind(task, Unretained(this)));
  context_->event_dispatcher()->PostTask(pending_task_.callback());
}

void UsbModemSwitchOperation::Complete(bool success) {
  CHECK(!completion_callback_.is_null());

  if (!success) {
    LOG(ERROR) << "Could not switch device '" << switch_context_->sys_path()
               << "' into the modem mode.";
  }

  pending_task_.Cancel();
  reconnect_timeout_callback_.Cancel();
  context_->usb_device_event_notifier()->RemoveObserver(this);

  // Defer the execution of the completion callback for two reasons:
  // 1. To prevent a task in this switch operation from occupying the message
  //    loop for too long as Complete() can be called from one of the tasks.
  // 2. The completion callback may delete this object, so this object should
  //    not be accessed after this method returns.
  context_->event_dispatcher()->PostTask(
      Bind(completion_callback_, Unretained(this), success));
}

void UsbModemSwitchOperation::CloseDevice() {
  if (!device_)
    return;

  if (interface_claimed_) {
    if (!device_->ReleaseInterface(interface_number_) &&
        // UsbDevice::ReleaseInterface may return UsbError::kErrorNoDevice
        // as the original device may no longer exist after switching to the
        // modem mode. Do not report such an error.
        device_->error().type() != UsbError::kErrorNoDevice) {
      LOG(ERROR) << StringPrintf("Could not release interface %u: %s",
                                 interface_number_,
                                 device_->error().ToString());
    }
    interface_claimed_ = false;
  }

  device_.reset();
}

void UsbModemSwitchOperation::OpenDeviceAndClaimMassStorageInterface() {
  CHECK(!interface_claimed_);

  device_.reset(
      context_->usb_manager()->GetDevice(switch_context_->bus_number(),
                                         switch_context_->device_address(),
                                         switch_context_->vendor_id(),
                                         switch_context_->product_id()));
  if (!device_) {
    LOG(ERROR) << StringPrintf("Could not find USB device '%s' "
                               "(Bus %03d Address %03d ID %04x:%04x).",
                               switch_context_->sys_path().c_str(),
                               switch_context_->bus_number(),
                               switch_context_->device_address(),
                               switch_context_->vendor_id(),
                               switch_context_->product_id());
    Complete(false);
    return;
  }

  if (!device_->Open()) {
    LOG(ERROR) << "Could not open device '" << switch_context_->sys_path()
               << "'.";
    Complete(false);
    return;
  }

  scoped_ptr<UsbConfigDescriptor> config_descriptor(
      device_->GetActiveConfigDescriptor());
  if (!config_descriptor) {
    LOG(ERROR) << "Could not get active configuration descriptor: "
               << device_->error();
    Complete(false);
    return;
  }
  VLOG(2) << *config_descriptor;

  scoped_ptr<UsbInterface> interface(
      config_descriptor->GetInterface(kDefaultUsbInterfaceIndex));
  if (!interface) {
    LOG(ERROR) << "Could not get interface 0.";
    Complete(false);
    return;
  }

  scoped_ptr<UsbInterfaceDescriptor> interface_descriptor(
      interface->GetAlternateSetting(
          kDefaultUsbInterfaceAlternateSettingIndex));
  if (!interface_descriptor) {
    LOG(ERROR) << "Could not get interface alternate setting 0.";
    Complete(false);
    return;
  }
  VLOG(2) << *interface_descriptor;

  if (interface_descriptor->GetInterfaceClass() != kUsbClassMassStorage) {
    LOG(ERROR) << "Device is not currently in mass storage mode.";
    Complete(false);
    return;
  }

  scoped_ptr<UsbEndpointDescriptor> out_endpoint_descriptor(
      interface_descriptor->GetEndpointDescriptorByTransferTypeAndDirection(
          kUsbTransferTypeBulk, kUsbDirectionOut));
  if (!out_endpoint_descriptor) {
    LOG(ERROR) << "Could not find an output bulk endpoint.";
    Complete(false);
    return;
  }
  VLOG(2) << "Bulk output endpoint: " << *out_endpoint_descriptor;

  interface_number_ = interface_descriptor->GetInterfaceNumber();
  out_endpoint_address_ = out_endpoint_descriptor->GetEndpointAddress();
  bool expect_response = switch_context_->modem_info()->expect_response();

  if (expect_response) {
    scoped_ptr<UsbEndpointDescriptor> in_endpoint_descriptor(
        interface_descriptor->GetEndpointDescriptorByTransferTypeAndDirection(
            kUsbTransferTypeBulk, kUsbDirectionIn));
    if (!in_endpoint_descriptor) {
      LOG(ERROR) << "Could not find an input bulk endpoint.";
      Complete(false);
      return;
    }
    VLOG(2) << "Bulk input endpoint: " << *in_endpoint_descriptor;
    in_endpoint_address_ = in_endpoint_descriptor->GetEndpointAddress();
  }

  if (!device_->DetachKernelDriver(interface_number_) &&
      // UsbDevice::DetachKernelDriver returns UsbError::kErrorNotFound when
      // there is no driver attached to the device.
      device_->error().type() != UsbError::kErrorNotFound) {
    LOG(ERROR) << StringPrintf(
        "Could not detach kernel driver from interface %u: %s",
        interface_number_, device_->error().ToString());
    Complete(false);
    return;
  }

  if (!device_->ClaimInterface(interface_number_)) {
    LOG(ERROR) << StringPrintf("Could not claim interface %u: %s",
                               interface_number_, device_->error().ToString());
    Complete(false);
    return;
  }

  interface_claimed_ = true;
  message_index_ = 0;
  num_usb_messages_ = switch_context_->modem_info()->usb_message_size();
  // TODO(benchan): Remove this check when we support some modem that does not
  // require a special USB message for the switch operation.
  CHECK_GT(num_usb_messages_, 0);

  context_->usb_device_event_notifier()->AddObserver(this);

  ScheduleTask(&UsbModemSwitchOperation::SendMessageToMassStorageEndpoint);
}

bool UsbModemSwitchOperation::ClearHalt(uint8 endpoint_address) {
  if (device_->ClearHalt(endpoint_address))
    return true;

  LOG(ERROR) << StringPrintf(
      "Could not clear halt condition for endpoint %u: %s",
      endpoint_address, device_->error().ToString());
  return false;
}

void UsbModemSwitchOperation::SendMessageToMassStorageEndpoint() {
  CHECK_LT(message_index_, num_usb_messages_);

  const string& usb_message =
      switch_context_->modem_info()->usb_message(message_index_);
  vector<uint8> bytes;
  if (!base::HexStringToBytes(usb_message, &bytes)) {
    LOG(ERROR) << StringPrintf("Invalid USB message (%d/%d): %s",
                               message_index_,
                               num_usb_messages_,
                               usb_message.c_str());
    Complete(false);
    return;
  }

  VLOG(1) << StringPrintf("Prepare to send USB message (%d/%d): %s",
                          message_index_ + 1,
                          num_usb_messages_,
                          usb_message.c_str());

  InitiateUsbBulkTransfer(out_endpoint_address_,
                          &bytes[0],
                          bytes.size(),
                          &UsbModemSwitchOperation::OnSendMessageCompleted);
}

void UsbModemSwitchOperation::ReceiveMessageFromMassStorageEndpoint() {
  CHECK_LT(message_index_, num_usb_messages_);

  VLOG(1) << StringPrintf("Prepare to receive USB message (%d/%d)",
                          message_index_ + 1,
                          num_usb_messages_);

  InitiateUsbBulkTransfer(in_endpoint_address_,
                          NULL,
                          kExpectedResponseLength,
                          &UsbModemSwitchOperation::OnReceiveMessageCompleted);
}

void UsbModemSwitchOperation::InitiateUsbBulkTransfer(
    uint8 endpoint_address,
    const uint8* data,
    int length,
    UsbTransferCompletionHandler completion_handler) {
  CHECK_GT(length, 0);

  scoped_ptr<UsbBulkTransfer> bulk_transfer(new UsbBulkTransfer());
  if (!bulk_transfer->Initialize(*device_,
                                 endpoint_address,
                                 length,
                                 kUsbMessageTransferTimeoutMilliseconds)) {
    LOG(ERROR) << "Could not create USB bulk transfer: "
               << bulk_transfer->error();
    Complete(false);
    return;
  }

  if (GetUsbDirectionOfEndpointAddress(endpoint_address) == kUsbDirectionOut) {
    CHECK(data);
    memcpy(bulk_transfer->buffer(), data, length);
  }
  // For a device-to-host transfer, |data| is not used and thus ignored.

  // Pass a weak pointer of this operation object to the completion callback
  // of the USB bulk transfer. This avoids the need to defer the destruction
  // of this object in order to wait for the completion callback of the
  // transfer when the transfer is cancelled by this object.
  if (!bulk_transfer->Submit(Bind(completion_handler, AsWeakPtr()))) {
    LOG(ERROR) << "Could not submit USB bulk transfer: "
               << bulk_transfer->error();
    Complete(false);
    return;
  }

  bulk_transfer_ = bulk_transfer.Pass();
}

void UsbModemSwitchOperation::OnSendMessageCompleted(UsbTransfer* transfer) {
  VLOG(1) << "USB bulk output transfer completed: " << *transfer;

  CHECK_EQ(bulk_transfer_.get(), transfer);
  CHECK_EQ(out_endpoint_address_, transfer->GetEndpointAddress());

  // Keep the bulk transfer valid until this method goes out of scope.
  scoped_ptr<UsbBulkTransfer> scoped_bulk_transfer = bulk_transfer_.Pass();

  if (transfer->GetStatus() == kUsbTransferStatusStall) {
    if (!ClearHalt(transfer->GetEndpointAddress())) {
      Complete(false);
      return;
    }

    ScheduleTask(&UsbModemSwitchOperation::SendMessageToMassStorageEndpoint);
    return;
  }

  if (!transfer->IsCompletedWithExpectedLength(transfer->GetLength())) {
    LOG(ERROR) << StringPrintf(
                      "Could not successfully send USB message (%d/%d).",
                      message_index_ + 1,
                      num_usb_messages_);
    Complete(false);
    return;
  }

  LOG(INFO) << StringPrintf("Successfully sent USB message (%d/%d).",
                            message_index_ + 1,
                            num_usb_messages_);

  if (switch_context_->modem_info()->expect_response()) {
    ScheduleTask(
        &UsbModemSwitchOperation::ReceiveMessageFromMassStorageEndpoint);
    return;
  }

  ScheduleNextMessageToMassStorageEndpoint();
}

void UsbModemSwitchOperation::OnReceiveMessageCompleted(UsbTransfer* transfer) {
  VLOG(1) << "USB bulk input transfer completed: " << *transfer;

  CHECK_EQ(bulk_transfer_.get(), transfer);
  CHECK_EQ(in_endpoint_address_, transfer->GetEndpointAddress());

  // Keep the bulk transfer valid until this method goes out of scope.
  scoped_ptr<UsbBulkTransfer> scoped_bulk_transfer = bulk_transfer_.Pass();

  if (transfer->GetStatus() == kUsbTransferStatusStall) {
    if (!ClearHalt(transfer->GetEndpointAddress())) {
      Complete(false);
      return;
    }

    ScheduleTask(
        &UsbModemSwitchOperation::ReceiveMessageFromMassStorageEndpoint);
    return;
  }

  if (!transfer->IsCompletedWithExpectedLength(kExpectedResponseLength)) {
    LOG(ERROR) << StringPrintf(
                      "Could not successfully receive USB message (%d/%d).",
                      message_index_ + 1,
                      num_usb_messages_);
    Complete(false);
    return;
  }

  LOG(INFO) << StringPrintf("Successfully received USB message (%d/%d).",
                            message_index_ + 1,
                            num_usb_messages_);

  ScheduleNextMessageToMassStorageEndpoint();
}

void UsbModemSwitchOperation::ScheduleNextMessageToMassStorageEndpoint() {
  ++message_index_;
  if (message_index_ < num_usb_messages_) {
    ScheduleTask(&UsbModemSwitchOperation::SendMessageToMassStorageEndpoint);
    return;
  }

  // Be a bit cautious, clear any halt condition on the bulk endpoints, but
  // ignore any error as the device may already disconnect from USB.
  ClearHalt(out_endpoint_address_);
  if (switch_context_->modem_info()->expect_response())
    ClearHalt(in_endpoint_address_);

  // After sending the last message (and receiving its response, if expected),
  // wait for the device to reconnect.
  pending_task_.Cancel();
  reconnect_timeout_callback_.Reset(
      Bind(&UsbModemSwitchOperation::OnReconnectTimeout, Unretained(this)));
  context_->event_dispatcher()->PostDelayedTask(
      reconnect_timeout_callback_.callback(),
      base::TimeDelta::FromMilliseconds(kReconnectTimeoutMilliseconds));
}

void UsbModemSwitchOperation::OnReconnectTimeout() {
  LOG(ERROR) << "Timed out waiting for the device to reconnect.";
  Complete(false);
}

void UsbModemSwitchOperation::OnUsbDeviceAdded(const string& sys_path,
                                               uint8 bus_number,
                                               uint8 device_address,
                                               uint16 vendor_id,
                                               uint16 product_id) {
  if (sys_path != switch_context_->sys_path())
    return;

  const UsbModemInfo* modem_info = switch_context_->modem_info();
  if (modem_info->final_usb_id_size() == 0) {
    VLOG(1) << "No final USB identifiers are specified. Assuming device '"
            << switch_context_->sys_path()
            << "' has been switched to the modem mode.";
    Complete(true);
    return;
  }

  for (int i = 0; i < modem_info->final_usb_id_size(); ++i) {
    const UsbId& final_usb_id = modem_info->final_usb_id(i);
    if (vendor_id == final_usb_id.vendor_id() &&
        product_id == final_usb_id.product_id()) {
      const UsbId& initial_usb_id = modem_info->initial_usb_id();
      LOG(INFO) << StringPrintf(
          "Successfully switched device '%s' from %04x:%04x to %04x:%04x.",
          switch_context_->sys_path().c_str(),
          initial_usb_id.vendor_id(),
          initial_usb_id.product_id(),
          final_usb_id.vendor_id(),
          final_usb_id.product_id());
      Complete(true);
      return;
    }
  }
}

void UsbModemSwitchOperation::OnUsbDeviceRemoved(const string& sys_path) {
  if (sys_path == switch_context_->sys_path()) {
    VLOG(1) << "Device '" << switch_context_->sys_path()
            << "' has been removed and is switching to the modem mode.";
    // TODO(benchan): Investigate if the device will always be removed from
    // the bus before it reconnects. If so, add a check.
  }
}

}  // namespace mist
