# BUILD-IOT-06 — Diagnóstico MQTT físico del gateway

Fecha: 2026-11-15
Estado: **modo de diagnóstico implementado, compilado y validado en host y
contra broker aislado. El ESP32 físico NO se ha probado.** No se ha hecho
ninguna publicación física.

## Qué hace

Comprueba desde el ESP32 real la cadena Wi-Fi → TLS → credenciales
individuales → MQTT QoS 1 → PUBACK → recepción durable en el backend, **sin
radio LoRa**. Cuando el operador lo pide, inyecta el vector oficial Frame v1
en la misma cola de store-and-forward que usan las tramas LoRa. A partir de
ahí la trama recorre el mismo camino de producción: `enqueueFrame()`, cola
FreeRTOS, `mqttTask`, `buildMqttPayload()` y `publishFrozenPayload()`.

## Garantías de aislamiento

| Garantía | Mecanismo |
|---|---|
| Desactivado por defecto | `GATEWAY_MQTT_DIAG_ENABLED 0` en `GatewayDiagnostics.h`; solo se activa en el `GatewayConfig.local.h` ignorado por git |
| Sin efecto en builds normales | con el diagnóstico apagado, el binario mide lo mismo que en BUILD-IOT-05 (566.304 B / 31.932 B): todo el código de diagnóstico se elimina |
| LoRa nunca se activa | `static_assert`: la build falla si `AGROTECH_LORA_TRANSPORT_ENABLED` o `AGROTECH_LORA_PROFILE_AUTHORIZED` están en true; además `setup()` no inicializa la radio en este modo |
| Separado de producción | `static_assert`: la build falla si `AGROTECH_MQTT_TRANSPORT_ENABLED` está en true. Una build de diagnóstico nunca es una build de producción |
| Separado del link test LoRa | `static_assert` contra `agrotech_link_test::kEnabled` |
| Sin publicación automática | solo publica cuando el operador escribe `DIAG PUBLISH`; máximo `GATEWAY_MQTT_DIAG_MAX_PUBLISHES` (10) por arranque |
| Vector íntegro | al arrancar se ejecutan `parseFrame()` y `verifyFrameTag()` con la clave **pública** de prueba; si fallan, `DIAG PUBLISH` se rechaza |
| Sin secretos en Serial | nunca se imprimen la contraseña MQTT ni el SSID/clave Wi-Fi; los comandos **no se repiten en pantalla** (una línea `SET` pegada por error no se filtra) y el búfer se borra tras cada línea |
| TLS y credenciales | las mismas de producción: `setCACert`, usuario/contraseña individuales de NVS, client id `gw-<UID>` |

Frame v1, HMAC, perfil RF, builder JSON, backend y dashboard: sin cambios.

## Payload que genera (determinista)

Sin radio no hay RSSI/SNR reales, así que se usan los valores de referencia
del cross-check de BUILD-IOT-04 (−87 / 7.50). `rx_time=0` produce
`lora_rx_at:null` y `clock_synced:false`. El resultado son dos payloads
posibles, siempre idénticos byte a byte:

| buffered | bytes | payload_sha256 (= `IotMqttDelivery.payload_sha256`) |
|---|---|---|
| false | 188 | `c4ce765421841ac930f2aa091a60e16839f6c0f94ece847e5792374484162ecd` |
| true | 187 | `344a30ffbb48a612349c0902a50b7179f3f6ff37fcb4b45005a8220bc11fa0b2` |

`buffered=true` aparece cuando se pide `DIAG PUBLISH` con el enlace caído:
la trama espera en la cola y sale al reconectar. Así se prueba físicamente el
store-and-forward. El firmware imprime el SHA-256 antes de publicar, para
encontrar la fila en el backend sin descifrar nada.

Decisión de diseño: con payloads deterministas, una repetición produce la
misma clave de transporte en el backend (se registra como redelivery, no como
fila nueva) y la ingesta da DUPLICATE en vez de CONFLICT. Usar la hora NTP
real habría generado un CONFLICT de metadatos en cada repetición.

## Comandos Serial (115200 baudios, fin de línea "Nueva línea" o "Ambos NL & CR")

| Comando | Efecto |
|---|---|
| `DIAG HELP` | lista los comandos |
| `DIAG STATUS` | Wi-Fi, MQTT, NTP, autocheck del vector, broker/client id/topic, cola, contadores, heap y pila de la tarea |
| `DIAG PUBLISH` | encola el vector oficial (una publicación QoS 1) |

## Métricas de memoria que imprime

| Momento | Línea |
|---|---|
| arranque | `[DIAG] heap boot` |
| creación de cola y tarea | `[DIAG] queue heap=… task heap=…` + `heap after-task` |
| antes / después de cada `connect()` | `heap pre-TLS`, `heap post-TLS`, `connect ok in N ms; handshake_peak<=X B session_held=Y B` |
| después de cada PUBACK | `PUBACK ok packet_id latency_ms`, `heap post-PUBACK` |
| pila de la tarea MQTT | `mqtt task stack size never_used peak_used` (high-water mark en bytes) |

