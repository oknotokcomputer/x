// Copyright (c) 2013 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "mist/usb_error.h"

#include <gtest/gtest.h>

namespace mist {

TEST(UsbErrorTest, DefaultConstructor) {
  UsbError error;
  EXPECT_TRUE(error.IsSuccess());
  EXPECT_EQ(UsbError::kSuccess, error.type());
}

TEST(UsbErrorTest, ConstructorWithType) {
  UsbError error(UsbError::kErrorInvalidParameter);
  EXPECT_FALSE(error.IsSuccess());
  EXPECT_EQ(UsbError::kErrorInvalidParameter, error.type());
}

TEST(UsbErrorTest, ConstructorWithLibUsbError) {
  UsbError error(LIBUSB_ERROR_INVALID_PARAM);
  EXPECT_FALSE(error.IsSuccess());
  EXPECT_EQ(UsbError::kErrorInvalidParameter, error.type());
}

TEST(UsbErrorTest, IsSuccess) {
  UsbError error;
  EXPECT_TRUE(error.IsSuccess());

  error.set_type(UsbError::kErrorIO);
  EXPECT_FALSE(error.IsSuccess());

  error.set_type(UsbError::kSuccess);
  EXPECT_TRUE(error.IsSuccess());
}

TEST(UsbErrorTest, Clear) {
  UsbError error(UsbError::kErrorIO);
  EXPECT_EQ(UsbError::kErrorIO, error.type());
  EXPECT_FALSE(error.IsSuccess());

  error.Clear();
  EXPECT_EQ(UsbError::kSuccess, error.type());
  EXPECT_TRUE(error.IsSuccess());
}

TEST(UsbError, SetFromLibUsbError) {
  UsbError error;

  error.SetFromLibUsbError(LIBUSB_SUCCESS);
  EXPECT_EQ(UsbError::kSuccess, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_IO);
  EXPECT_EQ(UsbError::kErrorIO, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_INVALID_PARAM);
  EXPECT_EQ(UsbError::kErrorInvalidParameter, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_ACCESS);
  EXPECT_EQ(UsbError::kErrorAccess, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_NO_DEVICE);
  EXPECT_EQ(UsbError::kErrorNoDevice, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_NOT_FOUND);
  EXPECT_EQ(UsbError::kErrorNotFound, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_BUSY);
  EXPECT_EQ(UsbError::kErrorBusy, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_TIMEOUT);
  EXPECT_EQ(UsbError::kErrorTimeout, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_OVERFLOW);
  EXPECT_EQ(UsbError::kErrorOverflow, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_PIPE);
  EXPECT_EQ(UsbError::kErrorPipe, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_INTERRUPTED);
  EXPECT_EQ(UsbError::kErrorInterrupted, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_NO_MEM);
  EXPECT_EQ(UsbError::kErrorNoMemory, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_NOT_SUPPORTED);
  EXPECT_EQ(UsbError::kErrorNotSupported, error.type());

  error.SetFromLibUsbError(LIBUSB_ERROR_OTHER);
  EXPECT_EQ(UsbError::kErrorOther, error.type());
}

}  // namespace mist
