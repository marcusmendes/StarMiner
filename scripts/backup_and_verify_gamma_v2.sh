#!/usr/bin/env bash
set -euo pipefail

SCRIPT_NAME="$(basename "$0")"
DEFAULT_BAUD_RATE="460800"

usage() {
    cat <<EOF
Usage: ${SCRIPT_NAME} <subcommand> [options]

Backup factory firmware/telemetry and audit post-flash health for StarMiner Bitaxe Gamma (601a* / GammaV2).

Subcommands:
  backup-http <device_ip>       Capture raw GET /api/system/info and GET /api/system/asic JSON snapshots
                                into BACKUP_DIR (default: backups/<YYYYMMDD-HHMMSS>/).
  backup-flash <serial_port>    Dump nvs (0x9000, 24 KB), otadata (0xf10000, 8 KB), and full 16 MB flash
                                via esptool.py, verify exact byte sizes, and create/verify SHA256SUMS.
  verify-post-flash <device_ip> Audit GET /api/system/info for boardVersion starting with "601a" (e.g. 601a-2, 601a-4),
                                freeHeapInternal>=110000, absence of power_fault, and all 10 Phase 4/5 telemetry fields.
  --help, -h, help              Show this help message and exit.

Environment Variables:
  BACKUP_DIR   Override output directory for backup-http and backup-flash (default: backups/<timestamp>)
  BAUD_RATE    Serial baud rate for esptool.py in backup-flash (default: ${DEFAULT_BAUD_RATE})
  ESPTOOL_CMD  Override esptool command (default: esptool.py or 'python3 -m esptool')
EOF
}

die() {
    echo "ERROR: $*" >&2
    exit 1
}

require_cmd() {
    command -v "$1" >/dev/null 2>&1 || die "Required command not found: $1"
}

