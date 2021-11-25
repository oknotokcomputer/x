// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

/*
 * I2C device handler.
 */
#include "hps/hal/i2c.h"

#include <utility>
#include <vector>

#include <fcntl.h>
#include <stdint.h>

#include <linux/i2c.h>
#include <linux/i2c-dev.h>
#include <sys/ioctl.h>

#include <base/check.h>
#include <base/check_op.h>
#include <base/logging.h>
#include <base/numerics/safe_conversions.h>

namespace hps {

I2CDev::I2CDev(const std::string& bus, uint8_t addr)
    : bus_(bus), address_(addr), fd_(-1) {}

int I2CDev::Open() {
  if (this->bus_.empty()) {
    LOG(ERROR) << "Empty i2c path: \"" << this->bus_ << "\"";
    return -1;
  }
  this->fd_ = open(this->bus_.c_str(), O_RDWR);
  if (this->fd_ < 0) {
    PLOG(ERROR) << "Cannot open: \"" << this->bus_ << "\"";
  }
  return this->fd_;
}

bool I2CDev::ReadDevice(uint8_t cmd, uint8_t* data, size_t len) {
  struct i2c_msg m[2];

  m[0].addr = this->address_;
  m[0].flags = 0;
  m[0].len = sizeof(cmd);
  m[0].buf = &cmd;
  m[1].addr = this->address_;
  m[1].flags = I2C_M_RD;
  m[1].len = base::checked_cast<uint16_t>(len);
  m[1].buf = data;
  return this->Ioc(m, sizeof(m) / sizeof(m[0]));
}

bool I2CDev::WriteDevice(uint8_t cmd, const uint8_t* data, size_t len) {
  struct i2c_msg m[1];
  std::vector<uint8_t> buffer;
  buffer.reserve(len + 1);
  buffer.push_back(cmd);
  buffer.insert(buffer.end(), data, data + len);

  m[0].addr = this->address_;
  m[0].flags = I2C_M_STOP;
  m[0].len = base::checked_cast<uint16_t>(buffer.size());
  m[0].buf = buffer.data();
  return this->Ioc(m, sizeof(m) / sizeof(m[0]));
}

bool I2CDev::Ioc(struct i2c_msg* msg, size_t count) {
  struct i2c_rdwr_ioctl_data ioblk;

  ioblk.msgs = msg;
  ioblk.nmsgs = static_cast<uint32_t>(count);
  int ret = ioctl(this->fd_, I2C_RDWR, &ioblk);
  if (ret < 0) {
    VPLOG(3) << "i2c read/write failed";
  }
  return ret != -1;
}

// Static factory method.
std::unique_ptr<DevInterface> I2CDev::Create(const std::string& bus,
                                             uint8_t addr) {
  // Use new so that private constructor can be accessed.
  auto i2c_dev = std::unique_ptr<I2CDev>(new I2CDev(bus, addr));
  CHECK_GE(i2c_dev->Open(), 0);
  return std::unique_ptr<DevInterface>(std::move(i2c_dev));
}

}  // namespace hps
