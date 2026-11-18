# Design

## Context

Conforme documentado na Seção 2.1 e na Fase 0 de `doc/FIRMWARE_IMPROVEMENTS.md`, o hardware físico em produção (`192.168.1.108`) roda a build de fábrica `V1.0.1-rc.10` (`LT-001` / `601a-4`), que expõe campos ausentes neste repositório (`selfTestParams`, `error-code`) e contém dados únicos de calibração/Wi-Fi/pool na partição `nvs`. Os offsets reais definidos em `partitions.csv` são:
- `nvs`: offset `0x9000`, tamanho `0x6000` (`24.576` bytes / `24 KB`)
- `otadata`: offset `0xf10000`, tamanho `0x2000` (`8.192` bytes / `8 KB`)
- Flash completa ESP32-S3: offset `0x0`, tamanho `0x1000000` (`16.777.216` bytes / `16 MB`)

Atualmente o repositório possui apenas `tools/upload2device.py` (OTA HTTP) e `tools/run_qemu_tests.sh`, mas nenhum script padronizado para backup físico pré-flash e auditoria pós-flash em bancada.

## Goals / Non-Goals

**Goals:**
- Fornecer um único script CLI (`scripts/backup_and_verify_gamma_v2.sh`) com subcomandos claros (`backup-http`, `backup-flash`, `verify-post-flash`) e falhas explícitas (`set -euo pipefail`).
- Garantir que os backups nunca sejam versionados acidentalmente adicionando `backups/` ao `.gitignore`.
- Documentar no `doc/FIRMWARE_IMPROVEMENTS.md` o fluxo de uso do script e o estado das 3 changes complementares no OpenSpec.

**Non-Goals:**
- Reimplementar `selfTestParams` ou `error-code` no firmware C sem possuir o schema completo capturado da placa física (essa decisão fica para depois da captura do snapshot com `backup-http`).
- Executar gravação/flash automático dentro do script de backup (separação intencional entre leitura/verificação segura e comando de gravação manual).

## Decisions

### 1. Estrutura do Script `scripts/backup_and_verify_gamma_v2.sh`
- **Decisão:** Script Bash (`#!/usr/bin/env bash`, `set -euo pipefail`) utilizando `curl` + `python3` (stdlib `json` / `sys`) para parse/validação JSON (evitando dependência obrigatória de `jq`), `esptool.py` (ou fallback `python3 -m esptool`) para leitura via serial USB-C, e `sha256sum` / `shasum -a 256` (portável entre Linux e macOS).
- **Rationale:** O ambiente de desenvolvimento dos usuários inclui macOS e Linux com ESP-IDF (`python3` e `esptool.py` sempre presentes no shell do IDF), enquanto `jq` pode não estar instalado por padrão.
- **Diretório de saída:** `backups/<YYYYMMDD-HHMMSS>/` (ou variável de ambiente `BACKUP_DIR` opcional), permitindo múltiplas capturas sem sobrescrever snapshots anteriores.
- **Subcomandos:**
  1. `backup-http <device_ip>`: baixa `/api/system/info` e `/api/system/asic` com timeout curto (`--fail --max-time 10`), valida sintaxe JSON via `python3 -m json.tool` e destaca se `selfTestParams` ou `error-code` foram preservados.
  2. `backup-flash <serial_port>`: lê `0x9000 0x6000` (`nvs.bin`), `0xf10000 0x2000` (`otadata.bin`) e `0x0 0x1000000` (`flash_16mb.bin`), verifica tamanhos exatos (`24576`, `8192`, `16777216`), gera `SHA256SUMS` e valida o manifesto com `-c`.
  3. `verify-post-flash <device_ip>`: valida `boardVersion == "601a-4"`, `freeHeapInternal >= 110000`, ausência de `power_fault` não vazio e presença dos 7 campos da Fase 4 (`throttleActive`, `targetFrequency`, `targetVoltage`, `vdroopOffsetMv`, `dvfsProfile`, `cmlErrorCount`, `watchdogResets`).

### 2. Proteção contra Vazamento de Credenciais no `.gitignore`
- **Decisão:** Adicionar `backups/` ao `.gitignore` (mantendo a entrada existente `bitaxe-backup/` intacta).
- **Rationale:** `nvs.bin` contém credenciais Wi-Fi e strings de pool Stratum em claro/NVS; ignorar `backups/` impede `git add .` acidental.

## Risks / Trade-offs

- **[Risk] Endereço ou baud rate da porta serial variar entre cabos USB-C/CH340/CDC no macOS/Linux** → **Mitigação**: Receber `<serial_port>` como argumento explícito (e permitir `ESPTool` baud rate configurável via `BAUD_RATE`, padrão `460800` com fallback seguro documentado).
- **[Risk] Dispositivo físico ainda não estar conectado durante testes em CI** → **Mitigação**: Validar o script localmente com sintaxe (`bash -n`), `--help` e simulação HTTP/JSON via servidor mock temporário em Python na fase de verificação da change.
