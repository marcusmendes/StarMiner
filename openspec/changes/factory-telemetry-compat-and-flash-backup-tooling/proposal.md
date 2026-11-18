# Proposal

## Why

A Seção 2.1 e a Fase 0 de `doc/FIRMWARE_IMPROVEMENTS.md` exigem backup físico integral da Flash (`16 MB`), da partição NVS (`0x9000`, `24 KB`) e captura do payload completo de telemetria da build de fábrica `V1.0.1-rc.10` (`LT-001` / `601a-4`, incluindo `selfTestParams` e `error-code`) antes de qualquer gravação via USB-C, além de auditoria pós-flash em bancada (`freeHeapInternal >= 110 KB`, `boardVersion == "601a-4"`, novos campos de telemetria). Hoje o repositório não possui ferramental automatizado que execute esse protocolo com verificação de integridade.

## What Changes

- **Script de Backup e Auditoria Pós-Flash (`scripts/backup_and_verify_gamma_v2.sh`)**:
  - Subcomando `backup-http <device_ip>`: coleta e salva em `backups/<timestamp>/` as respostas brutas de `GET /api/system/info` (`system_info.json`) e `GET /api/system/asic` (`system_asic.json`), preservando integralmente campos exclusivos da build de fábrica `V1.0.1-rc.10` (`selfTestParams`, `error-code`).
  - Subcomando `backup-flash <serial_port>`: executa `esptool.py read_flash` para extrair a partição `nvs` (`0x9000`, `0x6000` / `24 KB`), `otadata` (`0xf10000`, `0x2000` / `8 KB`, conforme `partitions.csv`) e a flash completa (`0x0`, `0x1000000` / `16 MB`) em `backups/<timestamp>/`, valida os tamanhos exatos em bytes e gera manifesto `SHA256SUMS`.
  - Subcomando `verify-post-flash <device_ip>`: consulta `GET /api/system/info` após o flash e valida automaticamente `boardVersion == "601a-4"`, `freeHeapInternal >= 110000`, ausência de `power_fault` e presença dos campos da Fase 4 (`throttleActive`, `targetFrequency`, `targetVoltage`, `vdroopOffsetMv`, `dvfsProfile`, `cmlErrorCount`, `watchdogResets`).
- **Higiene de Repositório e Documentação**:
  - Adicionar `backups/` ao `.gitignore` para impedir commit acidental de dumps binários de NVS contendo credenciais Wi-Fi e de pool Stratum.
  - Atualizar `doc/FIRMWARE_IMPROVEMENTS.md` documentando o uso do script `scripts/backup_and_verify_gamma_v2.sh` na Seção 2.1 / Fase 0 e registrando as changes complementares de UI, testes unitários e ferramental.

## Capabilities

### New Capabilities
- `hardware-backup-and-factory-verification`: Ferramental automatizado para snapshot HTTP da telemetria de fábrica (`V1.0.1-rc.10`), backup binário pré-flash com verificação de tamanho e SHA-256 (Flash `16 MB`, `nvs` e `otadata`) e auditoria automática de saúde pós-flash.

### Modified Capabilities
<!-- Nenhuma capability do firmware em openspec/specs/ é alterada por esta change de ferramental e documentação. -->

## Impact

- **Arquivos Afetados**:
  - `scripts/backup_and_verify_gamma_v2.sh` (novo script executável POSIX/bash usando `curl`, `python3`/`jq`, `esptool.py`, `shasum`/`sha256sum`)
  - `.gitignore` (inclusão de `backups/`)
  - `doc/FIRMWARE_IMPROVEMENTS.md` (instruções operacionais do protocolo de backup e auditoria)
- **Dependências Externas de Bancada**:
  - Requer `esptool.py` (ambiente ESP-IDF) apenas para `backup-flash` e acesso HTTP na rede local para `backup-http` e `verify-post-flash`.
