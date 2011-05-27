// Copyright (c) 2011 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef LOGIN_MANAGER_DEVICE_POLICY_SERVICE_H_
#define LOGIN_MANAGER_DEVICE_POLICY_SERVICE_H_

#include <string>

#include <dbus/dbus-glib-lowlevel.h>

#include <base/basictypes.h>
#include <base/memory/ref_counted.h>
#include <base/memory/scoped_ptr.h>

#include "login_manager/owner_key_loss_mitigator.h"
#include "login_manager/policy_service.h"

namespace login_manager {
class KeyGenerator;
class NssUtil;
class OwnerKeyLossMitigator;

// A policy service specifically for device policy, adding in a few helpers for
// generating a new key for the device owner, handling key loss mitigation,
// storing owner properties etc.
class DevicePolicyService : public PolicyService {
 public:
  virtual ~DevicePolicyService();

  // Instantiates a regular (non-testing) device policy service instance.
  static DevicePolicyService* Create(
      OwnerKeyLossMitigator* mitigator,
      const scoped_refptr<base::MessageLoopProxy>& main_loop,
      const scoped_refptr<base::MessageLoopProxy>& io_loop);

  // Checks whether the given |current_user| is the device owner. The result of
  // the check is returned in |is_owner|. If so, it is validated that the device
  // policy settings are set up appropriately:
  // - If |current_user| has the owner key, put her on the login white list.
  // - If policy claims |current_user| is the device owner but she doesn't
  //   appear to have the owner key, run key mitigation.
  // Returns true on success. Fills in |error| upon encountering an error.
  virtual bool CheckAndHandleOwnerLogin(const std::string& current_user,
                                        bool* is_owner,
                                        GError** error);

  // Ensures that the public key in |buf| is legitimately paired with
  // a private key held by the current user, signs and stores some
  // ownership-related metadata, and then stores this key off as the
  // new device owner key.
  virtual void ValidateAndStoreOwnerKey(const std::string& current_user,
                                        const std::string& buf);

  // Checks whether the key is missing.
  virtual bool KeyMissing();

  static const char kPolicyPath[];
  // Format of this string is documented in device_management_backend.proto.
  static const char kDevicePolicyType[];

 private:
  friend class DevicePolicyServiceTest;
  friend class MockDevicePolicyService;

  // Takes ownership of |policy_store|, |policy_key|, |system_utils|, and |nss|.
  DevicePolicyService(PolicyStore* policy_store,
                      OwnerKey* policy_key,
                      SystemUtils* system_utils,
                      const scoped_refptr<base::MessageLoopProxy>& main_loop,
                      const scoped_refptr<base::MessageLoopProxy>& io_loop,
                      NssUtil* nss,
                      OwnerKeyLossMitigator* mitigator);

  // Assuming the current user has access to the owner private key (read: is the
  // owner), this call whitelists |current_user| and sets a property indicating
  // |current_user| is the owner in the current policy and schedules a
  // PersistPolicy().
  // Returns false on failure, with |error| set appropriately. |error| can be
  // NULL, should you wish to ignore the particulars.
  bool StoreOwnerProperties(const std::string& current_user,
                            GError** error);

  // Checks the user's NSS database to see if she has the private key.
  bool CurrentUserHasOwnerKey(const std::vector<uint8>& key,
                              GError** error);

  // Returns true if the current user is listed in |policy_| as the
  // device owner.  Returns false if not, or if that cannot be determined.
  bool CurrentUserIsOwner(const std::string& current_user);

  // Sends a signal to chromium.
  void SendSignal(const char* signal_name, bool status);

  // Overrides that additionally send notifications to chromium.
  virtual void PersistKeyOnIOLoop(bool* result);
  virtual void PersistPolicyOnIOLoop(base::WaitableEvent* event, bool* result);

  scoped_ptr<NssUtil> nss_;
  OwnerKeyLossMitigator* mitigator_;

  DISALLOW_COPY_AND_ASSIGN(DevicePolicyService);
};

}  // namespace login_manager

#endif  // LOGIN_MANAGER_DEVICE_POLICY_SERVICE_H_
