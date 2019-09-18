// Copyright 2019 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "bluetooth/newblued/util.h"

#include <algorithm>
#include <cstdint>
#include <regex>  // NOLINT(build/c++11)
#include <string>
#include <utility>

#include <base/stl_util.h>
#include <base/strings/string_number_conversions.h>
#include <base/strings/string_split.h>
#include <base/strings/string_util.h>
#include <base/strings/stringprintf.h>
#include <newblue/bt.h>
#include <newblue/uuid.h>

#include "bluetooth/newblued/uuid.h"

namespace {

uint64_t GetNumFromLE(const uint8_t* buf, uint8_t bits) {
  uint64_t val = 0;
  uint8_t bytes = bits / 8;

  CHECK(buf);

  buf += bytes;

  while (bytes--)
    val = (val << 8) | *--buf;

  return val;
}

}  // namespace

namespace bluetooth {

////////////////////////////////////////////////////////////////////////////////
// Miscellaneous utility functions
////////////////////////////////////////////////////////////////////////////////

uint16_t GetNumFromLE16(const uint8_t* buf) {
  return static_cast<uint16_t>(GetNumFromLE(buf, 16));
}

uint32_t GetNumFromLE24(const uint8_t* buf) {
  return static_cast<uint32_t>(GetNumFromLE(buf, 24));
}

std::vector<uint8_t> GetBytesFromLE(const uint8_t* buf, size_t buf_len) {
  std::vector<uint8_t> ret;

  CHECK(buf);

  if (!buf_len)
    return ret;

  ret.assign(buf, buf + buf_len);
  std::reverse(ret.begin(), ret.end());
  return ret;
}

UniqueId GetNextId() {
  static UniqueId next_id = 1;
  uint64_t id = next_id++;
  if (id)
    return id;
  next_id -= 1;
  LOG(ERROR) << "Run out of unique IDs";
  return 0;
}

////////////////////////////////////////////////////////////////////////////////
// Parsing Discovered Device Information
////////////////////////////////////////////////////////////////////////////////

std::string ConvertAppearanceToIcon(uint16_t appearance) {
  // These value are defined at https://www.bluetooth.com/specifications/gatt/
  // viewer?attributeXmlFile=org.bluetooth.characteristic.gap.appearance.xml.
  // The translated strings come from BlueZ.
  switch ((appearance & kAppearanceMask) >> 6) {
    case 0x00:
      return "unknown";
    case 0x01:
      return "phone";
    case 0x02:
      return "computer";
    case 0x03:
      return "watch";
    case 0x04:
      return "clock";
    case 0x05:
      return "video-display";
    case 0x06:
      return "remote-control";
    case 0x07:
      return "eye-glasses";
    case 0x08:
      return "tag";
    case 0x09:
      return "key-ring";
    case 0x0a:
      return "multimedia-player";
    case 0x0b:
      return "scanner";
    case 0x0c:
      return "thermometer";
    case 0x0d:
      return "heart-rate-sensor";
    case 0x0e:
      return "blood-pressure";
    case 0x0f:  // HID Generic
      switch (appearance & 0x3f) {
        case 0x01:
          return "input-keyboard";
        case 0x02:
          return "input-mouse";
        case 0x03:
        case 0x04:
          return "input-gaming";
        case 0x05:
          return "input-tablet";
        case 0x08:
          return "scanner";
      }
      break;
    case 0x10:
      return "glucose-meter";
    case 0x11:
      return "running-walking-sensor";
    case 0x12:
      return "cycling";
    case 0x31:
      return "pulse-oximeter";
    case 0x32:
      return "weight-scale";
    case 0x33:
      return "personal-mobility-device";
    case 0x34:
      return "continuous-glucose-monitor";
    case 0x35:
      return "insulin-pump";
    case 0x36:
      return "medication-delivery";
    case 0x51:
      return "outdoor-sports-activity";
    default:
      break;
  }
  return std::string();
}

std::string ConvertToAsciiString(std::string name) {
  /* Replace all non-ASCII characters with spaces */
  for (auto& ch : name) {
    if (!isascii(ch))
      ch = ' ';
  }

  return name;
}

std::map<uint16_t, std::vector<uint8_t>> ParseDataIntoManufacturer(
    uint16_t manufacturer_id, std::vector<uint8_t> manufacturer_data) {
  std::map<uint16_t, std::vector<uint8_t>> manufacturer;
  manufacturer.emplace(manufacturer_id, std::move(manufacturer_data));
  return manufacturer;
}

void ParseDataIntoUuids(std::set<Uuid>* service_uuids,
                        uint8_t uuid_size,
                        const uint8_t* data,
                        uint8_t data_len) {
  CHECK(service_uuids && data);

  if (!data_len || data_len % uuid_size != 0) {
    LOG(WARNING) << "Failed to parse EIR service UUIDs";
    return;
  }

  // Service UUIDs are presented in little-endian order.
  for (uint8_t i = 0; i < data_len; i += uuid_size) {
    Uuid uuid(GetBytesFromLE(data + i, uuid_size));
    CHECK(uuid.format() != UuidFormat::UUID_INVALID);
    service_uuids->insert(uuid);
  }
}

void ParseDataIntoServiceData(
    std::map<Uuid, std::vector<uint8_t>>* service_data,
    uint8_t uuid_size,
    const uint8_t* data,
    uint8_t data_len) {
  CHECK(service_data && data);

  if (!data_len || data_len <= uuid_size) {
    LOG(WARNING) << "Failed to parse EIR service data";
    return;
  }

  // A service UUID and its data are presented in little-endian order where the
  // format is {<bytes of service UUID>, <bytes of service data>}. For instance,
  // the service data associated with the battery service can be
  // {0x0F, 0x18, 0x22, 0x11}
  // where {0x18 0x0F} is the UUID and {0x11, 0x22} is the data.
  Uuid uuid(GetBytesFromLE(data, uuid_size));
  CHECK_NE(UuidFormat::UUID_INVALID, uuid.format());
  service_data->emplace(uuid,
                        GetBytesFromLE(data + uuid_size, data_len - uuid_size));
}

////////////////////////////////////////////////////////////////////////////////
// Translation between D-Bus object path and newblued types.
////////////////////////////////////////////////////////////////////////////////

bool TrimAdapterFromObjectPath(std::string* path) {
  std::regex rgx("^/org/bluez/hci[0-9]+$");
  std::smatch match;

  if (!std::regex_search(*path, match, rgx) || match.size() != 1)
    return false;

  path->clear();
  return true;
}

std::string TrimDeviceFromObjectPath(std::string* device) {
  std::regex rgx("/dev_([0-9a-fA-F]{2}_){5}[0-9a-fA-F]{2}$");
  std::smatch m;
  std::string address;

  if (!std::regex_search(*device, m, rgx) || m.empty())
    return "";

  address = m.str(0).substr(5);
  std::replace(address.begin(), address.end(), '_', ':');
  *device = device->substr(0, device->size() - m.str(0).size());
  return address;
}

int32_t TrimServiceFromObjectPath(std::string* service) {
  std::regex rgx("/service[0-9a-fA-F]{4}$");
  std::smatch m;
  std::string srv;

  if (!std::regex_search(*service, m, rgx) || m.empty())
    return kInvalidServiceHandle;

  srv = m.str(0).substr(8, 4);
  *service = service->substr(0, service->size() - m.str(0).size());
  return std::stol(srv, nullptr, 16);
}

int32_t TrimCharacteristicFromObjectPath(std::string* characteristic) {
  std::regex rgx("/char[0-9a-fA-F]{4}$");
  std::smatch m;
  std::string charac;

  if (!std::regex_search(*characteristic, m, rgx) || m.empty())
    return kInvalidCharacteristicHandle;

  charac = m.str(0).substr(5, 4);
  *characteristic =
      characteristic->substr(0, characteristic->size() - m.str(0).size());
  return std::stol(charac, nullptr, 16);
}

int32_t TrimDescriptorFromObjectPath(std::string* descriptor) {
  std::regex rgx("/desc[0-9a-fA-F]{4}$");
  std::smatch m;
  std::string desc;

  if (!std::regex_search(*descriptor, m, rgx) || m.empty())
    return kInvalidDescriptorHandle;

  desc = m.str(0).substr(5);
  *descriptor = descriptor->substr(0, descriptor->size() - m.str(0).size());
  return std::stol(desc, nullptr, 16);
}

std::string ConvertDeviceObjectPathToAddress(const std::string& path) {
  std::string p(path);
  std::string address = TrimDeviceFromObjectPath(&p);

  if (address.empty() || p.empty())
    return "";

  if (!TrimAdapterFromObjectPath(&p))
    return "";

  return address;
}

std::string ConvertDeviceAddressToObjectPath(const std::string& address) {
  std::string path;

  if (address.empty())
    return "";

  path = base::StringPrintf("%s/dev_%s", kAdapterObjectPath, address.c_str());
  std::replace(path.begin(), path.end(), ':', '_');
  return path;
}

bool ConvertServiceObjectPathToHandle(std::string* address,
                                      uint16_t* handle,
                                      const std::string& path) {
  std::string p(path);
  std::string addr;
  int32_t h = TrimServiceFromObjectPath(&p);
  if (h == kInvalidServiceHandle || p.empty())
    return false;

  addr = ConvertDeviceObjectPathToAddress(p);
  if (addr.empty())
    return false;

  *address = addr;
  *handle = h;
  return true;
}

std::string ConvertServiceHandleToObjectPath(const std::string& address,
                                             uint16_t handle) {
  std::string dev = ConvertDeviceAddressToObjectPath(address);
  std::string s = base::StringPrintf("/service%04X", handle);
  if (dev.empty() || s.empty())
    return "";
  return dev + s;
}

bool ConvertCharacteristicObjectPathToHandles(std::string* address,
                                              uint16_t* service_handle,
                                              uint16_t* char_handle,
                                              const std::string& path) {
  std::string p(path);
  std::string addr;
  uint16_t sh;
  int32_t ch = TrimCharacteristicFromObjectPath(&p);
  if (ch == kInvalidCharacteristicHandle || p.empty())
    return false;

  if (!ConvertServiceObjectPathToHandle(&addr, &sh, p))
    return false;

  *address = addr;
  *service_handle = sh;
  *char_handle = ch;
  return true;
}

std::string ConvertCharacteristicHandleToObjectPath(const std::string& address,
                                                    uint16_t service_handle,
                                                    uint16_t char_handle) {
  std::string s = ConvertServiceHandleToObjectPath(address, service_handle);
  std::string c = base::StringPrintf("/char%04X", char_handle);
  if (s.empty() || c.empty())
    return "";
  return s + c;
}

bool ConvertDescriptorObjectPathToHandles(std::string* address,
                                          uint16_t* service_handle,
                                          uint16_t* char_handle,
                                          uint16_t* desc_handle,
                                          const std::string& path) {
  std::string p(path);
  std::string addr;
  uint16_t sh;
  uint16_t ch;
  int32_t dh = TrimDescriptorFromObjectPath(&p);
  if (dh == kInvalidDescriptorHandle || p.empty())
    return false;

  if (!ConvertCharacteristicObjectPathToHandles(&addr, &sh, &ch, p))
    return false;

  *address = addr;
  *service_handle = sh;
  *char_handle = ch;
  *desc_handle = dh;
  return true;
}

std::string ConvertDescriptorHandleToObjectPath(const std::string& address,
                                                uint16_t service_handle,
                                                uint16_t char_handle,
                                                uint16_t desc_handle) {
  std::string c = ConvertCharacteristicHandleToObjectPath(
      address, service_handle, char_handle);
  std::string d = base::StringPrintf("/desc%04X", desc_handle);
  if (c.empty() || d.empty())
    return "";
  return c + d;
}

////////////////////////////////////////////////////////////////////////////////
// Translation between libnewblue types and newblued types.
////////////////////////////////////////////////////////////////////////////////

bool ConvertToBtAddr(bool is_random_address,
                     const std::string& address,
                     struct bt_addr* result) {
  CHECK(result);

