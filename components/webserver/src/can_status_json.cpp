#include "can_status_json.h"

namespace webserver {

bool write_can_status(JsonOut& j, const can::Status& s, uint32_t ring_size, uint32_t ring_used) {
    j.begin();
    j.boolean("enabled", s.enabled);
    j.str("state", can::state_name(s.state));
    j.str("error", s.error.c_str());
    j.num("bitrate", s.bitrate);
    j.num("frames", (double)s.frames_total);
    j.num("fps", s.fps);
    j.num("bus_errors", s.bus_errors);
    j.num("rx_overflows", s.rx_overflows);
    j.num("rec", s.rec);
    j.num("ids", s.ids);
    j.num("ids_dropped", s.ids_dropped);
    j.num("blocked_cmds", s.blocked_cmds);
    j.num("last_frame_age_ms", (double)s.last_frame_age_ms);
    j.num("ring_size", ring_size);
    j.num("ring_used", ring_used);
    j.arr("ports");
    for (const auto& p : s.ports) {
        j.obj();
        j.str("name", p.name);
        j.boolean("enabled", p.enabled);
        j.str("state", can::state_name(p.state));
        j.num("bitrate", p.bitrate);
        j.num("frames", (double)p.frames);
        j.num("fps", p.fps);
        j.num("bus_errors", p.bus_errors);
        j.num("rx_overflows", p.rx_overflows);
        j.num("rec", p.rec);
        j.num("tec", p.tec);
        j.num("tx_frames", (double)p.tx_frames);
        j.num("tx_failed", p.tx_failed);
        j.boolean("fallback", p.fallback);
        j.num("last_frame_age_ms", (double)p.last_frame_age_ms);
        j.str("error", p.error);
        j.end_obj();
    }
    j.end_arr();
    j.obj("detect");
    j.boolean("running", s.detect_running);
    j.arr("results");
    for (int i = 0; i < s.detect_count; i++) {
        j.obj();
        j.num("bitrate", s.detect[i].bitrate);
        j.num("frames", s.detect[i].frames);
        j.num("errors", s.detect[i].errors);
        j.end_obj();
    }
    j.end_arr();
    j.num("suggestion", s.detect_suggestion);
    j.str("note", s.detect_note.c_str());
    j.end_obj();
    j.obj("probe");
    j.boolean("running", s.probe_running);
    j.boolean("done", s.probe_done);
    j.num("bitrate", s.probe_bitrate);
    j.num("age_s", (double)s.probe_age_s);
    j.arr("ports");
    for (const auto& p : s.probe) {
        if (!s.probe_done) break;
        j.obj();
        j.str("name", p.name);
        j.num("cs", p.cs);
        j.num("int", p.int_gpio);
        j.boolean("spi_ok", p.spi_ok);
        j.boolean("int_ok", p.int_ok);
        j.boolean("listen_ok", p.listen_ok);
        j.num("canstat", p.canstat);
        j.num("canctrl", p.canctrl);
        j.num("frames", p.frames);
        j.num("errors", p.errors);
        j.str("note", p.note);
        j.end_obj();
    }
    j.end_arr();
    j.end_obj();
    // INT-line mapping (bench only).
    j.obj("intmap");
    j.boolean("running", s.intmap_running);
    j.boolean("done", s.intmap_done);
    j.str("port", s.intmap_port >= 0 ? s.intmap[s.intmap_port].name : "");
    j.num("bitrate", s.intmap_bitrate);
    j.num("age_s", (double)s.intmap_age_s);
    j.num("rounds_wanted", can::kIntMapRounds);
    j.arr("candidates");
    for (int i = 0; i < s.intmap_ncand; i++) j.num(s.intmap_cand[i]);
    j.end_arr();
    j.arr("ports");
    for (const auto& p : s.intmap) {
        if (!s.intmap_done && !s.intmap_running) break;
        static const char* const kKind[] = {"not_run", "ok", "no_spi", "no_frames", "no_int", "multi_int",
                                            "inconsistent"};
        j.obj();
        j.str("name", p.name);
        j.num("cs", p.cs);
        j.num("int_pins_h", p.int_pins_h);
        j.num("found", p.found);
        j.str("result", kKind[(int)p.result]);
        j.str("result_text", can::intmap_result_name(p.result));
        j.arr("per_round");
        for (int i = 0; i < p.rounds; i++) j.num(p.per_round[i]);
        j.end_arr();
        j.num("foreign", p.foreign);
        j.num("rx_errors", p.rx_errors);
        j.num("first_id", p.first_id);
        j.str("note", p.note);
        j.end_obj();
    }
    j.end_arr();
    j.end_obj();
    return j.end();
}

}  // namespace webserver
