# Tasks

## 1. Script de Backup Físico, Snapshot HTTP e Auditoria Pós-Flash

- [x] 1.1 Criar `scripts/backup_and_verify_gamma_v2.sh` (executável com `chmod +x` e `set -euo pipefail`) implementando os subcomandos `backup-http <device_ip>` (salvando `system_info.json` e `system_asic.json` completos em `backups/<timestamp>/`), `backup-flash <serial_port>` (extraindo `nvs` `0x9000`/`0x6000`, `otadata` `0xf10000`/`0x2000` e flash completa `0x0`/`0x1000000` via `esptool.py`, validando tamanhos exatos e gerando `SHA256SUMS`) e `verify-post-flash <device_ip>` (validando `boardVersion == "601a-4"`, `freeHeapInternal >= 110000`, ausência de `power_fault` e presença dos 7 campos da Fase 4), verificando a sintaxe com `bash -n scripts/backup_and_verify_gamma_v2.sh` e saída de `--help`.
- [x] 1.2 Validar os subcomandos `backup-http` e `verify-post-flash` contra um servidor HTTP local mockado em Python (incluindo payload simulando `V1.0.1-rc.10` com `selfTestParams`/`error-code` e payload pós-flash positivo e negativo), confirmando códigos de saída `0` nos cenários válidos e não-zero nos cenários de falha sem deixar arquivos residuais temporários.

## 2. Higiene do Repositório e Documentação Operacional

- [x] 2.1 Adicionar `backups/` ao `.gitignore` preservando a regra existente `bitaxe-backup/`, verificando via `git check-ignore -v backups/test.bin` que dumps locais são ignorados.
- [x] 2.2 Atualizar `doc/FIRMWARE_IMPROVEMENTS.md` documentando o uso de `scripts/backup_and_verify_gamma_v2.sh` na Seção 2.1 / Fase 0 e listando as changes complementares de UI (`axeos-ui-dvfs-telemetry-and-oled-sync`), testes (`unit-tests-thermal-dvfs-watchdog-and-ui`) e ferramental (`factory-telemetry-compat-and-flash-backup-tooling`), verificando os links markdown do arquivo.

## Workflow follow-up

- No hardware físico (`192.168.1.108`), executar `backup-http` e `backup-flash` via USB-C antes de gravar o firmware compilado e validar com `verify-post-flash` após a gravação.
