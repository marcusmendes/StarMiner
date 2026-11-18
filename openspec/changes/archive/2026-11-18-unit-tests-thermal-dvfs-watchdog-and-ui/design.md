# Design

## Context

O pipeline de CI em `.github/workflows/unittest.yml` executa duas suítes independentes:
1. **Backend C (`build-and-test`)**: compila o subprojeto `test-ci` (`test-ci/CMakeLists.txt` é symlink para `test/CMakeLists.txt`), que inclui `EXTRA_COMPONENT_DIRS "../components"` e `TEST_COMPONENTS "stratum asic"`, rodando os `TEST_CASE` Unity no emulador ESP32-S3 via QEMU (`bitaxeorg/esp32-qemu-test-action`). O diretório `main/` não é compilado como componente de teste no `test-ci`.
2. **Frontend Angular (`frontend-test`)**: executa `npm run test:ci` em `main/http_server/axe-os/` com Karma/Jasmine e `ChromeHeadlessCI`.

Atualmente, os `.spec.ts` de `EditComponent`, `HomeComponent` e `SystemComponent` possuem apenas testes básicos de criação (e `HomeComponent` testa estado de visibilidade), e não existe nenhum `TEST_CASE` em C cobrindo as regras de clamping, *Vdroop* ou DVFS de `power_management_task.c`.

## Goals / Non-Goals

**Goals:**
- Tornar a aritmética crítica de regulação (clamping de VCore, piso de frequência, offsets de *Soft-Throttling*/Watchdog, *Vdroop* e envelopes DVFS) testável no `test-ci` / QEMU sem duplicar lógica entre `main/` e `components/`.
- Garantir cobertura Karma/Jasmine para os fluxos críticos da UI adicionados em `axeos-ui-dvfs-telemetry-and-oled-sync`.

**Non-Goals:**
- Refatorar `test/CMakeLists.txt` para puxar FreeRTOS/NVS/I2C completos de `main/` para dentro do QEMU (o que exigiria mocks extensos de `driver`, `nvs_flash` e `esp_wifi`).
- Testar loops infinitos de tasks FreeRTOS em hardware real dentro desta change.

## Decisions

### 1. Extração de Helper Puro de Controle em `components/asic/`
- **Decisão:** Extrair as funções puras de cálculo numérico de `main/tasks/power_management_task.c` (sem dependências de FreeRTOS, `esp_timer`, `nvs_config` ou `GlobalState`) para `components/asic/include/power_control_math.h` e `components/asic/power_control_math.c` (registrado em `components/asic/CMakeLists.txt`), contendo:
  - `pcm_clamp_vcore_mv(int32_t voltage_mv)` (`1000..1250 mV`)
  - `pcm_get_dvfs_freq_bounds(uint16_t profile, float *min_freq, float *max_freq)`
  - `pcm_compute_dvfs_base_freq(...)`, `pcm_compute_effective_freq(...)`, `pcm_compute_effective_vcore(...)`
  - `pcm_step_vdroop_offset_mv(...)` (deadband `±5 mV`, passo `5 mV`, teto `20 mV`, limite de `32.0 W` e `1250 mV`)
- **Rationale:** `main/CMakeLists.txt` já inclui `../components/asic/include` e linka o componente `asic`, enquanto `test/CMakeLists.txt` / `test-ci/CMakeLists.txt` já testam `"stratum asic"`. Assim, tanto o firmware de produção (`power_management_task.c`) quanto `components/asic/test/test_power_control.c` executam exatamente o mesmo código C sem duplicação nem dependência de drivers I2C/NVS.
- **Alternativa considerada:** Duplicar as fórmulas em um arquivo de teste isolado ou compilar `power_management_task.c` inteiro em `test/` — descartado porque duplicar lógica viola qualidade de código e compilar `power_management_task.c` exigiria mockar `vcore.h`, `thermal.h`, `power.h`, `asic.h`, `nvs_config.h`, `esp_timer` e `driver/uart.h`.

### 2. Testes Isolados de UI no `AxeOS` (`TestBed` + Mocks RxJS)
- **Decisão:** Nos arquivos `edit.component.spec.ts`, `home.component.spec.ts` e `system.component.spec.ts`, fornecer stubs/spies de `SystemApiService` e `LiveDataService` (e importar os módulos PrimeNG / pipes necessários conforme padrão de `home.component.spec.ts`, evitando `RouterTestingModule` depreciado conforme `AGENTS.md`).
- **Rationale:** Mantém os testes determinísticos, rápidos e compatíveis com `npm run test:ci` em ambiente headless sem depender de backend HTTP ou WebSocket ativo.

## Risks / Trade-offs

- **[Risk] Divergência de tipos se `axeos-ui-dvfs-telemetry-and-oled-sync` ainda não tiver sido aplicada quando alguém aplicar esta change** → **Mitigação**: Documentar explicitamente a precedência na `proposal.md` e nas tarefas, mantendo os testes alinhados aos métodos/campos exatos definidos naquela change (`dvfsProfile`, `THERMAL_THROTTLE`, `getSystemRows()`).
- **[Risk] `edit.component.spec.ts` e `system.component.spec.ts` hoje não importam módulos PrimeNG (`DropdownModule`, `CheckboxModule`, `SliderModule`, `MessageModule`) porque o template não era exercitado com dados** → **Mitigação**: Ao fornecer mocks que fazem o `*ngIf="form"` / `*ngIf="combinedData$ | async"` renderizar o DOM completo nos testes, declarar explicitamente os módulos PrimeNG e pipes no `TestBed`.
