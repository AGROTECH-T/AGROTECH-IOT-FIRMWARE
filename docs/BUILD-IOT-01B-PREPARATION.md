# BUILD-IOT-01B — preparación para pruebas físicas

**Fecha:** 2026-11-15  
**Estado:** diagnósticos de adquisición implementados y compilación de software correcta.  
**Límite:** no se conectó ni flasheó el ESP32; no se hicieron pruebas físicas.

## Archivos inspeccionados

- `coordinator/coordinator.ino`
- `coordinator/CoordinatorConfig.h`
- `coordinator/CoordinatorSensors.cpp`
- `coordinator/CoordinatorSensors.h`
- `coordinator/CoordinatorSensorMath.h`
- `coordinator/CoordinatorCounters.cpp` y `.h`
- `shared/src/AgrotechProtocol.h` y `.cpp` (encoder Frame v1)
- `shared/src/AgrotechCounters.h` y `.cpp`
- `docs/PROTOCOL.md`
- `docs/BUILD-IOT-01.md`
- `docs/BUILD-IOT-01A-VALIDATION.md`

Se buscó `nodo_esp32_1.ino` en el repositorio y en el sketchbook Arduino
configurado (`C:\Users\israe\OneDrive\Documentos\Arduino`), pero no se
encontró. No fue posible contrastar el pinout con ese firmware de referencia.
No se utilizó como firmware de AGROTECH-T.

## Cambios implementados

Se modificó `coordinator/CoordinatorSensors.cpp` para añadir diagnósticos a la adquisición. Se guarda la dirección que respondió durante la
detección del BME680 y se informa si la inicialización y configuración tuvieron
éxito. El código prueba primero `0x77` y después `0x76` mediante
`Adafruit_BME680::begin(address)`; no se agregó un escáner I2C.

Cada lectura informa temperatura y humedad originales con dos decimales, así
como el resultado individual de validación. La conversión existente ×100,
rangos y rechazo de NaN/infinito se conservan mediante `scaleX100`. Una pareja
se acepta únicamente si ambos campos son válidos y proceden de la misma fuente.
Las lecturas se limpian al comienzo de cada adquisición; si ambos sensores
fallan, no se crea una lectura ni se pide generar un frame.

Se conserva BME680 como sensor principal y DHT11 como respaldo. `dht.begin()`
solo inicializa el driver; la presencia física del DHT11 se evalúa al leerlo.
El límite de intervalo del DHT11 continúa vigente. No se modificaron
`CoordinatorConfig.h`, el encoder/frame compartido, HMAC, contadores,
identidad, clave configurada, canales, configuración de radio ni vectores
oficiales.

## Funcionamiento y salida Serial

Serial opera a 115200 baudios. El `.ino` emite el banner de inicio, los
mensajes NVS y `[READY]`. El archivo de sensores ahora emite diagnósticos como:

```text
AGROTECH Coordinator BUILD-IOT-01 - acquire and encode only
[I2C] Initializing SDA=21 SCL=22
[I2C] Initialized
[BME680] Detected address=0x77
[BME680] Initialization successful
[DHT11] Driver initialized on GPIO 4; availability checked on read
[READY] sensor acquisition enabled; LoRa is not activated
```

Lectura primaria aceptada:

```text
[BME680] Temperature=23.50 C validation=valid
[BME680] Humidity=51.20 % validation=valid
[SENSOR] Source=BME680
[FRAME] valid Frame v1 generated; boot=... seq=... readings=2 bytes=67
```

Fallback (las cifras son ilustrativas):

```text
[BME680] Read failed
[DHT11] Attempting fallback
[DHT11] Temperature=24.00 C validation=valid
[DHT11] Humidity=50.00 % validation=valid
[SENSOR] Source=DHT11
[FRAME] valid Frame v1 generated; boot=... seq=... readings=2 bytes=67
```

Ambos sensores sin pareja válida:

```text
[BME680] Read failed
[DHT11] Attempting fallback
[DHT11] Temperature=nan C validation=invalid
[DHT11] Humidity=nan % validation=invalid
[DHT11] Read failed (invalid measurement pair)
[SENSOR] No valid measurement
[FRAME] Not generated
```

