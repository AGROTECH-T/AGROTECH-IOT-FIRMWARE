# DEMO-01 — Diagnóstico del flujo Coordinador → LoRa → Gateway

Auditoría de solo lectura del código existente. No se modificó ningún archivo del proyecto (este informe es un archivo nuevo). No se compiló, no se ejecutaron pruebas, no se usó Git ni puertos serie, no se conectaron, flashearon ni modificaron dispositivos o NVS, no se leyeron secretos y no se contactó con ningún broker.

**Clasificación usada:**
- IMPLEMENTADO;
- IMPLEMENTADO PERO NO VALIDADO;
- INCOMPLETO;
- NO IMPLEMENTADO;
- INCOMPATIBLE;
- PENDIENTE DE VALIDACIÓN FÍSICA.

Que exista código no se toma como prueba de que funcione.

---

## 1. Resumen ejecutivo

- **Hoy los dos ESP32 no pueden comunicarse por LoRa, y es intencionado.** El perfil de radio compartido mantiene el transporte bloqueado:
  - `AGROTECH_LORA_TRANSPORT_ENABLED = false` y `AGROTECH_LORA_PROFILE_AUTHORIZED = false`;
  - frecuencia operativa = 0, potencia = −1, preámbulo = 0 y CRC = −1 (`shared/src/AgrotechRadioConfig.h:20-21, 89-96`).

  Con eso, `agrotechRadioCanActivate()` devuelve `false` en ambos dispositivos y ninguno llega a llamar a `LoRa.begin()`.
- **Fuera de ese bloqueo, no hay incompatibilidades entre los dos extremos:**
  - los dos leen los mismos parámetros desde la misma cabecera;
  - usan la misma asignación de pines y el mismo formato de trama (`shared/src/AgrotechProtocol.*`);
  - y el mismo validador de activación.

  Un desajuste se vería como una firma de perfil distinta en el arranque (`agrotechRadioProfileSignature()`).
- **Trabajo mínimo para la demostración:**
  1. que el propietario **autorice un perfil RF** (frecuencia y potencia; hay una propuesta documentada);
  2. rellenar las constantes operativas y activar las dos banderas;
  3. aprovisionar los dos dispositivos por USB serie;
  4. compilar, flashear y observar.

  **No hace falta escribir código nuevo para el enlace.** Para la demostración puede usarse el modo de prueba de enlace que ya existe (`shared/src/AgrotechLinkTest.h`). MQTT puede quedar desactivado.
- **Validación física previa:** solo está documentado el bus SPI del RA-01 en ambos ESP32 (BUILD-IOT-03B: registro VERSION = 0x12, 10/10). **Nunca se ha transmitido ni recibido por radio** y **los sensores no tienen validación física** (`docs/BUILD-IOT-03C-LORA-PROFILE.md:39-40, 249`; `docs/BUILD-IOT-01.md:51-53`; `docs/TESTS.md:257-269`).

---

## 2. Archivos inspeccionados

| Archivo | Uso en la auditoría |
|---|---|
| `shared/src/AgrotechRadioConfig.h` | Perfil de radio, pines, banderas, validación, firma |
| `shared/src/AgrotechProtocol.h/.cpp` | Formato Frame v1, codificación y validación |
| `shared/src/AgrotechLinkTest.h` | Modo de prueba de enlace y estadísticas TX/RX |
| `coordinator/coordinator.ino` | Arranque, radio, ciclo de adquisición, envío |
| `coordinator/CoordinatorConfig.h` | Pines de sensores y LED, intervalos, canales, umbrales |
| `coordinator/CoordinatorSensors.h/.cpp` | BME680, DHT11 y conmutación entre ambos |
| `gateway/gateway.ino` | Recepción LoRa, validación, entrega a MQTT |
| `gateway/GatewayConfig.h` | Mapeo del perfil y de la cola |
| Librería LoRa 0.8.0 instalada (`LoRa.cpp`: `parsePacket`, `receive`) | Comportamiento real del modo de recepción |
| `docs/BUILD-IOT-03C-LORA-PROFILE.md`, `BUILD-IOT-03D-LINK-TEST.md`, `BUILD-IOT-03D-FINAL-PREFLIGHT.md`, `BUILD-IOT-01.md`, `TESTS.md` | Estado documentado de las validaciones |

`CoordinatorCredentials.cpp` y `GatewayCredentials.cpp` solo se tienen en cuenta por su efecto en el arranque, documentado en auditorías anteriores. No se leyeron credenciales.

