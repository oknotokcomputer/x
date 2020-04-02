// Copyright 2017 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef ARC_NETWORK_MAC_ADDRESS_GENERATOR_H_
#define ARC_NETWORK_MAC_ADDRESS_GENERATOR_H_

#include <stdint.h>

#include <array>
#include <functional>
#include <unordered_set>

#include <base/macros.h>
#include <brillo/brillo_export.h>

namespace arc_networkd {

using MacAddress = std::array<uint8_t, 6>;

// Generates locally managed EUI-48 MAC addresses and ensures no collisions
// with any previously generated addresses by this instance.
class BRILLO_EXPORT MacAddressGenerator {
 public:
  MacAddressGenerator() = default;
  ~MacAddressGenerator() = default;

  // Generates a new EUI-48 MAC address and ensures that there are no
  // collisions with any addresses previously generated by this instance of
  // the generator.
  MacAddress Generate();

  // Returns a stable MAC address whose first 5 octets are fixed and using |id|
  // as the sixth. The base address is itself random and was not generated from
  // any particular device, physical or virtual. Additionally, the |id| should
  // associated with any specific device either, and should be set indepedently.
  MacAddress GetStable(uint8_t id) const;

 private:
  // The standard library sadly does not provide a hash function for std::array.
  // So implement one here for MacAddress based off boost::hash_combine.
  struct MacAddressHasher {
    size_t operator()(const MacAddress& addr) const noexcept {
      std::hash<uint8_t> hasher;

      size_t hash = 0;
      for (uint8_t octet : addr) {
        hash ^= hasher(octet) + 0x9e3779b9 + (hash << 6) + (hash >> 2);
      }

      return hash;
    }
  };

  // Set of all addresses generated by this instance.  This doesn't _need_ to be
  // an unordered_set but making it one improves the performance of the
  // "Duplicates" unit test by ~33% (~150 seconds -> ~100 seconds) and it
  // doesn't have a huge impact in production use so that's why we use it here.
  std::unordered_set<MacAddress, MacAddressHasher> addrs_;

  DISALLOW_COPY_AND_ASSIGN(MacAddressGenerator);
};

}  // namespace arc_networkd

#endif  // ARC_NETWORK_MAC_ADDRESS_GENERATOR_H_
