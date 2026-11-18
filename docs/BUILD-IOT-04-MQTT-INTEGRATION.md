# BUILD-IOT-04 — Integración MQTT gateway ↔ backend Django

Fecha: 2026-11-15
Alcance: verificación de compatibilidad y de la cadena MQTT → recepción durable →
worker contra el backend existente, con broker y base de datos desechables.
No se activó LoRa, no se cambió el perfil RF y no se modificó el firmware.

## Resultado

La cadena de transporte y persistencia funciona de extremo a extremo con el
payload que genera el firmware y el contrato del backend:

1. El gateway publica QoS 1 en `agrotech/v1/gw/<gateway_uid>/up` sobre TLS con
   CA y autenticación individual.
2. El consumidor valida el sobre, cifra con AES-256-GCM y persiste antes de
   enviar el PUBACK manual.
3. El worker descifra, autentica la huella de transporte, ejecuta la ingesta
   existente y deja la entrega y las mediciones en estado final.
4. Las redeliveries QoS 1 del mismo topic+payload se asocian a la misma fila y
   se registran como `REDELIVERY`; un mensaje en cola se recupera al reconectar
   con la misma sesión persistente.
5. Una entrega cuyo worker murió después del claim se recupera al vencer el
   lease y se procesa sin intervención manual.

No se encontró ninguna incompatibilidad de contrato que obligue a cambiar el
firmware o el backend.

## Compatibilidad firmware ↔ backend

Los campos, formatos y límites coinciden:

| Elemento | Firmware | Backend | Resultado |
|---|---|---|---|
| Topic | `agrotech/v1/gw/<uid>/up` | `parse_topic` + filtro `agrotech/v1/gw/+/up` | OK |
| `v` | `1` | entero exacto `1` | OK |
| `frame` | Base64 estándar con padding | Base64 canónico, longitud 57+5·n | OK |
| `lora_rx_at` | ISO-8601 UTC con `Z`, o `null` | ISO-8601 con zona, o `null` | OK |
| `clock_synced` / `buffered` | booleanos JSON reales | booleanos JSON reales | OK |
| `rssi` | entero, siempre presente | opcional, -164..0 | OK |
| `snr` | `%.2f`, siempre presente | opcional, -20..20, 2 decimales | OK |
| Tamaño | buffer máximo 512 B | `MAX_MESSAGE_BYTES = 444` | OK |
| QoS | publicación QoS 1 | solo acepta QoS 1 | OK |
| Autenticación | usuario = `gateway_uid`, secreto individual | usuario/ACL por gateway | compatible; provisión pendiente |
| TLS | `WiFiClientSecure` + `MQTT_CA_CERT`, sin modo inseguro | `MQTT_TLS=true` + `MQTT_CA_CERTS` | OK |

Comprobaciones ejecutadas:

- `test_vectors/test_protocol.py`: 34 checks, 0 fallos (Frame v1 oficial,
  Base64, longitudes, UID canónico y contrato del payload).
- Parseo en el backend del JSON con el formato exacto del firmware (incluyendo
  `rssi` y `snr`): frame idéntico byte a byte, topic resuelto y variante con
  `lora_rx_at:null`/`clock_synced:false` aceptada.

## Broker aislado

Se usó un Mosquitto 2.1.2 en Docker, sin datos de producción:

- listener 1883 con `allow_anonymous false`, `password_file` y `acl_file`;
- listener 8883 con TLS 1.2+ y CA propia de pruebas;
- usuario gateway de prueba: solo `write` a su topic;
- usuario gateway ajeno: solo `write` a su propio topic;
- usuario consumidor: solo `read` de `agrotech/v1/gw/+/up`;
- persistencia local y sesión MQTT persistente.

Resultados de la prueba de cable (12/12):

- conexión TLS del consumidor y suscripción al filtro del contrato;
- publicación QoS 1 del gateway y recepción del payload exacto;
- contraseña incorrecta rechazada con CONNACK `Not authorized`;
- publicación a un topic de otro gateway rechazada (PUBACK rc135) y no
  entregada;
