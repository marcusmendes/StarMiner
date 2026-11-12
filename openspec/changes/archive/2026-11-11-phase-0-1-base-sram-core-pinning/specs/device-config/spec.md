# Spec Delta: device-config

## Purpose

Garante o reconhecimento determinístico da configuração de hardware, família elétrica (12 V / 5 V) e sensores para placas Bitaxe/StarMiner, incluindo sub-revisões `601a*` (`601a-4`) e o alias de modelo de fábrica `LT-001`.

## ADDED Requirements

### Requirement: Reconhecimento explícito da revisão `601a-4`
O sistema SHALL reconhecer a revisão de placa `"601a-4"` diretamente na tabela de configurações padrão com os mesmos parâmetros de família (`GammaV2`, entrada nominal 12 V, 1 domínio de tensão, `BM1370`), regulador `TPS546` e controlador térmico `EMC2101` da placa `"601a"`.

#### Scenario: Boot com `boardversion` igual a `"601a-4"` na NVS
- **WHEN** o dispositivo inicializa com `boardversion = "601a-4"` armazenado na NVS
- **THEN** o sistema carrega a configuração da família `GammaV2` (`nominal_voltage = 12`, `voltage_domains = 1`, `max_power = 40`, `TPS546 = true`, `EMC2101 = true`, `power_consumption_target = 22`) sem cair no fluxo de placa desconhecida

### Requirement: Fallback por prefixo para sub-revisões `601a*`
O sistema SHALL aplicar fallback para a configuração base da placa `"601a"` sempre que `boardversion` iniciar pelo prefixo `"601a"` e não possuir correspondência exata na tabela padrão, preservando a string original de `boardversion` reportada pelo sistema.

#### Scenario: Boot com futura sub-revisão `"601a-5"`
- **WHEN** o dispositivo inicializa com `boardversion = "601a-5"` na NVS
- **THEN** o sistema aplica o perfil elétrico e térmico da `"601a"` (`GammaV2`, 12 V) e mantém `board_version` igual a `"601a-5"`

### Requirement: Catálogo completo de famílias e suporte ao alias `LT-001`
O sistema SHALL incluir `GammaV2` e `GammaDuo` na tabela de famílias padrão para fallback customizado e mapear o modelo de fábrica `"LT-001"` para a família `GammaV2`.

#### Scenario: Fallback por modelo de dispositivo `"LT-001"`
- **WHEN** `boardversion` não corresponde a nenhuma entrada ou prefixo conhecido e `devicemodel` na NVS é `"LT-001"`
- **THEN** o sistema seleciona a família `GammaV2` (`nominal_voltage = 12`, `voltage_domains = 1`, `max_power = 40`) evitando que `voltage_domains` permaneça zerado ou que o `TPS546` receba limites de 5 V
