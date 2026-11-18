# BUILD-IOT-05 — Cierre de integración MQTT en el gateway

Fecha: 2026-11-15
Estado: **firmware preparado y validado contra broker aislado. Gateway físico
NO integrado** (no probado con hardware; LoRa y MQTT siguen desactivados).

## Restricciones respetadas

- `AGROTECH_LORA_TRANSPORT_ENABLED`, `AGROTECH_LORA_PROFILE_AUTHORIZED`,
  `AGROTECH_MQTT_TRANSPORT_ENABLED` y `agrotech_link_test::kEnabled` siguen en
  `false`. Perfil RF sin cambios (revisión 1, firma `0x82811B29`).
- Frame v1, HMAC, identidad, builder JSON (`shared/src/AgrotechMqttPayload.*`),
  dashboard, backend y bases de datos: sin modificar.
- No se usaron ni se escribieron secretos reales. El script de pruebas usa
  credenciales y certificados desechables, que se generan y destruyen en cada
  ejecución.

## Revisión del contrato

| Aspecto | Firmware | Backend | Estado |
|---|---|---|---|
| Tópico | `agrotech/v1/gw/<UID>/up` | filtro `agrotech/v1/gw/+/up` | OK |
| Payload | `{"v":1,"frame","lora_rx_at","clock_synced","buffered","rssi","snr"}` | `apps/iot/mqtt/payload.py` | OK (byte-exacto, validado en BUILD-IOT-04) |
| Tamaño | buffer 512 B, trama máx. 137 B → JSON ≤ 444 B | `MAX_MESSAGE_BYTES=444` | OK |
| Protocolo | MQTT 3.1.1 (lwmqtt) | `MQTTv311` | OK |
| QoS | publish QoS 1, retain=false | suscripción QoS 1, ACK manual tras persistir | OK |
| Identidad | client id `gw-<UID>`, usuario/contraseña individuales desde NVS | ACL por usuario | OK |
| TLS | `WiFiClientSecure` + `setCACert`, sin modo inseguro; CA vacía → falla cerrado | broker TLS-only | OK |
| Reloj | `clock_synced` por umbral de época; si no, `lora_rx_at:null` | `measured_at_approximate` | OK |

## Defectos encontrados y corregidos en `gateway/gateway.ino`

1. **Descarte de tramas LoRa con MQTT caído.** `publishFrame()` descartaba la
   trama si no había conexión ("no buffer in MVP"). Ahora hay una cola FreeRTOS
   acotada (`GATEWAY_MQTT_QUEUE_DEPTH`, 16 por defecto, ~2,6 KB de heap). Una
   trama sale de la cola solo cuando hay enlace y queda pendiente hasta recibir
   el PUBACK.
2. **`loop()` quedaba sordo a LoRa** mientras `connect()` o `publish()`
   esperaban su timeout (≥ 5 s más el handshake TLS). Wi-Fi, TLS, lwmqtt y la
   construcción del payload ahora corren en una tarea propia
   (`mqttTask`, core 0, pila de `GATEWAY_MQTT_TASK_STACK_BYTES` = 16 KB).
   `loop()` (core 1) solo recibe y encola, sin bloquear.
3. **Reintentos que habrían generado CONFLICT en el backend.** Un timeout de
   QoS 1 es ambiguo: el broker puede haber guardado el mensaje y haberse
   perdido el PUBACK. La ingesta compara un hash que incluye `buffered`
   (`ingestion.py::_resolve_existing`). Si el reintento cambiara `buffered` de
   false a true, el resultado sería `CONFLICT` en lugar de `DUPLICATE`. Ahora
   el JSON se construye **una sola vez**, justo antes del primer intento, y
   cada reintento reenvía exactamente los mismos bytes. Así se mantiene la
   misma clave SHA-256 de recepción y la misma clasificación DUPLICATE.
4. **Contadores compartidos entre núcleos.** Pasaron de `volatile uint32_t`
   con `++`, que no es atómico, a `std::atomic<uint32_t>`. El LED lee el flag
   `mqtt_link_up` en lugar de llamar a `mqtt.connected()` desde otro núcleo.

### Semántica de `buffered`

`buffered=true` si la trama se encoló sin enlace, si esperaba detrás de otras
o si pasó ≥ 1 s entre la recepción LoRa y el primer intento de publicación.
Se decide una sola vez y no vuelve a cambiar.

### Política ante fallos

> **Actualización (MQTT-RESILIENCE-04 y MQTT-QUEUE-PERSISTENCE-05, pendiente
> de validación):** la cola FreeRTOS en RAM se sustituyó por un almacén
> durable en flash (`gateway/GatewayFrameStore.*`, `gateway/GatewayFrameLog.*`)
> con 16 tramas vivas que se recuperan tras un reinicio, y el reintento fijo de
> 5 s se sustituyó por un backoff exponencial con jitter. La tabla refleja el
> comportamiento actual; el diseño está en `docs/MQTT-QUEUE-PERSISTENCE-05.md`.

