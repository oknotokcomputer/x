// Copyright 2015 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "trunks/resource_manager.h"

#include <algorithm>
#include <map>
#include <memory>
#include <set>
#include <string>
#include <tuple>
#include <utility>
#include <vector>

#include <base/check.h>
#include <base/check_op.h>
#include <base/functional/callback.h>
#include <base/logging.h>

#include "trunks/error_codes.h"
#include "trunks/tpm_generated.h"

#define IS_TPM_CC_VENDOR_CMD(c)            \
  (((c) == TPM_CC_VENDOR_SPECIFIC_MASK) || \
   ((c) == TPM_CC_CR50_EXTENSION_COMMAND))

#define IS_TPM2_STD_CMD(x) \
  ((x) >= trunks::TPM_CC_FIRST && (x) <= trunks::TPM_CC_LAST)
#define IS_TPM2_EXT_CMD(x) \
  ((x) >= trunks::TPM_CCE_FIRST && (x) <= trunks::TPM_CCE_LAST)
#define IS_TPM2_CMD(x) (IS_TPM2_STD_CMD(x) || IS_TPM2_EXT_CMD(x))

namespace {

const int kMaxSuspendDurationSec = 10;
const int kMaxCommandAttempts = 3;
const size_t kMinimumAuthorizationSize = 9;
const size_t kMessageHeaderSize = 10;
const trunks::TPM_HANDLE kMaxVirtualHandle =
    (trunks::HR_TRANSIENT + trunks::HR_HANDLE_MASK);
const uint64_t kUnknownSender = 0;

class ScopedBool {
 public:
  ScopedBool() : target_(nullptr) {}
  ~ScopedBool() {
    if (target_) {
      *target_ = false;
    }
  }
  void Enable(bool* target) {
    target_ = target;
    *target_ = true;
  }

 private:
  bool* target_;
};

}  // namespace

