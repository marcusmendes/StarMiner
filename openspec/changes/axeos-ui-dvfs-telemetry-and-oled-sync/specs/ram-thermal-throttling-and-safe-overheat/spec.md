# Spec Delta

## ADDED Requirements

### Requirement: Coherent NVS-Preserving Overheat Messaging on OLED and AxeOS
The OLED overheat screen (`SCR_OVERHEAT`) and the AxeOS Settings overheat recovery hint SHALL accurately state that user NVS frequency, voltage, and fan settings were preserved rather than reset.

#### Scenario: OLED overheat screen informs user that NVS settings were preserved
- **WHEN** the device enters `overheat_mode` and displays `SCR_OVERHEAT` on the OLED
- **THEN** the scrolling status label displays `"Thermal protection active. NVS settings preserved. Clear overheat mode in AxeOS when cool."` instead of claiming that power, frequency, and fan configurations were reset

#### Scenario: AxeOS Settings overheat recovery hint reflects preserved NVS configuration
- **WHEN** `overheat_mode === 1` and the `Disable Overheat Mode` section is shown in `EditComponent`
- **THEN** the helper text informs the user that frequency, voltage, and fan settings in NVS were preserved and can be adjusted or resumed once temperatures are safe
