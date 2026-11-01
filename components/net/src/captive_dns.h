#pragma once
// Minimal DNS server for the setup AP: answers every A query with the AP's
// own address so phones open the setup page as a captive portal.
namespace net {
void captive_dns_start();
void captive_dns_stop();
}  // namespace net
