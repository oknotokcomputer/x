// Copyright 2021 The Chromium OS Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#ifndef RMAD_RMAD_INTERFACE_IMPL_H_
#define RMAD_RMAD_INTERFACE_IMPL_H_

#include "rmad/rmad_interface.h"

#include <algorithm>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include <base/callback.h>
#include <base/memory/scoped_refptr.h>
#include <base/timer/timer.h>

#include "rmad/constants.h"
#include "rmad/daemon_callback.h"
#include "rmad/metrics/metrics_utils.h"
#include "rmad/state_handler/state_handler_manager.h"
#include "rmad/system/power_manager_client.h"
#include "rmad/system/runtime_probe_client.h"
#include "rmad/system/shill_client.h"
#include "rmad/system/tpm_manager_client.h"
#include "rmad/utils/cmd_utils.h"
#include "rmad/utils/json_store.h"

namespace rmad {

class RmadInterfaceImpl final : public RmadInterface {
 public:
  static constexpr base::TimeDelta kTestModeMonitorInterval = base::Seconds(2);

  RmadInterfaceImpl();
  // Used to inject mocked |json_store_|, |state_handler_manager_|,
  // |runtime_probe_client_|, |shill_client_|, |tpm_manager_client_|,
  // |power_manager_client_|, |cmd_utils_| and |metrics_utils_|.
  RmadInterfaceImpl(scoped_refptr<JsonStore> json_store,
                    std::unique_ptr<StateHandlerManager> state_handler_manager,
                    std::unique_ptr<RuntimeProbeClient> runtime_probe_client,
                    std::unique_ptr<ShillClient> shill_client,
                    std::unique_ptr<TpmManagerClient> tpm_manager_client,
                    std::unique_ptr<PowerManagerClient> power_manager_client,
                    std::unique_ptr<CmdUtils> cmd_utils_,
                    std::unique_ptr<MetricsUtils> metrics_utils);
  RmadInterfaceImpl(const RmadInterfaceImpl&) = delete;
  RmadInterfaceImpl& operator=(const RmadInterfaceImpl&) = delete;

  ~RmadInterfaceImpl() override = default;

  bool SetUp(scoped_refptr<DaemonCallback> daemon_callback) override;

  RmadState::StateCase GetCurrentStateCase() override {
    return current_state_case_;
  }
  void TryTransitionNextStateFromCurrentState() override;
  void GetCurrentState(GetStateCallback callback) override;
  void TransitionNextState(const TransitionNextStateRequest& request,
                           GetStateCallback callback) override;
  void TransitionPreviousState(GetStateCallback callback) override;
  void AbortRma(AbortRmaCallback callback) override;
  void GetLog(GetLogCallback callback) override;
  void SaveLog(const std::string& diagnostics_log_path,
               SaveLogCallback callback) override;
  void RecordBrowserActionMetric(
      const RecordBrowserActionMetricRequest& browser_action,
      RecordBrowserActionMetricCallback callback) override;
  bool CanAbort() const override { return can_abort_; }

  void SetTestMode() { test_mode_ = true; }

 private:
  void InitializeExternalUtils(scoped_refptr<DaemonCallback> daemon_callback);
  bool WaitForServices();

  // Wrapper to trigger D-Bus callbacks.
  template <typename ReplyProtobufType>
  void ReplyCallback(
      base::OnceCallback<void(const ReplyProtobufType&, bool)> callback,
      const ReplyProtobufType reply) {
    // Quit the daemon if we are no longer in RMA, or the current state requires
    // to restart the daemon.
    bool quit_daemon = false;
    if (reply.error() == RMAD_ERROR_RMA_NOT_REQUIRED ||
        std::find(kQuitDaemonStates.begin(), kQuitDaemonStates.end(),
                  current_state_case_) != kQuitDaemonStates.end()) {
      quit_daemon = true;
    }
    std::move(callback).Run(reply, quit_daemon);
  }

  // Get and initialize the state handler for |state case|, and store it to
  // |state_handler|. If there's no state handler for |state_case|, or the
  // initialization fails, return an error, and |state_handler| is unchanged.
  RmadErrorCode GetInitializedStateHandler(
      RmadState::StateCase state_case,
      scoped_refptr<BaseStateHandler>* state_handler) const;

  GetStateReply GetCurrentStateInternal();
  GetStateReply TransitionNextStateInternal(
      const TransitionNextStateRequest& request, bool try_at_boot);
  GetStateReply TransitionPreviousStateInternal();

  // Store the state history to |json_store_|.
  bool StoreStateHistory();

  // Check if it's allowed to go back to the previous state.
  bool CanGoBack() const;

  // Monitor files created by fake state handlers in test mode.
  void ClearTestRequests();
  void MonitorTestRequests();

  // External utilities.
  scoped_refptr<JsonStore> json_store_;
  std::unique_ptr<StateHandlerManager> state_handler_manager_;
  std::unique_ptr<RuntimeProbeClient> runtime_probe_client_;
  std::unique_ptr<ShillClient> shill_client_;
  std::unique_ptr<TpmManagerClient> tpm_manager_client_;
  std::unique_ptr<PowerManagerClient> power_manager_client_;
  std::unique_ptr<CmdUtils> cmd_utils_;
  std::unique_ptr<MetricsUtils> metrics_utils_;

  // Internal states.
  bool external_utils_initialized_;
  RmadState::StateCase current_state_case_;
  std::vector<RmadState::StateCase> state_history_;
  bool can_abort_;

  // Test mode. Use fake state handlers.
  bool test_mode_;
  base::RepeatingTimer test_mode_monitor_timer_;
};

}  // namespace rmad

#endif  // RMAD_RMAD_INTERFACE_IMPL_H_
