// Copyright 2014 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

// Helper utilities to simplify testing of D-Bus object implementations.
// Since the method handlers could now be asynchronous, they use callbacks to
// provide method return values. This makes it really difficult to invoke
// such handlers in unit tests (even if they are actually synchronous but
// still use DBusMethodResponse to send back the method results).
// This file provide testing-only helpers to make calling D-Bus method handlers
// easier.
#ifndef LIBCHROMEOS_CHROMEOS_DBUS_DBUS_OBJECT_TEST_HELPERS_H_
#define LIBCHROMEOS_CHROMEOS_DBUS_DBUS_OBJECT_TEST_HELPERS_H_

#include <base/bind.h>
#include <base/memory/weak_ptr.h>
#include <chromeos/dbus/dbus_method_invoker.h>
#include <chromeos/dbus/dbus_object.h>

namespace chromeos {
namespace dbus_utils {
namespace testing {

// This is a simple class that has weak pointer semantics and holds an
// instance of D-Bus method call response message. We use this in tests
// to get the response in case the handler processes a method call request
// synchronously. Otherwise the ResponseHolder object will be destroyed and
// ResponseHolder::ReceiveResponse() will not be called since we bind the
// callback to the object instance via a weak pointer.
struct ResponseHolder : public base::SupportsWeakPtr<ResponseHolder> {
  void ReceiveResponse(scoped_ptr<dbus::Response> response) {
    response_.reset(response.release());
  }

  std::unique_ptr<dbus::Response> response_;
};

// Dispatches a D-Bus method call to the corresponding handler.
// Used mostly for testing purposes. This method is inlined so that it is
// not included in the shipping code of libchromeos, and included at the
// call sites. Returns a response from the method handler or nullptr if the
// method hasn't provided the response message immediately
// (i.e. it is asynchronous).
inline std::unique_ptr<dbus::Response> CallMethod(
    const DBusObject& object, dbus::MethodCall* method_call) {
  DBusInterfaceMethodHandlerInterface* handler = object.FindMethodHandler(
      method_call->GetInterface(), method_call->GetMember());
  std::unique_ptr<dbus::Response> response;
  if (!handler) {
    response = CreateDBusErrorResponse(method_call,
                                       DBUS_ERROR_UNKNOWN_METHOD,
                                       "Unknown method");
  } else {
    ResponseHolder response_holder;
    handler->HandleMethod(method_call,
                          base::Bind(&ResponseHolder::ReceiveResponse,
                                     response_holder.AsWeakPtr()));
    response = std::move(response_holder.response_);
  }
  return response;
}

// MethodHandlerInvoker is similar to CallMethod() function above, except
// it allows the callers to invoke the method handlers directly bypassing
// the DBusObject/DBusInterface infrastructure.
// This works only on synchronous methods though. The handler must reply
// before the handler exits.
template<typename RetType>
struct MethodHandlerInvoker {
  // MethodHandlerInvoker<RetType>::Call() calls a member |method| of a class
  // |instance| and passes the |args| to it. The method's return value provided
  // via handler's DBusMethodResponse is then extracted and returned.
  // If the method handler returns an error, the error information is passed
  // to the caller via the |error| object (and the method returns a default
  // value of type RetType as a placeholder).
  // If the method handler asynchronous and did not provide a reply (success or
  // error) before the handler exits, this method aborts with a CHECK().
  template<class Class, typename... Params, typename... Args>
  static RetType Call(
      ErrorPtr* error,
      Class* instance,
      void(Class::*method)(scoped_ptr<DBusMethodResponse>, Params...),
      Args... args) {
    ResponseHolder response_holder;
    dbus::MethodCall method_call("test.interface", "TestMethod");
    method_call.SetSerial(123);
    scoped_ptr<DBusMethodResponse> method_response{
      new DBusMethodResponse(&method_call,
                             base::Bind(&ResponseHolder::ReceiveResponse,
                                        response_holder.AsWeakPtr()))
    };
    (instance->*method)(method_response.Pass(), args...);
    CHECK(response_holder.response_.get())
        << "No response received. Asynchronous methods are not supported.";
    RetType ret_val;
    ExtractMethodCallResults(response_holder.response_.get(), error, &ret_val);
    return ret_val;
  }
};

// Specialization of MethodHandlerInvoker for methods that do not return
// values (void methods).
template<>
struct MethodHandlerInvoker<void> {
  template<class Class, typename... Params, typename... Args>
  static void Call(
      ErrorPtr* error,
      Class* instance,
      void(Class::*method)(scoped_ptr<DBusMethodResponse>, Params...),
      Args... args) {
    ResponseHolder response_holder;
    dbus::MethodCall method_call("test.interface", "TestMethod");
    method_call.SetSerial(123);
    scoped_ptr<DBusMethodResponse> method_response{
      new DBusMethodResponse(&method_call,
                             base::Bind(&ResponseHolder::ReceiveResponse,
                                        response_holder.AsWeakPtr()))
    };
    (instance->*method)(method_response.Pass(), args...);
    CHECK(response_holder.response_.get())
        << "No response received. Asynchronous methods are not supported.";
    ExtractMethodCallResults(response_holder.response_.get(), error);
  }
};

}  // namespace testing
}  // namespace dbus_utils
}  // namespace chromeos

#endif  // LIBCHROMEOS_CHROMEOS_DBUS_DBUS_OBJECT_TEST_HELPERS_H_
