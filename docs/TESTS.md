# Informe de pruebas

## 1. Pruebas ejecutadas

### 1.1 Referencia independiente en Python

Comando ejecutado con Python 3.14.7:

```powershell
& 'C:\Users\israe\AppData\Local\Python\pythoncore-3.14-64\python.exe' test_vectors\test_protocol.py
```

Resultado en BUILD-IOT-01A: **34 checks, 0 failures**.

Cubre:

- Bytes firmados del vector oficial del backend.
- HMAC-SHA256 completo y tag del vector oficial.
- Trama completa y longitud de 67 bytes.
- Base64 estándar con relleno.
- Fórmula `57 + 5 * reading_count`.
- Centinelas de temperatura (`-32768`) y humedad (`65535`).
- Big-endian de `key_version`, `boot_counter`, `seq`, `channel` y `value`.
- Longitudes para 1, 2, 4 y 16 lecturas.
- Validación canónica del UID y rechazo de minúsculas, `0`, `I` y `L`.
- Modelo de contadores: arranque, incremento de `seq` y límite uint32.
- Contrato del payload MQTT: campos exactos, sin `gateway_uid` ni `farm_id`,
  `lora_rx_at=null` cuando no hay sincronización.

Esta prueba **no ejecuta el firmware C++**; es una referencia independiente.

### 1.2 Compilación del coordinador con Arduino CLI

Entorno usado: Arduino CLI 1.6.0, ESP32 core 3.3.12, placa genérica `ESP32 Dev
Module` (`esp32:esp32:esp32`). El proyecto no fija versión del core ni contiene
`platformio.ini`. Comando ejecutado con bibliotecas aisladas:

```powershell
& 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe' `
  --config-file '.validation-iot01a\arduino-cli-isolated.yaml' compile `
  --fqbn esp32:esp32:esp32 `
  --build-path '.validation-iot01a\build-isolated' `
  --libraries (Get-Location).Path coordinator
```

Resultado: **correcta**. Flash: 324.064 bytes (24 %); RAM global: 23.972 bytes
(7 %; 303.708 bytes libres). La compilación aislada usó solamente BME680 2.0.6,
DHT sensor library 1.4.7, Unified Sensor 1.1.15 y BusIO 1.17.4; no necesitó GFX
ni SSD1306. No se flasheó el ESP32.

### 1.3 Compilación del gateway con Arduino CLI

El gateway usa la biblioteca **MQTT de Joel Gaehwiler**, que no está instalada
en este equipo. Para comprobar solo el código propio se usó un stub temporal de
`MQTT.h` con la misma API declarada por la biblioteca real.

Resultado histórico: compilación del gateway correcta para
`esp32:esp32:esp32` con el stub.

- Flash: 927.457 bytes (70 %).
- RAM global: 48.588 bytes (14 %).

Esto valida la sintaxis y los tipos del código del gateway, pero **no** valida
la integración con la biblioteca MQTT real ni su comportamiento de QoS 1. Antes
de una prueba física hay que instalar la biblioteca `MQTT` de Joel Gaehwiler y
recompilar sin el stub.

## 2. Pruebas C++ de host

Archivo: `test_vectors/host_tests.cpp`.

No hay `g++` ni `clang++` instalados. Se usó Zig 0.17.1 como compilador C++
temporal dentro del directorio de validación. Comando exacto:

```powershell
& '.validation-iot01a\host-tools\ziglang\zig.exe' c++ -std=c++11 -Wall -Wextra `
  -I shared\src -I coordinator `
  test_vectors\host_tests.cpp shared\src\AgrotechProtocol.cpp `
  shared\src\AgrotechSha256.cpp shared\src\AgrotechBase64.cpp `
  shared\src\AgrotechMqttPayload.cpp shared\src\AgrotechCounters.cpp `
  -o .validation-iot01a\host_tests.exe
