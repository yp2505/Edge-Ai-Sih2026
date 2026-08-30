#!/usr/bin/env python3
"""
latency_analyzer.py — End-to-End Keyword→Cloud Latency Measurement

Correlates two timestamp sources:
  1. ESP32 serial log → "keyword_end_timestamp=<ms>" (NTP-synced)
  2. cloud_server/server_log.json → "cloud_receive_timestamp" (system time)

Computes per-session delta: cloud_receive_timestamp - keyword_end_timestamp

This satisfies the PS requirement for measuring latency between
"keyword ending and cloud ASR server receiving the audio stream."

Usage:
    # Capture ESP32 serial output to file, then run:
    python latency_analyzer.py \
        --serial-log esp32_serial.txt \
        --server-log ../cloud_server/server_log.json

    # Or pipe live serial output:
    python -m serial.tools.miniterm /dev/ttyUSB0 115200 | tee esp32_serial.txt

Output:
    ┌────────────────────────────────────────────────────┐
    │  Session │  keyword_end (ms)  │  cloud_rcv (ms)  │  Δ latency │
    ├──────────┼────────────────────┼──────────────────┼────────────┤
    │     1    │  1725000123456     │  1725000123780   │   324 ms   │
    │     2    │  1725000187234     │  1725000187601   │   367 ms   │
    ├──────────┴────────────────────┴──────────────────┴────────────┤
    │  Average latency:  345 ms  |  Min: 324 ms  |  Max: 367 ms    │
    └─────────────────────────────────────────────────────────────-─┘
"""

import os
import re
import sys
import json
import argparse
import statistics
from datetime import datetime

# ─── Defaults ───────────────────────────────────────────────────────────────
SCRIPT_DIR   = os.path.dirname(__file__)
DEFAULT_SERIAL = os.path.join(SCRIPT_DIR, "..", "esp32_serial.txt")
DEFAULT_SERVER = os.path.join(SCRIPT_DIR, "..", "cloud_server", "server_log.json")

# ─── Parse ESP32 Serial Log ─────────────────────────────────────────────────
def parse_esp32_log(serial_log_path: str) -> list:
    """
    Extract keyword_end_timestamp from ESP32 serial output.

    Expected line format (from main.cpp ESP_LOGI):
      I (12345) INFERENCE: 🔔 HEY VAANI DETECTED! prob=0.923  keyword_end_timestamp=1725000123456 ms
    OR:
      I (12345) STREAM: [Session 1] keyword_end_timestamp=1725000123456 | ...
    """
    events = []
    kw_ts_re = re.compile(r'keyword_end_timestamp=(\d+)')
    session_re = re.compile(r'\[Session\s+(\d+)\]')

    with open(serial_log_path, 'r', errors='replace') as f:
        for line in f:
            ts_match = kw_ts_re.search(line)
            if ts_match:
                ts_ms = int(ts_match.group(1))
                session = None
                s_match = session_re.search(line)
                if s_match:
                    session = int(s_match.group(1))
                events.append({
                    "session_id": session,
                    "keyword_end_ms": ts_ms,
                    "raw_line": line.strip()
                })

    return events


# ─── Parse Cloud Server Log ─────────────────────────────────────────────────
def parse_server_log(server_log_path: str) -> list:
    """
    Read cloud_server/server_log.json and extract cloud_receive_timestamp.
    The asr_server.py logs a JSON array of session records.
    """
    with open(server_log_path, 'r') as f:
        log_entries = json.load(f)

    sessions = []
    for entry in log_entries:
        # cloud_receive_timestamp is logged by the server the moment first audio arrives
        rcv_ts = entry.get("cloud_receive_timestamp_ms")
        if rcv_ts is None:
            # Fallback: parse from ISO timestamp field
            ts_str = entry.get("timestamp")
            if ts_str:
                dt = datetime.fromisoformat(ts_str)
                rcv_ts = int(dt.timestamp() * 1000)

        sessions.append({
            "session_id": entry.get("session_id"),
            "cloud_receive_ms": rcv_ts,
            "transcript": entry.get("transcript", ""),
            "transcribe_ms": entry.get("transcribe_ms", 0)
        })

    return sessions