  std::vector<std::string> tokens = base::SplitString(
      address, ":", base::KEEP_WHITESPACE, base::SPLIT_WANT_ALL);
  if (tokens.size() != BT_MAC_LEN)
    return false;

  uint8_t addr[BT_MAC_LEN];
  uint8_t* ptr = addr + BT_MAC_LEN;
  for (const auto& token : tokens) {
    uint32_t value;
    if (token.size() != 2 || !base::HexStringToUInt(token, &value))
      return false;
    *(--ptr) = static_cast<uint8_t>(value);
  }

  memcpy(result->addr, addr, BT_MAC_LEN);
  result->type =
      is_random_address ? BT_ADDR_TYPE_LE_RANDOM : BT_ADDR_TYPE_LE_PUBLIC;
  return true;
}

std::string ConvertBtAddrToString(const struct bt_addr& addr) {
  return base::StringPrintf("%02X:%02X:%02X:%02X:%02X:%02X", addr.addr[5],
                            addr.addr[4], addr.addr[3], addr.addr[2],
                            addr.addr[1], addr.addr[0]);
}

Uuid ConvertToUuid(const struct uuid& uuid) {
  std::vector<uint8_t> uuid_value;
  uint64_t lo = uuid.lo;
  uint64_t hi = uuid.hi;
  int i;

  for (i = 0; i < sizeof(lo); ++i, lo >>= 8)
    uuid_value.emplace(uuid_value.begin(), static_cast<uint8_t>(lo));
  for (i = 0; i < sizeof(hi); ++i, hi >>= 8)
    uuid_value.emplace(uuid_value.begin(), static_cast<uint8_t>(hi));

  return Uuid(uuid_value);
}

struct uuid ConvertToRawUuid(const Uuid& uuid) {
  struct uuid u;
  uuidZero(&u);

