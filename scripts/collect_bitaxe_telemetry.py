#!/usr/bin/env python3
"""Local telemetry collector, soak-test monitor, and Phase 1/2 baseline analyzer for Bitaxe/StarMiner."""

from __future__ import annotations

import argparse
import csv
from dataclasses import dataclass
from datetime import datetime
import json
import math
from pathlib import Path
import signal
import statistics
import sys
import time
from typing import Any, Dict, List, Optional, Tuple
import urllib.error
import urllib.request


DEFAULT_HOSTS = ["192.168.1.101", "192.168.1.102", "192.168.1.103"]
FREE_SRAM_ALERT_THRESHOLD = 110000

CSV_COLUMNS = [
    "timestamp_iso",
    "epoch_s",
    "host",
    "ok",
    "error_msg",
    "hostname",
    "version",
    "uptime_s",
    "reset_reason",
    "dvfs_profile",
    "target_freq_mhz",
    "freq_mhz",
    "actual_freq_mhz",
    "target_vcore_mv",
    "vcore_cmd_mv",
    "core_voltage_actual_mv",
    "vdroop_offset_mv",
    "power_w",
    "voltage_mv",
    "current_ma",
    "temp_c",
    "vr_temp_c",
    "fan_perc",
    "fan_rpm",
    "hashrate_ghs",
    "hashrate_1m_ghs",
    "hashrate_10m_ghs",
    "hashrate_1h_ghs",
    "expected_hashrate_ghs",
    "error_pct",
    "h_eff_ghs",
    "j_per_th",
    "dom0_ghs",
    "dom1_ghs",
    "dom2_ghs",
    "dom3_ghs",
    "dom_min_ratio",
    "cml_error_count",
    "watchdog_resets",
    "last_watchdog_reason",
    "throttle_active",
    "overheat_mode",
    "learned_max_freq",
    "learned_min_vcore",
    "free_heap_internal",
    "free_heap_spiram",
    "cpu_usage",
    "wifi_rssi",
    "shares_accepted",
    "shares_rejected",
    "stale_rejected",
    "pool_diff",
    "response_time_ms",
    "process_time_ms",
    "stratum_url",
]


@dataclass
class HostPrevState:
    """Tracks previous poll values per host to emit real-time transition alerts."""

    ok: Optional[bool] = None
    uptime_s: Optional[int] = None
    cml_error_count: Optional[int] = None
    watchdog_resets: Optional[int] = None
    throttle_active: Optional[int] = None
    overheat_mode: Optional[int] = None
    low_sram_alerted: bool = False


def normalize_host(raw_host: str) -> Tuple[str, str]:
    """Return (display_host, base_url) from an IP, hostname, or URL."""
    cleaned = raw_host.strip().rstrip("/")
    if cleaned.startswith("http://") or cleaned.startswith("https://"):
        base_url = cleaned
        display = cleaned.split("://", 1)[1].split("/", 1)[0]
    else:
        display = cleaned.split("/", 1)[0]
        base_url = f"http://{cleaned}"
    return display, base_url


def to_float(value: Any) -> Optional[float]:
    """Safely convert JSON/CSV value to finite float."""
    if value is None or value == "":
        return None
    try:
        num = float(value)
    except (TypeError, ValueError):
        return None
    return num if math.isfinite(num) else None


def to_int(value: Any) -> Optional[int]:
    """Safely convert JSON/CSV value to int."""
    num = to_float(value)
    return int(round(num)) if num is not None else None


def fmt_num(value: Optional[float], decimals: int = 4) -> str:
    """Format optional float cleanly for CSV output."""
    if value is None:
        return ""
    if decimals == 0:
        return str(int(round(value)))
    return f"{value:.{decimals}f}".rstrip("0").rstrip(".")


