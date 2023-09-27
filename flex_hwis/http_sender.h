// Copyright 2023 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef FLEX_HWIS_HTTP_SENDER_H_
#define FLEX_HWIS_HTTP_SENDER_H_

#include "flex_hwis/hwis_data.pb.h"

#include <string>

namespace flex_hwis {
class PostActionResponse {
 public:
  bool success = false;
  std::string serialized_uuid;
};

// Sender implemented using brillo HTTP library.
class HttpSender {
 public:
  HttpSender() = default;
  explicit HttpSender(std::string server_url);
  HttpSender(const HttpSender&) = delete;
  HttpSender& operator=(const HttpSender&) = delete;

  virtual ~HttpSender() {}
  // Send a delete request to the HWIS server to delete the hardware
  // data if the user does not grant permission and there is a device
  // ID on the client side.
  virtual bool DeleteDevice(const hwis_proto::Device& content);
  // Send a post request to the HWIS server to create a new hardware
  // information entry in the database if the device ID doesn’t exist
  // on the client side.
  virtual PostActionResponse RegisterNewDevice(
      const hwis_proto::Device& content);
  // Send a put request to the HWIS server to replace an existing device
  // entry in the database if the device ID exists on the client side.
  virtual bool UpdateDevice(const hwis_proto::Device& content);

 private:
  const std::string server_url_;
};
}  // namespace flex_hwis

#endif  // FLEX_HWIS_HTTP_SENDER_H_