---

## 3. Arquitectura actual

```text
BME680 (I2C 21/22) ┐                                                  ┌─> [MQTT/Flespi: desactivado]
                   ├─> conmutación → Frame v1 + HMAC → RA-01 ~~LoRa~~> RA-01 → parseFrame ┤
DHT11 (GPIO 4)    ─┘   (Coordinador)        (SPI 18/19/23, NSS 5, RST 14, DIO0 26)       └─> log serie (Gateway)
```

**Hechos:**
- El Coordinador firma cada trama con HMAC-SHA256 (`encodeFrame`, `AgrotechProtocol.cpp:206-228`).
- El Gateway **no** verifica la HMAC. Solo valida la estructura (`parseFrame`, `:230-304`); la HMAC la verifica el backend.
- Con `AGROTECH_MQTT_TRANSPORT_ENABLED = false`, el Gateway solo registra la recepción por serie y no la encola (`gateway.ino`, final de `loop()`).

---

## 4. Estado del Coordinador

| Componente | Evidencia | Clasificación |
|---|---|---|
| Inicialización del ESP32 (serie, LED, configuración, credenciales en NVS, sensores, radio, contadores) | `coordinator.ino` `setup()`, `:372-416`. Sin credenciales válidas espera un `SET` por USB y no continúa | IMPLEMENTADO PERO NO VALIDADO |
| Inicialización del RA-01 | `initializeRadio()`, `:204-229`. Con el bloqueo imprime `[LoRa] disabled: transport flag is false` y no llama a `LoRa.begin()` | INCOMPLETO (bloqueado por configuración) |
| Pines SPI, NSS, RESET, DIO0 | SCK 18, MISO 19, MOSI 23, NSS 5, RESET 14, DIO0 26 (`AgrotechRadioConfig.h:42-47`) | IMPLEMENTADO. Solo hay validación física de SPI (03B); DIO0, RESET y RF: PENDIENTE DE VALIDACIÓN FÍSICA |
| Frecuencia y parámetros de radio | Operativos sin definir (`:89-96`). Propuesta documentada: 433,5 MHz, SF7, 125 kHz, CR 4/5, 7 dBm, sync word 0xA6, preámbulo 8, CRC activo (`:73-83`), no autorizada | INCOMPLETO |
| Lectura del BME680 | `CoordinatorSensors.cpp` (inicialización en 0x77/0x76, sobremuestreo, calefactor desactivado, rangos −40..85 °C y 0..100 %RH, reintentos cada K = 5 ciclos) | IMPLEMENTADO PERO NO VALIDADO (pruebas de host con dobles; PENDIENTE DE VALIDACIÓN FÍSICA) |
| Lectura del DHT11 de respaldo | GPIO 4, intervalo mínimo de 2 s, rangos 0..50 °C y 20..90 %RH | IMPLEMENTADO PERO NO VALIDADO (PENDIENTE DE VALIDACIÓN FÍSICA) |
| Selección entre sensor principal y respaldo | Máquina de estados PRINCIPAL / SOSPECHA / RESPALDO / RECUPERACION / SIN_SENSOR. Umbrales F = 3 y R = 3 (`CoordinatorConfig.h:54-56`). BME680 en los canales 1/2 y DHT11 en los 3/4 | IMPLEMENTADO PERO NO VALIDADO (331 comprobaciones de host según `docs/TESTS.md`; físico pendiente) |
| Construcción del paquete | Frame v1: versión 1, UID `CO-` de 13 caracteres, `key_version`, `boot_counter`, `seq`, número de lecturas, lecturas (canal u16, métrica u8, valor ×100 u16) y HMAC de 32 B. Dos lecturas = **67 B** | IMPLEMENTADO (vector oficial byte a byte en pruebas de host) |
| Identificación del coordinador | UID `CO-XXXXXXXXXX` aprovisionado en NVS | IMPLEMENTADO PERO NO VALIDADO |
| Envío LoRa | `transmitFrame()`, `:231-259`: `beginPacket`, `write`, `endPacket(true)` asíncrono con `onTxDone` (requiere DIO0). Si la transmisión anterior sigue en curso, la trama se descarta | IMPLEMENTADO PERO NO VALIDADO (nunca ejecutado con la radio activa) |
| Errores | Trama descartada si el transmisor está ocupado, si la escritura queda incompleta o si falla el arranque de la transmisión. Sin sensor no hay trama (LED de error). Agotamiento de `seq` o `boot`: parada. Fallo de NVS: parada | IMPLEMENTADO PERO NO VALIDADO |
| Intervalo de transmisión | 60 s (`COORDINATOR_TX_INTERVAL_MS`); el primer ciclo es inmediato | IMPLEMENTADO |
| Reintentos o confirmación | No existen (LoRa unidireccional, sin ACK) | NO IMPLEMENTADO (por diseño) |
| Modo de prueba de enlace | `kEnabled = false`. Con `true` envía 10 tramas sintéticas (25,00 °C y 50,00 %RH), con registros `[LT]` de identificador, etiqueta, bytes, tiempo en aire y hex | IMPLEMENTADO PERO NO VALIDADO |