| Situación | Comportamiento |
|---|---|
| Wi-Fi caído | reintento cada `GATEWAY_WIFI_RETRY_MS` (10 s); las tramas se guardan en el almacén durable |
| Broker caído / connect falla | backoff exponencial con jitter (`GATEWAY_MQTT_BACKOFF_*`, MQTT-RESILIENCE-04); LED en patrón de error |
| Timeout de PUBACK | la librería cierra el socket; se reconecta y se reenvían los mismos bytes |
| Cola llena (16 tramas vivas) | se rechaza la trama **más nueva** sin escribir en flash; las 16 guardadas no se tocan; contador y log limitado a 1 cada 5 s |
| Almacén no disponible o fallo de escritura | la trama no se acepta ni se publica; contador, log y LED en patrón de error |
| Payload no construible | no alcanzable para una trama aceptada; si ocurriera, la trama se retiene (no se borra sin PUBACK) |
| Reinicio del ESP32 | las tramas sin ACK se **recuperan** del almacén en orden FIFO y se reenvían con los mismos bytes; el backend deduplica los posibles duplicados |

## Validación ejecutada

### Compilación desde la ubicación definitiva

```
arduino-cli compile --fqbn esp32:esp32:esp32 --warnings default \
  --libraries "<Arduino>\libraries" --library shared gateway
```

| Variante | Flash | RAM global | Avisos en gateway.ino |
|---|---|---|---|
| Definitiva (flags en false) | 566.304 B (43 %) | 31.932 B (9 %) | 0 |
| Copia temporal con MQTT=true y broker ficticio (fuera del repo) | 1.030.277 B (78 %) | 50.140 B (15 %) | 0 |

La segunda variante solo demuestra que la ruta MQTT compila y enlaza; el
binario no se flasheó.

### Broker aislado: `tools/test_gateway_mqtt_broker.py`

Mosquitto 2.1.2 desechable (Docker): solo TLS, usuarios por dispositivo y ACL.
Los clientes usan los parámetros del firmware (3.1.1, clean session,
keepalive 30, `gw-<UID>`) y del consumidor backend (3.1.1,
`clean_session=False`). Se ejecutó dos veces seguidas: **14/14 PASS** en ambas,
sin contenedores residuales.

```
PASS 01 consumer (backend profile) TLS connect + QoS1 subscribe
PASS 02 server certificate rejected when the client trusts a different CA
PASS 03 plaintext MQTT is not accepted (TLS-only listener)
PASS 04 wrong password rejected by CONNACK (Not authorized)
PASS 05 gateway TLS connect (3.1.1, clean session, keepalive 30, gw-<UID>)
PASS 06 QoS1 publish delivered byte-exact to agrotech/v1/gw/<UID>/up
PASS 07 ACL: gateway cannot publish to another gateway topic (dropped)
PASS 08 ACL: a gateway cannot read other gateways' uplinks
PASS 09 retransmission is byte-identical (same SHA-256 receipt key)
PASS 10 backend offline: persistent session delivers on reconnect
PASS 11 gateway detects broker outage
PASS 12 gateway and consumer reconnect after broker restart
PASS 13 queued frames flushed after reconnect, in order, buffered=true
PASS 14 live forwarding resumes with buffered=false
```

Nota sobre 3.1.1: Mosquitto no devuelve un PUBACK negativo cuando el ACL
deniega la escritura. Confirma el PUBACK y descarta el mensaje en silencio.
El rc135 visto en BUILD-IOT-04 era de MQTT v5. El firmware no puede detectar
un ACL mal configurado por el PUBACK; hay que vigilarlo en el backend.

Regresión de protocolo: `test_vectors/test_protocol.py` → 34 checks, 0 fallos.

### Alcance real de estas pruebas

Validan el broker, el contrato y el protocolo de store-and-forward que sigue
el firmware. **No ejecutan el firmware ESP32**: no hay compilador C++ de host
para pruebas unitarias y el gateway no tiene un modo para inyectar tramas sin
radio. La cola y la tarea MQTT se validaron por revisión y compilación, no en
ejecución.

## Archivos modificados

- `gateway/gateway.ino`: cola, tarea MQTT, payload congelado, contadores
  atómicos, LED por flag.
- `gateway/GatewayConfig.h`: `GATEWAY_MQTT_QUEUE_DEPTH`,
  `GATEWAY_MQTT_TASK_STACK_BYTES`.
- `tools/test_gateway_mqtt_broker.py` (nuevo).
- `docs/BUILD-IOT-05-GATEWAY-MQTT.md` (nuevo) y `docs/SECURITY-PROVISIONING.md`
  (sección de producción).

## Pendientes

1. **Prueba con hardware real** (bloqueante para declarar la integración):
   requiere autorizar el perfil RF, un broker de staging y las flags. Plan:
   Wi-Fi off → enviar N tramas → Wi-Fi on → verificar N tramas en el backend
   con `buffered=true`, sin CONFLICT y con `seq` contiguos. Repetir con el
   broker parado y con el backend parado.
2. Medir en el dispositivo la pila libre de `mqttTask`
   (`uxTaskGetStackHighWaterMark`) y el heap durante el handshake TLS; ajustar
   los 16 KB si hace falta.
3. Opcional: un modo de diagnóstico que inyecte el vector oficial en la cola
   sin radio, para validar MQTT físico sin habilitar LoRa.
4. ~~La cola no sobrevive a reinicios.~~ Implementado en
   MQTT-QUEUE-PERSISTENCE-05 como log de bloques en la partición `spiffs`
   existente (no NVS ni LittleFS). **Pendiente de compilación, pruebas de host
   y validación física** (ver `docs/MQTT-QUEUE-PERSISTENCE-05.md`).
5. Backend (documentado, no corregido): el worker solo difiere errores
   inesperados por vencimiento de lease (60 s) y no guarda RETRY_PENDING ni
   código de error.
6. Integrar la autenticación del broker de producción con el servicio de
   credenciales del backend (plugin de auth / dynamic security) y el ACL por
   UID.