namespace trunks {

ResourceManager::ResourceManager(const TrunksFactory& factory,
                                 CommandTransceiver* next_transceiver)
    : factory_(factory),
      next_transceiver_(next_transceiver),
      max_suspend_duration_(base::Seconds(kMaxSuspendDurationSec)) {}

ResourceManager::~ResourceManager() {}

void ResourceManager::Initialize() {
  // Abort if the TPM is not in a reasonable state and we can't get it into one.
  std::unique_ptr<TpmUtility> tpm_utility = factory_.GetTpmUtility();
  CHECK_EQ(tpm_utility->CheckState(), TPM_RC_SUCCESS);

  // Full control of the TPM is assumed and required. Existing transient object
  // and session handles are mercilessly flushed.
  for (UINT32 handle_type :
       {HR_TRANSIENT, HR_HMAC_SESSION, HR_POLICY_SESSION}) {
    TPMI_YES_NO more_data = YES;
    TPMS_CAPABILITY_DATA data;
    UINT32 handle_range = handle_type;
    while (more_data) {
      TPM_RC result = factory_.GetTpm()->GetCapabilitySync(
          TPM_CAP_HANDLES, handle_range, MAX_CAP_HANDLES, &more_data, &data,
          nullptr);
      if (result != TPM_RC_SUCCESS) {
        LOG(WARNING) << "Failed to query existing handles: "
                     << GetErrorString(result);
        break;
      }
      const TPML_HANDLE& handle_list = data.data.handles;
      for (UINT32 i = 0; i < handle_list.count; ++i) {
        factory_.GetTpm()->FlushContextSync(handle_list.handle[i], nullptr);
      }
      if (more_data) {
        // Adjust the range to be greater than the most recent handle so on the
        // next query we'll start where we left off.
        handle_range = handle_list.handle[handle_list.count - 1];
      }
    }
  }
}

void ResourceManager::SendCommand(const std::string& command,
                                  ResponseCallback callback) {
  SendCommandWithSender(command, kUnknownSender, std::move(callback));
}

std::string ResourceManager::SendCommandAndWait(const std::string& command) {
  return SendCommandWithSenderAndWait(command, kUnknownSender);
}

void ResourceManager::SendCommandWithSender(const std::string& command,
                                            uint64_t sender,
                                            ResponseCallback callback) {
  std::move(callback).Run(SendCommandAndWait(command));
}

std::string ResourceManager::SendCommandWithSenderAndWait(
    const std::string& command, uint64_t sender) {
  // Sanitize the |command|. If this succeeds consistency of the command header
  // and the size of all other sections can be assumed.
  MessageInfo command_info;
  TPM_RC result = ParseCommand(command, sender, &command_info);
  if (result != TPM_RC_SUCCESS) {
    return CreateErrorResponse(result);
  }
  // Block all commands with handles when suspended.
  // TODO(apronin): Add metrics to track cases when we receive commands
  // while in the suspended state, auto-resume from it, block commands
  // with handles as a result.
  if (suspended_) {
    LOG(WARNING) << "Received command CC 0x" << std::hex << command_info.code
                 << " while suspended.";
    // Make sure we resume after the maximum allowed suspend duration even
    // if the resume event is somehow lost. Should be enough to go through
    // suspend preparaion - and that's all we care about.
    base::TimeTicks now = base::TimeTicks::Now();
    if (now < suspended_timestamp_ ||
        now >= suspended_timestamp_ + max_suspend_duration_) {
      LOG(WARNING) << "Auto-resuming Resource Manager.";
      suspended_ = false;
    } else if (GetNumberOfRequestHandles(command_info.code) ||
               GetNumberOfResponseHandles(command_info.code)) {
      LOG(WARNING) << "Blocking command while suspended.";
      return CreateErrorResponse(TPM_RC_RETRY);
    }
  }

  // We don't support these commands.
  if (command_info.code == TPM_CC_ContextLoad ||
      command_info.code == TPM_CC_ContextSave) {
    return CreateErrorResponse(SAPI_RC_ABI_MISMATCH);
  }

  // A special case for FlushContext. It requires special handling because it
  // has a handle as a parameter and because we need to cleanup if it succeeds.
  if (command_info.code == TPM_CC_FlushContext) {
    return ProcessFlushContext(command, command_info);
  }

  // Update the virtual handles LRU.
  for (size_t i = 0; i + 1 < loaded_object_infos_.size(); i++) {
    if (std::find(command_info.handles.begin(), command_info.handles.end(),
                  *loaded_object_infos_[i].handle) !=
        command_info.handles.end()) {
      std::rotate(loaded_object_infos_.begin() + i,
                  loaded_object_infos_.begin() + i + 1,
                  loaded_object_infos_.end());
    }
  }

  if (command_info.code == TPM_CC_ReadPublic) {
    // Only reading the public area cache if the command didn't need
    // authorization.
    if (command_info.handles.size() == 1 &&
        command_info.auth_session_handles.size() == 0) {
      auto iter =
          public_area_cache_.find(VirtualHandle(command_info.handles[0]));
      if (iter != public_area_cache_.end()) {
        return iter->second;
      }
    }
  }

  // Process all the input handles, e.g. map virtual handles.
  std::vector<TPM_HANDLE> updated_handles;
  for (auto handle : command_info.handles) {
    TPM_HANDLE tpm_handle;
    result = ProcessInputHandle(command_info, handle, &tpm_handle);
    if (result != TPM_RC_SUCCESS) {
      return CreateErrorResponse(result);
    }
    updated_handles.push_back(tpm_handle);
  }
  std::string updated_command = ReplaceHandles(command, updated_handles);
  // Make sure all the required sessions are loaded.
  for (const auto& handle : command_info.all_session_handles) {
    result = EnsureSessionIsLoaded(command_info, handle);
    if (result != TPM_RC_SUCCESS) {
      return CreateErrorResponse(result);
    }
  }
  // Send the |updated_command| to the next layer. Attempt to fix any actionable
  // warnings for up to kMaxCommandAttempts.
  std::string response;
  MessageInfo response_info;
  int attempts = 0;
  while (attempts++ < kMaxCommandAttempts) {
    response = next_transceiver_->SendCommandAndWait(updated_command);
    result = ParseResponse(command_info, response, &response_info);
    if (result != TPM_RC_SUCCESS) {
      return CreateErrorResponse(result);
    }
    if (!FixWarnings(command_info, response_info.code)) {
      // No actionable warnings were handled.
      break;
    }
  }
  if (response_info.code == TPM_RC_SUCCESS) {
    if (response_info.session_continued.size() !=
        command_info.auth_session_handles.size()) {
      LOG(WARNING) << "Session count mismatch!";
    }
    // Cleanup any sessions that were not continued.
    for (size_t i = 0; i < command_info.auth_session_handles.size(); ++i) {
      if (i < response_info.session_continued.size() &&
          !response_info.session_continued[i]) {
        CleanupFlushedHandle(command_info.auth_session_handles[i]);
      }
    }

    // Process all the output handles, which is loosely the inverse of the input
    // handle processing. E.g. virtualize handles.
    std::vector<TPM_HANDLE> replace_handles;
    for (auto handle : response_info.handles) {
      replace_handles.push_back(ProcessOutputHandle(handle, sender));
    }
    response = ReplaceHandles(response, replace_handles);
    if (command_info.code == TPM_CC_ReadPublic) {
      // Only caching the public area cache if the command didn't need
      // authorization.
      if (command_info.handles.size() == 1 &&
          command_info.auth_session_handles.size() == 0) {
        public_area_cache_[VirtualHandle(command_info.handles[0])] = response;
      }
    }
  }
  return response;
}

void ResourceManager::Suspend() {
  VLOG(1) << __func__;
  if (!suspended_) {
    suspended_timestamp_ = base::TimeTicks::Now();
    suspended_ = true;
    SaveAllContexts();
  }
}

void ResourceManager::Resume() {
  VLOG(1) << __func__ << " (suspended = " << suspended_ << ").";
  suspended_ = false;
}

bool ResourceManager::ChooseSessionToEvict(const MessageInfo& command_info,
                                           SessionHandle* session_to_evict) {
  const std::vector<SessionHandle>& sessions_to_retain =
      command_info.all_session_handles;
  // Build a list of candidates by excluding |sessions_to_retain|.
  std::vector<SessionHandle> candidates;
  for (const auto& [session, info] : session_handles_) {
    if (info.is_loaded &&
        std::find(sessions_to_retain.begin(), sessions_to_retain.end(),
                  session) == sessions_to_retain.end()) {
      candidates.push_back(session);
    }
  }
  if (candidates.empty()) {
    LOG(WARNING) << "No sessions to evict.";
    return false;
  }
  // Choose the candidate with the earliest |time_of_last_use|.
  auto oldest_iter = std::min_element(
      candidates.begin(), candidates.end(),
      [&](const SessionHandle& a, const SessionHandle& b) {
        // If the sender is different with the current command sender, we will
        // want to evict it first. (false is smaller than true)
        return std::make_tuple(command_info.sender == a.sender,
                               session_handles_[a].time_of_last_use) <
               std::make_tuple(command_info.sender == b.sender,
                               session_handles_[b].time_of_last_use);
      });
  *session_to_evict = *oldest_iter;
  return true;
}

void ResourceManager::CleanupFlushedHandle(
    const SessionHandle& flushed_handle) {
  if (!IsSessionHandle(flushed_handle.handle)) {
    LOG(WARNING) << "Flushing non-session handle with session handler";
    return;
  }

  auto iter = session_handles_.find(flushed_handle);
  if (iter == session_handles_.end()) {
    return;
  }
  // For session handles, remove the handle and any associated context data.
  session_handles_.erase(flushed_handle);
  VLOG(1) << "CLEANUP_SESSION: " << std::hex << flushed_handle.handle;
}

void ResourceManager::CleanupFlushedHandle(VirtualHandle flushed_handle) {
  if (!IsTransientObjectHandle(*flushed_handle)) {
    LOG(WARNING) << "Flushing non-object handle with object handler";
    return;
  }

  // For transient object handles, remove both the actual and virtual handles.
  if (unloaded_object_infos_.count(flushed_handle) > 0) {
    unloaded_object_infos_.erase(flushed_handle);
    public_area_cache_.erase(flushed_handle);
  } else {
    auto iter = FindLoadedObjectInfo(flushed_handle);
    if (iter != loaded_object_infos_.end()) {
      tpm_to_virtual_handle_.erase(iter->info.tpm_handle);
      loaded_object_infos_.erase(iter);
      public_area_cache_.erase(flushed_handle);
    }
  }
}

ResourceManager::VirtualHandle ResourceManager::CreateVirtualHandle() {
  VirtualHandle handle;
  do {
    handle = next_virtual_handle_;
    if (next_virtual_handle_ == VirtualHandle(kMaxVirtualHandle)) {
      LOG(WARNING) << "Re-using the object handle!";
      next_virtual_handle_ = VirtualHandle(TRANSIENT_FIRST);
    } else {
      next_virtual_handle_ = VirtualHandle(*next_virtual_handle_ + 1);
    }
  } while (unloaded_object_infos_.count(handle) > 0 ||
           FindLoadedObjectInfo(handle) != loaded_object_infos_.end());
  return handle;
}

TPM_RC ResourceManager::EnsureSessionIsLoaded(
    const MessageInfo& command_info, const SessionHandle& session_handle) {
  // A password authorization can skip all this.
  if (session_handle.handle == TPM_RS_PW) {
    return TPM_RC_SUCCESS;
  }

  auto handle_iter = session_handles_.find(session_handle);
  if (handle_iter == session_handles_.end()) {
    return MakeError(TPM_RC_HANDLE, FROM_HERE);
  }

  std::vector<SessionHandle> sessions_to_evict;
  for (const auto& [session, info] : session_handles_) {
    // The session conflicts with the session we want to ensure.
    if (info.is_loaded && session.handle == session_handle.handle &&
        session.sender != session_handle.sender) {
      sessions_to_evict.push_back(session);
    }
  }

  for (const SessionHandle& session : sessions_to_evict) {
    TPM_RC result = SaveContext(command_info, &session_handles_[session]);
    if (result == TPM_RC_SUCCESS) {
      continue;
    }
    LOG(WARNING) << "Failed to evict session: " << GetErrorString(result);

    result = factory_.GetTpm()->FlushContextSync(session.handle, nullptr);
    // Only clean up the handle if we flushed the handle successfully or the
    // handle is not exist.
    if (result == TPM_RC_SUCCESS || result == TPM_RC_HANDLE) {
      CleanupFlushedHandle(session);
      continue;
    }
    LOG(WARNING) << "Failed to flush session: " << GetErrorString(result);
  }

  HandleInfo& handle_info = handle_iter->second;
  if (!handle_info.is_loaded) {
    TPM_RC result = LoadContext(command_info, &handle_info);
    if (result != TPM_RC_SUCCESS) {
      return result;
    }
    VLOG(1) << "RELOAD_SESSION: " << std::hex << session_handle.handle;
  }
  handle_info.time_of_last_use = base::TimeTicks::Now();
  return TPM_RC_SUCCESS;
}

void ResourceManager::EvictOneObject(const MessageInfo& command_info,
                                     bool ignore_same_sender) {
  bool evicted = false;
  for (size_t i = 0; i < loaded_object_infos_.size(); i++) {
    auto& item = loaded_object_infos_[i];
    if (ignore_same_sender && item.info.sender == command_info.sender) {
      continue;
    }
    HandleInfo& info = item.info;
    if (std::find(command_info.handles.begin(), command_info.handles.end(),
                  *item.handle) != command_info.handles.end()) {
      continue;
    }
    TPM_RC result = SaveContext(command_info, &info);
    if (result == TPM_RC_REFERENCE_H0 || result == TPM_RC_HANDLE) {
      LOG(WARNING) << "Attempted to save conext for an unknown handle: "
                   << GetErrorString(result);
      // Ignore this result, because the context may be flushed by previous
      // command.
    } else if (result != TPM_RC_SUCCESS) {
      LOG(WARNING) << "Failed to save transient object: "
                   << GetErrorString(result);
      continue;
    }
    result = factory_.GetTpm()->FlushContextSync(info.tpm_handle, nullptr);
    if (result == TPM_RC_HANDLE) {
      LOG(WARNING) << "Attempted to flush conext for an unknown handle: "
                   << GetErrorString(result);
      // Ignore this result, because the context may be flushed by previous
      // command.
    } else if (result != TPM_RC_SUCCESS) {
      LOG(WARNING) << "Failed to evict transient object: "
                   << GetErrorString(result);
      continue;
    }
    VLOG(1) << "EVICT_OBJECT: " << std::hex << info.tpm_handle;
    info.is_loaded = false;
    tpm_to_virtual_handle_.erase(info.tpm_handle);
    unloaded_object_infos_.emplace(item.handle, std::move(item.info));
    loaded_object_infos_.erase(loaded_object_infos_.begin() + i);
    evicted = true;
    break;
  }
  if (!evicted && ignore_same_sender) {
    EvictOneObject(command_info, /*ignore_same_sender=*/false);
  }
}

void ResourceManager::EvictObjects(const MessageInfo& command_info) {
  size_t evict_num = 0;
  for (size_t i = 0; i < loaded_object_infos_.size(); i++) {
    if (evict_num) {
      loaded_object_infos_[i - evict_num] = std::move(loaded_object_infos_[i]);
    }
    auto& item = loaded_object_infos_[i - evict_num];
    HandleInfo& info = item.info;
    if (std::find(command_info.handles.begin(), command_info.handles.end(),
                  *item.handle) != command_info.handles.end()) {
      continue;
    }
    TPM_RC result = SaveContext(command_info, &info);
    if (result == TPM_RC_REFERENCE_H0 || result == TPM_RC_HANDLE) {
      LOG(WARNING) << "Attempted to save conext for an unknown handle: "
                   << GetErrorString(result);
      // Ignore this result, because the context may be flushed by previous
      // command.
    } else if (result != TPM_RC_SUCCESS) {
      LOG(WARNING) << "Failed to save transient object: "
                   << GetErrorString(result);
      continue;
    }
    result = factory_.GetTpm()->FlushContextSync(info.tpm_handle, nullptr);
    if (result == TPM_RC_HANDLE) {
      LOG(WARNING) << "Attempted to flush conext for an unknown handle: "
                   << GetErrorString(result);
      // Ignore this result, because the context may be flushed by previous
      // command.
    } else if (result != TPM_RC_SUCCESS) {
      LOG(WARNING) << "Failed to evict transient object: "
                   << GetErrorString(result);
      continue;
    }
    VLOG(1) << "EVICT_OBJECT: " << std::hex << info.tpm_handle;
    info.is_loaded = false;
    tpm_to_virtual_handle_.erase(info.tpm_handle);
    unloaded_object_infos_.emplace(item.handle, std::move(item.info));
    evict_num++;
  }
  loaded_object_infos_.resize(loaded_object_infos_.size() - evict_num);
}

void ResourceManager::EvictSession(const MessageInfo& command_info) {
  SessionHandle session_to_evict;
  if (!ChooseSessionToEvict(command_info, &session_to_evict)) {
    return;
  }
  HandleInfo& info = session_handles_[session_to_evict];
  TPM_RC result = SaveContext(command_info, &info);
  if (result != TPM_RC_SUCCESS) {
    LOG(WARNING) << "Failed to evict session: " << GetErrorString(result);

    // If we failed to evict a session, we should try to flush the session.
    // Otherwise there is no way to fix the TPM_RC_*_MEMORY issues.
    FlushSession(command_info);
  }
  VLOG(1) << "EVICT_SESSION: " << std::hex << session_to_evict.handle;
}

void ResourceManager::SaveAllContexts() {
  EvictObjects(MessageInfo());
  LOG(INFO) << "Finished saving contexts.";
}

std::vector<TPM_HANDLE> ResourceManager::ExtractHandlesFromBuffer(
    size_t number_of_handles, std::string* buffer) {
  std::vector<TPM_HANDLE> handles(number_of_handles);
  for (auto& handle : handles) {
    if (Parse_TPM_HANDLE(buffer, &handle, nullptr) != TPM_RC_SUCCESS) {
      LOG(WARNING) << "Failed to extract the expected number of handles";
      handles.clear();
      break;
    }
  }
  return handles;
}

void ResourceManager::FixContextGap(const MessageInfo& command_info) {
  std::vector<SessionHandle> sessions_to_ungap;
  for (const auto& item : session_handles_) {
    const HandleInfo& info = item.second;
    if (!info.is_loaded) {
      sessions_to_ungap.push_back(item.first);
    }
  }
  // Sort by |time_of_create|.
  std::sort(sessions_to_ungap.begin(), sessions_to_ungap.end(),
            [this](const SessionHandle& a, const SessionHandle& b) {
              return (session_handles_[a].time_of_create <
                      session_handles_[b].time_of_create);
            });
  for (auto handle : sessions_to_ungap) {
    HandleInfo& info = session_handles_[handle];
    // Loading and re-saving allows the TPM to assign a new context counter.
    std::string old_context_blob;
    Serialize_TPMS_CONTEXT(info.context, &old_context_blob);
    TPM_RC result = LoadContext(command_info, &info);
    if (result != TPM_RC_SUCCESS) {
      LOG(WARNING) << "Failed to un-gap session (load): "
                   << GetErrorString(result);
      continue;
    }
    result = SaveContext(command_info, &info);
    if (result != TPM_RC_SUCCESS) {
      LOG(WARNING) << "Failed to un-gap session (save): "
                   << GetErrorString(result);
      continue;
    }
  }
}

bool ResourceManager::FixWarnings(const MessageInfo& command_info,
                                  TPM_RC result) {
  if ((result & RC_WARN) == 0) {
    return false;
  }
  // This method can be called anytime without tracking whether the current
  // operation is already an attempt to fix a warning. All re-entrance issues
  // are dealt with here using the following rule: Never attempt to fix the same
  // warning twice.
  ScopedBool scoped_bool;
  if (!fixing_warnings_) {
    scoped_bool.Enable(&fixing_warnings_);
    warnings_already_seen_.clear();
  } else if (warnings_already_seen_.count(result) > 0) {
    return false;
  }
  warnings_already_seen_.insert(result);
  switch (result) {
    case TPM_RC_CONTEXT_GAP:
      FixContextGap(command_info);
      return true;
    case TPM_RC_OBJECT_MEMORY:
    case TPM_RC_OBJECT_HANDLES:
      EvictOneObject(command_info);
      return true;
    case TPM_RC_SESSION_MEMORY:
      EvictSession(command_info);
      return true;
    case TPM_RC_MEMORY:
      EvictObjects(command_info);
      EvictSession(command_info);
      return true;
    case TPM_RC_SESSION_HANDLES:
      FlushSession(command_info);
      return true;
  }
  return false;
}

void ResourceManager::FlushSession(const MessageInfo& command_info) {
  SessionHandle session_to_flush;
  LOG(WARNING) << "Resource manager needs to flush a session.";
  if (!ChooseSessionToEvict(command_info, &session_to_flush)) {
    return;
  }
  TPM_RC result =
      factory_.GetTpm()->FlushContextSync(session_to_flush.handle, nullptr);
  // Ignore it case is the session already been flushed.
  if (result != TPM_RC_SUCCESS && result != TPM_RC_HANDLE) {
    LOG(WARNING) << "Failed to flush session: " << GetErrorString(result);
    return;
  }
  CleanupFlushedHandle(session_to_flush);
}

bool ResourceManager::IsTransientObjectHandle(TPM_HANDLE handle) const {
  return ((handle & HR_RANGE_MASK) == HR_TRANSIENT);
}

bool ResourceManager::IsSessionHandle(TPM_HANDLE handle) const {
  return ((handle & HR_RANGE_MASK) == HR_HMAC_SESSION ||
          (handle & HR_RANGE_MASK) == HR_POLICY_SESSION);
}

TPM_RC ResourceManager::LoadContext(const MessageInfo& command_info,
                                    HandleInfo* handle_info) {
  if (handle_info->is_loaded) {
    LOG(ERROR) << __func__ << ": Attempted to load a loaded handle.";
    return TCTI_RC_BAD_CONTEXT;
  }
  TPM_RC result = TPM_RC_SUCCESS;
  int attempts = 0;
  while (attempts++ < kMaxCommandAttempts) {
    result = factory_.GetTpm()->ContextLoadSync(
        handle_info->context, &handle_info->tpm_handle, nullptr);
    if (!FixWarnings(command_info, result)) {
      break;
    }
  }
  if (result != TPM_RC_SUCCESS) {
    LOG(ERROR) << __func__
               << ": Failed to load context: " << GetErrorString(result);
    return result;
  }
  handle_info->is_loaded = true;
  return result;
}

TPM_RC ResourceManager::MakeError(TPM_RC tpm_error,
                                  const ::base::Location& location) {
  LOG(ERROR) << "ResourceManager::" << location.function_name() << ":"
             << location.line_number() << ": " << GetErrorString(tpm_error);
  return tpm_error + kResourceManagerTpmErrorBase;
}

TPM_RC ResourceManager::ParseCommand(const std::string& command,
                                     uint64_t sender,
                                     MessageInfo* command_info) {
  CHECK(command_info);
  std::string buffer = command;
  TPM_ST tag;
  TPM_RC result = Parse_TPM_ST(&buffer, &tag, nullptr);
  if (result != TPM_RC_SUCCESS) {
    return MakeError(result, FROM_HERE);
  }
  if (tag != TPM_ST_SESSIONS && tag != TPM_ST_NO_SESSIONS) {
    return MakeError(TPM_RC_TAG, FROM_HERE);
  }
  command_info->has_sessions = (tag == TPM_ST_SESSIONS);
  command_info->sender = sender;

  UINT32 size = 0;
  result = Parse_UINT32(&buffer, &size, nullptr);
  if (result != TPM_RC_SUCCESS) {
    return MakeError(result, FROM_HERE);
  }
  if (size != command.size()) {
    return MakeError(TPM_RC_SIZE, FROM_HERE);
  }

  result = Parse_TPM_CC(&buffer, &command_info->code, nullptr);
  if (result != TPM_RC_SUCCESS) {
    return MakeError(result, FROM_HERE);
  }

  if (IS_TPM_CC_VENDOR_CMD(command_info->code)) {
    // Vendor-specific commands must have no sessions & no handles.
    // All remaining data is parameter data.
    if (!command_info->has_sessions) {
      command_info->parameter_data = buffer;
      return TPM_RC_SUCCESS;
    }
  }

  if (!IS_TPM2_CMD(command_info->code)) {
    return MakeError(TPM_RC_COMMAND_CODE, FROM_HERE);
  }

  size_t number_of_handles = GetNumberOfRequestHandles(command_info->code);
  command_info->handles = ExtractHandlesFromBuffer(number_of_handles, &buffer);
  if (number_of_handles != command_info->handles.size()) {
    return MakeError(TPM_RC_SIZE, FROM_HERE);
  }
  for (const auto handle : command_info->handles) {
    if (IsSessionHandle(handle)) {
      command_info->all_session_handles.push_back(
          SessionHandle{.handle = handle, .sender = sender});
    }
  }

  if (command_info->has_sessions) {
    // Sessions exist, so we're expecting a valid authorization size value.
    UINT32 authorization_size = 0;
    result = Parse_UINT32(&buffer, &authorization_size, nullptr);
    if (result != TPM_RC_SUCCESS) {
      return MakeError(result, FROM_HERE);
    }
    if (buffer.size() < authorization_size ||
        authorization_size < kMinimumAuthorizationSize) {
      return MakeError(TPM_RC_SIZE, FROM_HERE);
    }
    // Move out the parameter bytes, leaving only the authorization section.
    command_info->parameter_data = buffer.substr(authorization_size);
    buffer.erase(authorization_size);
    // Parse as many authorization sessions as there are in the section.
    while (!buffer.empty()) {
      TPM_HANDLE handle;
      result = Parse_TPM_HANDLE(&buffer, &handle, nullptr);
      if (result != TPM_RC_SUCCESS) {
        return MakeError(result, FROM_HERE);
      }
      if (handle != TPM_RS_PW &&
          session_handles_.count(
              SessionHandle{.handle = handle, .sender = sender}) == 0) {
        return MakeError(TPM_RC_HANDLE, FROM_HERE);
      }
      TPM2B_NONCE nonce;
      result = Parse_TPM2B_NONCE(&buffer, &nonce, nullptr);
      if (result != TPM_RC_SUCCESS) {
        return MakeError(result, FROM_HERE);
      }
      BYTE attributes;
      result = Parse_BYTE(&buffer, &attributes, nullptr);
      if (result != TPM_RC_SUCCESS) {
        return MakeError(result, FROM_HERE);
      }
      TPM2B_DIGEST authorization;
      result = Parse_TPM2B_DIGEST(&buffer, &authorization, nullptr);
      if (result != TPM_RC_SUCCESS) {
        return MakeError(result, FROM_HERE);
      }
      command_info->auth_session_handles.push_back(
          SessionHandle{.handle = handle, .sender = sender});
      command_info->all_session_handles.push_back(
          SessionHandle{.handle = handle, .sender = sender});
      command_info->session_continued.push_back((attributes & 1) == 1);
    }
  } else {
    // No sessions, so all remaining data is parameter data.
    command_info->parameter_data = buffer;
  }
  return TPM_RC_SUCCESS;
}

TPM_RC ResourceManager::ParseResponse(const MessageInfo& command_info,
                                      const std::string& response,
                                      MessageInfo* response_info) {
  CHECK(response_info);
  std::string buffer = response;
  TPM_ST tag;
  TPM_RC result = Parse_TPM_ST(&buffer, &tag, nullptr);
  if (result != TPM_RC_SUCCESS) {
    return MakeError(result, FROM_HERE);
  }
  if (tag != TPM_ST_SESSIONS && tag != TPM_ST_NO_SESSIONS) {
    return MakeError(TPM_RC_TAG, FROM_HERE);
  }
  response_info->has_sessions = (tag == TPM_ST_SESSIONS);
  response_info->sender = command_info.sender;

  UINT32 size = 0;
  result = Parse_UINT32(&buffer, &size, nullptr);
  if (result != TPM_RC_SUCCESS) {
    return MakeError(result, FROM_HERE);
  }
  if (size != response.size()) {
    LOG(ERROR) << "Invalid response: size field = " << size
               << ", actual = " << response.size();
    return MakeError(TPM_RC_SIZE, FROM_HERE);
  }

  result = Parse_TPM_RC(&buffer, &response_info->code, nullptr);
  if (result != TPM_RC_SUCCESS) {
    return MakeError(result, FROM_HERE);
  }

  if (IS_TPM_CC_VENDOR_CMD(command_info.code)) {
    // Vendor-specific commands should have no sessions & no handles.
    // All remaining data is parameter data.
    response_info->parameter_data = buffer;
    return TPM_RC_SUCCESS;
  }

  if (response_info->code != TPM_RC_SUCCESS) {
    // We have received an error response for a standard (non vendor-specific)
    // TPM command. Error responses include only a header and error code. Return
    // immediately; don't attempt to parse handles or sessions.
    return TPM_RC_SUCCESS;
  }

  size_t number_of_handles = GetNumberOfResponseHandles(command_info.code);
  response_info->handles = ExtractHandlesFromBuffer(number_of_handles, &buffer);
  if (number_of_handles != response_info->handles.size()) {
    return MakeError(TPM_RC_SIZE, FROM_HERE);
  }
  if (response_info->has_sessions) {
    // Sessions exist, so we're expecting a valid parameter size value.
    UINT32 parameter_size = 0;
    result = Parse_UINT32(&buffer, &parameter_size, nullptr);
    if (result != TPM_RC_SUCCESS) {
      return MakeError(result, FROM_HERE);
    }
    if (buffer.size() < parameter_size) {
      return MakeError(TPM_RC_SIZE, FROM_HERE);
    }
    // Move out the parameter bytes, leaving only the authorization section.
    response_info->parameter_data = buffer.substr(0, parameter_size);
    buffer.erase(0, parameter_size);
    // Parse as many authorization sessions as there are in the section.
    while (!buffer.empty()) {
      TPM2B_NONCE nonce;
      result = Parse_TPM2B_NONCE(&buffer, &nonce, nullptr);
      if (result != TPM_RC_SUCCESS) {
        return MakeError(result, FROM_HERE);
      }
      BYTE attributes;
      result = Parse_BYTE(&buffer, &attributes, nullptr);
      if (result != TPM_RC_SUCCESS) {
        return MakeError(result, FROM_HERE);
      }
      TPM2B_DIGEST acknowledgement;
      result = Parse_TPM2B_DIGEST(&buffer, &acknowledgement, nullptr);
      if (result != TPM_RC_SUCCESS) {
        return MakeError(result, FROM_HERE);
      }
      response_info->session_continued.push_back((attributes & 1) == 1);
    }
  } else {
    // No sessions, so all remaining data is parameter data.
    response_info->parameter_data = buffer;
  }
  return TPM_RC_SUCCESS;
}

std::string ResourceManager::ProcessFlushContext(
    const std::string& command, const MessageInfo& command_info) {
  std::string buffer = command_info.parameter_data;
  // There must be exactly one handle in the parameters section.
  std::vector<TPM_HANDLE> handles = ExtractHandlesFromBuffer(1, &buffer);
  if (handles.size() != 1) {
    return CreateErrorResponse(MakeError(TPM_RC_SIZE, FROM_HERE));
  }
  TPM_HANDLE raw_handle = handles[0];
  TPM_HANDLE actual_handle = raw_handle;
  if (IsTransientObjectHandle(raw_handle)) {
    VirtualHandle handle = VirtualHandle(raw_handle);
    if (unloaded_object_infos_.find(handle) != unloaded_object_infos_.end()) {
      // The handle wasn't loaded so no need to bother the TPM.
      CleanupFlushedHandle(handle);
      return CreateErrorResponse(TPM_RC_SUCCESS);
    }
    auto iter = FindLoadedObjectInfo(handle);
    if (iter == loaded_object_infos_.end()) {
      return CreateErrorResponse(MakeError(TPM_RC_HANDLE, FROM_HERE));
    }
    actual_handle = iter->info.tpm_handle;
  } else if (IsSessionHandle(raw_handle)) {
    SessionHandle handle{.handle = raw_handle, .sender = command_info.sender};
    auto iter = session_handles_.find(handle);
    if (iter == session_handles_.end()) {
      // The handle doesn't exist so no need to bother the TPM.
      CleanupFlushedHandle(handle);
      return CreateErrorResponse(TPM_RC_SUCCESS);
    }
  }

  // Send a command with the original header but with |actual_handle| as the
  // parameter.
  std::string handle_blob;
  Serialize_TPM_HANDLE(actual_handle, &handle_blob);
  std::string updated_command =
      command.substr(0, kMessageHeaderSize) + handle_blob;
  // No need to loop and fix warnings, there are no actionable warnings on when
  // flushing context.
  std::string response = next_transceiver_->SendCommandAndWait(updated_command);
  MessageInfo response_info;
  TPM_RC result = ParseResponse(command_info, response, &response_info);
  if (result != TPM_RC_SUCCESS) {
    return CreateErrorResponse(result);
  }

  // Cleanup the handle locally even if the TPM did not recognize it.
  if (response_info.code == TPM_RC_SUCCESS ||
      response_info.code == TPM_RC_HANDLE) {
    if (IsTransientObjectHandle(raw_handle)) {
      CleanupFlushedHandle(VirtualHandle(raw_handle));
    } else if (IsSessionHandle(raw_handle)) {
      CleanupFlushedHandle(
          SessionHandle{.handle = raw_handle, .sender = command_info.sender});
    }
  }
  return response;
}

TPM_RC ResourceManager::ProcessInputHandle(const MessageInfo& command_info,
                                           TPM_HANDLE input_handle,
                                           TPM_HANDLE* actual_handle) {
  // Only transient object handles are virtualized.
  if (!IsTransientObjectHandle(input_handle)) {
    *actual_handle = input_handle;
    return TPM_RC_SUCCESS;
  }

  VirtualHandle virtual_handle = VirtualHandle(input_handle);

  auto loaded_iter = FindLoadedObjectInfo(virtual_handle);
  if (loaded_iter != loaded_object_infos_.end()) {
    *actual_handle = loaded_iter->info.tpm_handle;
  } else {
    auto unloaded_iter = unloaded_object_infos_.find(virtual_handle);
    if (unloaded_iter != unloaded_object_infos_.end()) {
      HandleInfo& handle_info = unloaded_iter->second;
      TPM_RC result = LoadContext(command_info, &handle_info);
      if (result != TPM_RC_SUCCESS) {
        return result;
      }
      tpm_to_virtual_handle_[handle_info.tpm_handle] = virtual_handle;
      loaded_object_infos_.emplace_back(
          ObjectInfo{.handle = unloaded_iter->first,
                     .info = std::move(unloaded_iter->second)});
      VLOG(1) << "RELOAD_OBJECT: " << std::hex << input_handle;
      *actual_handle = handle_info.tpm_handle;
      unloaded_object_infos_.erase(unloaded_iter);
    } else {
      return MakeError(TPM_RC_HANDLE, FROM_HERE);
    }
  }
  VLOG(1) << "INPUT_HANDLE_REPLACE: " << std::hex << input_handle << " -> "
          << std::hex << *actual_handle;
  return TPM_RC_SUCCESS;
}

TPM_HANDLE ResourceManager::ProcessOutputHandle(TPM_HANDLE handle,
                                                uint64_t sender) {
  // Track, but do not virtualize, session handles.
  if (IsSessionHandle(handle)) {
    SessionHandle session_handle{.handle = handle, .sender = sender};
    auto session_handle_iter = session_handles_.find(session_handle);
    if (session_handle_iter == session_handles_.end()) {
      HandleInfo new_handle_info;
      new_handle_info.Init(handle, sender);
      session_handles_[session_handle] = new_handle_info;
      VLOG(1) << "OUTPUT_HANDLE_NEW_SESSION: " << std::hex << handle;
    }
    return handle;
  }
  // Only transient object handles are virtualized.
  if (!IsTransientObjectHandle(handle)) {
    return handle;
  }
  auto virtual_handle_iter = tpm_to_virtual_handle_.find(handle);
  if (virtual_handle_iter == tpm_to_virtual_handle_.end()) {
    VirtualHandle new_virtual_handle = CreateVirtualHandle();
    HandleInfo new_handle_info;
    new_handle_info.Init(handle, sender);
    loaded_object_infos_.emplace_back(ObjectInfo{
        .handle = new_virtual_handle, .info = std::move(new_handle_info)});
    tpm_to_virtual_handle_[handle] = new_virtual_handle;
    VLOG(1) << "OUTPUT_HANDLE_NEW_VIRTUAL: " << std::hex << handle << " -> "
            << std::hex << *new_virtual_handle;
    return *new_virtual_handle;
  }
  VLOG(1) << "OUTPUT_HANDLE_REPLACE: " << std::hex << handle << " -> "
          << std::hex << *virtual_handle_iter->second;
  return *virtual_handle_iter->second;
}

std::string ResourceManager::ReplaceHandles(
    const std::string& message, const std::vector<TPM_HANDLE>& new_handles) {
  std::string handles_blob;
  for (auto handle : new_handles) {
    CHECK_EQ(Serialize_TPM_HANDLE(handle, &handles_blob), TPM_RC_SUCCESS);
  }
  std::string mutable_message = message;
  CHECK_GE(message.size(), kMessageHeaderSize + handles_blob.size());
  return mutable_message.replace(kMessageHeaderSize, handles_blob.size(),
                                 handles_blob);
}

TPM_RC ResourceManager::SaveContext(const MessageInfo& command_info,
                                    HandleInfo* handle_info) {
  if (!handle_info->is_loaded) {
    LOG(ERROR) << __func__ << ": Attempted to save an unloaded handle.";
    return TCTI_RC_BAD_CONTEXT;
  }

  // We already saved the context of this transient object, we don't need to
  // save it again.
  if (handle_info->has_context &&
      IsTransientObjectHandle(handle_info->tpm_handle)) {
    return TPM_RC_SUCCESS;
  }

  TPM_RC result = TPM_RC_SUCCESS;
  int attempts = 0;
  while (attempts++ < kMaxCommandAttempts) {
    std::string tpm_handle_name;
    Serialize_TPM_HANDLE(handle_info->tpm_handle, &tpm_handle_name);
    result = factory_.GetTpm()->ContextSaveSync(handle_info->tpm_handle,
                                                tpm_handle_name,
                                                &handle_info->context, nullptr);
    if (!FixWarnings(command_info, result)) {
      break;
    }
  }
  if (result != TPM_RC_SUCCESS) {
    LOG(ERROR) << __func__
               << ": Failed to save context: " << GetErrorString(result);
    return result;
  }

  handle_info->has_context = true;

  // We only mark it as loaded when it is a session handle.
  if (IsSessionHandle(handle_info->tpm_handle)) {
    handle_info->is_loaded = false;
  }
  return result;
}

std::vector<ResourceManager::ObjectInfo>::iterator
ResourceManager::FindLoadedObjectInfo(VirtualHandle handle) {
  return std::find_if(
      loaded_object_infos_.begin(), loaded_object_infos_.end(),
      [handle](const auto& cmp) { return cmp.handle == handle; });
}

ResourceManager::HandleInfo::HandleInfo() : is_loaded(false), tpm_handle(0) {
  memset(&context, 0, sizeof(TPMS_CONTEXT));
}

void ResourceManager::HandleInfo::Init(TPM_HANDLE handle, uint64_t cmd_sender) {
  tpm_handle = handle;
  is_loaded = true;
  has_context = false;
  time_of_create = base::TimeTicks::Now();
  time_of_last_use = base::TimeTicks::Now();
  sender = cmd_sender;
}

}  // namespace trunks