---

## 5. Estado del Gateway

| Componente | Evidencia | Clasificación |
|---|---|---|
| Inicialización del RA-01 | `setup()`: `if (!gateway_diag::kEnabled && agrotechRadioCanActivate())` → `SPI.begin`, `setPins`, `LoRa.begin`, parámetros y `LoRa.receive()` | INCOMPLETO (bloqueado por el mismo perfil) |
| Pines y parámetros | Los mismos que en el Coordinador, desde la misma cabecera (`GatewayConfig.h`) | IMPLEMENTADO (coincidencia garantizada en compilación) |
| Recepción | `loop()`: `LoRa.parsePacket()` cada ~5 ms, lectura del FIFO hasta 137 B | IMPLEMENTADO PERO NO VALIDADO |
| Modo de recepción | `receive()` pone la radio en recepción continua, pero el primer `parsePacket()` sin paquete la pasa a recepción única, y cada paquete la deja en espera (librería LoRa 0.8.0, `parsePacket`). Funciona, pero hay ventanas breves en las que la radio no escucha | IMPLEMENTADO PERO NO VALIDADO (riesgo menor, sección 8) |
| Validación de estructura | `parseFrame`: versión, longitud exacta, número de lecturas, UID `CO-`, `key_version` ≥ 1, canales ≥ 1 y sin duplicados, métricas conocidas | IMPLEMENTADO |
| Autenticidad (HMAC) | No se verifica en el Gateway | NO IMPLEMENTADO (por diseño: la verifica el backend) |
| Identificación del emisor | `parsed_frame.fields.coordinator_uid`. Solo se imprime en modo de prueba de enlace (`[LT] rx id=…`); en modo normal solo se registran bytes, RSSI y SNR | IMPLEMENTADO (visibilidad limitada en modo normal) |
| Paquetes incompletos o inválidos | Más grandes que el búfer, lectura incompleta o Frame v1 mal formado: se descartan con un mensaje. Los errores de CRC de la radio los descarta `parsePacket` sin contarlos | IMPLEMENTADO PERO NO VALIDADO |
| Duplicados | Solo hay contabilidad en el modo de prueba (`observeRxFrame`: perdidas, duplicadas, desordenadas). La deduplicación real la hace el backend | INCOMPLETO en el Gateway (por diseño) |
| Entrega a MQTT | Solo si `AGROTECH_MQTT_TRANSPORT_ENABLED`: `enqueueFrame()` y el almacén durable (MQTT-QUEUE-PERSISTENCE-05). Hoy está en `false` | IMPLEMENTADO PERO NO VALIDADO (fuera del alcance de DEMO-01; el almacén tiene fallos pendientes de validar) |
| Sin paquetes recibidos | El bucle sigue sondeando. El LED muestra el patrón de estado. No hay registro periódico de "sin tramas" | IMPLEMENTADO (sin aviso periódico) |
| Credenciales en el arranque | `loadOrProvision()` es **obligatorio** aunque MQTT esté desactivado: exige un UID `GW-`, username de 64 caracteres, password vacío y SSID. Sin ellos, espera un `SET` y no arranca la radio | IMPLEMENTADO. Es una dependencia para la demostración (sección 10) |

---

## 6. Comparación de parámetros LoRa

