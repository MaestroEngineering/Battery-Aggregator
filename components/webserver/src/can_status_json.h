#pragma once
// GET /api/can/status body, streamed by JsonOut (no cJSON tree, no heap). Pure C++,
// host-tested by tools/test_status_json.cpp.
#include <stdint.h>

#include "can/can.h"
#include "json_stream.h"

namespace webserver {
bool write_can_status(JsonOut& j, const can::Status& s, uint32_t ring_size, uint32_t ring_used);
}  // namespace webserver