& .validation-iot01a\host_tests.exe
```

Resultado: **246 checks, 0 failures**. El compilador Zig se instaló de forma
temporal y local solo para la ejecución de estas pruebas; no es dependencia del
firmware ni del proyecto.

Las pruebas incluyen:

- SHA-256 con vectores conocidos.
- HMAC-SHA256 con vectores RFC 4231.
- Vector oficial byte por byte.
- Vector de centinelas.
- Vector de una lectura con `seq=0`.
- Big-endian de todos los campos.
- Longitudes de 1 a 16 lecturas.
- Validación y rechazo de UID, versión, `key_version`, canales, métricas,
  duplicados, rangos y buffers.
- Rechazo de versiones, longitudes, UID, canal, métrica y tag alterados.
- Agotamiento de `seq` y `boot_counter` sin desbordamiento silencioso.
- Base64.
- JSON exacto del payload MQTT, incluyendo ausencia de `gateway_uid` y
  `farm_id`.
- Conversión de sensor ×100 con signo, límites, NaN, infinito y puntero nulo.
- Conversión de temperatura y humedad, y redondeo positivo/negativo.
- Prioridad BME680, respaldo DHT11 y caso sin lecturas válidas.
- Reinicio del conteo de lecturas y rechazo de una trama con datos anteriores.

### 2.1 BUILD-IOT-SENSOR-FAILOVER-05 (conmutación BME680 → DHT11)

Entorno de compilación: Arduino CLI 1.6.0, ESP32 Arduino core 3.3.12, placa
`esp32:esp32:esp32` (ESP32 Dev Module); bibliotecas Adafruit BME680 2.0.6, DHT
sensor library 1.4.7, Adafruit Unified Sensor 1.1.15, Adafruit BusIO 1.17.4 y
LoRa 0.8.0. Sin flasheo. La compilación correcta no es una validación funcional
ni física.

Resultados reales de esta fase:

| Prueba | Comando | Resultado |
|---|---|---|
| Referencia Python del protocolo (incluye el vector DHT11 3/4) | `python -I -B test_vectors\test_protocol.py` | **49 checks, 0 failures** (30 previos + 19 nuevos) |
| LED (modelo por estado de failover + comprobaciones del `.ino`) | `python -I -B test_vectors\test_coordinator_led.py` | **7 tests, 0 failures** |
| Compilación del coordinador | `arduino-cli compile --fqbn esp32:esp32:esp32 --warnings all --build-path <temp> --libraries <repo> coordinator` | **Correcta**: 332.164 B flash (25 %), 24.100 B RAM (7 %); 0 avisos en `coordinator/` y `shared/` (2 avisos de la biblioteca externa LoRa) |
| C++11 de las pruebas de host (solo sintaxis) | `xtensa-esp32-elf-g++ -std=c++11 -Wall -Wextra -fsyntax-only` sobre `host_tests.cpp`, `failover_sensor_tests.cpp` y `CoordinatorSensors.cpp` con dobles | **Compilan, 0 avisos**. No se ejecutan: es un compilador cruzado |
| Pruebas C++ de host | `run_host_tests.ps1`, `run_failover_host_tests.ps1` | **NO EJECUTADAS**: no hay g++/clang++ en el equipo (exit 2) |

Pruebas añadidas en esta fase, sin ejecutar en ella (se ejecutaron después:
`host_tests.cpp` en §2.2 y el arnés integrado en §2.3):

- `host_tests.cpp`: máquina de estados pura (`failoverStep`) con todas las
  transiciones aprobadas, saturación de contadores, invariantes sobre 100 000
  ciclos pseudoaleatorios, temporizador K=5 (`reinitDue`), nombres y motivos de
  `[SOURCE]`, y codificación C++ del vector DHT11 3/4 frente a la referencia
  Python (incluido el parser del gateway y la verificación HMAC).
- `failover_sensor_tests.cpp` + `failover_mocks/`: el `CoordinatorSensors.cpp`
  REAL con dobles de Arduino/Wire/BME680/DHT. Escenarios ciclo a ciclo: arranque
  con y sin BME680, reconexión, fallo en operación, respuesta I2C con lecturas
  inválidas, fallo de configuración, DHT11 caído en RECUPERACIÓN, ambos sensores
  caídos; reintentos exactamente en los ciclos esperados, calefactor desactivado
  tras cada `begin()`, canales 1/2 y 3/4 y registros `[SOURCE]` solo en cambios.
  El doble del BME680 reproduce el comportamiento confirmado de la biblioteca
  2.0.6 (`begin()` reactiva el calefactor); no sustituye la validación física.

### 2.2 BUILD-IOT-SENSOR-FAILOVER-06 (ejecución de pruebas C++ de host)

Compilador temporal: Zig 0.17.1 (clang 22.1.8) para Windows x86_64, descargado
de `https://ziglang.org/download/0.17.1/zig-x86_64-windows-0.17.1.zip`; SHA-256
y tamaño verificados contra `https://ziglang.org/download/index.json`. Usado
desde una carpeta temporal con cachés propias, sin PATH ni instalación; retirado
al terminar. Los lanzadores aceptan `-ZigExe <ruta a zig.exe>`.

