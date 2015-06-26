// Copyright 2015 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef SHILL_DBUS_CHROMEOS_DBUS_ADAPTOR_H_
#define SHILL_DBUS_CHROMEOS_DBUS_ADAPTOR_H_

#include <string>

#include <base/callback.h>
#include <base/macros.h>
#include <base/memory/weak_ptr.h>
#include <chromeos/dbus/dbus_object.h>
#include <chromeos/dbus/exported_object_manager.h>
#include <gtest/gtest_prod.h>  // for FRIEND_TEST

#include "shill/callbacks.h"
#include "shill/error.h"
#include "shill/property_store.h"

namespace shill {

template<typename... Types>
using DBusMethodResponsePtr =
    std::unique_ptr<chromeos::dbus_utils::DBusMethodResponse<Types...>>;

// Superclass for all DBus-backed Adaptor objects
class ChromeosDBusAdaptor : public base::SupportsWeakPtr<ChromeosDBusAdaptor> {
 public:
  static const char kNullPath[];

  ChromeosDBusAdaptor(
      const base::WeakPtr<chromeos::dbus_utils::ExportedObjectManager>&
          object_manager,
      const scoped_refptr<dbus::Bus>& bus,
      const std::string& object_path);
  ~ChromeosDBusAdaptor();

  const dbus::ObjectPath& dbus_path() const { return dbus_path_; }

  // Register DBus object.
  virtual void RegisterAsync(
      chromeos::dbus_utils::AsyncEventSequencer* sequencer) = 0;

 protected:
  FRIEND_TEST(ChromeosDBusAdaptorTest, SanitizePathElement);

  // Callback to wrap around DBus method response.
  ResultCallback GetMethodReplyCallback(DBusMethodResponsePtr<> response);

  // It would be nice if these two methods could be templated.  Unfortunately,
  // attempts to do so will trigger some fairly esoteric warnings from the
  // base library.
  ResultStringCallback GetStringMethodReplyCallback(
      DBusMethodResponsePtr<std::string> response);
  ResultBoolCallback GetBoolMethodReplyCallback(
      DBusMethodResponsePtr<bool> response);

  // Adaptors call this method just before returning. If |error|
  // indicates that the operation has completed, with no asynchronously
  // delivered result expected, then a DBus method reply is immediately
  // sent to the client that initiated the method invocation. Otherwise,
  // the operation is ongoing, and the result will be sent to the client
  // when the operation completes at some later time.
  //
  // Adaptors should always construct an Error initialized to the value
  // Error::kOperationInitiated. A pointer to this Error is passed down
  // through the call stack. Any layer that determines that the operation
  // has completed, either because of a failure that prevents carrying it
  // out, or because it was possible to complete it without sending a request
  // to an external server, should call error.Reset() to indicate success,
  // or to some error type to reflect the kind of failure that occurred.
  // Otherwise, they should leave the Error alone.
  //
  // The general structure of an adaptor method is
  //
  // void XXXXDBusAdaptor::SomeMethod(<args...>, DBusMethodResponsePtr<> resp) {
  //   Error e(Error::kOperationInitiated);
  //   ResultCallback callback = GetMethodReplyCallback(resp);
  //   xxxx_->SomeMethod(<args...>, &e, callback);
  //   ReturnResultOrDefer(callback, e);
  // }
  //
  void ReturnResultOrDefer(const ResultCallback& callback,
                           const Error& error);

  chromeos::dbus_utils::DBusObject* dbus_object() const {
    return dbus_object_.get();
  }

  // Set the property with |name| through |store|. Returns true if and
  // only if the property was changed. Updates |error| if a) an error
  // was encountered, and b) |error| is non-NULL. Otherwise, |error| is
  // unchanged.
  static bool SetProperty(PropertyStore* store,
                          const std::string& name,
                          const chromeos::Any& value,
                          chromeos::ErrorPtr* error);
  static bool GetProperties(const PropertyStore& store,
                            chromeos::VariantDictionary* out_properties,
                            chromeos::ErrorPtr* error);
  // Look for a property with |name| in |store|. If found, reset the
  // property to its "factory" value. If the property can not be
  // found, or if it can not be cleared (e.g., because it is
  // read-only), set |error| accordingly.
  //
  // Returns true if the property was found and cleared; returns false
  // otherwise.
  static bool ClearProperty(PropertyStore* store,
                            const std::string& name,
                            chromeos::ErrorPtr* error);

  // Returns an object path fragment that conforms to D-Bus specifications.
  static std::string SanitizePathElement(const std::string& object_path);

 private:
  void MethodReplyCallback(DBusMethodResponsePtr<> response,
                           const Error& error);

  void StringMethodReplyCallback(DBusMethodResponsePtr<std::string> response,
                                 const Error& error,
                                 const std::string& returned);
  void BoolMethodReplyCallback(DBusMethodResponsePtr<bool> response,
                               const Error& error,
                               bool returned);
  template<typename T>
  void TypedMethodReplyCallback(DBusMethodResponsePtr<T> response,
                                const Error& error,
                                const T& returned);

  dbus::ObjectPath dbus_path_;
  std::unique_ptr<chromeos::dbus_utils::DBusObject> dbus_object_;

  DISALLOW_COPY_AND_ASSIGN(ChromeosDBusAdaptor);
};

}  // namespace shill

#endif  // SHILL_DBUS_CHROMEOS_DBUS_ADAPTOR_H_
