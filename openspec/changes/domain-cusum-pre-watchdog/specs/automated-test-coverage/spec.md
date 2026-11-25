# Spec Delta

## ADDED Requirements

### Requirement: Testes unitários em C para filtro CUSUM de domínios e alívio preventivo a quente
A suíte Unity em `components/asic/test/test_power_control.c` (executada em `test` / `test-ci` / QEMU) SHALL verificar imunidade do filtro `EWMA + CUSUM` a ruído curto, detecção de degradação sustentada, reset de estado e seleção de alívio preventivo a quente (`+5 mV` vs `-12.5 MHz`) nos perfis `0..3` dentro dos *Hard Caps*.

#### Scenario: Filtro CUSUM rejeita variância curta e detecta queda sustentada
- **WHEN** o teste Unity injeta séries sintéticas a `1 Hz` com quedas curtas em `~0.78` seguidas de degradação sustentada em `~0.50` em um domínio
- **THEN** as oscilações curtas não disparam detecção, a queda sustentada dispara dentro da janela esperada informando o índice do domínio degradado e `pcm_domain_cusum_reset()` zera os acumuladores

#### Scenario: Decisão de alívio a quente respeita perfis DVFS e Hard Caps
- **WHEN** o teste Unity exercita `pcm_pre_watchdog_apply_relief()` nos perfis `0..3` com e sem folga de potência, temperatura de VR e `VCore`
- **THEN** `Manual` e `Balanced` nunca elevam `VCore`, `Efficiency` e `Performance` aplicam `+5 mV` apenas dentro de `1250 mV` / `30.5 W` / `84.0 °C` e o recuo de frequência nunca ultrapassa o piso de `400.0 MHz`
