# A/B Test STM32 vs ESP32 (Paridad de Audio)

## Objetivo
Verificar que el motor STM32 con protocolo SPI reproduce y procesa con comportamiento equivalente al `AudioEngine.cpp` de ESP32.

## Preparación
- Misma tarjeta SD / mismos samples en ambos sistemas.
- Misma ruta de monitoreo de audio (mismo DAC/ganancia/monitor).
- Volumen de salida igualado a oído y con medidor RMS.

## Escenarios de prueba
1. **Dry trigger test**
   - 1 pad por vez, velocity fija 127, sin FX.
   - Medir ataque, sustain, release y nivel pico.
2. **Polyphony stress**
   - 8 disparos simultáneos por step (kick/snare/ch/oh/clap + dobles).
   - Verificar clipping, dropouts y voice stealing.
3. **Master FX**
   - Delay ON/OFF, mix 0.3/0.5, feedback 0.3/0.6.
   - Flanger ON/OFF, depth 0.3/0.7, mix 0.3/0.6.
4. **Bulk commands SPI**
   - `CMD_BULK_TRIGGERS` con 3, 6 y 8 eventos.
   - `CMD_BULK_FX` con 3 cambios por paquete.
5. **Status/Telemetry**
   - `CMD_GET_STATUS`, `CMD_GET_VOICES`, `CMD_GET_PEAKS`, `CMD_PING`.

## Métricas
- Latencia trigger->audio (ms).
- Pico master (normalizado 0..1).
- Nº de voces activas reportadas.
- Errores de CRC / paquetes descartados.
- Estabilidad: 10 min sin dropout.

## Criterio de aceptación
- Latencia y dinámica percibida equivalentes.
- Sin dropouts en test de 8 voces.
- Respuestas SPI coherentes para status/peaks/voices/ping.

## Comandos clave implementados en STM32 actual
- Trigger: `0x01`, `0x02`, `0x03`, `0x04`
- Volumen: `0x10`, `0x11`, `0x12`
- Delay: `0x30`, `0x31`, `0x32`, `0x33`
- Flanger: `0x38`, `0x39`, `0x3A`, `0x3B`, `0x3C`
- Queries: `0xE0`, `0xE1`, `0xE2`, `0xE3`, `0xEE`
- Bulk: `0xF0`, `0xF1`

## Notas
- La migración completa de TODO el `AudioEngine.cpp` (incluyendo sidechain/per-track FX completos) sigue en fase incremental.
- Este plan permite validar paridad por etapas sin bloquear integración.
