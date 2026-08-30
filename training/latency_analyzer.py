#!/usr/bin/env python3
"""
latency_analyzer.py — End-to-End Keyword→Cloud Latency Analyzer

MEASUREMENT METHOD (NTP-FREE, single-clock):
  No internet, no NTP, no synced clocks between two machines.

  Each detection event produces these three independently-measured values,
  all of which are in server_log.json from server.py:

    kw_to_connect_ms    — ESP32's own monotonic clock (esp_timer_get_time):
                          time from "keyword confirmed" to "TCP connect() returned".
                          Reported by the ESP32 in the HVP1 header field.

    receive_gap_ms      — Server's own clock:
                          time from "connection accepted" to "first audio byte received".
                          Measures WiFi + TCP transfer overhead.

    transcribe_ms       — Server's own clock:
                          faster-whisper inference time.

  TOTAL END-TO-END = kw_to_connect_ms + receive_gap_ms + transcribe_ms

  Precision: ±1ms (ESP32 timer ±1µs, server clock ±1ms, TCP stack ±few ms).
  Error margin: ~5-10ms dominated by TCP stack jitter, NOT by clock sync issues.

Usage:
    python latency_analyzer.py                     # reads default log paths
    python latency_analyzer.py --server-log ../cloud_server/server_log.json

    (No --serial-log needed — all data is now in server_log.json)
"""

import os
import sys
import json
import argparse
import statistics

SCRIPT_DIR     = os.path.dirname(__file__)
DEFAULT_SERVER = os.path.join(SCRIPT_DIR, "..", "cloud_server", "server_log.json")


def load_server_log(path: str) -> list:
    """Load server_log.json written by server.py."""
    with open(path, "r") as f:
        return json.load(f)


def compute_latencies(entries: list) -> list:
    """Extract latency components from each log entry."""
    results = []
    for e in entries:
        kw_ms   = e.get("kw_to_connect_ms")
        rcv_ms  = e.get("receive_gap_ms")
        txs_ms  = e.get("transcribe_ms")
        e2e_ms  = e.get("end_to_end_ms")

        # Fallback: recompute end-to-end if field present
        if e2e_ms is None and kw_ms is not None and rcv_ms is not None and txs_ms is not None:
            e2e_ms = kw_ms + rcv_ms + txs_ms

        results.append({
            "session_id":      e.get("session_id"),
            "timestamp":       e.get("timestamp", ""),
            "kw_to_connect_ms": kw_ms,
            "receive_gap_ms":  rcv_ms,
            "transcribe_ms":   txs_ms,
            "end_to_end_ms":   e2e_ms,
            "transcript":      e.get("transcript", ""),
            "audio_duration_s": e.get("audio_duration_s"),
        })
    return results


def print_report(results: list):
    valid = [r for r in results if r["end_to_end_ms"] is not None]

    print("\n")
    print("  ╔══════╦════════════╦═══════════╦═══════════╦════════════╦══════════════╗")
    print("  ║  Ses ║ kw→connect ║ rcv_gap   ║ transcribe║ end-to-end ║ transcript   ║")
    print("  ╠══════╬════════════╬═══════════╬═══════════╬════════════╬══════════════╣")

    for r in results:
        sid    = str(r["session_id"] or "?").rjust(4)
        kw     = f"{r['kw_to_connect_ms']}ms".rjust(10) if r["kw_to_connect_ms"] is not None else "    N/A   "
        rcv    = f"{r['receive_gap_ms']}ms".rjust(9)    if r["receive_gap_ms"]    is not None else "   N/A   "
        txs    = f"{r['transcribe_ms']}ms".rjust(9)     if r["transcribe_ms"]     is not None else "   N/A   "
        e2e    = f"{r['end_to_end_ms']}ms".rjust(10)   if r["end_to_end_ms"]     is not None else "    N/A   "
        txt    = (r["transcript"] or "")[:12].ljust(12)
        print(f"  ║ {sid} ║ {kw} ║ {rcv} ║ {txs} ║ {e2e} ║ {txt} ║")

    print("  ╠══════╩════════════╩═══════════╩═══════════╩════════════╩══════════════╣")

    if valid:
        e2es = [r["end_to_end_ms"] for r in valid]
        kws  = [r["kw_to_connect_ms"] for r in valid if r["kw_to_connect_ms"] is not None]
        rcvs = [r["receive_gap_ms"] for r in valid if r["receive_gap_ms"] is not None]
        txss = [r["transcribe_ms"] for r in valid if r["transcribe_ms"] is not None]

        avg_e2e = statistics.mean(e2es)
        print(f"  ║  Sessions: {len(valid)}/{len(results)}")
        print(f"  ║  End-to-end:  avg={avg_e2e:.0f}ms  min={min(e2es)}ms  max={max(e2es)}ms")
        if kws:  print(f"  ║  kw→connect:  avg={statistics.mean(kws):.0f}ms")
        if rcvs: print(f"  ║  rcv_gap:     avg={statistics.mean(rcvs):.0f}ms")
        if txss: print(f"  ║  transcribe:  avg={statistics.mean(txss):.0f}ms")
        ps_ok = avg_e2e < 1500
        print(f"  ║  PS Target (<1500ms): {'✅ PASS' if ps_ok else '❌ FAIL'}  (avg={avg_e2e:.0f}ms)")
        print(f"  ║  Precision: ±5-10ms (TCP jitter, no NTP required)")
    else:
        print("  ║  ⚠️  No valid entries found in server log.")
    print("  ╚══════════════════════════════════════════════════════════════════════════╝")

    if valid:
        print("\n  📋 Transcripts:")
        for r in valid:
            print(f"    [{r['session_id']}] \"{r['transcript']}\" "
                  f"({r['audio_duration_s']}s audio, e2e={r['end_to_end_ms']}ms)")


def main():
    parser = argparse.ArgumentParser(
        description="Hey Vaani — Keyword→Cloud Latency Analyzer (NTP-free single-clock)",
        epilog="""
All latency data is now in server_log.json — no separate serial log needed.
The server.py logs kw_to_connect_ms (sent by ESP32 in HVP1 header) plus its own
receive_gap_ms and transcribe_ms. This script sums them for end-to-end latency.
        """
    )
    parser.add_argument("--server-log", default=DEFAULT_SERVER,
                        help="Path to server_log.json from server.py")
    args = parser.parse_args()

    print("=" * 60)
    print("  Hey Vaani — Latency Analyzer (NTP-free)")
    print("=" * 60)

    if not os.path.exists(args.server_log):
        print(f"\n  ❌ server_log.json not found: {args.server_log}")
        print("  Run the cloud server (server.py) and trigger at least one detection.")
        sys.exit(1)

    entries = load_server_log(args.server_log)
    print(f"\n  Found {len(entries)} session(s) in log")

    if not entries:
        print("  ⚠️  Log is empty — no detections recorded yet.")
        sys.exit(0)

    results = compute_latencies(entries)
    print_report(results)


if __name__ == "__main__":
    main()
