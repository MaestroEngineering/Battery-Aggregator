#include "hamqtt/hamqtt.h"

#include <stdio.h>
#include <string.h>

#include <string>

#include "agg/agg.h"
#include "cJSON.h"
#include "config/config.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_timer.h"
#include "heapmon/heapmon.h"
#include "discovery_json.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "mqtt_client.h"
#include "net/net.h"

namespace hamqtt {
namespace {

const char* TAG = "mqtt";
TaskHandle_t s_task = nullptr;
esp_mqtt_client_handle_t s_client = nullptr;
volatile bool s_restart = true;
volatile bool s_connected = false;
volatile bool s_need_discovery = false;
const char* volatile s_state = "off";
uint32_t s_published = 0, s_events_sent = 0;
int64_t s_last_pub_us = 0;
cfg::Settings s_cfg;
std::string s_node;  // bmsagg_<mac>
agg::Snapshot* s_snap_p = nullptr;  // heap, allocated when the task starts
#define s_snap (*s_snap_p)

struct Sensor {
    const char* path;   // under <base>/
    const char* name;
    const char* unit;   // nullptr = none
    const char* dev_class;
    const char* state_class;  // "measurement", "total" or nullptr (text)
};
const Sensor kSensors[] = {
    {"state", "Gateway state", nullptr, nullptr, nullptr},
    {"heap_free", "Free heap", "B", "data_size", "measurement"},
    {"heap_largest", "Largest free heap block", "B", "data_size", "measurement"},
    {"merged/cvl", "Charge voltage limit", "V", "voltage", "measurement"},
    {"merged/ccl", "Charge current limit", "A", "current", "measurement"},
    {"merged/dcl", "Discharge current limit", "A", "current", "measurement"},
    {"merged/dvl", "Discharge voltage limit", "V", "voltage", "measurement"},
    {"merged/soc", "SOC", "%", "battery", "measurement"},
    {"merged/soh", "SOH", "%", nullptr, "measurement"},
    {"merged/voltage", "Voltage", "V", "voltage", "measurement"},
    {"merged/current", "Current", "A", "current", "measurement"},
    {"merged/temperature", "Temperature", "°C", "temperature", "measurement"},
    {"merged/capacity", "Capacity", "Ah", nullptr, "measurement"},
    {"merged/energy_charged", "Energy charged", "kWh", "energy", "total"},
    {"merged/energy_discharged", "Energy discharged", "kWh", "energy", "total"},
    {"battery_a/state", "A state", nullptr, nullptr, nullptr},
    {"battery_a/soc", "A SOC", "%", "battery", "measurement"},
    {"battery_a/soh", "A SOH", "%", nullptr, "measurement"},
    {"battery_a/voltage", "A voltage", "V", "voltage", "measurement"},
    {"battery_a/current", "A current", "A", "current", "measurement"},
    {"battery_a/temperature", "A temperature", "°C", "temperature", "measurement"},
    {"battery_a/cvl", "A CVL", "V", "voltage", "measurement"},
    {"battery_a/ccl", "A CCL", "A", "current", "measurement"},
    {"battery_a/dcl", "A DCL", "A", "current", "measurement"},
    {"battery_a/capacity", "A capacity", "Ah", nullptr, "measurement"},
    {"battery_a/cell_min", "A cell min", "mV", "voltage", "measurement"},
    {"battery_a/cell_max", "A cell max", "mV", "voltage", "measurement"},
    {"battery_a/energy_charged", "A energy charged", "kWh", "energy", "total"},
    {"battery_a/energy_discharged", "A energy discharged", "kWh", "energy", "total"},
    {"battery_a/pack2_current", "A pack 2 current", "A", "current", "measurement"},
    {"battery_a/pack2_soc", "A pack 2 SOC", "%", "battery", "measurement"},
    {"battery_a/pack2_ccl", "A pack 2 CCL", "A", "current", "measurement"},
    {"battery_a/pack2_energy_charged", "A pack 2 energy charged", "kWh", "energy", "total"},
    {"battery_a/pack2_energy_discharged", "A pack 2 energy discharged", "kWh", "energy", "total"},
    {"battery_b/state", "B state", nullptr, nullptr, nullptr},
    {"battery_b/soc", "B SOC", "%", "battery", "measurement"},
    {"battery_b/soh", "B SOH", "%", nullptr, "measurement"},
    {"battery_b/voltage", "B voltage", "V", "voltage", "measurement"},
    {"battery_b/current", "B current", "A", "current", "measurement"},
    {"battery_b/temperature", "B temperature", "°C", "temperature", "measurement"},
    {"battery_b/cvl", "B CVL", "V", "voltage", "measurement"},
    {"battery_b/ccl", "B CCL", "A", "current", "measurement"},
    {"battery_b/dcl", "B DCL", "A", "current", "measurement"},
    {"battery_b/alarm_bits", "B alarm bits", nullptr, nullptr, nullptr},
    // 0.1.7
    {"battery_a/cell_spread", "A cell spread", "mV", "voltage", "measurement"},
    {"battery_a/energy_in", "A energy in (V x I)", "kWh", "energy", "total"},
    {"battery_a/energy_out", "A energy out (V x I)", "kWh", "energy", "total"},
    {"battery_a/pack1_current", "A pack 1 current", "A", "current", "measurement"},
    {"battery_a/pack1_soc", "A pack 1 SOC", "%", "battery", "measurement"},
    {"battery_a/pack1_ccl", "A pack 1 CCL", "A", "current", "measurement"},
    {"battery_a/pack1_dcl", "A pack 1 DCL", "A", "current", "measurement"},
    {"battery_a/pack1_energy_in", "A pack 1 energy in", "kWh", "energy", "total"},
    {"battery_a/pack1_energy_out", "A pack 1 energy out", "kWh", "energy", "total"},
    {"battery_a/pack2_dcl", "A pack 2 DCL", "A", "current", "measurement"},
    {"battery_a/pack2_energy_in", "A pack 2 energy in", "kWh", "energy", "total"},
    {"battery_a/pack2_energy_out", "A pack 2 energy out", "kWh", "energy", "total"},
    {"battery_a/rejected_frames", "A rejected frames", nullptr, nullptr, "total_increasing"},
    {"battery_a/guard_charge", "A over-current guard charge", nullptr, nullptr, nullptr},
    {"battery_a/guard_discharge", "A over-current guard discharge", nullptr, nullptr, nullptr},
    {"battery_a/pack2_guard_charge", "A pack 2 over-current guard charge", nullptr, nullptr, nullptr},
    {"battery_a/pack2_guard_discharge", "A pack 2 over-current guard discharge", nullptr, nullptr, nullptr},
    {"battery_b/capacity", "B capacity", "Ah", nullptr, "measurement"},
    {"battery_b/cell_min", "B cell min", "mV", "voltage", "measurement"},
    {"battery_b/cell_max", "B cell max", "mV", "voltage", "measurement"},
    {"battery_b/cell_spread", "B cell spread", "mV", "voltage", "measurement"},
    {"battery_b/charge_enable", "B charge enabled", nullptr, nullptr, nullptr},
    {"battery_b/discharge_enable", "B discharge enabled", nullptr, nullptr, nullptr},
    {"battery_b/energy_in", "B energy in (V x I)", "kWh", "energy", "total"},
    {"battery_b/energy_out", "B energy out (V x I)", "kWh", "energy", "total"},
    {"battery_b/rejected_frames", "B rejected frames", nullptr, nullptr, "total_increasing"},
    {"battery_b/guard_charge", "B over-current guard charge", nullptr, nullptr, nullptr},
    {"battery_b/guard_discharge", "B over-current guard discharge", nullptr, nullptr, nullptr},
};

std::string topic(const char* path) { return s_cfg.mqtt_base + "/" + path; }

void pub(const std::string& t, const char* payload, bool retain) {
    if (!s_client || !s_connected) return;
    if (esp_mqtt_client_publish(s_client, t.c_str(), payload, 0, 0, retain) >= 0) {
        s_published++;
        s_last_pub_us = esp_timer_get_time();
    }
}
void pubf(const char* path, float v, int decimals) {
    char b[24];
    snprintf(b, sizeof(b), "%.*f", decimals, (double)v);
    pub(topic(path), b, s_cfg.mqtt_retain);
}

void publish_discovery() {
    const esp_app_desc_t* d = esp_app_get_description();
    std::string avail = topic("availability");
    for (const auto& s : kSensors) {
        std::string obj = s.path;
        for (char& c : obj)
            if (c == '/') c = '_';
        std::string uid = s_node + "_" + obj, st = topic(s.path);
        DiscoveryInfo di = {s.name, uid.c_str(), st.c_str(), avail.c_str(), s.unit, s.dev_class, s.state_class,
                            s_node.c_str(), d->version};
        char* p = discovery_payload(di);
        if (p) {
            pub("homeassistant/sensor/" + s_node + "/" + obj + "/config", p, true);
            cJSON_free(p);
        }
        vTaskDelay(pdMS_TO_TICKS(20));  // let the socket drain
    }
    ESP_LOGI(TAG, "Home Assistant discovery published (%u sensors)", (unsigned)(sizeof(kSensors) / sizeof(kSensors[0])));
}

// Values are published only when the source has delivered them: nothing is invented.
void publish_state() {
    agg::snapshot(s_snap);
    const agg::Output& o = s_snap.out;
    pub(topic("state"), agg::out_state_name(o.state), s_cfg.mqtt_retain);
    pub(topic("reason"), o.reason, s_cfg.mqtt_retain);
    {
        heapmon::Stats hs = heapmon::stats();
        pubf("heap_free", (float)hs.free, 0);
        pubf("heap_largest", (float)hs.largest, 0);
    }
    bool any = s_snap.cfg.enabled[0] || s_snap.cfg.enabled[1];
    if (any) {
        pubf("merged/cvl", o.cvl, 1);
        pubf("merged/ccl", o.ccl, 1);
        pubf("merged/dcl", o.dcl, 1);
        pubf("merged/dvl", o.dvl, 1);
        pubf("merged/capacity", o.capacity_ah, 0);
        bool meas = false;
        for (int i = 0; i < agg::kSources; i++) meas |= s_snap.cfg.enabled[i] && s_snap.src[i].have_meas;
        if (meas) {
            pubf("merged/soc", o.soc, 1);
            pubf("merged/soh", o.soh, 0);
            pubf("merged/voltage", o.voltage, 2);
            pubf("merged/current", o.current, 1);
            pubf("merged/temperature", o.temperature, 1);
        }
        if (o.have_energy) {
            pubf("merged/energy_charged", (float)o.e_chg_kwh, 1);
            pubf("merged/energy_discharged", (float)o.e_dis_kwh, 1);
        }
    }
    for (int i = 0; i < agg::kSources; i++) {
        if (!s_snap.cfg.enabled[i]) continue;
        const agg::Source& s = s_snap.src[i];
        const char* b = i == 0 ? "battery_a/" : "battery_b/";
        auto P = [&](const char* f, float v, int dec) { pubf((std::string(b) + f).c_str(), v, dec); };
        pub(topic((std::string(b) + "state").c_str()), agg::src_state_name(s.state), s_cfg.mqtt_retain);
        if (s.have_soc) P("soc", s.soc, 1), P("soh", s.soh, 0);
        if (s.have_meas) P("voltage", s.voltage, 2), P("current", s.current, 1), P("temperature", s.temperature, 1);
        if (s.have_limits) P("cvl", s.cvl, 1), P("ccl", s.ccl, 1), P("dcl", s.dcl, 1);
        auto T = [&](const char* f, const char* v) { pub(topic((std::string(b) + f).c_str()), v, s_cfg.mqtt_retain); };
        const int g = i == 0 ? 0 : 2;
        T("guard_charge", agg::guard_state_name(o.guard[g][0]));
        T("guard_discharge", agg::guard_state_name(o.guard[g][1]));
        P("rejected_frames", (float)(s.rejected_frames + s_snap.malformed[i]), 0);
        if (s.have_meas) P("energy_in", (float)s.e_in_kwh, 2), P("energy_out", (float)s.e_out_kwh, 2);
        if (s.have_cells) P("cell_spread", s.cell_max_mv - s.cell_min_mv, 0);
        if (i == 0) {
            if (s.have_cap) P("capacity", s.capacity_ah, 0);
            if (s.have_cells) P("cell_min", s.cell_min_mv, 0), P("cell_max", s.cell_max_mv, 0);
            if (s.have_p1) {
                P("pack1_current", s.p1_i, 2), P("pack1_ccl", s.p1_ccl, 1), P("pack1_dcl", s.p1_dcl, 1);
                P("pack1_energy_in", (float)s.p1_e_in_kwh, 2), P("pack1_energy_out", (float)s.p1_e_out_kwh, 2);
                if (s.have_p1_soc) P("pack1_soc", s.p1_soc, 0);
            }
            if (s.have_p2) {
                P("pack2_dcl", s.p2_dcl, 1);
                P("pack2_energy_in", (float)s.p2_e_in_kwh, 2), P("pack2_energy_out", (float)s.p2_e_out_kwh, 2);
                T("pack2_guard_charge", agg::guard_state_name(o.guard[1][0]));
                T("pack2_guard_discharge", agg::guard_state_name(o.guard[1][1]));
            }
            if (s.have_energy) P("energy_charged", (float)s.e_chg_kwh, 1), P("energy_discharged", (float)s.e_dis_kwh, 1);
            if (s.have_p2) P("pack2_current", s.p2_i, 2), P("pack2_soc", s.p2_soc, 0), P("pack2_ccl", s.p2_ccl, 1);
            if (s.have_p2_energy)
                P("pack2_energy_charged", (float)s.p2_e_chg_kwh, 1), P("pack2_energy_discharged", (float)s.p2_e_dis_kwh, 1);
        } else {
            char hex[12];
            snprintf(hex, sizeof(hex), "0x%08lX", (unsigned long)s.alarm_bits);
            pub(topic("battery_b/alarm_bits"), hex, s_cfg.mqtt_retain);
            if (s.have_cap) P("capacity", s.capacity_ah, 0);
            if (s.have_cells) P("cell_min", s.cell_min_mv, 0), P("cell_max", s.cell_max_mv, 0);
            T("charge_enable", s.charge_enable ? "on" : "off");
            T("discharge_enable", s.discharge_enable ? "on" : "off");
        }
    }
}

uint32_t s_ev_sent_seq = 0;
void publish_events() {
    agg::Event ev[8];
    size_t n;
    while (s_connected && (n = agg::events_since(s_ev_sent_seq, ev, 8)) > 0) {
        for (size_t i = 0; i < n; i++) {
            const agg::Event& e = ev[i];
            s_ev_sent_seq = e.seq;
            cJSON* o = cJSON_CreateObject();
            cJSON_AddNumberToObject(o, "seq", e.seq);
            cJSON_AddStringToObject(o, "type", agg::ev_type_name(e.type));
            cJSON_AddStringToObject(o, "source", e.source == 0 ? "A" : e.source == 1 ? "B" : "gateway");
            cJSON_AddStringToObject(o, "text", e.text);
            int64_t off = net::epoch_offset_us();
            if (off) cJSON_AddNumberToObject(o, "unix_ms", (double)((int64_t)e.t_ms + off / 1000));
            cJSON_AddNumberToObject(o, "uptime_ms", (double)e.t_ms);
            char* p = cJSON_PrintUnformatted(o);
            cJSON_Delete(o);
            if (p) {
                pub(topic("events"), p, false);
                s_events_sent++;
                cJSON_free(p);
            }
        }
    }
}

void on_event(void*, esp_event_base_t, int32_t id, void* data) {
    auto* ev = (esp_mqtt_event_handle_t)data;
    switch ((esp_mqtt_event_id_t)id) {
        case MQTT_EVENT_CONNECTED:
            s_connected = true;
            s_state = "connected";
            s_need_discovery = true;
            ESP_LOGI(TAG, "connected to %s:%lu", s_cfg.mqtt_host.c_str(), (unsigned long)s_cfg.mqtt_port);
            break;
        case MQTT_EVENT_DISCONNECTED:
            if (s_connected) ESP_LOGW(TAG, "disconnected");
            s_connected = false;
            s_state = "connecting";
            break;
        case MQTT_EVENT_ERROR:
            s_state = "error";
            if (ev && ev->error_handle && ev->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED)
                ESP_LOGW(TAG, "connection refused by the broker (code %d: check user/password)",
                         ev->error_handle->connect_return_code);
            break;
        default:
            break;
    }
}

void stop_client() {
    if (!s_client) return;
    if (s_connected) {
        esp_mqtt_client_publish(s_client, topic("availability").c_str(), "offline", 0, 1, true);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    esp_mqtt_client_stop(s_client);
    esp_mqtt_client_destroy(s_client);
    s_client = nullptr;
    s_connected = false;
}

void start_client() {
    static std::string uri, user, pass, lwt, cid;
    uri = "mqtt://" + s_cfg.mqtt_host + ":" + std::to_string(s_cfg.mqtt_port);
    user = s_cfg.mqtt_user;
    pass = cfg::mqtt_password();
    lwt = topic("availability");
    cid = s_cfg.hostname.empty() ? s_node : s_cfg.hostname;
    esp_mqtt_client_config_t c = {};
    c.broker.address.uri = uri.c_str();
    if (!user.empty()) c.credentials.username = user.c_str();
    if (!pass.empty()) c.credentials.authentication.password = pass.c_str();
    c.credentials.client_id = cid.c_str();
    c.session.last_will.topic = lwt.c_str();
    c.session.last_will.msg = "offline";
    c.session.last_will.qos = 1;
    c.session.last_will.retain = 1;
    c.session.keepalive = 30;
    c.network.reconnect_timeout_ms = 10000;
    c.task.priority = 3;
    c.task.stack_size = 4096;
    c.buffer.size = 1024;
    s_client = esp_mqtt_client_init(&c);
    if (!s_client) {
        s_state = "error";
        return;
    }
    esp_mqtt_client_register_event(s_client, MQTT_EVENT_ANY, on_event, nullptr);
    esp_mqtt_client_start(s_client);
    s_state = "connecting";
}

void task(void*) {
    s_snap_p = new agg::Snapshot;
    int64_t next_pub = 0;
    for (;;) {
        if (s_restart) {
            s_restart = false;
            stop_client();
            s_cfg = cfg::get();
            s_state = s_cfg.mqtt_enabled ? "waiting for WiFi" : "off";
            agg::snapshot(s_snap);
            s_ev_sent_seq = s_snap.event_seq;  // only new events after a (re)start
        }
        if (s_cfg.mqtt_enabled && !s_client && net::sta_connected()) start_client();
        if (s_connected) {
            // Discovery (one ~1 KB cJSON at a time, ~70 messages) waits while the heap is low.
            if (s_need_discovery && !heapmon::low()) {
                s_need_discovery = false;
                pub(topic("availability"), "online", true);
                if (s_cfg.mqtt_discovery) publish_discovery();
                heapmon::mark("mqtt discovery done");
                next_pub = 0;
            }
            publish_events();
            int64_t now = esp_timer_get_time();
            if (now >= next_pub) {
                next_pub = now + (int64_t)s_cfg.mqtt_interval_s * 1000000;
                publish_state();
            }
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }
}

}  // namespace

// The task (and its ~7 KB of stack and buffers) exists only once MQTT has been enabled.
void ensure_task() {
    if (s_task || !cfg::get().mqtt_enabled) return;
    xTaskCreate(task, "hamqtt", 5120, nullptr, 3, &s_task);
}

void start() {
    uint8_t mac[6] = {};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    char n[24];
    snprintf(n, sizeof(n), "bmsagg_%02x%02x%02x", mac[3], mac[4], mac[5]);
    s_node = n;
    ensure_task();
}

void apply_settings() {
    s_restart = true;
    ensure_task();
}

Status status() {
    Status s;
    s.state = s_state;
    s.published = s_published;
    s.events_sent = s_events_sent;
    s.last_publish_age_s = s_last_pub_us ? (esp_timer_get_time() - s_last_pub_us) / 1000000 : -1;
    return s;
}

}  // namespace hamqtt