| Prueba | Comando | Resultado |
|---|---|---|
| Pruebas C++ de host (incluye máquina de estados, invariantes de 100 000 ciclos, temporizador K y vector DHT11 3/4) | `run_host_tests.ps1 -ZigExe <zig.exe>` | **Ejecutadas: 331 checks, 0 failures, 0 avisos de compilación** |
| Arnés del `CoordinatorSensors.cpp` real con dobles | `run_failover_host_tests.ps1 -ZigExe <zig.exe>` | **Compila sin avisos; ejecución BLOQUEADA** por Smart App Control de Windows ("Una directiva de Control de aplicaciones bloqueó este archivo"). No se desactivó ni se esquivó el control. Ejecutado después en BUILD-IOT-SENSOR-FAILOVER-07 (§2.3) |
| Referencia Python del protocolo | `python -I -B test_vectors\test_protocol.py` | 49 checks, 0 failures |
| LED | `python -I -B test_vectors\test_coordinator_led.py` | 7 tests, 0 failures |

Las 331 comprobaciones de `host_tests.cpp` validan la lógica **pura**
(`CoordinatorSensorMath.h` y la biblioteca compartida), no la integración con
`CoordinatorSensors.cpp`. Cubren:

- F=3: SOSPECHA en los fallos 1 y 2, RESPALDO en el 3.º; `entered_degraded`
  una sola vez en 50 fallos; `fail` saturado en 3.
- R=3: RECUPERACIÓN con 1 y 2 lecturas válidas; PRINCIPAL y BME680 en la 3.ª,
  en ese mismo ciclo; `recovered` solo en la 3.ª.
- Vuelta inmediata a PRINCIPAL desde SOSPECHA; DHT11 caído en RECUPERACIÓN
  (BME680 por 1/2 sin detener el contador); ambos sensores caídos (SIN_SENSOR,
  sin publicación) y estado coherente después; fallos intermitentes sin
  acumulación.
- Invariantes del diseño sobre 100 000 ciclos pseudoaleatorios.
- K=5 (temporizador puro `reinitDue`): intentos en los ciclos 5, 10, 15 y 20
  tras el intento del ciclo 0; tras `reinitNextCycle`, en los ciclos 1, 6 y 11;
  nunca en ciclos consecutivos (200 ciclos).
- Canales 1–4 y umbrales en `CoordinatorConfig.h`; el `encodeFrame` C++ produce
  exactamente los bytes del vector Python `dht_channels_vector.json`, el parser
  del gateway lo acepta, el HMAC (contexto v1 sin cambios) se verifica y una
  trama alterada se rechaza; un canal repetido se rechaza.
- Motivos de los registros `[SOURCE]`.

### 2.3 BUILD-IOT-SENSOR-FAILOVER-07 (arnés integrado de `CoordinatorSensors.cpp`)

Entorno: g++ (Debian 10.2.1-6) 10.2.1 dentro del contenedor local
`node:20-bullseye` (imagen ya presente en el equipo), lanzado con `--rm`,
`--pull never` y `--network none`; repositorio montado en **solo lectura**. Los
binarios son de Linux, por lo que Smart App Control no intervino; no se
desactivó ninguna protección ni se instaló nada. Se compilaron las mismas
fuentes y opciones que `run_failover_host_tests.ps1` (`-std=c++11 -Wall
-Wextra`, dobles de `test_vectors/failover_mocks/`, `CoordinatorSensors.cpp`
real, `AgrotechProtocol.cpp` y `AgrotechSha256.cpp`).