def compute_domain_metrics(payload: Dict[str, Any]) -> Tuple[List[Optional[float]], Optional[float]]:
    """Extract 4 ASIC domain hashrates and minimum ratio against mean of peers."""
    doms: List[Optional[float]] = [None, None, None, None]
    monitor = payload.get("hashrateMonitor")
    if not isinstance(monitor, dict):
        return doms, None
    asics = monitor.get("asics")
    if not isinstance(asics, list) or not asics or not isinstance(asics[0], dict):
        return doms, None
    raw_domains = asics[0].get("domains")
    if not isinstance(raw_domains, list):
        return doms, None

    for idx in range(min(4, len(raw_domains))):
        doms[idx] = to_float(raw_domains[idx])

    if len(raw_domains) >= 4 and all(d is not None and d > 0.0 for d in doms):
        valid_doms = [d for d in doms if d is not None]
        total = sum(valid_doms)
        ratios: List[float] = []
        for val in valid_doms:
            other_avg = (total - val) / 3.0
            if other_avg > 0.0:
                ratios.append(val / other_avg)
        if ratios:
            return doms, min(ratios)

    return doms, None


def compute_stale_rejected(payload: Dict[str, Any]) -> int:
    """Sum rejected share counts whose reason mentions 'stale'."""
    reasons = payload.get("sharesRejectedReasons")
    if not isinstance(reasons, list):
        return 0
    stale_total = 0
    for item in reasons:
        if not isinstance(item, dict):
            continue
        msg = str(item.get("message", "")).lower()
        if "stale" in msg:
            cnt = to_int(item.get("count"))
            if cnt is not None:
                stale_total += cnt
    return stale_total


def build_csv_row(
    now_dt: datetime,
    epoch_s: float,
    host: str,
    payload: Optional[Dict[str, Any]],
    error_msg: str = "",
) -> Dict[str, str]:
    """Build a normalized CSV row dictionary from a Bitaxe /api/system/info response."""
    row = {col: "" for col in CSV_COLUMNS}
    row["timestamp_iso"] = now_dt.isoformat(timespec="seconds")
    row["epoch_s"] = f"{epoch_s:.3f}"
    row["host"] = host

    if payload is None:
        row["ok"] = "0"
        row["error_msg"] = error_msg
        return row

    row["ok"] = "1"
    row["hostname"] = str(payload.get("hostname", ""))
    row["version"] = str(payload.get("version", ""))
    row["uptime_s"] = fmt_num(to_float(payload.get("uptimeSeconds")), 0)
    row["reset_reason"] = str(payload.get("resetReason", ""))
    row["dvfs_profile"] = fmt_num(to_float(payload.get("dvfsProfile")), 0)
    row["target_freq_mhz"] = fmt_num(to_float(payload.get("targetFrequency")), 2)
    row["freq_mhz"] = fmt_num(to_float(payload.get("frequency")), 2)
    row["actual_freq_mhz"] = fmt_num(to_float(payload.get("actualFrequency")), 2)
    row["target_vcore_mv"] = fmt_num(to_float(payload.get("targetVoltage")), 0)
    row["vcore_cmd_mv"] = fmt_num(to_float(payload.get("coreVoltage")), 0)
    row["core_voltage_actual_mv"] = fmt_num(to_float(payload.get("coreVoltageActual")), 1)
    row["vdroop_offset_mv"] = fmt_num(to_float(payload.get("vdroopOffsetMv")), 0)

    power_w = to_float(payload.get("power"))
    row["power_w"] = fmt_num(power_w, 4)
    row["voltage_mv"] = fmt_num(to_float(payload.get("voltage")), 2)
    row["current_ma"] = fmt_num(to_float(payload.get("current")), 2)
    row["temp_c"] = fmt_num(to_float(payload.get("temp")), 3)
    row["vr_temp_c"] = fmt_num(to_float(payload.get("vrTemp")), 2)
    row["fan_perc"] = fmt_num(to_float(payload.get("fanspeed")), 2)
    row["fan_rpm"] = fmt_num(to_float(payload.get("fanrpm")), 0)

    hr = to_float(payload.get("hashRate"))
    hr_1m = to_float(payload.get("hashRate_1m"))
    hr_10m = to_float(payload.get("hashRate_10m"))
    hr_1h = to_float(payload.get("hashRate_1h"))
    err_pct = to_float(payload.get("errorPercentage"))

    row["hashrate_ghs"] = fmt_num(hr, 3)
    row["hashrate_1m_ghs"] = fmt_num(hr_1m, 3)
    row["hashrate_10m_ghs"] = fmt_num(hr_10m, 3)
    row["hashrate_1h_ghs"] = fmt_num(hr_1h, 3)
    row["expected_hashrate_ghs"] = fmt_num(to_float(payload.get("expectedHashrate")), 2)
    row["error_pct"] = fmt_num(err_pct, 5)

    hash_base = hr_1m if (hr_1m is not None and hr_1m > 0.0) else hr
    if hash_base is not None and hash_base > 0.0:
        err_factor = 1.0 - ((err_pct if err_pct is not None else 0.0) / 100.0)
        row["h_eff_ghs"] = fmt_num(hash_base * max(0.0, err_factor), 3)
        if power_w is not None and power_w > 0.0:
            row["j_per_th"] = fmt_num(power_w / (hash_base / 1000.0), 3)

    doms, dom_min_ratio = compute_domain_metrics(payload)
    for idx, val in enumerate(doms):
        row[f"dom{idx}_ghs"] = fmt_num(val, 3)
    row["dom_min_ratio"] = fmt_num(dom_min_ratio, 5)

    row["cml_error_count"] = fmt_num(to_float(payload.get("cmlErrorCount")), 0)
    row["watchdog_resets"] = fmt_num(to_float(payload.get("watchdogResets")), 0)
    row["last_watchdog_reason"] = fmt_num(to_float(payload.get("lastWatchdogReason")), 0)
    row["throttle_active"] = fmt_num(to_float(payload.get("throttleActive")), 0)
    row["overheat_mode"] = fmt_num(to_float(payload.get("overheat_mode")), 0)
    row["learned_max_freq"] = fmt_num(to_float(payload.get("learnedMaxFreq")), 2)
    row["learned_min_vcore"] = fmt_num(to_float(payload.get("learnedMinVcore")), 0)
    row["free_heap_internal"] = fmt_num(to_float(payload.get("freeHeapInternal")), 0)
    row["free_heap_spiram"] = fmt_num(to_float(payload.get("freeHeapSpiram")), 0)
    row["cpu_usage"] = fmt_num(to_float(payload.get("cpuUsage")), 2)
    row["wifi_rssi"] = fmt_num(to_float(payload.get("wifiRSSI")), 0)
    row["shares_accepted"] = fmt_num(to_float(payload.get("sharesAccepted")), 0)
    row["shares_rejected"] = fmt_num(to_float(payload.get("sharesRejected")), 0)
    row["stale_rejected"] = str(compute_stale_rejected(payload))
    row["pool_diff"] = fmt_num(to_float(payload.get("poolDifficulty")), 0)
    row["response_time_ms"] = fmt_num(to_float(payload.get("responseTime")), 3)
    row["process_time_ms"] = fmt_num(to_float(payload.get("processTime")), 3)
    row["stratum_url"] = str(payload.get("stratumURL", ""))
    return row


