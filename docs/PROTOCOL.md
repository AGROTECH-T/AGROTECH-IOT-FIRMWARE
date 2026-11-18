# Contrato LoRa v1 y payload MQTT

Implementación basada en `apps/iot/lora_frame.py` y
`docs/modules/iot-contrato-lora.md` del backend AGROTECH-T. Todos los campos
numéricos son **big-endian**. El orden y el tamaño de los campos no se modifican.

## 1. Cabecera (25 bytes)

| Offset | Campo | Bytes | Tipo | Valor |
|---:|---|---:|---|---|
| 0 | `frame_version` | 1 | uint8 | 1 |
| 1 | `coordinator_uid` | 13 | ASCII | `CO-` + 10 caracteres del alfabeto canónico |
| 14 | `key_version` | 2 | uint16 BE | 1–65535 |
| 16 | `boot_counter` | 4 | uint32 BE | 0–4294967295 |
| 20 | `seq` | 4 | uint32 BE | 0–4294967295 |
| 24 | `reading_count` | 1 | uint8 | 1–16 (máximo provisional del contrato) |

`frame_version`, `coordinator_uid`, `key_version`, `boot_counter`, `seq` y
`reading_count` forman parte de `signed_bytes` y van dentro del HMAC.

## 2. Lecturas

Cada lectura ocupa exactamente 5 bytes. La lectura `i` empieza en el offset
`25 + 5 * i`.

| Offset relativo | Campo | Bytes | Tipo |
|---:|---|---:|---|
| 0 | `channel` | 2 | uint16 BE, 1–65535 |
| 2 | `metric_code` | 1 | uint8 |
| 3 | `value` | 2 | int16 BE o uint16 BE según la métrica |

Métricas:

| `metric_code` | Métrica | Escala | Rango válido | Centinela |
|---:|---|---|---|---|
| 1 | `temperature` | ×100 | pendiente del sensor | `-32768` (`0x8000`) |
| 2 | `relative_humidity` | ×100 | 0–10000 | `65535` (`0xFFFF`) |

Ejemplos: 25,34 °C -> `2534` -> `09 E6`; 63,25 % -> `6325` -> `18 B5`.

El parser del backend no aplica el rango físico de humedad, pero el firmware sí
rechaza valores de humedad fuera de `0..10000` salvo el centinela `65535`. La
temperatura se limita a lo representable en int16. El rango físico de
temperatura sigue pendiente.

### 2.1 Asignación de canales del coordinador (BUILD-IOT-SENSOR-FAILOVER)

El canal identifica la fuente física. Cada trama lleva una sola pareja de una
sola fuente; nunca se mezclan sensores ni se envía el DHT11 por los canales del
BME680. No cambia el formato v1, la cabecera, el HMAC ni su contexto.

| Canal | `metric_code` | Fuente | Rol |
|---:|---:|---|---|
| 1 | 1 (`temperature`) | BME680 | principal |
| 2 | 2 (`relative_humidity`) | BME680 | principal |
| 3 | 1 (`temperature`) | DHT11 | respaldo |
| 4 | 2 (`relative_humidity`) | DHT11 | respaldo |

Conmutación (firmware, sin cambios de protocolo): F = 3 fallos consecutivos del
BME680 para RESPALDO, R = 3 lecturas válidas consecutivas para volver a
PRINCIPAL (publicando el BME680 en ese mismo ciclo) y reintentos de
inicialización del BME680 cada K = 5 ciclos. Durante SOSPECHA, RESPALDO y
RECUPERACIÓN se publica el DHT11 por 3/4; si en RECUPERACIÓN falla el DHT11 se
publica el BME680 por 1/2. Si fallan ambos no hay trama ni se consume `seq`.

Vector de referencia de la pareja DHT11: `test_vectors/dht_channels_vector.json`
(mismo UID, clave pública de prueba y contexto HMAC que el vector oficial;
`boot_counter` 7, `seq` 121, canal 3 = 3010, canal 4 = 7060, 67 bytes).

El backend necesita, antes de recibir tramas con los canales 3/4, un sensor
lógico activo por canal con su métrica y su fuente; si falta, rechaza la trama
completa (`REJECTED_SENSOR_MAPPING`).

## 3. Firma

```text
tag = HMAC-SHA256(clave_del_coordinador, CONTEXTO || signed_bytes)
```

- `CONTEXTO` = bytes ASCII de `agrotech:lora-frame:v1` (22 bytes), no se
  transmite.
- `signed_bytes` = cabecera + lecturas (sin el tag).
- `tag` = HMAC-SHA256 completo de 32 bytes.
- El gateway **no** verifica ni modifica el tag. Django valida el HMAC sobre los
  bytes exactos.

