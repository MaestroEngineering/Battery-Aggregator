#pragma once
namespace selftest {
// Starts the post-boot self-test. For an image in PENDING_VERIFY state (first
// boot after OTA) it marks the app valid only when all checks pass, and rolls
// back to the previous image if that has not happened within
// CONFIG_BMS_SELFTEST_TIMEOUT_S.
void start();
}  // namespace selftest