| Parámetro | Coordinador | Gateway | Valor operativo actual | Propuesta documentada | Resultado |
|---|---|---|---|---|---|
| Frecuencia | `AGROTECH_LORA_FREQUENCY_HZ` | La misma | **0** (no definida) | 433 500 000 Hz | Coinciden; INCOMPLETO |
| Spreading Factor | `…_SPREADING_FACTOR` | La misma | 7 | 7 | Coinciden |
| Ancho de banda | `…_BANDWIDTH_HZ` | La misma | 125 000 | 125 000 | Coinciden |
| Coding Rate | `…_CODING_RATE_DENOMINATOR` | La misma | 5 (4/5) | 5 | Coinciden |
| Sync word | `…_SYNC_WORD` | La misma | 0xA6 | 0xA6 | Coinciden |
| Preámbulo | `…_PREAMBLE_LENGTH` | La misma | **0** | 8 | Coinciden; INCOMPLETO |
| CRC | `…_USE_CRC` | La misma | **−1** | 1 (activo) | Coinciden; INCOMPLETO |
| Potencia TX | `…_TX_POWER_DBM` (solo TX) | Se aplica, pero no influye en la recepción | **−1** | 7 dBm | INCOMPLETO |
| Modo de cabecera | Explícito (por defecto de la librería) | Explícito (`parsePacket()` sin tamaño) | — | — | Coinciden |
| Pines | 18/19/23/5/14/26 | Los mismos | — | — | Coinciden; SPI validado (03B) |
| Firma de perfil | Se imprime en el arranque | Se imprime en el arranque | `0x82811B29` con el perfil incompleto | Cambiará al autorizar | Comprobable en el monitor serie |

**No hay INCOMPATIBILIDADES entre los dos extremos [código].** El único bloqueante es el perfil incompleto y sin autorizar, que es común a ambos.

---

## 7. Compatibilidad del protocolo

| Aspecto | Coordinador | Gateway | Resultado |
|---|---|---|---|
| Formato | `encodeFrame` (librería compartida) | `parseFrame` (la misma librería) | Compatible |
| Versión | 1 | Acepta solo la 1 | Compatible |
| Tipos y unidades | Temperatura en °C ×100 (int16 codificado), humedad en %RH ×100, `boot` y `seq` u32 big-endian, canal u16, métrica u8 | Decodifica igual (`decodeTemperature`) | Compatible |
| Canales | 1/2 (BME680) y 3/4 (DHT11) | Acepta cualquier canal ≥ 1 sin duplicados | Compatible |
| Identificadores | UID `CO-` y `key_version` ≥ 1 | Los mismos requisitos | Compatible |
| Tamaño | 67 B con 2 lecturas; máximo 137 | Búfer de 137 | Compatible |
| Confirmaciones y reintentos | No | No | Coherente: enlace unidireccional |
| Autenticación | HMAC generada | No verificada en el Gateway | Por diseño |

---

## 8. Problemas encontrados

| # | Problema | Gravedad | Clasificación |
|---|---|---|---|
| P1 | Perfil RF operativo sin definir y banderas en `false`: la radio no arranca en ningún extremo | **Bloqueante** | INCOMPLETO |
| P2 | La autorización de frecuencia y potencia depende de la normativa ANE (433–434,79 MHz, 10 mW P.R.A.). No se han medido la potencia, la antena ni las pérdidas (`BUILD-IOT-03C §5, §7`) | **Bloqueante** (decisión del propietario) | PENDIENTE DE VALIDACIÓN FÍSICA |
| P3 | Nunca se ha transmitido ni recibido por radio | Alta | PENDIENTE DE VALIDACIÓN FÍSICA |
| P4 | El Gateway exige credenciales MQTT (64 caracteres) y SSID para arrancar aunque MQTT esté desactivado | Media (dependencia operativa) | IMPLEMENTADO |
| P5 | `receive()` seguido de `parsePacket()`: la librería pasa a recepción única y deja breves ventanas en las que no escucha | Baja (posible pérdida de algún paquete) | IMPLEMENTADO PERO NO VALIDADO |
| P6 | En modo normal el Gateway no imprime UID, `seq` ni valores; solo bytes, RSSI y SNR. Sí se ven en modo de prueba de enlace | Baja (visibilidad de la demostración) | INCOMPLETO |
| P7 | Los sensores (BME680, DHT11, conmutación) no tienen validación física | Media | PENDIENTE DE VALIDACIÓN FÍSICA |
| P8 | Las rutas de radio nunca se han compilado con el transporte activo [inferencia]: con las banderas en `false` el compilador puede eliminar ese código | Media | IMPLEMENTADO PERO NO VALIDADO |
| P9 | Errores de CRC de la radio sin contar en el Gateway | Baja | INCOMPLETO |

---

## 9. Trabajo mínimo necesario para la demostración

Sin arquitecturas nuevas y aprovechando el código existente. Requiere autorizaciones separadas.

