// Copyright (c) 2012 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "shill/device.h"
#include "shill/device_dbus_adaptor.h"

#include <map>
#include <string>

#include "shill/device.h"
#include "shill/error.h"

using std::map;
using std::string;

namespace shill {

// static
const char DeviceDBusAdaptor::kInterfaceName[] = SHILL_INTERFACE;
// static
const char DeviceDBusAdaptor::kPath[] = "/device/";

DeviceDBusAdaptor::DeviceDBusAdaptor(DBus::Connection* conn, Device *device)
    : DBusAdaptor(conn, kPath + device->UniqueName()),
      device_(device),
      connection_name_(conn->unique_name()) {
}

DeviceDBusAdaptor::~DeviceDBusAdaptor() {
  device_ = NULL;
}
const std::string &DeviceDBusAdaptor::GetRpcIdentifier() {
  return path();
}

const std::string &DeviceDBusAdaptor::GetRpcConnectionIdentifier() {
  return connection_name_;
}

void DeviceDBusAdaptor::UpdateEnabled() {}

void DeviceDBusAdaptor::EmitBoolChanged(const std::string& name, bool value) {
  PropertyChanged(name, DBusAdaptor::BoolToVariant(value));
}

void DeviceDBusAdaptor::EmitUintChanged(const std::string& name, uint32 value) {
  PropertyChanged(name, DBusAdaptor::Uint32ToVariant(value));
}

void DeviceDBusAdaptor::EmitIntChanged(const std::string& name, int value) {
  PropertyChanged(name, DBusAdaptor::Int32ToVariant(value));
}

void DeviceDBusAdaptor::EmitStringChanged(const std::string& name,
                                          const std::string& value) {
  PropertyChanged(name, DBusAdaptor::StringToVariant(value));
}

void DeviceDBusAdaptor::EmitStringmapsChanged(const std::string &name,
                                              const Stringmaps &value) {
  PropertyChanged(name, DBusAdaptor::StringmapsToVariant(value));
}

void DeviceDBusAdaptor::EmitKeyValueStoreChanged(const std::string &name,
                                                 const KeyValueStore &value) {
  PropertyChanged(name, DBusAdaptor::KeyValueStoreToVariant(value));
}

map<string, ::DBus::Variant> DeviceDBusAdaptor::GetProperties(
    ::DBus::Error &error) {
  map<string, ::DBus::Variant> properties;
  DBusAdaptor::GetProperties(device_->store(), &properties, &error);
  return properties;
}

void DeviceDBusAdaptor::SetProperty(const string &name,
                                    const ::DBus::Variant &value,
                                    ::DBus::Error &error) {
  DBusAdaptor::DispatchOnType(device_->mutable_store(), name, value, &error);
}

void DeviceDBusAdaptor::ClearProperty(const std::string &name,
                                      ::DBus::Error &/*error*/) {
  NOTIMPLEMENTED() << " Ignoring request to clear " << name
                   << " property of Device " << device_->FriendlyName();
}

void DeviceDBusAdaptor::ProposeScan(::DBus::Error &error) {
  Error e;
  device_->Scan(&e);
  e.ToDBusError(&error);
}

::DBus::Path DeviceDBusAdaptor::AddIPConfig(const string& ,
                                            ::DBus::Error &/*error*/) {
  return ::DBus::Path();
}

void DeviceDBusAdaptor::Register(const string &network_id,
                                 ::DBus::Error &error) {
  VLOG(2) << __func__ << "(" << network_id << ")";
  Returner *returner = Returner::Create(this);
  device_->RegisterOnNetwork(network_id, returner);
  returner->DelayOrReturn(&error);
}

void DeviceDBusAdaptor::RequirePin(
    const string &pin, const bool &require, DBus::Error &error) {
  VLOG(2) << __func__;
  Returner *returner = Returner::Create(this);
  device_->RequirePIN(pin, require, returner);
  returner->DelayOrReturn(&error);
}

void DeviceDBusAdaptor::EnterPin(const string &pin, DBus::Error &error) {
  VLOG(2) << __func__;
  Returner *returner = Returner::Create(this);
  device_->EnterPIN(pin, returner);
  returner->DelayOrReturn(&error);
}

void DeviceDBusAdaptor::UnblockPin(
    const string &unblock_code, const string &pin, DBus::Error &error) {
  VLOG(2) << __func__;
  Returner *returner = Returner::Create(this);
  device_->UnblockPIN(unblock_code, pin, returner);
  returner->DelayOrReturn(&error);
}

void DeviceDBusAdaptor::ChangePin(
    const string &old_pin, const string &new_pin, DBus::Error &error) {
  VLOG(2) << __func__;
  Returner *returner = Returner::Create(this);
  device_->ChangePIN(old_pin, new_pin, returner);
  returner->DelayOrReturn(&error);
}

}  // namespace shill