| Programa | Compilación | Resultado |
|---|---|---|
| Arnés integrado `test_vectors/failover_sensor_tests.cpp` | Correcta, 0 avisos | **134 checks, 0 failures**, exit 0 |
| Trazador ciclo a ciclo (programa auxiliar fuera del repositorio, mismo código real y mismos dobles) | Correcta, 0 avisos | **194 checks, 0 failures**, exit 0 |

Las 194 comprobaciones del trazador son independientes de las 134 del arnés y
de las 331 de §2.2; no se suman entre sí.

Escenarios ejecutados sobre el código real de `CoordinatorSensors.cpp`
(ciclos de adquisición numerados desde 1; el intento de `setup()` es el ciclo 0):

| Escenario | Entrada simulada | Resultado observado |
|---|---|---|
| E1 Inicialización correcta | BME680 en `0x76`, lecturas válidas | Arranque: `begin()` en `0x77` (falla) y `0x76` (responde), calefactor desactivado, inicialización correcta. Ciclo 1: PRINCIPAL, BME680 por canales 1/2 (2534/6325), sin `[SOURCE]`, sin leer el DHT11 |
| E2 Fallo de inicialización | BME680 ausente al arrancar | Ciclos 1–2: SOSPECHA con DHT11 por 3/4; ciclo 3: RESPALDO. Reintentos solo en los ciclos 5 y 10. Variante con chip presente y configuración fallida: inicialización informada como fallida y SOSPECHA en el ciclo 1 |
| E3 Fallo operativo | Inicializa bien; desconexión en el ciclo 2. Variante: inicializa bien con lecturas NaN | Ciclos 2–3: SOSPECHA; ciclo 4: RESPALDO (F=3); DHT11 por 3/4 en todos los ciclos degradados |
| E4 Recuperación R=3 | Reconexión antes del ciclo 8 (sobre E2) | Ciclo 10: reintento correcto, RECUPERACIÓN con DHT11 (`ok=1`); ciclo 11: RECUPERACIÓN (`ok=2`); ciclo 12: PRINCIPAL con BME680 por 1/2 en ese mismo ciclo |
| E5 DHT11 cae durante la recuperación | DHT11 NaN tras el primer ciclo de RECUPERACIÓN | Ciclo 5: RECUPERACIÓN con DHT11 (`ok=1`); ciclo 6: BME680 por 1/2, `reason=dht_failed ok=2`; ciclo 7: PRINCIPAL |
| E6 Fallo simultáneo | BME680 ausente y DHT11 NaN | Ciclos 1–2: SIN_SENSOR, sin fuente publicada y `reading_count=0` (sin trama); `[SOURCE] ...both_failed` una sola vez. Ciclo 3 con DHT11 válido: RESPALDO, `source_restored` |
| E7 Reintentos integrados K=5 | E3 y su variante NaN | Degradación estando inicializado (fallo en n=2): intentos en los ciclos 5, 10 y 15 (n+3, n+8, n+13). Variante NaN: intentos en los ciclos 4, 9 y 14, todos correctos, sin entrar nunca en RECUPERACIÓN. Llamadas a `begin()` comprobadas ciclo a ciclo: 2 (sondeo `0x77`/`0x76`) en los ciclos de intento y 0 en el resto; ningún intento en ciclos consecutivos |

Comprobaciones transversales del arnés y del trazador: cada inicialización
correcta (arranques y reintentos) aplicó `setGasHeater(0, 0)` en ese mismo
ciclo; los reintentos fallidos se registran como `result=fail`; los canales
nunca mezclan fuentes; `[SOURCE]` aparece solo en los ciclos con cambio de
estado o de fuente (como máximo una vez por ciclo). Si el chip responde pero la
configuración falla, `bmeInitialize()` también desactiva el calefactor y sigue
informando el fallo.

### 2.4 Reglas funcionales del failover validadas

Decisiones aprobadas (detalle de canales y conmutación en `docs/PROTOCOL.md`,
§2.1):

- BME680 principal por los canales 1 (temperatura) y 2 (humedad); DHT11
  respaldo por los canales 3 (temperatura) y 4 (humedad). Una trama lleva una
  sola pareja de una sola fuente.
