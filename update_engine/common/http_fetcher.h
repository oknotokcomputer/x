// Copyright 2009 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef UPDATE_ENGINE_COMMON_HTTP_FETCHER_H_
#define UPDATE_ENGINE_COMMON_HTTP_FETCHER_H_

#include <deque>
#include <memory>
#include <string>
#include <vector>

#include <base/functional/callback.h>
#include <base/logging.h>
#include <brillo/message_loops/message_loop.h>

#include "update_engine/common/http_common.h"
#include "update_engine/common/metrics_constants.h"
#include "update_engine/common/proxy_resolver.h"

// This class is a simple wrapper around an HTTP library (libcurl). We can
// easily mock out this interface for testing.

// Implementations of this class should use asynchronous i/o. They can access
// the MessageLoop to request callbacks when timers or file descriptors change.

namespace chromeos_update_engine {

class HttpFetcherDelegate;

class HttpFetcher {
 public:
  // |proxy_resolver| is the resolver that will be consulted for proxy
  // settings. It may be null, in which case direct connections will
  // be used. Does not take ownership of the resolver.
  explicit HttpFetcher(ProxyResolver* proxy_resolver)
      : post_data_set_(false),
        http_response_code_(0),
        delegate_(nullptr),
        proxies_(1, kNoProxy),
        proxy_resolver_(proxy_resolver),
        callback_(nullptr) {}
  HttpFetcher(const HttpFetcher&) = delete;
  HttpFetcher& operator=(const HttpFetcher&) = delete;

  virtual ~HttpFetcher();

  void set_delegate(HttpFetcherDelegate* delegate) { delegate_ = delegate; }
  HttpFetcherDelegate* delegate() const { return delegate_; }
  int http_response_code() const { return http_response_code_; }

  // Returns additional error code that can't be expressed in terms of an HTTP
  // response code. For example, if there was a specific internal error code in
  // the objects used in the implementation of this class (like libcurl) that we
  // are interested about, we can communicate it through this value.
  ErrorCode GetAuxiliaryErrorCode() const { return auxiliary_error_code_; }

  // Optional: Post data to the server. The HttpFetcher should make a copy
  // of this data and upload it via HTTP POST during the transfer. The type of
  // the data is necessary for properly setting the Content-Type HTTP header.
  void SetPostData(const void* data, size_t size, HttpContentType type);

  // Same without a specified Content-Type.
  void SetPostData(const void* data, size_t size);

  // Proxy methods to set the proxies, then to pop them off.
  void ResolveProxiesForUrl(const std::string& url, base::OnceClosure callback);

  void SetProxies(const std::deque<std::string>& proxies) {
    proxies_ = proxies;
  }
  const std::string& GetCurrentProxy() const { return proxies_.front(); }
  bool HasProxy() const { return !proxies_.empty(); }
  void PopProxy() { proxies_.pop_front(); }

  // Downloading should resume from this offset
  virtual void SetOffset(off_t offset) = 0;

  // Set/unset the length of the range to be downloaded.
  virtual void SetLength(size_t length) = 0;
  virtual void UnsetLength() = 0;

  // Begins the transfer to the specified URL. This fetcher instance should not
  // be destroyed until either TransferComplete, or TransferTerminated is
  // called.
  virtual void BeginTransfer(const std::string& url) = 0;

  // Aborts the transfer. The transfer may not abort right away -- delegate's
  // TransferTerminated() will be called when the transfer is actually done.
  virtual void TerminateTransfer() = 0;

  // Add or update a custom header to be sent with every request. If the same
  // |header_name| is passed twice, the second |header_value| would override the
  // previous value.
  virtual void SetHeader(const std::string& header_name,
                         const std::string& header_value) = 0;

  // Only used for testing.
  // If |header_name| is set, the value will be set into |header_value|.
  // On success the boolean true will be returned, hoewever on failture to find
  // the |header_name| in the header the return value will be false. The state
  // in which |header_value| is left in for failures is an empty string.
  virtual bool GetHeader(const std::string& header_name,
                         std::string* header_value) const = 0;

