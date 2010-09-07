// Copyright (c) 2010 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef CROMO_SERVER_H_
#define CROMO_SERVER_H_

#include "base/basictypes.h"

#include <dbus-c++/dbus.h>

#include "manager_server_glue.h"
#include "hooktable.h"

class ModemHandler;

// Implements the ModemManager DBus API, and manages the
// modem manager instances that handle specific types of
// modems.
class CromoServer
    : public org::freedesktop::ModemManager_adaptor,
      public DBus::IntrospectableAdaptor,
      public DBus::ObjectAdaptor {
 public:
  explicit CromoServer(DBus::Connection& connection);
  ~CromoServer();

  void AddModemHandler(ModemHandler* handler);

  // ModemManager DBUS API methods.
  std::vector<DBus::Path> EnumerateDevices(DBus::Error& error);

  static const char* kServiceName;
  static const char* kServicePath;

  HookTable& start_exit_hooks() { return start_exit_hooks_; }
  HookTable& exit_ok_hooks() { return exit_ok_hooks_; }

 private:
  // The modem handlers that we are managing.
  std::vector<ModemHandler*> modem_handlers_;
  HookTable start_exit_hooks_;
  HookTable exit_ok_hooks_;

  DISALLOW_COPY_AND_ASSIGN(CromoServer);
};

#endif // CROMO_SERVER_H_
