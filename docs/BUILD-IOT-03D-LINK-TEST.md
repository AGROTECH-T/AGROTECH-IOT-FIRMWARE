# BUILD-IOT-03D — prueba de enlace LoRa

Estado: **modo de prueba preparado y compilado, pero bloqueado**. No se ha
transmitido ni recibido por radio. El perfil RF sigue sin autorización y las
banderas de activación siguen en `false`.

## 1. Qué se implementó

Un modo de prueba controlado que **reutiliza Frame v1 y HMAC sin cambios** y no
añade bytes en el aire. No hay protocolo alternativo.

| Archivo | Cambio |
|---|---|
| `shared/src/AgrotechLinkTest.h` | Nuevo: banderas/constantes del modo, estadísticas TX/RX, identificador de frame, cálculo de airtime y conteo de pérdidas por secuencia |
| `coordinator/coordinator.ino` | Modo de prueba: tramas Frame v1 con lecturas sintéticas, log de identificación, hex, aire y estadísticas |
| `gateway/gateway.ino` | Modo de prueba: log de frame recibido, comparación de longitud, hex, tiempos, RSSI/SNR y estadísticas de pérdidas |
| `docs/BUILD-IOT-03D-LINK-TEST.md` | Este documento |

No se modificaron credenciales, identidad, backend, frontend, sensores ni la
lógica de Frame v1/HMAC.

## 2. Modo de prueba

En `shared/src/AgrotechLinkTest.h`:

| Constante | Valor | Significado |
|---|---|---|
| `agrotech_link_test::kEnabled` | `false` | Modo de prueba desactivado |
| `agrotech_link_test::kFrameLimit` | `10` | Detiene la programación tras 10 envíos; `0` = continuo |
| `agrotech_link_test::kTemperatureX100` | `2500` | Lectura sintética 25.00 °C |
| `agrotech_link_test::kHumidityX100` | `5000` | Lectura sintética 50.00 %RH |

Las lecturas sintéticas solo se usan en modo prueba y son valores válidos de
Frame v1 (canales 1 y 2, códigos de métrica 1 y 2). **No son mediciones** y no
tocan el camino de sensores.

El modo de prueba **no** autoriza la radio. Aunque `kEnabled` sea `true`, el
radio permanece bloqueado por `agrotechRadioCanActivate()` mientras
`AGROTECH_LORA_TRANSPORT_ENABLED` o `AGROTECH_LORA_PROFILE_AUTHORIZED` sean
`false`, o el perfil esté incompleto. En ese caso el monitor muestra:

```text
[LT] link test mode ENABLED
[LT] radio not ready: transport flag is false
```

## 3. Identificación y comparación de paquetes

Cada log usa el mismo identificador derivado de Frame v1, sin campos extra:

```text
id=<uid>/b<boot_counter>/s<seq>/t<prefijo_tag>
```

- `uid`, `boot_counter` y `seq` son campos existentes de Frame v1.
- `t` son los primeros 4 bytes del tag HMAC-SHA256 (8 dígitos hex), suficientes
  para emparejar el envío con la recepción.
- `bytes` y `hex=` permiten comparar longitud y contenido byte a byte.
- `expected=` en el gateway es `25 + 5·lecturas + 32`; `parseFrame` ya rechaza
  longitudes distintas.

El gateway **no** verifica el HMAC: no dispone de la clave del coordinador por
diseño de credenciales. El gateway valida estructura, versión y longitud; el
backend seguiría siendo quien valida el tag. La comparación de `t` es de
diagnóstico, no una prueba de autenticidad.

## 4. Secuencias, tiempos y errores

Coordinador (`[LT] tx`):

```text
[LT] tx id=CO-XXXXXXXXXXXX/b3/s42/tA1B2C3D4 bytes=67 air_ms=123.14 hex=01 43 4F ...
[LT] tx_stats encoded=1 encode_err=0 started=1 done=0 busy=0 write_err=0 start_err=0 timeouts=0
```

- `encoded`: frames válidos generados.
- `started`: transmisiones aceptadas por el driver.
- `done`: callbacks `TxDone` observados.
- `timeouts`: sin `TxDone` en 5 s (se registra una vez).
- `air_ms`: airtime calculado del formato LoRa (67 B ≈ 123.1 ms con SF7,
  BW125, CR4/5, preámbulo 8, CRC on, encabezado explícito). Es un cálculo, no
  una medición. Se muestra `n/a` si el perfil operativo está incompleto.