normalize_base_url() {
    local input="$1"
    if [[ ! "$input" =~ ^https?:// ]]; then
        input="http://${input}"
    fi
    echo "${input%/}"
}

resolve_backup_dir() {
    if [[ -n "${BACKUP_DIR:-}" ]]; then
        echo "${BACKUP_DIR}"
    else
        echo "backups/$(date +%Y%m%d-%H%M%S)"
    fi
}

detect_sha256_tool() {
    if command -v sha256sum >/dev/null 2>&1; then
        echo "sha256sum"
    elif command -v shasum >/dev/null 2>&1; then
        echo "shasum"
    else
        die "Neither 'sha256sum' nor 'shasum' was found in PATH"
    fi
}

generate_sha256sums() {
    local dir="$1"
    shift
    local tool
    tool="$(detect_sha256_tool)"
    if [[ "$tool" == "sha256sum" ]]; then
        (cd "$dir" && sha256sum "$@" > SHA256SUMS)
    else
        (cd "$dir" && shasum -a 256 "$@" > SHA256SUMS)
    fi
}

verify_sha256sums() {
    local dir="$1"
    local tool
    tool="$(detect_sha256_tool)"
    if [[ "$tool" == "sha256sum" ]]; then
        (cd "$dir" && sha256sum -c SHA256SUMS)
    else
        (cd "$dir" && shasum -a 256 -c SHA256SUMS)
    fi
}

resolve_esptool() {
    if [[ -n "${ESPTOOL_CMD:-}" ]]; then
        read -r -a ESPTOOL_ARR <<< "${ESPTOOL_CMD}"
        return 0
    fi
    if command -v esptool.py >/dev/null 2>&1; then
        ESPTOOL_ARR=("esptool.py")
        return 0
    fi
    if command -v esptool >/dev/null 2>&1; then
        ESPTOOL_ARR=("esptool")
        return 0
    fi
    if command -v python3 >/dev/null 2>&1 && python3 -m esptool version >/dev/null 2>&1; then
        ESPTOOL_ARR=("python3" "-m" "esptool")
        return 0
    fi
    die "esptool.py not found. Source your ESP-IDF environment or set ESPTOOL_CMD."
}

verify_file_size() {
    local file_path="$1"
    local expected_bytes="$2"
    [[ -f "$file_path" ]] || die "Expected backup file not found: ${file_path}"
    local actual_bytes
    actual_bytes="$(python3 -c 'import os, sys; print(os.path.getsize(sys.argv[1]))' "$file_path")"
    if [[ "$actual_bytes" != "$expected_bytes" ]]; then
        die "Size mismatch for ${file_path}: expected ${expected_bytes} bytes, got ${actual_bytes} bytes"
    fi
}

cmd_backup_http() {
    if [[ $# -ne 1 || -z "${1:-}" ]]; then
        die "backup-http requires <device_ip>. Usage: ${SCRIPT_NAME} backup-http <device_ip>"
    fi

    require_cmd curl
    require_cmd python3

    local base_url out_dir info_file asic_file
    base_url="$(normalize_base_url "$1")"
    out_dir="$(resolve_backup_dir)"
    mkdir -p "$out_dir"

    info_file="${out_dir}/system_info.json"
    asic_file="${out_dir}/system_asic.json"

    echo "Capturing factory telemetry from ${base_url} into ${out_dir}/ ..."
    curl --fail --silent --show-error --max-time 10 "${base_url}/api/system/info" -o "$info_file"
    curl --fail --silent --show-error --max-time 10 "${base_url}/api/system/asic" -o "$asic_file"

    python3 - "$info_file" "$asic_file" <<'PY'
import json
import os
import sys

info_path, asic_path = sys.argv[1], sys.argv[2]

def load_json_dict(path):
    if not os.path.exists(path) or os.path.getsize(path) == 0:
        raise ValueError(f"Empty or missing JSON file: {path}")
    with open(path, "r", encoding="utf-8") as f:
        data = json.load(f)
    if not isinstance(data, dict) or len(data) == 0:
        raise ValueError(f"Expected non-empty JSON object in {path}")
    return data

info = load_json_dict(info_path)
asic = load_json_dict(asic_path)

print(f"Saved {info_path} ({os.path.getsize(info_path)} bytes, {len(info)} top-level keys)")
print(f"Saved {asic_path} ({os.path.getsize(asic_path)} bytes, {len(asic)} top-level keys)")

for key in ("version", "axeOSVersion", "boardVersion", "model", "ASICModel"):
    if key in info:
        print(f"  {key}: {info[key]!r}")

for factory_key in ("selfTestParams", "error-code"):
    if factory_key in info:
        print(f"  [factory key preserved] {factory_key}: present")
    else:
        print(f"  [factory key note] {factory_key}: not present in /api/system/info")
PY

    echo "HTTP telemetry snapshot completed successfully."
}

cmd_backup_flash() {
    if [[ $# -ne 1 || -z "${1:-}" ]]; then
        die "backup-flash requires <serial_port>. Usage: ${SCRIPT_NAME} backup-flash <serial_port>"
    fi

    require_cmd python3
    resolve_esptool

    local serial_port="$1"
    local baud_rate="${BAUD_RATE:-$DEFAULT_BAUD_RATE}"
    local out_dir
    out_dir="$(resolve_backup_dir)"
    mkdir -p "$out_dir"

    local nvs_file="${out_dir}/nvs.bin"
    local otadata_file="${out_dir}/otadata.bin"
    local flash_file="${out_dir}/flash_16mb.bin"

    echo "Starting binary flash backup from ${serial_port} (baud ${baud_rate}) into ${out_dir}/ ..."

    echo "[1/3] Reading nvs partition (0x9000, 0x6000 / 24576 bytes)..."
    "${ESPTOOL_ARR[@]}" --port "$serial_port" --baud "$baud_rate" read_flash 0x9000 0x6000 "$nvs_file"
    verify_file_size "$nvs_file" 24576

    echo "[2/3] Reading otadata partition (0xf10000, 0x2000 / 8192 bytes)..."
    "${ESPTOOL_ARR[@]}" --port "$serial_port" --baud "$baud_rate" read_flash 0xf10000 0x2000 "$otadata_file"
    verify_file_size "$otadata_file" 8192

    echo "[3/3] Reading full 16 MB flash (0x0, 0x1000000 / 16777216 bytes)..."
    "${ESPTOOL_ARR[@]}" --port "$serial_port" --baud "$baud_rate" read_flash 0x0 0x1000000 "$flash_file"
    verify_file_size "$flash_file" 16777216

    echo "Generating and verifying SHA256SUMS..."
    generate_sha256sums "$out_dir" "nvs.bin" "otadata.bin" "flash_16mb.bin"
    verify_sha256sums "$out_dir"

    echo "Flash backup completed and verified in ${out_dir}/"
}

cmd_verify_post_flash() {
    if [[ $# -ne 1 || -z "${1:-}" ]]; then
        die "verify-post-flash requires <device_ip>. Usage: ${SCRIPT_NAME} verify-post-flash <device_ip>"
    fi

    require_cmd curl
    require_cmd python3

    local base_url payload
    base_url="$(normalize_base_url "$1")"
    payload="$(curl --fail --silent --show-error --max-time 10 "${base_url}/api/system/info")"

    python3 - "$payload" <<'PY'
import json
import sys

raw = sys.argv[1]
try:
    data = json.loads(raw)
except json.JSONDecodeError as exc:
    print(f"ERROR: Invalid JSON from /api/system/info: {exc}", file=sys.stderr)
    sys.exit(1)

if not isinstance(data, dict):
    print("ERROR: Expected JSON object from /api/system/info", file=sys.stderr)
    sys.exit(1)

phase4_fields = [
    "throttleActive",
    "targetFrequency",
    "targetVoltage",
    "vdroopOffsetMv",
    "dvfsProfile",
    "cmlErrorCount",
    "watchdogResets",
    "learnedMaxFreq",
    "learnedMinVcore",
    "lastWatchdogReason",
]

checks = [
    ("version", repr(data.get("version")), "INFO"),
    ("axeOSVersion", repr(data.get("axeOSVersion")), "INFO"),
    ("runningPartition", repr(data.get("runningPartition")), "INFO"),
]
failures = []

board_version = data.get("boardVersion")
board_ok = isinstance(board_version, str) and board_version.startswith("601a")
checks.append(("boardVersion", repr(board_version), "PASS" if board_ok else "FAIL (expected '601a*')"))
if not board_ok:
    failures.append(f"boardVersion mismatch: expected prefix '601a', got {board_version!r}")

free_heap = data.get("freeHeapInternal")
heap_is_num = isinstance(free_heap, (int, float)) and not isinstance(free_heap, bool)
heap_ok = heap_is_num and free_heap >= 110000
checks.append(("freeHeapInternal", repr(free_heap), "PASS" if heap_ok else "FAIL (expected >= 110000)"))
if not heap_ok:
    failures.append(f"freeHeapInternal check failed: expected >= 110000, got {free_heap!r}")

power_fault = data.get("power_fault")
fault_ok = power_fault in (None, "", 0, False)
checks.append(("power_fault", repr(power_fault), "PASS" if fault_ok else "FAIL (active fault present)"))
if not fault_ok:
    failures.append(f"power_fault present: {power_fault!r}")

for field in phase4_fields:
    present = field in data and data[field] is not None
    val = data.get(field)
    checks.append((field, repr(val), "PASS" if present else "FAIL (missing)"))
    if not present:
        failures.append(f"Missing Phase 4 telemetry field: {field}")

print(f"{'Check / Field':<22} | {'Observed Value':<22} | {'Status'}")
print("-" * 72)
for name, val_str, status in checks:
    print(f"{name:<22} | {val_str:<22} | {status}")

if failures:
    print("\nPost-flash audit FAILED:", file=sys.stderr)
    for err in failures:
        print(f"  - {err}", file=sys.stderr)
    sys.exit(1)

print("\nPost-flash audit PASSED.")
PY
}

main() {
    if [[ $# -eq 0 ]]; then
        usage >&2
        exit 1
    fi

    local subcmd="$1"
    shift

    case "$subcmd" in
        --help|-h|help)
            usage
            exit 0
            ;;
        backup-http)
            cmd_backup_http "$@"
            ;;
        backup-flash)
            cmd_backup_flash "$@"
            ;;
        verify-post-flash)
            cmd_verify_post_flash "$@"
            ;;
        *)
            echo "ERROR: Unknown subcommand '${subcmd}'" >&2
            echo "" >&2
            usage >&2
            exit 1
            ;;
    esac
}

main "$@"
