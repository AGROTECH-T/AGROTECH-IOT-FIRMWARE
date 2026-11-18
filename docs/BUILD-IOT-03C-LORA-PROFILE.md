# BUILD-IOT-03C — preparación del perfil LoRa

Estado: **perfil propuesto, no autorizado**. Este documento prepara los
parámetros y la validación necesarios para habilitar más adelante el enlace
LoRa entre coordinador y gateway. No se ha transmitido ni recibido por radio.

## 1. Alcance

- Fuente única de verdad: `shared/src/AgrotechRadioConfig.h`.
- Validación compartida y bloqueo de activación por doble bandera.
- Documentación del perfil, del cálculo de P.R.A. y de lo que falta confirmar.
- Sin cambios en Frame v1, HMAC, credenciales, identidad, backend, frontend,
  sensores, pines ni arquitectura.
- Sin transmisión, sin Wi-Fi, sin MQTT y sin escritura de registros RF.

## 2. Condiciones de activación

La radio solo puede arrancar si se cumplen **todas** estas condiciones:

| Condición | Constante | Valor actual |
|---|---|---|
| Intención de compilación | `AGROTECH_LORA_TRANSPORT_ENABLED` | `false` |
| Autorización normativa/RF | `AGROTECH_LORA_PROFILE_AUTHORIZED` | `false` |
| Mapa de pines coherente | `agrotechRadioPinsValid()` | válido |
| Perfil completo y en rango | `agrotechRadioProfileValid()` | **inválido** (frecuencia, potencia, preámbulo y CRC sin definir) |

`agrotechRadioCanActivate()` devuelve `false` mientras falte cualquiera de
ellas y `agrotechRadioActivationBlockReason()` explica el motivo en el puerto
serie. Además, un `static_assert` impide compilar con el transporte habilitado
si el perfil no está autorizado.

Los valores operativos (`AGROTECH_LORA_FREQUENCY_HZ = 0`,
`AGROTECH_LORA_TX_POWER_DBM = -1`, `AGROTECH_LORA_PREAMBLE_LENGTH = 0`,
`AGROTECH_LORA_USE_CRC = -1`) se dejan deliberadamente incompletos: aunque
alguien active una bandera, la validación sigue bloqueando la radio.

## 3. Pines y SPI confirmados

Confirmados físicamente en BUILD-IOT-03B: lectura de `VERSION` (`0x42`) =
`0x12`, 10/10 en coordinador y 10/10 en gateway, comunicación estable.

| Señal | GPIO |
|---|---:|
| NSS | 5 |
| RESET | 14 |
| DIO0 | 26 |
| SCK | 18 |
| MOSI | 23 |
| MISO | 19 |

La validación compartida exige seis GPIO distintos, usables en ESP32, no
pertenecientes al bus de flash (`6..11`) y con capacidad de salida para NSS,
RESET, SCK y MOSI (GPIO `34..39` son solo entrada). MISO y DIO0 pueden ser de
solo entrada. No se modificó el cableado.

## 4. Perfil de radio: histórico, propuesta y autorizado

| Parámetro | Histórico (referencia, no usado) | Propuesta 03C (no autorizada) | Autorizado |
|---|---|---|---|
| Frecuencia central | 433.000 MHz | 433.500 MHz | **Ninguno** |
| Potencia TX (conducida) | 17 dBm | 7 dBm | **Ninguno** |
| Ancho de banda | 125 kHz | 125 kHz | **Ninguno** |
| Spreading Factor | 7 | 7 | **Ninguno** |
| Coding Rate | 4/5 (denominador 5) | 4/5 (denominador 5) | **Ninguno** |
| Sync Word | `0xA6` | `0xA6` | **Ninguno** |
| Preámbulo | No especificado | 8 símbolos | **Ninguno** |
| CRC | No especificado | Habilitado | **Ninguno** |

Los valores históricos provienen del par anterior y quedan solo como
referencia (`agrotech_radio_historic`). La propuesta
(`agrotech_radio_proposed`) es documentación: el transporte no la consume.
Ningún valor está autorizado todavía.

### Rango ANE aplicable a telemetría y telecontrol

La tabla de ANE para **telemetría y telecontrol inalámbricos** publica el rango
**433–434.79 MHz** con máximo de **10 mW P.R.A.** El uso libre aplica bajo las
condiciones de esa tabla y no protege frente a interferencias:

