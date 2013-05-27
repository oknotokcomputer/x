// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mist/usb_constants.h"

#include <libusb.h>

#include <gtest/gtest.h>

namespace mist {

TEST(UsbConstantsTest, UsbDirection) {
  EXPECT_EQ(kUsbDirectionIn, static_cast<UsbDirection>(LIBUSB_ENDPOINT_IN));
  EXPECT_EQ(kUsbDirectionOut, static_cast<UsbDirection>(LIBUSB_ENDPOINT_OUT));
}

TEST(UsbConstantsTest, UsbSpeed) {
  EXPECT_EQ(kUsbSpeedUnknown, static_cast<UsbSpeed>(LIBUSB_SPEED_UNKNOWN));
  EXPECT_EQ(kUsbSpeedLow, static_cast<UsbSpeed>(LIBUSB_SPEED_LOW));
  EXPECT_EQ(kUsbSpeedFull, static_cast<UsbSpeed>(LIBUSB_SPEED_FULL));
  EXPECT_EQ(kUsbSpeedHigh, static_cast<UsbSpeed>(LIBUSB_SPEED_HIGH));
  EXPECT_EQ(kUsbSpeedSuper, static_cast<UsbSpeed>(LIBUSB_SPEED_SUPER));
}

TEST(UsbConstantsTest, UsbTransferType) {
  EXPECT_EQ(kUsbTransferTypeControl,
            static_cast<UsbTransferType>(LIBUSB_TRANSFER_TYPE_CONTROL));
  EXPECT_EQ(kUsbTransferTypeIsochronous,
            static_cast<UsbTransferType>(LIBUSB_TRANSFER_TYPE_ISOCHRONOUS));
  EXPECT_EQ(kUsbTransferTypeBulk,
            static_cast<UsbTransferType>(LIBUSB_TRANSFER_TYPE_BULK));
  EXPECT_EQ(kUsbTransferTypeInterrupt,
            static_cast<UsbTransferType>(LIBUSB_TRANSFER_TYPE_INTERRUPT));
}

TEST(UsbConstantsTest, UsbTransferStatus) {
  EXPECT_EQ(kUsbTransferStatusCompleted,
            static_cast<UsbTransferStatus>(LIBUSB_TRANSFER_COMPLETED));
  EXPECT_EQ(kUsbTransferStatusError,
            static_cast<UsbTransferStatus>(LIBUSB_TRANSFER_ERROR));
  EXPECT_EQ(kUsbTransferStatusTimedOut,
            static_cast<UsbTransferStatus>(LIBUSB_TRANSFER_TIMED_OUT));
  EXPECT_EQ(kUsbTransferStatusCancelled,
            static_cast<UsbTransferStatus>(LIBUSB_TRANSFER_CANCELLED));
  EXPECT_EQ(kUsbTransferStatusStall,
            static_cast<UsbTransferStatus>(LIBUSB_TRANSFER_STALL));
  EXPECT_EQ(kUsbTransferStatusNoDevice,
            static_cast<UsbTransferStatus>(LIBUSB_TRANSFER_NO_DEVICE));
  EXPECT_EQ(kUsbTransferStatusOverflow,
            static_cast<UsbTransferStatus>(LIBUSB_TRANSFER_OVERFLOW));
}

}  // namespace mist
