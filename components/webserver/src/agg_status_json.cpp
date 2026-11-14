#include "agg_status_json.h"

#include <stdio.h>

namespace webserver {
namespace {

void hexdata(char* out, const uint8_t* d, int n) {
    static const char* k = "0123456789ABCDEF";
    if (n > 8) n = 8;
    for (int i = 0; i < n; i++) out[2 * i] = k[d[i] >> 4], out[2 * i + 1] = k[d[i] & 15];
    out[2 * (n < 0 ? 0 : n)] = 0;
}

void limits(JsonOut& j, const agg::Source& s) {
    j.num("cvl", s.cvl);
    j.num("ccl", s.ccl);
    j.num("dcl", s.dcl);
    j.num("dvl", s.dvl);
}

}  // namespace

bool write_agg_status(JsonOut& j, const AggStatusData& d, const AggStatusExtras& x, uint64_t now) {
    const agg::Snapshot& sn = d.snap;
    const agg::Output& out = sn.out;
    char hx[24];
    j.begin();
    j.str("state", agg::out_state_name(out.state));
    j.str("reason", out.reason);
    j.boolean("output_valid", out.send);
    // Inverter transmit (port INV): setting, port state and counters.
    j.boolean("transmit", x.tx_enabled);
    j.obj("tx");
    j.boolean("enabled", x.tx_enabled);
    j.boolean("inv_enabled", x.inv_enabled);
    j.str("inv_state", x.inv_state);
    j.boolean("sending", x.sending);
    j.num("frames", (double)x.tx_frames);
    j.num("failed", x.tx_failed);
    j.num("tec", x.tec);
    j.num("blocked_cmds", x.blocked_cmds);
    j.str("allow_list", "0x351/8 0x355/4 0x356/6 0x359/7 0x35C/2 0x35E/8, max 1 Hz per ID");
    j.end_obj();
    const char* why = "";
    j.boolean("configured", agg::config_valid(sn.cfg, &why));
    j.str("config_problem", why);

    j.obj("out");
    j.num("cvl", out.cvl);
    j.num("cvl_raw", out.cvl_raw);
    j.num("ccl", out.ccl);
    j.num("dcl", out.dcl);
    j.num("dvl", out.dvl);
    j.num("ccl_sum", out.ccl_sum);
    j.num("dcl_sum", out.dcl_sum);
    j.num("ccl_share_cap", out.ccl_share_cap);
    j.num("dcl_share_cap", out.dcl_share_cap);
    j.boolean("ccl_clamped", out.ccl_clamped);
    j.boolean("dcl_clamped", out.dcl_clamped);
    j.num("soc", out.soc);
    j.num("soh", out.soh);
    j.num("voltage", out.voltage);
    j.num("current", out.current);
    j.num("temperature", out.temperature);
    j.num("capacity_ah", out.capacity_ah);
    if (out.have_energy) {
        j.num("energy_charged_kwh", out.e_chg_kwh);
        j.num("energy_discharged_kwh", out.e_dis_kwh);
    }
    snprintf(hx, sizeof(hx), "0x%08lX", (unsigned long)out.alarm_bits);
    j.str("alarm_bits", hx);
    j.boolean("charge_enable", out.charge_enable);
    j.boolean("discharge_enable", out.discharge_enable);
    j.num("guard_charge_factor", out.chg_factor);
    j.num("guard_discharge_factor", out.dis_factor);
    if (out.state != agg::OutState::Normal) {
        j.num("failsafe_cvl", out.failsafe_cvl);
        j.num("failsafe_dvl", out.failsafe_dvl);
    }
    j.end_obj();

    // Over-current guard per source and direction.
    j.arr("guard");
    for (int g = 0; g < agg::kGuards; g++) {
        j.obj();
        j.str("source", agg::guard_name(g));
        j.num("current", out.guard_current[g]);
        j.str("charge", agg::guard_state_name(out.guard[g][0]));
        j.str("discharge", agg::guard_state_name(out.guard[g][1]));
        j.num("charge_limit", out.guard_limit[g][0]);
        j.num("discharge_limit", out.guard_limit[g][1]);
        j.end_obj();
    }
    j.end_arr();

    j.arr("frames");
    for (const auto& f : out.frames) {
        j.obj();
        snprintf(hx, sizeof(hx), "0x%03lX", (unsigned long)f.id);
        j.str("id", hx);
        char dd[17];
        hexdata(dd, f.data, f.dlc);
        j.str("data", dd);
        j.end_obj();
    }
    j.end_arr();

    j.arr("sources");
    for (int i = 0; i < agg::kSources; i++) {
        const agg::Source& s = sn.src[i];
        j.obj();
        j.str("port", i == 0 ? "A" : "B");
        j.str("profile", i == 0 ? "Blue Nova (master + pack 2)" : "KNY-P51300 (Pylontech V1.3)");
        j.boolean("enabled", sn.cfg.enabled[i]);
        j.str("state", agg::src_state_name(s.state));
        j.str("reason", s.reason);
        j.str("name", s.name);
        j.num("last_frame_age_ms", s.last_frame_ms ? (double)(now - s.last_frame_ms) : -1);
        if (s.have_limits) limits(j, s);
        if (s.have_soc) {
            j.num("soc", s.soc);
            j.num("soh", s.soh);
        }
        if (s.have_meas) {
            j.num("voltage", s.voltage);
            j.num("current", s.current);
            j.num("temperature", s.temperature);
        }
        j.num("capacity_ah", s.have_cap ? s.capacity_ah : (i == 1 ? sn.cfg.kny_capacity_ah : 0));
        j.boolean("capacity_reported", s.have_cap);
        j.num("cap_user", i == 0 ? sn.cfg.cap_a : sn.cfg.cap_b);
        j.num("frames_ok", s.frames_ok);
        j.num("rejected_frames", s.rejected_frames);
        j.num("malformed_frames", sn.malformed[i]);
        j.num("energy_in_kwh", s.e_in_kwh);
        j.num("energy_out_kwh", s.e_out_kwh);
        j.boolean("charge_enable", s.charge_enable);
        j.boolean("discharge_enable", s.discharge_enable);
        snprintf(hx, sizeof(hx), "0x%08lX", (unsigned long)s.alarm_bits);
        j.str("alarm_bits", hx);
        j.boolean("latched", s.latched);
        j.str("latch_reason", s.latch_reason);
        j.num("plausibility_rejects", s.plausibility_rejects);
        if (s.have_cells) {
            j.num("cell_min_mv", s.cell_min_mv);
            j.num("cell_max_mv", s.cell_max_mv);
            j.num("cell_spread_mv", s.cell_max_mv - s.cell_min_mv);
            j.num("t_min", s.t_min);
            j.num("t_max", s.t_max);
        }
        if (s.have_energy) {
            j.num("energy_charged_kwh", s.e_chg_kwh);
            j.num("energy_discharged_kwh", s.e_dis_kwh);
        }
        if (s.have_p2) {
            j.obj("pack2");
            j.num("voltage", s.p2_v);
            j.num("current", s.p2_i);
            j.num("soc", s.p2_soc);
            j.num("ccl", s.p2_ccl);
            j.num("dcl", s.p2_dcl);
            j.num("temperature", s.p2_t);
            j.num("cell_max_mv", s.p2_cell_max);
            j.num("cell_min_mv", s.p2_cell_min);
            j.boolean("balancing", s.p2_balancing);
            j.num("energy_in_kwh", s.p2_e_in_kwh);
            j.num("energy_out_kwh", s.p2_e_out_kwh);
            j.num("cap_user", sn.cfg.cap_a_p2);
            j.end_obj();
        }
        if (s.have_p1) {
            j.obj("pack1");
            j.num("current", s.p1_i);
            if (s.have_p1_soc) j.num("soc", s.p1_soc);
            j.num("ccl", s.p1_ccl);
            j.num("dcl", s.p1_dcl);
            j.num("energy_in_kwh", s.p1_e_in_kwh);
            j.num("energy_out_kwh", s.p1_e_out_kwh);
            j.end_obj();
        }
        // Frames that are missing or late (the staleness rule).
        j.arr("frames");
        for (int k = 0; k < s.nreq; k++) {
            j.obj();
            snprintf(hx, sizeof(hx), "0x%03X", s.req[k].id);
            j.str("id", hx);
            j.boolean("pack2", s.req[k].pack2);
            j.num("age_ms", s.req[k].seen ? (double)(now - s.req[k].last_ms) : -1);
            j.end_obj();
        }
        j.end_arr();
        j.end_obj();
    }
    j.end_arr();

    // Recent events, newest first.
    j.arr("events");
    for (size_t k = d.nev; k-- > 0;) {
        const agg::Event& e = d.ev[k];
        j.obj();
        j.num("seq", e.seq);
        j.str("type", agg::ev_type_name(e.type));
        j.str("source", e.source == 0 ? "A" : e.source == 1 ? "B" : "");
        j.str("text", e.text);
        j.num("age_s", (double)(now - e.t_ms) / 1000.0);
        if (x.epoch_offset_us) j.num("unix_ms", (double)((int64_t)e.t_ms + x.epoch_offset_us / 1000));
        j.end_obj();
    }
    j.end_arr();

    j.obj("mqtt");
    j.str("state", x.mqtt_state);
    j.num("published", x.mqtt_published);
    j.num("events_sent", x.mqtt_events_sent);
    j.num("last_publish_age_s", (double)x.mqtt_last_publish_age_s);
    j.end_obj();
    return j.end();
}

}  // namespace webserver
