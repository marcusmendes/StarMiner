# hardware-backup-and-factory-verification Specification

## Purpose

Fornece ferramental automatizado para capturar a telemetria original da build de fábrica (`V1.0.1-rc.10`), executar backup binário íntegro da Flash e NVS antes de gravações via USB-C e auditar automaticamente os indicadores de saúde pós-flash no hardware StarMiner Bitaxe Gamma (`601a-4` / `LT-001`).

## Requirements

### Requirement: Pre-Flash Factory HTTP Telemetry Snapshot
The tooling script (`scripts/backup_and_verify_gamma_v2.sh backup-http <device_ip>`) SHALL capture raw JSON responses from `GET /api/system/info` and `GET /api/system/asic` into a timestamped backup directory without stripping factory-specific keys such as `selfTestParams` and `error-code`.

#### Scenario: Capturing factory V1.0.1-rc.10 telemetry over HTTP
- **WHEN** an operator runs `scripts/backup_and_verify_gamma_v2.sh backup-http 192.168.1.108` against a reachable miner
- **THEN** the script creates `backups/<timestamp>/system_info.json` and `backups/<timestamp>/system_asic.json`, validates non-empty valid JSON, and preserves all original fields including `selfTestParams` and `error-code` when present

### Requirement: Pre-Flash Binary Backup with Size and SHA-256 Verification
The tooling script (`scripts/backup_and_verify_gamma_v2.sh backup-flash <serial_port>`) SHALL dump the `nvs` partition (`0x9000`, `0x6000`), the `otadata` partition (`0xf10000`, `0x2000`), and the full `16 MB` flash (`0x0`, `0x1000000`) via `esptool.py read_flash`, verify exact byte counts, and generate a `SHA256SUMS` manifest.

#### Scenario: Successful full 16 MB and NVS backup before flashing
- **WHEN** an operator runs `scripts/backup_and_verify_gamma_v2.sh backup-flash <serial_port>` with ESP-IDF / `esptool.py` available
- **THEN** the script produces `nvs.bin` (`24576` bytes), `otadata.bin` (`8192` bytes), `flash_16mb.bin` (`16777216` bytes), and a verified `SHA256SUMS` file inside `backups/<timestamp>/`, exiting non-zero if any dump size mismatches or fails verification

### Requirement: Post-Flash Health and Telemetry Audit
The tooling script (`scripts/backup_and_verify_gamma_v2.sh verify-post-flash <device_ip>`) SHALL inspect `GET /api/system/info` after flashing and fail with a clear diagnostic if critical `GammaV2` (`601a-4`), SRAM, or Phase 4 telemetry criteria are not met.

#### Scenario: Post-flash audit passes on healthy 601a-4 firmware
- **WHEN** `GET /api/system/info` returns `boardVersion == "601a-4"`, `freeHeapInternal >= 110000`, no `power_fault`, and includes `throttleActive`, `targetFrequency`, `targetVoltage`, `vdroopOffsetMv`, `dvfsProfile`, `cmlErrorCount`, and `watchdogResets`
- **THEN** `verify-post-flash` prints a summary table of the observed values and exits with status `0`

#### Scenario: Post-flash audit flags board mismatch or insufficient internal SRAM
- **WHEN** `boardVersion` differs from `"601a-4"`, `freeHeapInternal < 110000`, `power_fault` is present, or any Phase 4 telemetry field is missing
- **THEN** `verify-post-flash` reports each failed check clearly and exits with a non-zero status code