- F=3 fallos consecutivos del BME680 para pasar a RESPALDO.
- R=3 lecturas válidas consecutivas del BME680 para recuperar PRINCIPAL; la 3.ª
  lectura válida vuelve a PRINCIPAL y publica el BME680 en ese mismo ciclo.
- K=5 ciclos de adquisición entre intentos de reinicialización del BME680.
- Si el DHT11 falla durante RECUPERACIÓN, se publica el BME680 por 1/2 y el
  contador de recuperación continúa.
- Sin ningún sensor válido no hay lectura ni trama.
- Una inicialización correcta no equivale a una lectura válida.
- `setGasHeater(0, 0)` se aplica después de cada inicialización correcta.
- `[SOURCE]` se emite solo cuando cambia el estado o la fuente publicada.

### 2.5 Alcance de la validación del failover

| Nivel | Alcance |
|---|---|
| Validado en software | Lógica pura (§2.2); arnés integrado con `CoordinatorSensors.cpp` (§2.3); canales; reintentos simulados; selección de fuente; control simulado del calefactor; registros de cambio de fuente; codificación de la trama v1 y HMAC |
| Verificado solo de forma estática | Que `loop()` no consuma secuencia cuando no hay medición válida (`test_coordinator_led.py`, `test_07`: el retorno precede a `takeSeq`). La ruta completa de `loop()` en `coordinator.ino` no se ejecutó en el arnés |
| Pendiente de validación física | Comportamiento real del BME680 y del DHT11; tiempos y bloqueos del bus I2C; fallos internos del controlador BME680; temporización real del DHT11; efecto real de la inicialización sobre `Wire`; confirmación física del calefactor desactivado |

Limitaciones:

- La compilación del arnés en BUILD-IOT-SENSOR-FAILOVER-07 usó g++ para Linux,
  no el compilador cruzado Xtensa; la compilación para ESP32 es la de §2.1.
- Los dobles de `test_vectors/failover_mocks/` reproducen las llamadas que usa
  el firmware y el comportamiento confirmado de la biblioteca BME680 2.0.6, pero
  no el comportamiento eléctrico ni temporal del hardware.

## 3. Pruebas físicas pendientes

No se conectó hardware. Quedan pendientes:

- Detección real del SX1278 y lectura del registro de versión.
- Enlace LoRa entre coordinador y gateway con los pines y frecuencia
  confirmados.
- Alcance, RSSI, SNR, pérdida de tramas y tiempo en aire.
- Legalidad de frecuencia y potencia para la región.
- Lectura real de sensores, conversión ×100 y centinelas.
- Inicialización/detección BME680 en `0x77` y `0x76`, fallo/NaN/rangos,
  selección de respaldo DHT11 y descarte cuando ambos fallan.
- Conmutación física BME680 → DHT11 (desconexión/reconexión de SDA, arranque
  sin BME680), reintentos K=5 y calefactor desactivado en el sensor real.
- Comprobación visual/eléctrica del LED GPIO 25; el firmware de esta fase no lo
  acciona.
- Persistencia de `boot_counter` y del checkpoint de `seq` en NVS a través de
  cortes de energía, watchdog y reseteos por software (el contrato reinicia
  `seq` a 0 en cada arranque).
- Comportamiento de NVS ante fallos de escritura.
- Conexión Wi-Fi, NTP y reconexión.
- Publicación MQTT QoS 1, `retain=false`, PUBACK y reconexión.
- Aceptación del HMAC por parte del backend Django.
- Confirmación de la variante Base64 por el backend.

## 4. Limitaciones conocidas

- No se declara el firmware listo para producción.
- El gateway no implementa búfer local; si no hay MQTT, descarta la trama.
- El gateway no usa TLS en este MVP.
- La clave incluida es una clave pública de vector de prueba.
- La compilación del gateway se hizo con un stub de MQTT, no con la biblioteca
  real.
- La compilación del coordinador fue validada para software; no demuestra que
  los sensores ni el resto del hardware funcionen físicamente.
- Cualquier reset del ESP32 se cuenta como arranque; no se distingue un corte de
  energía real de un reset por watchdog o software.
