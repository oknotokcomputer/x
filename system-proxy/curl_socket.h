// Copyright 2020 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.
#ifndef SYSTEM_PROXY_CURL_SOCKET_H_
#define SYSTEM_PROXY_CURL_SOCKET_H_

#include <memory>

#include <base/files/scoped_file.h>
#include <curl/curl.h>
#include <curl/easy.h>
#include <net-base/socket.h>

namespace system_proxy {

// Frees the resources allocated by curl_easy_init.
struct FreeCurlEasyhandle {
  void operator()(CURL* ptr) const { curl_easy_cleanup(ptr); }
};

typedef std::unique_ptr<CURL, FreeCurlEasyhandle> ScopedCurlEasyhandle;

// CurlSocket wraps a socket opened by curl in an net_base::Socket object
// with an owned CURL handle.
// TODO(b/324429360): This class makes a mess out of some abstractions, so
// we should refactor how ProxyConnectJob starts up a forwarder and remove
// this class.
class CurlSocket : public net_base::Socket {
 public:
  CurlSocket(base::ScopedFD fd, ScopedCurlEasyhandle curl_easyhandle);
  CurlSocket(const CurlSocket&) = delete;
  CurlSocket& operator=(const CurlSocket&) = delete;
  ~CurlSocket() override;

 private:
  ScopedCurlEasyhandle curl_easyhandle_;
};

}  // namespace system_proxy

#endif  // SYSTEM_PROXY_CURL_SOCKET_H_
