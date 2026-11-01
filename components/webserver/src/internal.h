#pragma once
#include <string>

#include "cJSON.h"
#include "esp_http_server.h"

namespace webserver {

// Route flags.
enum : unsigned {
    R_PUBLIC = 0,
    R_AUTH = 1,        // valid session required
    R_SETUP_ONLY = 2,  // only while not provisioned (no login needed)
};

void register_api(httpd_handle_t h);
void add_route(httpd_handle_t h, const char* uri, httpd_method_t method,
               esp_err_t (*fn)(httpd_req_t*), unsigned flags);

// Helpers.
esp_err_t send_json(httpd_req_t* req, cJSON* root, const char* status = "200 OK");  // frees root
esp_err_t send_error(httpd_req_t* req, const char* status, const std::string& msg);
esp_err_t send_ok(httpd_req_t* req);
// Reads a JSON body (max `limit` bytes). Sends the error response itself and
// returns nullptr on failure.
cJSON* read_json(httpd_req_t* req, size_t limit = 2048);
std::string json_str(cJSON* obj, const char* key, bool* present = nullptr);
uint32_t client_ip(httpd_req_t* req);
std::string ip_to_str(uint32_t ip);

// Sessions and login.
bool session_valid(httpd_req_t* req);
std::string session_create(uint32_t ip);
void session_drop(httpd_req_t* req);
void sessions_drop_all_except(httpd_req_t* req);
// Returns seconds the client must wait (0 = may try now).
int login_wait_s(uint32_t ip);
void login_result(uint32_t ip, bool ok);

// Assets.
esp_err_t serve_asset(httpd_req_t* req, const char* path);
bool asset_exists(const char* path);

}  // namespace webserver