def fetch_system_info(base_url: str, timeout_s: float) -> Tuple[Optional[Dict[str, Any]], str]:
    """Fetch JSON telemetry from /api/system/info."""
    url = f"{base_url}/api/system/info"
    req = urllib.request.Request(url, headers={"Accept": "application/json"})
    try:
        with urllib.request.urlopen(req, timeout=timeout_s) as resp:
            raw = resp.read().decode("utf-8", errors="replace")
        data = json.loads(raw)
        if not isinstance(data, dict):
            return None, "Unexpected JSON root type"
        return data, ""
    except urllib.error.URLError as exc:
        return None, str(exc.reason)
    except Exception as exc:  # noqa: BLE001
        return None, str(exc)


def emit_transition_alerts(ts_str: str, host: str, row: Dict[str, str], prev: HostPrevState) -> None:
    """Print timestamped soak-test anomaly alerts on state transitions."""
    ok_now = row["ok"] == "1"
    if not ok_now:
        if prev.ok is not False:
            print(f"[{ts_str}] ALERT {host}: HTTP poll failed ({row['error_msg']})", flush=True)
        prev.ok = False
        return

    if prev.ok is False:
        print(f"[{ts_str}] RECOVERED {host}: HTTP telemetry online", flush=True)
    prev.ok = True

    uptime = to_int(row["uptime_s"])
    if uptime is not None:
        if prev.uptime_s is not None and uptime < prev.uptime_s:
            print(
                f"[{ts_str}] ALERT {host}: REBOOT detected (uptime {prev.uptime_s}s -> {uptime}s, "
                f"reason={row['reset_reason']!r})",
                flush=True,
            )
        prev.uptime_s = uptime

    cml = to_int(row["cml_error_count"])
    if cml is not None:
        if prev.cml_error_count is not None and cml > prev.cml_error_count:
            print(
                f"[{ts_str}] ALERT {host}: cmlErrorCount increased ({prev.cml_error_count} -> {cml})",
                flush=True,
            )
        prev.cml_error_count = cml

    wd = to_int(row["watchdog_resets"])
    if wd is not None:
        if prev.watchdog_resets is not None and wd > prev.watchdog_resets:
            print(
                f"[{ts_str}] ALERT {host}: watchdogResets increased ({prev.watchdog_resets} -> {wd}, "
                f"reason={row['last_watchdog_reason']})",
                flush=True,
            )
        prev.watchdog_resets = wd

    throttle = to_int(row["throttle_active"])
    if throttle is not None:
        if prev.throttle_active == 0 and throttle == 1:
            print(
                f"[{ts_str}] ALERT {host}: throttleActive=1 (temp={row['temp_c']}C, vr={row['vr_temp_c']}C, "
                f"power={row['power_w']}W)",
                flush=True,
            )
        prev.throttle_active = throttle

    overheat = to_int(row["overheat_mode"])
    if overheat is not None:
        if prev.overheat_mode == 0 and overheat == 1:
            print(f"[{ts_str}] ALERT {host}: overheat_mode=1!", flush=True)
        prev.overheat_mode = overheat

    free_sram = to_int(row["free_heap_internal"])
    if free_sram is not None:
        if free_sram < FREE_SRAM_ALERT_THRESHOLD and not prev.low_sram_alerted:
            print(
                f"[{ts_str}] ALERT {host}: low internal SRAM ({free_sram} B < {FREE_SRAM_ALERT_THRESHOLD} B)",
                flush=True,
            )
            prev.low_sram_alerted = True
        elif free_sram >= FREE_SRAM_ALERT_THRESHOLD and prev.low_sram_alerted:
            print(f"[{ts_str}] RECOVERED {host}: internal SRAM back above floor ({free_sram} B)", flush=True)
            prev.low_sram_alerted = False