Gateway (`[LT] rx`):

```text
[LT] rx id=CO-XXXXXXXXXXXX/b3/s42/tA1B2C3D4 bytes=67 expected=67 rssi=-45 snr=9.75 dt_ms=60000 hex=01 43 4F ...
[LT] rx_stats frames=1 malformed=0 oversized=0 incomplete=0 lost=0 duplicates=0 out_of_order=0
```

- `dt_ms`: tiempo desde la trama válida anterior (incluye el intervalo de 60 s).
- `lost`: suma de huecos de `seq` dentro del mismo `uid` y `boot_counter`.
- `duplicates`: mismo `seq` que la última trama.
- `out_of_order`: `seq` menor que la última (no descuenta pérdidas).
- `malformed`/`oversized`/`incomplete`: errores de estructura o lectura.

Limitaciones del conteo de pérdidas:

- La primera trama (o tras reinicio del coordinador o del gateway) solo fija la
  línea base; no se cuentan pérdidas anteriores.
- No hay ACK: el coordinador no puede saber si una trama llegó; `started/done`
  solo confirman el envío local.
- `seq` es persistente entre reinicios, por lo que el primer `seq` observado
  puede ser alto sin implicar pérdidas.
- Paquetes perdidos que no dejan hueco (por ejemplo, una trama final perdida)
  no son detectables por secuencia.

## 5. Cómo ejecutar la prueba (solo con perfil autorizado)

> Hoy **no** debe ejecutarse: el perfil no está autorizado y las banderas están
> en `false`. Los pasos describen el procedimiento futuro.

1. Completar y autorizar el perfil RF según
   `docs/BUILD-IOT-03C-LORA-PROFILE.md` y los pendientes de medición.
2. En `shared/src/AgrotechLinkTest.h`, poner `kEnabled = true` y ajustar
   `kFrameLimit` (10 por defecto, `0` continuo).
3. Verificar que ambas placas compilan con la **misma** revisión/firma de perfil
   y cargar coordinador y gateway desde Arduino IDE.
4. Conectar ambas antenas antes de energizar. Nunca transmitir sin antena.
5. Abrir dos instancias del Monitor Serie a **115200 baudios**: primero el
   gateway, después el coordinador.
6. El coordinador envía una trama Frame v1 cada
   `COORDINATOR_TX_INTERVAL_MS` (60 s), hasta `kFrameLimit`.
7. Comparar por cada `id`: mismo `t`, mismo `bytes=67`, mismo `hex=`, y revisar
   `air_ms`, `rssi`, `snr`, `dt_ms`.
8. Al terminar, volver a `kEnabled = false` antes de cualquier uso normal.

Intervalo y ciclo de trabajo: 123.1 ms cada 60 s ≈ 0.205 % de ocupación, sin
reintentos. El límite normativo de ciclo de trabajo sigue pendiente de
confirmar; el modo de prueba no lo autoriza.

## 6. Ejecución desde Arduino IDE

1. Instalar el core ESP32 y las bibliotecas del `README.md`.
2. Copiar `shared/` a `Documents\Arduino\libraries\AgrotechProtocol`.
3. Abrir `coordinator/coordinator.ino` o `gateway/gateway.ino` (carpeta y
   archivo comparten nombre).
4. Elegir el modelo exacto de ESP32 y el puerto COM.
5. `Verificar` para compilar y `Subir` para cargar (BOOT si no inicia).
6. Monitor Serie a 115200 baudios.

## 7. Validación de esta fase

- `coordinator`: correcto — 330.800 B de programa (25 %), 24.092 B de RAM.
- `gateway`: correcto — 566.304 B de programa (43 %), 31.932 B de RAM.
- Compilado con `arduino-cli` de Arduino IDE, core ESP32 3.3.12, FQBN
  `esp32:esp32:esp32`, desde las carpetas definitivas.
- `AGROTECH_LORA_TRANSPORT_ENABLED=false` y
  `AGROTECH_LORA_PROFILE_AUTHORIZED=false`; MQTT también en `false`.
- Sin carga de firmware, sin transmisión/recepción y sin Wi-Fi/MQTT.

Pendiente: autorizar el perfil, medir antena/pérdidas/potencia, definir
frecuencia exacta y ciclo de trabajo, y ejecutar la prueba física comparando
logs. Hasta entonces **no existe evidencia de comunicación LoRa**.
