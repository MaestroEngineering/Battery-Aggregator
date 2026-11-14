# Plan: aggregator events to the receiver (firmware 0.1.9)

Status: planned, not started (owner, 14 Nov 2026). Today the engine's events (trip, stale,
plausibility, recovered, capacity_step, frame_rejected, overcurrent_derate, ...) reach the log, the
Aggregator page and MQTT only. The receiver's "events" are Verbose Diagnostics trigger batches.

1. **Format.** A new batch kind `F_AGG_EVENTS` (flag 0x10) in `batch_format.h`. The body is fixed
   records (seq u32, t_ms u64 device time, type u8, source u8, text[76]), at most 32 per batch.
   The existing header carries boot_id/seq/epoch offset, so the receiver's dedup and time back-fill
   work unchanged.
2. **Device.** diag keeps `last_sent_event_seq`. Every flush interval, and at once on a trip, it
   reads `agg::events_since()` into a static 32-record buffer (no heap) and uploads it ahead of
   frame batches. A failed upload is retried; events older than the engine ring (32) are lost and
   counted. The upload is off whenever Verbose Diagnostics is off.
3. **Receiver.** New table `agg_events` (device, boot_id, seq, t_ms, unix_ms, type, source, text)
   with a unique (device, boot_id, seq); a `kind=agg_events` query; an export column. Old clients
   are unaffected: an unknown flag is stored as a batch only.
4. **Export.** `tools/export_receiver_db.py` writes `agg_events_since_<since>.csv.gz`.
5. **Tests.** test_batch writes an event batch; a receiver round trip; dedup after a resend; the
   heap model gets one 3 KB transient per upload (the HTTP body is streamed from the static buffer).