def percentile_sorted(sorted_vals: List[float], pct: float) -> float:
    """Compute linear-interpolated percentile (0..100) on a non-empty sorted list."""
    if len(sorted_vals) == 1:
        return sorted_vals[0]
    pos = (pct / 100.0) * (len(sorted_vals) - 1)
    lo = int(math.floor(pos))
    hi = int(math.ceil(pos))
    if lo == hi:
        return sorted_vals[lo]
    frac = pos - lo
    return sorted_vals[lo] * (1.0 - frac) + sorted_vals[hi] * frac


def analyze_csv(csv_path: Path) -> int:
    """Read a collected telemetry CSV and print soak, baseline, and CUSUM calibration stats."""
    if not csv_path.exists():
        print(f"ERROR: CSV file not found: {csv_path}", file=sys.stderr)
        return 1

    per_host: Dict[str, List[Dict[str, str]]] = {}
    with csv_path.open("r", newline="", encoding="utf-8") as fh:
        reader = csv.DictReader(fh)
        for row in reader:
            host = row.get("host", "unknown")
            per_host.setdefault(host, []).append(row)

    if not per_host:
        print(f"No rows found in {csv_path}")
        return 0

    print(f"\n=== Bitaxe Telemetry Analysis: {csv_path} ===")
    for host, rows in per_host.items():
        ok_rows = [r for r in rows if r.get("ok") == "1"]
        fail_count = len(rows) - len(ok_rows)

        epochs = [v for r in rows if (v := to_float(r.get("epoch_s"))) is not None]
        span_h = ((max(epochs) - min(epochs)) / 3600.0) if len(epochs) >= 2 else 0.0

        reboots = 0
        prev_up: Optional[int] = None
        for r in ok_rows:
            up = to_int(r.get("uptime_s"))
            if up is not None:
                if prev_up is not None and up < prev_up:
                    reboots += 1
                prev_up = up

        cmls = [v for r in ok_rows if (v := to_int(r.get("cml_error_count"))) is not None]
        wds = [v for r in ok_rows if (v := to_int(r.get("watchdog_resets"))) is not None]
        stales = [v for r in ok_rows if (v := to_int(r.get("stale_rejected"))) is not None]
        throttles = sum(1 for r in ok_rows if to_int(r.get("throttle_active")) == 1)
        srams = [v for r in ok_rows if (v := to_int(r.get("free_heap_internal"))) is not None]

        h_effs = [v for r in ok_rows if (v := to_float(r.get("h_eff_ghs"))) is not None]
        powers = [v for r in ok_rows if (v := to_float(r.get("power_w"))) is not None]
        jths = [v for r in ok_rows if (v := to_float(r.get("j_per_th"))) is not None]
        errs = [v for r in ok_rows if (v := to_float(r.get("error_pct"))) is not None]
        temps = [v for r in ok_rows if (v := to_float(r.get("temp_c"))) is not None]
        vr_temps = [v for r in ok_rows if (v := to_float(r.get("vr_temp_c"))) is not None]
        rtts = sorted(v for r in ok_rows if (v := to_float(r.get("response_time_ms"))) is not None)
        dom_ratios = sorted(v for r in ok_rows if (v := to_float(r.get("dom_min_ratio"))) is not None)

        hostname = ok_rows[-1].get("hostname", "") if ok_rows else ""
        version = ok_rows[-1].get("version", "") if ok_rows else ""
        print(f"\nHost: {host} ({hostname}, fw={version})")
        print(f"  Samples      : ok={len(ok_rows)}, fail={fail_count}, span={span_h:.2f} h")
        cml_delta = (max(cmls) - min(cmls)) if len(cmls) >= 2 else 0
        wd_delta = (max(wds) - min(wds)) if len(wds) >= 2 else 0
        stale_delta = (max(stales) - min(stales)) if len(stales) >= 2 else 0
        print(
            f"  Soak Health  : reboots={reboots}, cml_delta={cml_delta}, wd_delta={wd_delta}, "
            f"throttle_samples={throttles}"
        )
        if srams:
            print(f"  Internal SRAM: min={min(srams)} B, avg={statistics.mean(srams):.0f} B")
        if h_effs and powers and jths:
            print(
                f"  Performance  : H_eff={statistics.mean(h_effs):.1f} GH/s, "
                f"power={statistics.mean(powers):.2f} W, eff={statistics.mean(jths):.2f} J/TH"
            )
        if errs:
            print(f"  ASIC Error % : avg={statistics.mean(errs):.3f}%, max={max(errs):.3f}%")
        if temps and vr_temps:
            print(
                f"  Thermal      : ASIC avg/max={statistics.mean(temps):.1f}/{max(temps):.1f} C, "
                f"VR avg/max={statistics.mean(vr_temps):.1f}/{max(vr_temps):.1f} C"
            )
        if rtts:
            print(
                f"  Pool RTT     : avg={statistics.mean(rtts):.1f} ms, p95={percentile_sorted(rtts, 95):.1f} ms, "
                f"stale_delta={stale_delta}"
            )
        if dom_ratios:
            p10 = percentile_sorted(dom_ratios, 10)
            p05 = percentile_sorted(dom_ratios, 5)
            p01 = percentile_sorted(dom_ratios, 1)
            med = statistics.median(dom_ratios)
            suggested_k = min(0.80, max(0.65, round(p01 - 0.03, 2)))
            print(
                f"  Domain Ratio : n={len(dom_ratios)}, med={med:.3f}, p10={p10:.3f}, "
                f"p05={p05:.3f}, p01={p01:.3f}, min={dom_ratios[0]:.3f} -> suggested CUSUM k={suggested_k:.2f}"
            )

    return 0