Otros mensajes posibles incluyen `[I2C] Initialization failed`,
`[BME680] Initialization failed (not detected at 0x77 or 0x76)`,
`[BME680] Initialization skipped (I2C unavailable)`, error de configuración
del BME680, driver DHT no inicializado/lectura omitida por intervalo y
`[FRAME] encode failed: <status>`. Si `Wire.begin()` falla, el resultado se
describe como I2C no disponible y no como falta de detección del BME680.

La dirección registrada es el argumento cuya llamada `bme.begin(address)` tuvo
éxito; no es una lectura independiente de escáner ni una confirmación eléctrica
del cableado. El encoder Frame v1 compartido conserva su resultado; el `.ino`
registra éxito o estado de error y tamaño, pero no imprime clave, HMAC, ni bytes
del frame. Ningún mensaje de estos diagnósticos revela secretos.

## Ciclo de adquisición

Tras inicializar sensores y contadores, `setup()` asigna
`last_acquisition_ms = millis() - COORDINATOR_TX_INTERVAL_MS` inmediatamente
antes de emitir `[READY]`. Por tanto, la siguiente iteración de `loop()` ve
vencido el temporizador y ejecuta una única primera adquisición inmediatamente
después de `[READY]` (con el breve retorno del control al `loop()`). Al iniciar
ese ciclo, `loop()` guarda `now`; las adquisiciones posteriores quedan
separadas por `COORDINATOR_TX_INTERVAL_MS`, que continúa en **60000 ms** en
`CoordinatorConfig.h`. No se añadió ningún intervalo temporal alternativo.

## Pines y hardware de referencia

| Señal | GPIO | Uso declarado |
|---|---:|---|
| BME680 SDA | 21 | I2C |
| BME680 SCL | 22 | I2C |
| DHT11 DATA | 4 | respaldo |
| LED azul | 25 | definido en configuración, no controlado por este firmware |
| LoRa NSS | 5 | histórico; radio no inicializada |
| LoRa RESET | 14 | histórico; radio no inicializada |
| LoRa DIO0 | 26 | histórico; radio no inicializada |
| LoRa SCK | 18 | histórico; radio no inicializada |
| LoRa MOSI | 23 | histórico; radio no inicializada |
| LoRa MISO | 19 | histórico; radio no inicializada |

En `CoordinatorConfig.h`, los `LORA_PIN_*` están en `-1`. El sketch no incluye
driver LoRa, no configura SPI de radio, no inicializa LoRa ni transmite. El
pinout procede de referencias históricas; la continuidad eléctrica no está
comprobada. Que se hayan retirado únicamente los pulsadores no verifica el
estado eléctrico de las demás conexiones.

## Dependencias y compilación

Configuración solicitada y usada: Arduino IDE **2.3.11**, Arduino CLI **1.6.0**,
ESP32 core **3.3.12**, placa `esp32:esp32:esp32` (ESP32 Dev Module). Bibliotecas
presentes, sin instalación ni actualización en esta tarea:

- Adafruit BME680 2.0.6
- DHT sensor library 1.4.7
- Adafruit Unified Sensor 1.1.15
- Adafruit BusIO 1.17.4

Comando ejecutado desde la raíz del repositorio (la ruta de bibliotecas se
resolvió al sketchbook instalado; se usó configuración temporal para evitar
cambiar la configuración del usuario):

```powershell
arduino-cli --config-file .validation-iot01b/arduino-cli.yaml compile `
  --fqbn esp32:esp32:esp32 `
  --libraries (Get-Location).Path `
  --build-path .validation-iot01b/build coordinator