`min_free` es el mínimo histórico desde el arranque. `handshake_peak` es
`free(pre-TLS) − min_free(post-TLS)`: es exacto si el mínimo se alcanzó
durante el handshake, que es lo normal en el primer `connect()`, y en otro
caso es una cota superior. Todas las cifras son del heap interno de 8 bits
(la placa no tiene PSRAM).

### Estimación previa (sin medir en dispositivo)

| Elemento | Estimado | Fuente |
|---|---|---|
| `QueuedFrame` | ~168 B (137 B de trama + metadatos, `time_t` de 64 bits) | `DIAG STATUS` imprime el `sizeof` real |
| Cola (16 elementos) | ~2,7 KB + ~80 B de control FreeRTOS | delta de heap medido al crearla |
| Tarea MQTT | 16.384 B de pila + TCB (~0,35 KB) | delta de heap medido al crearla |
| Buffers estáticos | payload 512 B + línea de consola 32 B (BSS) | compilador |
| Coste del diagnóstico en la build | +5,7 KB de flash, +56 B de RAM global frente a la build MQTT sin diagnóstico | compilación |
| Handshake TLS (mbedTLS, RSA-2048) | típicamente decenas de KB de pico | **pendiente de medir**; es la cifra clave |

## Validación ejecutada

| Prueba | Resultado |
|---|---|
| Compilar el gateway desde su ubicación definitiva, diagnóstico apagado | OK, 566.304 B (43 %) / 31.932 B (9 %), sin avisos en `gateway.ino` |
| Compilar una copia temporal con `GATEWAY_MQTT_DIAG_ENABLED 1` | OK, 1.036.005 B (79 %) / 50.196 B (15 %), sin avisos en `gateway.ino` |
| Copia con diagnóstico + MQTT de producción | **rechazada** por `static_assert` (esperado) |
| Copia con diagnóstico + LoRa | **rechazada** por `static_assert` (esperado) |
| `tools/test_gateway_diag_vector.py` | 11/11: bytes y clave idénticos al vector oficial, HMAC válido, payloads ≤ 444 B |
| `tools/test_gateway_mqtt_broker.py` | 15/15 (nuevo check 15: payloads de diagnóstico entregados byte a byte con el SHA-256 esperado) |
| `test_vectors/test_protocol.py` | 34/34 |

Las copias temporales se borraron. Ninguna prueba ejecuta el firmware en el
ESP32.

## Procedimiento en Arduino IDE + broker de staging

**Requisito previo:** autorización expresa del propietario para la
publicación física. Usar solo broker, backend y credenciales de
**staging**, nunca de producción.

### A. Preparar staging (lado servidor)

1. Broker de staging configurado como en `SECURITY-PROVISIONING.md`, sección
   "Production MQTT provisioning": solo TLS, ACL por UID, persistencia.
   Antes, validar su ACL con `tools/test_gateway_mqtt_broker.py`.
2. Backend de staging con el consumidor durable conectado a ese broker
   (`clean_session=false`, QoS 1) y su propia base de datos (nunca la de
   producción).
3. Registrar un gateway **de prueba** en el backend de staging. Su UID y su
   contraseña MQTT individual se usan solo para esta prueba.
4. Opcional, para llegar hasta la ingesta: registrar en staging el
   coordinador `CO-7K3M9QXT2A` con la clave **pública** del vector
   (`key_version 1`), asociarlo al gateway de prueba y mapear sensores a los
   canales 1 (métrica 1) y 2 (métrica 2). Sin esto, la recepción durable se
   registra igual, pero el worker la deja en `REJECTED` (p. ej.
   `rejected_gateway`, `rejected_credential` o `rejected_sensor_mapping`).

### B. Preparar la build

1. Comprobar que `Documents\Arduino\libraries\AgrotechProtocol` es un enlace
   a `shared` o una copia **actualizada**; una copia vieja compila código
   desfasado.
2. Crear `gateway\GatewayConfig.local.h` (ignorado por git; no se sube):
   ```c
   #pragma once
   #define MQTT_HOST "<host de staging>"
   #define MQTT_PORT 8883
   #define MQTT_CA_CERT "-----BEGIN CERTIFICATE-----\n...\n-----END CERTIFICATE-----\n"
   #define GATEWAY_MQTT_DIAG_ENABLED 1
   ```
3. No tocar `AgrotechRadioConfig.h`: todas las flags deben seguir en `false`.
   Si alguna está en `true`, la build falla a propósito.
4. Arduino IDE: abrir `gateway\gateway.ino`, placa "ESP32 Dev Module",
   puerto COM del gateway, Verificar y luego Subir.

### C. Ejecutar

