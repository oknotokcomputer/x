// Copyright 2013 The ChromiumOS Authors
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file.

#include "update_engine/common/constants.h"

namespace chromeos_update_engine {

const char kExclusionPrefsSubDir[] = "exclusion";

const char kDlcPrefsSubDir[] = "dlc";

const char kMiniOSPrefsSubDir[] = "minios";

const char kPowerwashSafePrefsSubDirectory[] = "update_engine/prefs";

const char kPrefsSubDirectory[] = "prefs";

const char kStatefulPartition[] = "/mnt/stateful_partition";

const char kPostinstallDefaultScript[] = "postinst";

// Constants defining keys for the persisted state of update engine.
const char kPrefsAllowRepeatedUpdates[] = "allow-repeated-updates";
const char kPrefsAttemptInProgress[] = "attempt-in-progress";
const char kPrefsBackoffExpiryTime[] = "backoff-expiry-time";
const char kPrefsBootId[] = "boot-id";
const char kPrefsConsecutiveUpdateCount[] = "consecutive-update-count";
const char kPrefsCurrentBytesDownloaded[] = "current-bytes-downloaded";
const char kPrefsCurrentResponseSignature[] = "current-response-signature";
const char kPrefsCurrentUrlFailureCount[] = "current-url-failure-count";
const char kPrefsCurrentUrlIndex[] = "current-url-index";
const char kPrefsDailyMetricsLastReportedAt[] =
    "daily-metrics-last-reported-at";
const char kPrefsDeltaUpdateFailures[] = "delta-update-failures";
const char kPrefsMarketSegmentDisabled[] = "market-segment-disabled";
const char kPrefsDynamicPartitionMetadataUpdated[] =
    "dynamic-partition-metadata-updated";
const char kPrefsFullPayloadAttemptNumber[] = "full-payload-attempt-number";
const char kPrefsInstallDateDays[] = "install-date-days";
const char kPrefsLastActivePingDay[] = "last-active-ping-day";
const char kPrefsLastRollCallPingDay[] = "last-roll-call-ping-day";
const char kPrefsManifestMetadataSize[] = "manifest-metadata-size";
const char kPrefsManifestSignatureSize[] = "manifest-signature-size";
const char kPrefsMetricsAttemptLastReportingTime[] =
    "metrics-attempt-last-reporting-time";
const char kPrefsMetricsCheckLastReportingTime[] =
    "metrics-check-last-reporting-time";
const char kPrefsNoIgnoreBackoff[] = "no-ignore-backoff";
const char kPrefsNumReboots[] = "num-reboots";
const char kPrefsNumResponsesSeen[] = "num-responses-seen";
const char kPrefsOmahaCohort[] = "omaha-cohort";
const char kPrefsOmahaCohortHint[] = "omaha-cohort-hint";
const char kPrefsOmahaCohortName[] = "omaha-cohort-name";
const char kPrefsOmahaEolDate[] = "omaha-eol-date";
const char kPrefsOmahaExtendedDate[] = "omaha-extended-date";
const char kPrefsOmahaExtendedOptInRequired[] =
    "omaha-extended-opt-in-required";
const char kPrefsP2PEnabled[] = "p2p-enabled";
const char kPrefsP2PFirstAttemptTimestamp[] = "p2p-first-attempt-timestamp";
const char kPrefsP2PNumAttempts[] = "p2p-num-attempts";
const char kPrefsPayloadAttemptNumber[] = "payload-attempt-number";
const char kPrefsTestUpdateCheckIntervalTimeout[] =
    "test-update-check-interval-timeout";
// Keep |kPrefsPingActive| in sync with |kDlcMetadataFilePingActive| in
// dlcservice.
const char kPrefsPingActive[] = "active";
const char kPrefsPingLastActive[] = "date_last_active";
const char kPrefsPingLastRollcall[] = "date_last_rollcall";
const char kPrefsLastFp[] = "last-fp";
const char kPrefsPostInstallSucceeded[] = "post-install-succeeded";
const char kPrefsPreviousVersion[] = "previous-version";
const char kPrefsResumedUpdateFailures[] = "resumed-update-failures";
const char kPrefsRollbackHappened[] = "rollback-happened";
const char kPrefsRollbackVersion[] = "rollback-version";
const char kPrefsChannelOnSlotPrefix[] = "channel-on-slot-";
const char kPrefsSystemUpdatedMarker[] = "system-updated-marker";
const char kPrefsTargetVersionAttempt[] = "target-version-attempt";
const char kPrefsTargetVersionInstalledFrom[] = "target-version-installed-from";
const char kPrefsTargetVersionUniqueId[] = "target-version-unique-id";
const char kPrefsTotalBytesDownloaded[] = "total-bytes-downloaded";
const char kPrefsUpdateCheckCount[] = "update-check-count";
const char kPrefsUpdateCheckResponseHash[] = "update-check-response-hash";
const char kPrefsUpdateCompletedBootTime[] = "update-completed-boot-time";
const char kPrefsUpdateCompletedOnBootId[] = "update-completed-on-boot-id";
const char kPrefsUpdateDurationUptime[] = "update-duration-uptime";
const char kPrefsUpdateFirstSeenAt[] = "update-first-seen-at";
const char kPrefsUpdateOverCellularPermission[] =
    "update-over-cellular-permission";
const char kPrefsUpdateOverCellularTargetVersion[] =
    "update-over-cellular-target-version";
const char kPrefsUpdateOverCellularTargetSize[] =
    "update-over-cellular-target-size";
const char kPrefsUpdateServerCertificate[] = "update-server-cert";
const char kPrefsUpdateStateNextDataLength[] = "update-state-next-data-length";
const char kPrefsUpdateStateNextDataOffset[] = "update-state-next-data-offset";
const char kPrefsUpdateStateNextOperation[] = "update-state-next-operation";
const char kPrefsUpdateStatePayloadIndex[] = "update-state-payload-index";
const char kPrefsUpdateStateSHA256Context[] = "update-state-sha-256-context";
const char kPrefsUpdateStateSignatureBlob[] = "update-state-signature-blob";
const char kPrefsUpdateStateSignedSHA256Context[] =
    "update-state-signed-sha-256-context";
const char kPrefsUpdateBootTimestampStart[] = "update-boot-timestamp-start";
const char kPrefsUpdateTimestampStart[] = "update-timestamp-start";
const char kPrefsUrlSwitchCount[] = "url-switch-count";
const char kPrefsVerityWritten[] = "verity-written";
const char kPrefsWallClockScatteringWaitPeriod[] = "wall-clock-wait-period";
const char kPrefsWallClockStagingWaitPeriod[] =
    "wall-clock-staging-wait-period";
const char kPrefsManifestBytes[] = "manifest-bytes";
const char kPrefsConsumerAutoUpdateDisabled[] = "consumer-auto-update-disabled";
const char kPrefsDeferredUpdateCompleted[] = "deferred-update-completed";

// These four fields are generated by scripts/brillo_update_payload.
const char kPayloadPropertyFileSize[] = "FILE_SIZE";
const char kPayloadPropertyFileHash[] = "FILE_HASH";
const char kPayloadPropertyMetadataSize[] = "METADATA_SIZE";
const char kPayloadPropertyMetadataHash[] = "METADATA_HASH";
// The Authorization: HTTP header to be sent when downloading the payload.
const char kPayloadPropertyAuthorization[] = "AUTHORIZATION";
// The User-Agent HTTP header to be sent when downloading the payload.
const char kPayloadPropertyUserAgent[] = "USER_AGENT";
// Set "POWERWASH=1" to powerwash (factory data reset) the device after
// applying the update.
const char kPayloadPropertyPowerwash[] = "POWERWASH";
// The network id to pass to android_setprocnetwork before downloading.
// This can be used to zero-rate OTA traffic by sending it over the correct
// network.
const char kPayloadPropertyNetworkId[] = "NETWORK_ID";
// Set "SWITCH_SLOT_ON_REBOOT=0" to skip marking the updated partitions active.
// The default is 1 (always switch slot if update succeeded).
const char kPayloadPropertySwitchSlotOnReboot[] = "SWITCH_SLOT_ON_REBOOT";
// Set "RUN_POST_INSTALL=0" to skip running optional post install.
// The default is 1 (always run post install).
const char kPayloadPropertyRunPostInstall[] = "RUN_POST_INSTALL";

const char kOmahaUpdaterVersion[] = "0.1.0.0";

// X-Goog-Update headers.
const char kXGoogleUpdateInteractivity[] = "X-Goog-Update-Interactivity";
const char kXGoogleUpdateAppId[] = "X-Goog-Update-AppId";
const char kXGoogleUpdateUpdater[] = "X-Goog-Update-Updater";
const char kXGoogleUpdateSessionId[] = "X-Goog-SessionId";

}  // namespace chromeos_update_engine