# ─── Correlate and Compute Latency ──────────────────────────────────────────
def compute_latency(esp32_events: list, server_sessions: list) -> list:
    """
    Match ESP32 keyword events to cloud sessions by session_id (or by order).
    Returns list of dicts with latency_ms per event.
    """
    results = []

    # Build lookup by session_id
    server_by_id = {s["session_id"]: s for s in server_sessions if s["session_id"] is not None}

    for i, ev in enumerate(esp32_events):
        sid = ev.get("session_id")
        server = server_by_id.get(sid)

        if server is None and i < len(server_sessions):
            # Fallback: match by order
            server = server_sessions[i]

        if server and server.get("cloud_receive_ms") and ev.get("keyword_end_ms"):
            latency_ms = server["cloud_receive_ms"] - ev["keyword_end_ms"]
            results.append({
                "session_id": sid or (i + 1),
                "keyword_end_ms": ev["keyword_end_ms"],
                "cloud_receive_ms": server["cloud_receive_ms"],
                "latency_ms": latency_ms,
                "transcript": server.get("transcript", ""),
                "transcribe_ms": server.get("transcribe_ms", 0)
            })
        else:
            results.append({
                "session_id": sid or (i + 1),
                "keyword_end_ms": ev["keyword_end_ms"],
                "cloud_receive_ms": None,
                "latency_ms": None,
                "transcript": "",
                "transcribe_ms": 0
            })

    return results


# ─── Print Report ────────────────────────────────────────────────────────────
def print_report(results: list):
    valid = [r for r in results if r["latency_ms"] is not None]

    print("\n")
    print("  ┌──────────┬────────────────────┬────────────────────┬────────────┐")
    print("  │ Session  │ keyword_end (ms)   │ cloud_rcv (ms)     │ Δ latency  │")
    print("  ├──────────┼────────────────────┼────────────────────┼────────────┤")

    for r in results:
        sid    = str(r["session_id"]).center(8)
        kw_ts  = str(r["keyword_end_ms"]).center(18) if r["keyword_end_ms"] else "     N/A      ".center(18)
        cl_ts  = str(r["cloud_receive_ms"]).center(18) if r["cloud_receive_ms"] else "     N/A      ".center(18)
        lat    = f"{r['latency_ms']} ms".center(10) if r["latency_ms"] is not None else "  N/A  ".center(10)
        print(f"  │ {sid} │ {kw_ts} │ {cl_ts} │ {lat} │")

    print("  ├──────────┴────────────────────┴────────────────────┴────────────┤")

    if valid:
        latencies = [r["latency_ms"] for r in valid]
        avg = statistics.mean(latencies)
        mn  = min(latencies)
        mx  = max(latencies)
        med = statistics.median(latencies)
        print(f"  │  ⚡ Average: {avg:.0f} ms  |  Min: {mn} ms  |  Max: {mx} ms  |  Median: {med:.0f} ms")
        print(f"  │  Sessions measured: {len(valid)}/{len(results)}")

        # PS compliance: target < 1500ms
        ps_ok = avg < 1500
        print(f"  │  PS Target (< 1500 ms):  {'✅ PASS' if ps_ok else '❌ FAIL'}  (avg={avg:.0f} ms)")
    else:
        print("  │  ⚠️  No valid latency measurements found.")

    print("  └─────────────────────────────────────────────────────────────────┘")

    if valid:
        print("\n  📋 Transcripts:")
        for r in valid:
            print(f"    Session {r['session_id']}: \"{r['transcript']}\" "
                  f"(transcribe: {r['transcribe_ms']} ms)")


def main():
    parser = argparse.ArgumentParser(
        description="Hey Vaani — Keyword→Cloud Latency Analyzer",
        epilog="""
Example:
  # Capture serial output while running the ESP32, then analyze:
  python latency_analyzer.py --serial-log esp32_serial.txt --server-log ../cloud_server/server_log.json
        """
    )
    parser.add_argument("--serial-log",  default=DEFAULT_SERIAL, help="ESP32 serial log file")
    parser.add_argument("--server-log",  default=DEFAULT_SERVER,  help="Cloud server_log.json")
    args = parser.parse_args()

    print("=" * 60)
    print("  Hey Vaani — End-to-End Latency Analyzer")
    print("=" * 60)

    # Parse ESP32 log
    if not os.path.exists(args.serial_log):
        print(f"\n  ❌ ESP32 serial log not found: {args.serial_log}")
        print("  Capture serial output with: python -m serial.tools.miniterm /dev/ttyUSB0 115200 | tee esp32_serial.txt")
        sys.exit(1)

    esp32_events = parse_esp32_log(args.serial_log)
    print(f"\n  Found {len(esp32_events)} keyword events in ESP32 log")

    # Parse server log
    if not os.path.exists(args.server_log):
        print(f"\n  ❌ Server log not found: {args.server_log}")
        print("  Make sure asr_server.py is running and has processed at least one request.")
        sys.exit(1)

    server_sessions = parse_server_log(args.server_log)
    print(f"  Found {len(server_sessions)} sessions in server log")

    if not esp32_events:
        print("\n  ⚠️  No keyword_end_timestamp found in ESP32 log.")
        print("  Make sure the ESP32 firmware is the version from main.cpp (not edge_inference.py).")
        sys.exit(1)

    results = compute_latency(esp32_events, server_sessions)
    print_report(results)


if __name__ == "__main__":
    main()