  if (uuid.format() == UuidFormat::UUID_INVALID)
    return u;

  uint64_t tmp;
  memcpy(&tmp, &uuid.value()[0], sizeof(uint64_t));
  u.hi = be64toh(tmp);
  memcpy(&tmp, &uuid.value()[8], sizeof(uint64_t));
  u.lo = be64toh(tmp);

  return u;
}

std::unique_ptr<GattService> ConvertToGattService(
    const struct GattTraversedService& service) {
  // struct GattTraversedService is the result of primary service traversal, so
  // it is safe to assume that primary is always true in this case.
  auto s = std::make_unique<GattService>(service.firstHandle,
                                         service.lastHandle, true /* primary */,
                                         ConvertToUuid(service.uuid));

  const auto* included_service = service.inclSvcs;
  for (int i = 0; i < service.numInclSvcs; ++i, ++included_service) {
    auto is = std::make_unique<GattIncludedService>(
        s.get(), included_service->includeDefHandle,
        included_service->firstHandle, included_service->lastHandle,
        ConvertToUuid(included_service->uuid));
    s->AddIncludedService(std::move(is));
  }

  const auto* characteristic = service.chars;
  for (int i = 0; i < service.numChars; ++i, ++characteristic) {
    auto c = std::make_unique<GattCharacteristic>(
        s.get(), characteristic->valHandle, characteristic->firstHandle,
        characteristic->lastHandle, characteristic->charProps,
        ConvertToUuid(characteristic->uuid));

    const auto* descriptor = characteristic->descrs;
    for (int j = 0; j < characteristic->numDescrs; ++j, ++descriptor) {
      auto d = std::make_unique<GattDescriptor>(
          c.get(), descriptor->handle, ConvertToUuid(descriptor->uuid));
      c->AddDescriptor(std::move(d));
    }
    s->AddCharacteristic(std::move(c));
  }

  return s;
}

std::vector<uint8_t> GetBytesFromSg(const sg data) {
  if (data == nullptr)
    return {};

  uint32_t data_length = sgLength(data);
  if (data_length == 0)
    return {};

  std::vector<uint8_t> d(data_length);
  if (!sgSerializeCutFront(data, d.data(), d.size())) {
    LOG(WARNING) << "Failed to extract bytes from sg";
    return {};
  }

  return d;
}

}  // namespace bluetooth