def parse_args() -> argparse.Namespace:
    """Parse command-line options."""
    parser = argparse.ArgumentParser(
        description="Collect Bitaxe /api/system/info telemetry or analyze an existing telemetry CSV."
    )
    parser.add_argument(
        "--hosts",
        default=",".join(DEFAULT_HOSTS),
        help=f"Comma-separated Bitaxe hosts or URLs (default: {','.join(DEFAULT_HOSTS)})",
    )
    parser.add_argument(
        "--interval",
        type=float,
        default=10.0,
        help="Polling interval in seconds (default: 10.0)",
    )
    parser.add_argument(
        "--duration",
        type=float,
        default=0.0,
        help="Total collection duration in seconds; 0 runs until Ctrl+C (default: 0)",
    )
    parser.add_argument(
        "--timeout",
        type=float,
        default=3.0,
        help="HTTP timeout per request in seconds (default: 3.0)",
    )
    parser.add_argument(
        "--out-dir",
        default="",
        help="Output directory (default: backups/telemetry/<YYYYMMDD-HHMMSS>)",
    )
    parser.add_argument(
        "--raw",
        action="store_true",
        help="Also append raw JSON responses to raw.jsonl in the output directory",
    )
    parser.add_argument(
        "--quiet",
        action="store_true",
        help="Suppress periodic per-cycle status lines (alerts and final summary still print)",
    )
    parser.add_argument(
        "--analyze",
        default="",
        help="Analyze an existing telemetry.csv file and exit",
    )
    return parser.parse_args()