  // If data is coming in too quickly, you can call Pause() to pause the
  // transfer. The delegate will not have ReceivedBytes() called while
  // an HttpFetcher is paused.
  virtual void Pause() = 0;

  // Used to unpause an HttpFetcher and let the bytes stream in again.
  // If a delegate is set, ReceivedBytes() may be called on it before
  // Unpause() returns
  virtual void Unpause() = 0;

  // These two function are overloaded in LibcurlHttp fetcher to speed
  // testing.
  virtual void set_idle_seconds(int seconds) {}
  virtual void set_retry_seconds(int seconds) {}

  // Sets the values used to time out the connection if the transfer
  // rate is less than |low_speed_bps| bytes/sec for more than
  // |low_speed_sec| seconds.
  virtual void set_low_speed_limit(int low_speed_bps, int low_speed_sec) = 0;

  // Sets the connect timeout, e.g. the maximum amount of time willing
  // to wait for establishing a connection to the server.
  virtual void set_connect_timeout(int connect_timeout_seconds) = 0;

  // Sets the number of allowed retries.
  virtual void set_max_retry_count(int max_retry_count) = 0;

  // Get the total number of bytes downloaded by fetcher.
  virtual size_t GetBytesDownloaded() = 0;

  ProxyResolver* proxy_resolver() const { return proxy_resolver_; }

 protected:
  // Cancels a proxy resolution in progress. The callback passed to
  // ResolveProxiesForUrl() will not be called. Returns whether there was a
  // pending proxy resolution to be canceled.
  bool CancelProxyResolution();

  // The URL we're actively fetching from
  std::string url_;

  // POST data for the transfer, and whether or not it was ever set
  bool post_data_set_;
  brillo::Blob post_data_;
  HttpContentType post_content_type_;

  // The server's HTTP response code from the last transfer. This
  // field should be set to 0 when a new transfer is initiated, and
  // set to the response code when the transfer is complete.
  int http_response_code_;

  // Set when there is an error that can't be expressed in the form of
  // |http_response_code_|.
  ErrorCode auxiliary_error_code_{ErrorCode::kSuccess};

  // The delegate; may be null.
  HttpFetcherDelegate* delegate_;

  // Proxy servers
  std::deque<std::string> proxies_;

  ProxyResolver* const proxy_resolver_;

  // The ID of the idle callback, used when we have no proxy resolver.
  brillo::MessageLoop::TaskId no_resolver_idle_id_{
      brillo::MessageLoop::kTaskIdNull};

  // Callback for when we are resolving proxies
  std::unique_ptr<base::OnceClosure> callback_;

 private:
  // Callback from the proxy resolver
  void ProxiesResolved(const std::deque<std::string>& proxies);

  // Callback used to run the proxy resolver callback when there is no
  // |proxy_resolver_|.
  void NoProxyResolverCallback();

  // Stores the ongoing proxy request id if there is one, otherwise
  // kProxyRequestIdNull.
  ProxyRequestId proxy_request_{kProxyRequestIdNull};
};

// Interface for delegates
class HttpFetcherDelegate {
 public:
  virtual ~HttpFetcherDelegate() = default;

  // Called every time bytes are received. Returns false if this call causes the
  // transfer be terminated or completed otherwise it returns true.
  virtual bool ReceivedBytes(HttpFetcher* fetcher,
                             const void* bytes,
                             size_t length) = 0;

  // Called if the fetcher seeks to a particular offset.
  virtual void SeekToOffset(off_t offset) {}

  // When a transfer has completed, exactly one of these two methods will be
  // called. TransferTerminated is called when the transfer has been aborted
  // through TerminateTransfer. TransferComplete is called in all other
  // situations. It's OK to destroy the |fetcher| object in this callback.
  virtual void TransferComplete(HttpFetcher* fetcher, bool successful) = 0;
  virtual void TransferTerminated(HttpFetcher* fetcher) {}
};

}  // namespace chromeos_update_engine

#endif  // UPDATE_ENGINE_COMMON_HTTP_FETCHER_H_