1. **Decisión del propietario:** autorizar el perfil RF. Propuesta documentada: 433,5 MHz, SF7, 125 kHz, CR 4/5, sync word 0xA6, preámbulo 8, CRC activo y **7 dBm**, dentro de la tabla ANE bajo los supuestos de `BUILD-IOT-03C §5`.
2. **Cambio de configuración, en un único archivo compartido** (`shared/src/AgrotechRadioConfig.h`):
   - rellenar las constantes operativas `AGROTECH_LORA_*`;
   - subir `AGROTECH_LORA_PROFILE_REVISION`;
   - `AGROTECH_LORA_PROFILE_AUTHORIZED = true` y `AGROTECH_LORA_TRANSPORT_ENABLED = true`;
   - dejar `AGROTECH_MQTT_TRANSPORT_ENABLED = false`.
3. **Para el primer contacto** (opcional, recomendado): `agrotech_link_test::kEnabled = true` (10 tramas sintéticas, con `[LT]` en TX y RX y estadísticas de pérdidas). Después, `false` para enviar tramas con los sensores.
4. **Aprovisionamiento por USB serie:**
   - Coordinador: UID `CO-`, `key_version` y clave HMAC;
   - Gateway: UID `GW-`, username de 64 caracteres, password vacío, SSID y password Wi-Fi (necesarios para arrancar aunque no se use MQTT).
5. **Compilar los dos sketches** (fase con autorización de builds) y comprobar que ambos imprimen la **misma firma** y `activation: allowed`.
6. **Hardware:** antenas conectadas en los dos RA-01 antes de emitir; carga artificial o analizador si lo exige `BUILD-IOT-03C §7`.
7. **Flashear y observar:**
   - Coordinador: `[LoRa] configured…` y `[LoRa] Frame v1 queued` (o `[LT] tx …`);
   - Gateway: `[LoRa] rx bytes=67 rssi=… snr=…` (o `[LT] rx id=…`, sin `malformed`).

**Cambio de código opcional** (solo si se quiere ver el UID, `seq` y los valores en modo normal): una línea de registro en el Gateway tras `parseFrame`. No es imprescindible.

---

## 10. Riesgos y dependencias

- **Normativa y RF:** frecuencia, potencia, ciclo de trabajo y antena sin medir. Responsabilidad del propietario (P2).
- **Aprovisionamiento:** sin credenciales en NVS ningún dispositivo pasa del `setup()`. El Gateway necesita un username de 64 caracteres aunque MQTT esté desactivado (P4).
- **Hardware:** solo está validado el SPI. Faltan DIO0 (la interrupción de fin de TX), el RESET y la RF. Una antena ausente o una variante del módulo distinta puede dañar el RA-01 o impedir el enlace.
- **Recepción:** posibles pérdidas puntuales (P5). Intervalo de 60 s en modo normal; el modo de prueba usa el mismo intervalo.
- **Sensores:** sin validación física (P7). Si fallan los dos, no hay trama (LED de error).
- **Compilación** con el transporte activo no verificada (P8).
- **MQTT y almacén durable:** fuera de DEMO-01. El almacén (MQTT-QUEUE-PERSISTENCE-05) **no está validado** y no debe activarse para la demostración.

---

## 11. Pasos propuestos para DEMO-02

Cada paso necesita una autorización explícita aparte.

1. Autorizar el perfil RF: valores concretos y medición de potencia, antena y pérdidas.
2. Autorizar el cambio de configuración en `AgrotechRadioConfig.h` (sección 9, punto 2) y activar el modo de prueba de enlace.
3. Autorizar la compilación de los dos sketches con el transporte activo; registrar tamaños, avisos y firmas.
4. Autorizar el aprovisionamiento de los dos dispositivos por USB serie, sin registrar secretos.
5. Autorizar el flasheo y una prueba de banco a corta distancia con antenas. Completar la tabla de `BUILD-IOT-03D-FINAL-PREFLIGHT.md §8`: 10/10 tramas, hex idéntico, RSSI, SNR y pérdidas.
6. Volver al modo normal con sensores: validar físicamente el BME680, el DHT11 y la conmutación (`docs/TESTS.md §3`) mientras se recibe en el Gateway.
7. (Opcional) Añadir el registro de UID, `seq` y valores en el Gateway para la demostración.
8. Solo después, en una fase distinta: activar MQTT y Flespi, con el almacén durable validado.