```

**Resultado:** compilación correcta con CLI 1.6.0 y core 3.3.12, ejecutada tras
el cambio de arranque descrito en este informe. Uso informado:
325868 bytes de programa (24% de 1310720) y 23972 bytes de variables globales
(7% de 327680). La compilación no cargó firmware.

## Pruebas de software ejecutadas

- `python test_vectors/test_protocol.py`: **34 checks, 0 failures**, ejecutados
  tras el cambio de arranque descrito en este informe.
- `test_vectors/run_host_tests.ps1`: **no ejecutadas**; el script no encontró
  `g++` ni `clang++`. No se instaló compilador. El resultado no se considera
  aprobación de las pruebas C++ de host relacionadas con adquisición.
- Compilación del coordinador: **correcta**, según los valores de arriba.
- No se ejecutaron pruebas físicas ni se conectó el hardware.

## Procedimiento manual cuando se autorice

1. Con el montaje desconectado, inspeccionar las etiquetas y el estado visible
   del montaje. No cambiar el cableado como parte de este procedimiento.
2. Abrir `coordinator/coordinator.ino` en Arduino IDE 2.3.11; seleccionar ESP32
   core 3.3.12 y `ESP32 Dev Module`.
3. Compilar con **Verify**. Solo tras autorización explícita, seleccionar el
   puerto COM y usar **Upload** manualmente.
4. Abrir Monitor Serial a **115200 baudios**, reiniciar y guardar la salida
   desde el banner. La primera adquisición debe aparecer inmediatamente tras
   `[READY]`; observar ciclos posteriores separados por 60000 ms.
5. Registrar la dirección BME680 si se detecta, resultado de cada lectura,
   fuente seleccionada y salida `[FRAME]`.

La placa exacta carrier de 38 pines no se identificó y el puerto COM no se pudo
determinar mientras estaba desconectado. No se flasheó el dispositivo.

## Riesgos y limitaciones pendientes

- ESP32-WROOM-32 recomienda 3,0–3,6 V y sus GPIO no deben recibir 5 V. Una
  resistencia pull-up de SDA/SCL o DATA elevada a 5 V puede exceder límites.
  [Datasheet ESP32-WROOM-32](https://documentation.espressif.com/esp32-wroom-32_datasheet_en.html)
- El chip BME680 admite hasta 3,6 V en VDD/VDDIO; eso no establece los límites
  del breakout concreto de seis terminales. [Datasheet Bosch BME680](https://www.bosch-sensortec.com/media/boschsensortec/downloads/datasheets/bst-bme680-ds001.pdf)
- DFRobot documenta el DHT11 DFR0067 para +5 V, pero no se confirmó que el
  “DFRobot V2” sea ese SKU ni el nivel de DATA. [Ficha DFRobot DFR0067](https://wiki.dfrobot.com/dfr0067/)
- GPIO 5 es pin de strapping del ESP32 y aparece en el pinout histórico como
  NSS del RA-01; el efecto de carga externa no se comprobó. [Guía de diseño ESP32](https://docs.espressif.com/projects/esp-hardware-design-guidelines/en/latest/esp32/schematic-checklist.html)
- La documentación del RA-01 indica 3,3 V. LoRa no se activa por firmware en
  esta fase, pero no se verificó si su módulo permanece conectado a alimentación.
  [Documentación Ai-Thinker RA-01](https://docs.ai-thinker.com/en/Ra-01/index.html)
- No se conocen resistencia del LED, regulador de la carrier ni capacidad de la
  fuente. No se verificó continuidad, niveles eléctricos ni presencia de sensores.
- La llamada `bme.begin()` puede identificar la dirección que responde según el
  driver, pero no certifica cableado, alimentación estable ni medición física.
  Los diagnósticos DHT ocurren al leerlo; se respeta su intervalo mínimo.
- La ruta DHT no se prueba si el BME680 proporciona una pareja válida. Sin
  modificar el cableado ni incorporar una inyección de fallo autorizada, el
  fallback y el fallo simultáneo no se pueden forzar de forma controlada.

## Cierre

Archivos modificados en BUILD-IOT-01B-DIAG: `coordinator/CoordinatorSensors.cpp`,
`coordinator/coordinator.ino` (solo la marca inicial del temporizador) y este
informe. Frame v1, HMAC, vectores, contadores, identidad, credenciales y
configuración de radio permanecen sin cambios. LoRa sigue desactivada; no se
conectó, flasheó ni transmitió. La validación eléctrica y el comportamiento
físico siguen pendientes. La carga manual y las pruebas físicas quedan pendientes
de autorización del propietario.