- un gateway suscrito al comodín no recibe mensajes de otros gateways (el
  broker acepta el SUBSCRIBE pero filtra la entrega por ACL);
- reconexión con la misma sesión persistente y entrega del mensaje encolado;
- dos publicaciones QoS 1 idénticas se reciben dos veces en el cable (la
  deduplicación es responsabilidad de la capa durable).

Nota de despliegue: un SUBACK concedido no demuestra permiso de lectura en
Mosquitto. La verificación válida es que no lleguen mensajes ajenos y que el
PUBACK de escritura sea denegado. La configuración de producción de Mosquitto
sigue pendiente según `docs/modules/iot-seguridad.md` del backend.

## Backend durable

Prueba de extremo a extremo añadida:
`apps/iot/tests/test_mqtt_durable_e2e.py` (backend).

Cubre:

- consumidor real Paho contra TLS, con `clean_session=False`;
- persistencia cifrada antes del PUBACK (`payload_ciphertext` distinto del
  payload y sin plaintext almacenado);
- evento `RECEIVED` y versión de clave;
- redelivery del mismo topic+payload → una sola fila + evento `REDELIVERY`;
- simulación de worker caído con lease vencido → reclaim y `ACCEPTED`;
- worker con ingesta real → 2 mediciones y `PROCESSING_COMPLETED`;
- segundo frame publicado con el consumidor detenido → cola persistente,
  recepción al reconectar, worker y 4 mediciones.

También se actualizó `apps/iot/tests/test_mqtt_consumer.py`, que estaba escrito
contra la API anterior (`ingest_func`) y fallaba con 26 errores. Ahora prueba la
API durable actual (`receipt_func`, `MqttReceiptResult`), incluido el ACK solo
tras persistencia/redelivery y la desconexión cuando la recepción no es durable.

Ejecución con base desechable:

```powershell
& ".\tools\run_permissions_disposable.ps1" -Validate `
  -SettingsModule config.settings_test_disposable_iot `
  -Tests @(
    "apps/iot/tests/test_mqtt_consumer.py",
    "apps/iot/tests/test_mqtt_payload.py",
    "apps/iot/tests/test_mqtt_durable_e2e.py"
  ) `
  -MariaDbImageDigest "sha256:24e76fcec8c003a0362d0dd53f4806e7e79458d7fdeaf47437760e19496f5a9c"
```

Resultado: 127 tests, 0 fallos, 0 errores (junit del orquestador).

## Archivos tocados

Firmware:

- `docs/BUILD-IOT-04-MQTT-INTEGRATION.md` (este documento).
- Sin cambios en `shared/`, `gateway/` ni `coordinator/`.

Backend:

- `apps/iot/tests/test_mqtt_consumer.py` (actualizado a la API durable).
- `apps/iot/tests/test_mqtt_durable_e2e.py` (nuevo; se omite si no existe el
  descriptor local del broker de pruebas).

## Pendientes y límites

1. El broker de producción, sus credenciales individuales, ACL y TLS siguen sin
   aprovisionarse en el repositorio del backend; la prueba usó un broker local
   desechable.
2. `AGROTECH_MQTT_TRANSPORT_ENABLED` sigue en `false`; no hubo conexión Wi-Fi,
   MQTT ni LoRa física.
3. El gateway solo publica tras una recepción LoRa válida (`radio_ready`). Con
   las banderas actuales no existe un camino para inyectar una trama de prueba
   sin radio; no se añadió un hook de test para no alterar el firmware.
4. El worker reintenta por vencimiento de lease (60 s) y registra el error solo
   en logs; no persiste `RETRY_PENDING`, `next_attempt_at` ni un código de error
   seguro. No se modificó porque no bloquea la recepción durable, pero conviene
   decidirlo antes de operación.
5. La validación física gateway↔broker↔backend queda pendiente de: perfil RF
   autorizado, broker/credenciales desplegados y activación explícita de MQTT.