def main() -> int:
    """Run either analysis mode or continuous telemetry collection."""
    args = parse_args()
    if args.analyze:
        return analyze_csv(Path(args.analyze))

    hosts = [normalize_host(h) for h in args.hosts.split(",") if h.strip()]
    if not hosts:
        print("ERROR: No valid hosts specified.", file=sys.stderr)
        return 1

    run_stamp = datetime.now().strftime("%Y%m%d-%H%M%S")
    out_dir = Path(args.out_dir) if args.out_dir else Path("backups") / "telemetry" / run_stamp
    out_dir.mkdir(parents=True, exist_ok=True)
    csv_path = out_dir / "telemetry.csv"
    raw_path = out_dir / "raw.jsonl"

    stop_requested = False

    def handle_signal(_signum: int, _frame: Any) -> None:
        nonlocal stop_requested
        stop_requested = True

    signal.signal(signal.SIGINT, handle_signal)
    signal.signal(signal.SIGTERM, handle_signal)

    prev_states = {display: HostPrevState() for display, _ in hosts}
    start_mono = time.monotonic()
    cycle = 0

    print(
        f"Collecting from {[h[0] for h in hosts]} every {args.interval:.1f}s -> {csv_path}",
        flush=True,
    )

    write_header = not csv_path.exists() or csv_path.stat().st_size == 0
    raw_fh = raw_path.open("a", encoding="utf-8") if args.raw else None
    try:
        with csv_path.open("a", newline="", encoding="utf-8") as csv_fh:
            writer = csv.DictWriter(csv_fh, fieldnames=CSV_COLUMNS)
            if write_header:
                writer.writeheader()
                csv_fh.flush()

            while not stop_requested:
                cycle += 1
                cycle_start_mono = time.monotonic()
                status_parts: List[str] = []

                for display, base_url in hosts:
                    if stop_requested:
                        break
                    now_dt = datetime.now().astimezone()
                    epoch_s = time.time()
                    payload, err_msg = fetch_system_info(base_url, args.timeout)
                    row = build_csv_row(now_dt, epoch_s, display, payload, err_msg)
                    writer.writerow(row)

                    if raw_fh is not None and payload is not None:
                        raw_fh.write(
                            json.dumps(
                                {"collected_at": row["timestamp_iso"], "host": display, "data": payload},
                                separators=(",", ":"),
                            )
                            + "\n"
                        )

                    emit_transition_alerts(now_dt.strftime("%H:%M:%S"), display, row, prev_states[display])
                    if payload is not None:
                        status_parts.append(
                            f"{display}:{row['h_eff_ghs'] or row['hashrate_ghs']}GH@{row['power_w']}W"
                        )
                    else:
                        status_parts.append(f"{display}:ERR")

                csv_fh.flush()
                if raw_fh is not None:
                    raw_fh.flush()

                if not args.quiet:
                    ts_short = datetime.now().strftime("%H:%M:%S")
                    print(f"[{ts_short}] #{cycle} | " + " | ".join(status_parts), flush=True)

                if args.duration > 0 and (time.monotonic() - start_mono) >= args.duration:
                    break

                elapsed = time.monotonic() - cycle_start_mono
                sleep_remaining = max(0.0, args.interval - elapsed)
                while sleep_remaining > 0 and not stop_requested:
                    step = min(0.5, sleep_remaining)
                    time.sleep(step)
                    sleep_remaining -= step
    finally:
        if raw_fh is not None:
            raw_fh.close()

    return analyze_csv(csv_path)


if __name__ == "__main__":
    raise SystemExit(main())