## 4. Longitud

```text
total_bytes = 25 + 5 * reading_count + 32 = 57 + 5 * reading_count
```

| Lecturas | Longitud |
|---:|---:|
| 1 | 62 |
| 2 | 67 |
| 4 | 77 |
| 16 | 137 (máximo provisional) |

El máximo de 16 lecturas y el tamaño de 137 bytes no son una capacidad física
validada del SX1278.

## 5. Vector oficial de referencia

Tomado de `apps/iot/tests/lora_vectors.py` del backend:

- `coordinator_uid`: `CO-7K3M9QXT2A`
- `key_version`: 1
- `boot_counter`: 7
- `seq`: 120
- Lecturas: canal 1 temperatura 2534; canal 2 humedad 6325
- Clave de prueba: bytes `00 01 02 ... 1f` (32 bytes)

```text
signed_bytes (35 B):
01 434f2d374b334d395158543241 0001 00000007 00000078 02
00010109e6 00020218b5

tag (32 B):
ddbab481e3b4f71dcc93452b1d0943ad24163c59d6a3c2e6f27ad3eb6bab1af5

frame completo (67 B):
01434f2d374b334d395158543241000100000007000000780200010109e600020218b5ddbab481e3b4f71dcc93452b1d0943ad24163c59d6a3c2e6f27ad3eb6bab1af5

Base64 estándar con relleno:
AUNPLTdLM005UVhUMkEAAQAAAAcAAAB4AgABAQnmAAICGLXdurSB47T3HcyTRSsdCUOtJBY8WdajwubyetPra6sa9Q==
```

La variante exacta de Base64 está pendiente de confirmación en el backend. Este
MVP usa el alfabeto estándar (`A-Z a-z 0-9 + /`) con relleno `=`.

## 6. Contadores y NVS

- `boot_counter` se lee de NVS, se incrementa una vez al arrancar, se vuelve a
  escribir y se verifica antes de la primera transmisión. Si la escritura o la
  verificación fallan, el coordinador no transmite.
- Si `boot_counter` guardado es `0xFFFFFFFF`, el coordinador se detiene y exige
  reprovisionamiento.
- `seq` empieza en 0 en cada arranque, vive en RAM y nunca se reutiliza dentro
  del mismo arranque.
- Al usar `seq = 0xFFFFFFFF`, la siguiente asignación devuelve agotamiento; no
  hay reinicio silencioso a cero.
- El firmware escribe un checkpoint de `seq` en NVS cada
  `COORDINATOR_SEQ_CHECKPOINT_EVERY` tramas y al agotarse. El contrato aprobado
  mantiene `seq` reiniciado en cada arranque; la garantía contra repetición de
  `(boot_counter, seq)` es el incremento persistente de `boot_counter`.
- **Limitación:** cualquier reset del ESP32 (encendido, watchdog, pánico o reset
  por software) se trata como un arranque. Distinguir solo el reinicio físico
  real requiere un mecanismo adicional que no está especificado.

## 7. Gateway y MQTT

Topic de publicación:

```text
agrotech/v1/gw/{gateway_uid}/up
```

QoS 1, `retain=false`, un mensaje por trama. El `gateway_uid` se obtiene del
topic y **no** va en el cuerpo.

Payload exacto:

```json
{
  "v": 1,
  "frame": "<BASE64_DE_LA_TRAMA_COMPLETA>",
  "lora_rx_at": "<timestamp ISO-8601 UTC o null>",
  "clock_synced": true,
  "buffered": false,
  "rssi": -90,
  "snr": 7.5
}
```

Reglas implementadas:

- `frame` contiene los bytes completos recibidos por LoRa (`signed_bytes` +
  tag), sin reconstruir ni modificar, codificados en Base64 estándar.
- `lora_rx_at` es la hora de recepción LoRa en UTC. Si NTP no está sincronizado,
  se publica `null` y `clock_synced=false`.
- `buffered` es `false`: este MVP no implementa búfer local.
- `rssi` es entero y `snr` decimal, tomados del SX1278.
- No se incluyen `gateway_uid` ni `farm_id`.
- El gateway no autentica la trama; Django realiza la validación criptográfica.

## 8. Fuera de alcance de este MVP

- ACK LoRa y reintentos por radio.
- Búfer local del gateway y reenvío tras reconexión.
- TLS para MQTT.
- Verificación HMAC en el gateway.
- Provisión segura de claves y credenciales.
- Validación física de frecuencia, potencia, alcance, sensores y consumo.