- [Portal ANE de acceso IoT y bandas de uso libre](https://portalespectro.ane.gov.co/Portal/mecanismos-acceso-iot.aspx)
- [CNABF / Resolución ANE 105 compilada](https://www.ane.gov.co/Sliders/archivos/GestionTecnica/cnabf/Resolucion000105.pdf)

Este documento solo evalúa la fila de 433–434.79 MHz. Otras filas de la tabla
(otras bandas, potencias o anchos de banda) no se revisaron ni se dan por
aplicables. Tampoco se afirma que el perfil propuesto cumpla la resolución: la
tabla debe verificarse en su versión vigente.

### Frecuencia central y márgenes de emisión

La candidata de 433.500 MHz deja, con BW nominal de 125 kHz (±62.5 kHz),
una banda nominal de 433.4375–433.5625 MHz:

- Margen al borde inferior (433.00 MHz): 0.4375 MHz.
- Margen al borde superior (434.79 MHz): 1.2275 MHz.

Estos márgenes usan el ancho de banda **nominal**, no el ocupado. La emisión
LoRa real incluye lóbulos laterales y tolerancia del oscilador del RA-01; el
ancho ocupado y la máscara de emisión aún no se han medido. La frecuencia sigue
**sin confirmar** y sujeta a la coordinación del despliegue.

## 5. Potencia: conducida, P.R.A. y P.I.R.E.

Son magnitudes distintas y no deben intercambiarse:

| Magnitud | Referencia | Fórmula |
|---|---|---|
| Potencia conducida | Puerto de antena | `P_cond` medida con carga y analizador |
| P.R.A. (radiada aparente) | Dipolo de media onda | `PRA = P_cond + G_dBd - L` |
| P.I.R.E. / EIRP | Radiador isotrópico | `EIRP = P_cond + G_dBi - L = PRA + 2.15 dB` |

El límite tabulado para la fila de 433–434.79 MHz es 10 mW P.R.A. ≈ 10 dBm
P.R.A. ≈ 12.15 dBm EIRP, siempre que la ganancia se exprese en la referencia
correcta. La propuesta usa 7 dBm conducidos suponiendo antena de 0 dBd
(2.15 dBi) y 0.5 dB de pérdidas en alimentador y conectores:

```text
PRA  = 7 dBm + 0 dBd - 0.5 dB = 6.5 dBm ≈ 4.47 mW
EIRP = 7 dBm + 2.15 dBi - 0.5 dB = 8.65 dBm ≈ 7.3 mW
```

Queda por debajo de 10 mW P.R.A. con ~3.5 dB de margen **bajo esos supuestos**,
que no son mediciones:

| Potencia conducida | PRA con 0 dBd y 0.5 dB de pérdidas | ¿Bajo 10 mW? |
|---:|---:|---|
| 7 dBm (propuesta) | 6.5 dBm ≈ 4.5 mW | Sí, con margen |
| 10 dBm | 9.5 dBm ≈ 8.9 mW | Sí, sin margen |
| 17 dBm (histórico) | 16.5 dBm ≈ 44.7 mW | No |

Pendiente de verificar, sin inventar valores:

- **Ganancia de antena:** no medida. El valor 0 dBd es una suposición de cálculo.
- **Pérdidas de cable y conectores:** no medidas; 0.5 dB es una suposición.
- **ROE y desadaptación:** no medidas.
- **Potencia real del RA-01:** falta confirmar en la hoja de datos del módulo
  concreto la potencia conducida máxima y su tolerancia. No se reutiliza el
  valor histórico de 17 dBm.

Cualquier valor final de potencia debe salir de una medición o de la hoja de
datos del módulo y de la antena, no de esta tabla de supuestos.

## 5.1 Ancho de banda, ocupación espectral y airtime

- **Ancho de banda nominal:** 125 kHz (candidato, no autorizado). La ocupación
  real es mayor que el nominal por lóbulos laterales, tolerancia de reloj y
  rampas del PA; no se ha medido con analizador.
- **Tiempo en aire del Frame v1:** con 2 lecturas el frame mide
  `25 + 5·2 + 32 = 67 bytes`. Para SF7, BW 125 kHz, CR 4/5, preámbulo 8,
  encabezado explícito y CRC activo:

```text
Tsym            = 2^7 / 125000 = 1.024 ms
Preámbulo       = (8 + 4.25) · 1.024 ms            = 12.544 ms
Símbolos payload= 8 + 5 · ceil((8·67 - 4·7 + 28 + 16)/28) = 108
Payload         = 108 · 1.024 ms                   = 110.592 ms
Airtime total   = 12.544 + 110.592                 ≈ 123.1 ms
```

  Con el frame máximo de 137 bytes (16 lecturas) el airtime sería
  ≈ 225.5 ms. Son cálculos del formato LoRa para el perfil candidato, no
  mediciones.
- **Ciclo de trabajo previsto:** el coordinador adquiere y transmitiría cada
  `COORDINATOR_TX_INTERVAL_MS = 60000` (60 s). Con 123.1 ms por frame, la
  ocupación sería `123.1 / 60000 ≈ 0.205 %` del tiempo, sin contar reintentos
  ni balizas. La normativa aplicable y su ciclo de trabajo permitido siguen
  pendientes de confirmar.

## 6. Parámetros que deben coincidir entre transmisor y receptor

Obligatorios para que el enlace funcione y para que ambas placas usen el mismo
perfil:

- Frecuencia central.
- Ancho de banda.
- Spreading Factor.
- Coding Rate.
- Sync Word.
- Longitud de preámbulo.
- CRC habilitado o deshabilitado.
- Modo de encabezado (el driver usa encabezado explícito).

El firmware toma todos ellos de la misma cabecera mediante
`CoordinatorConfig.h` y `GatewayConfig.h`, por lo que no pueden divergir. En
tiempo de ejecución cada placa imprime:

```text
[LoRa] profile revision=1 signature=0x82811B29
[LoRa] activation: <motivo del bloqueo>
```

Con el perfil operativo actual (incompleto) la firma es `0x82811B29` en ambas.
Si en el futuro los valores difieren, las firmas diferirán y el desajuste será
visible en el Monitor Serie.

## 7. Pendientes de confirmación antes de transmitir

1. Frecuencia exacta autorizada y verificación de borde de banda para 125 kHz.
2. Potencia conducida y P.R.A. medidas: ganancia de antena, pérdidas, ROE.
3. Potencia máxima real del RA-01 según su hoja de datos y variante de banda.
4. SF, BW y CR definitivos (la propuesta es una referencia histórica, no una
   decisión técnica validada para el despliegue).
5. Preámbulo y CRC acordados para el par.
6. Ciclo de trabajo/airtime permitidos y estrategia de reintentos.
7. Prueba de banco con carga artificial o analizador antes de emitir.
8. Prueba de alcance y coexistencia en el sitio real.

## 8. Validación implementada

| Elemento | Ubicación |
|---|---|
| Banderas, perfil histórico/propuesto, validadores y firma | `shared/src/AgrotechRadioConfig.h` |
| Bloqueo y uso del perfil en el coordinador | `coordinator/coordinator.ino` (`initializeRadio`) |
| Validación y bloqueo en el gateway | `gateway/gateway.ino` (`validateGatewayConfig`, `setup`) |
| Mapeo de constantes a cada sketch | `coordinator/CoordinatorConfig.h`, `gateway/GatewayConfig.h` |

Criterios aplicados en ambos: frecuencia dentro de 137–1020 MHz, SF 6–12,
BW soportado por el SX1278 (7.8 a 500 kHz), CR 5–8, potencia 0–20 dBm, sync
word 0–255, preámbulo ≥ 1, CRC 0/1 y pines válidos y distintos.

## 9. Cómo habilitar el enlace más adelante

Solo después de la autorización y de las mediciones:

1. Fijar los valores aprobados en las constantes operativas
   `AGROTECH_LORA_*` (no en las de documentación).
2. Incrementar `AGROTECH_LORA_PROFILE_REVISION`.
3. Poner `AGROTECH_LORA_PROFILE_AUTHORIZED = true` y después
   `AGROTECH_LORA_TRANSPORT_ENABLED = true`.
4. Compilar ambas placas, confirmar que ambas imprimen la misma firma y que
   `activation: allowed`.
5. Ejecutar primero una prueba de banco controlada; la prueba en aire requiere
   cumplir los límites aplicables.

## 10. Verificación de esta fase

- Compilación real con el `arduino-cli` incluido en Arduino IDE 2.x, core ESP32
  3.3.12, FQBN `esp32:esp32:esp32`, bibliotecas del proyecto (`LoRa` 0.8.0,
  `MQTT`) y `shared` como `AgrotechProtocol`, desde las carpetas definitivas
  del repositorio y sin espejos temporales:
  - Gateway: correcto, 566.260 bytes de programa y 31.932 bytes de RAM global.
  - Coordinador: correcto, 330.756 bytes y 24.092 bytes de RAM.
- Las copias locales de `Agrotech*.cpp/.h` en `coordinator/` se eliminaron
  tras comprobar que eran idénticas a `shared/src/`; ver
  `docs/BUILD-IOT-03C-FIX.md`. Ambos sketches enlazan ahora la misma
  implementación shared.
- `AGROTECH_LORA_TRANSPORT_ENABLED`, `AGROTECH_LORA_PROFILE_AUTHORIZED` y
  `AGROTECH_MQTT_TRANSPORT_ENABLED` permanecen en `false`.
- Misma cabecera compartida en ambos sketches; misma firma de perfil.
- No se cargó firmware, no se transmitió ni recibió por LoRa y no se intentó
  conectar a Wi-Fi/MQTT.

Esta fase **no** demuestra que el enlace LoRa funcione: no se ha realizado
ninguna prueba de transmisión.