1. Abrir el Monitor Serie (115200, "Nueva línea"). En los 2 s de la ventana
   de aprovisionamiento, cargar las credenciales de **staging** con la línea
   `SET` (ver `SECURITY-PROVISIONING.md`). Desactivar cualquier registro de la
   sesión del terminal.
2. Comprobar en el arranque:
   - `[DIAG] MQTT diagnostic mode ENABLED; LoRa radio forced off`
   - `[DIAG] official Frame v1 vector self-check: ok (67 bytes)`
   - `[LoRa] receiver disabled…`
   - `[DIAG] queue heap=…`, `[DIAG] heap pre-TLS…`, `[DIAG] connect ok in … ms`
3. `DIAG STATUS` → `wifi=up mqtt=up vector=ok radio=off`.
4. **Publicación en línea:** `DIAG PUBLISH` → anotar `payload_sha256`
   (`c4ce76…` si buffered=false) y esperar `[DIAG] PUBACK ok packet_id=…
   latency_ms=…`.
5. **Store-and-forward:** apagar el AP Wi-Fi o parar el broker de staging,
   esperar a `mqtt=down` en `DIAG STATUS`, enviar `DIAG PUBLISH` (aparece
   `link down: frame will wait…`), restablecer el enlace y esperar el PUBACK
   con `buffered=true` (`344a30…`).
6. Copiar al informe todas las líneas `[DIAG] heap`, `connect`, `PUBACK` y
   `stack`.

### D. Verificar la recepción durable en el backend de staging

Por SHA-256, sin descifrar el payload (base de datos de **staging**):

```sql
SELECT id, topic, qos, state, payload_size, received_at
FROM iot_mqtt_deliveries
WHERE payload_sha256 = UNHEX('c4ce765421841ac930f2aa091a60e16839f6c0f94ece847e5792374484162ecd');

SELECT occurred_at, event_type, from_state, to_state
FROM iot_mqtt_delivery_events
WHERE delivery_id = '<id anterior>' ORDER BY occurred_at;
```

Resultado esperado: una fila con `topic=agrotech/v1/gw/<UID de prueba>/up`,
`qos=1` y `payload_size=188` (187 para buffered=true), y un evento de
recepción. Tras el worker, el estado esperado es `ACCEPTED` si se hizo el
paso A.4 (`DUPLICATE` en repeticiones), o `REJECTED` si el coordinador no
está registrado. En ambos casos la recepción durable queda demostrada. Un
segundo `DIAG PUBLISH` idéntico no debe crear otra fila, sino registrarse como
redelivery de la misma.

### E. Cierre

Borrar `GATEWAY_MQTT_DIAG_ENABLED 1` del `GatewayConfig.local.h`, recompilar
y volver a flashear la build normal. Revocar en el backend de staging la
credencial MQTT de prueba.

## Riesgos

1. **Dejar flasheada una build de diagnóstico.** Mitigación: no publica sin
   comando, tiene límite de 10 por arranque, no puede compilarse junto con
   MQTT de producción ni con LoRa, y anuncia el modo en el arranque. El
   paso E es obligatorio.
2. **Contaminar datos con el vector.** El vector tiene un UID de coordinador
   de prueba y lecturas fijas. Usar solo backend de staging. Si llegara a
   producción, no se asocia a ningún coordinador real (la clave es pública y
   no está registrada) y se rechaza.
3. **Usar credenciales reales por error.** El firmware no puede distinguir
   credenciales de staging de las de producción; depende del procedimiento.
   Mitigación: gateway y contraseña creados solo para la prueba y revocados
   al terminar; host de staging en el `local.h`.
4. **Fuga de secretos por Serial.** Los comandos no se repiten y nunca se
   imprimen contraseñas. El riesgo que queda es el registro del propio
   terminal durante el `SET` (desactivarlo).
5. **Medición de heap aproximada.** `handshake_peak` es una cota superior si
   el mínimo se alcanzó antes del `connect()`; las mediciones del primer
   arranque son las fiables.
6. **Copia desfasada de `shared` en Arduino IDE** (ver B.1).
7. **Sin ejecución en hardware todavía.** Todo lo anterior está validado por
   compilación, host y broker aislado; el comportamiento en el ESP32 real
   (tiempos, heap, estabilidad de la tarea) está sin verificar.

## Archivos

- `gateway/GatewayDiagnostics.h` (nuevo): switch, vector oficial, clave
  pública de prueba, RSSI/SNR de referencia y `HeapSnapshot`.
- `gateway/gateway.ino`: `kMqttPathEnabled`, `static_assert`s, consola
  `DIAG`, métricas de heap y pila, SHA-256 del payload y log de PUBACK. Con el
  diagnóstico apagado no cambia ningún comportamiento.
- `tools/test_gateway_diag_vector.py` (nuevo).
- `tools/test_gateway_mqtt_broker.py`: check 15.
- `docs/BUILD-IOT-06-MQTT-DIAGNOSTIC.md` (este documento).
