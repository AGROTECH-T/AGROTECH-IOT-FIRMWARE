# BUILD-IOT-01A — validación de software del coordinador

**Fecha:** 2026-11-15  
**Resultado:** **COMPLETADO** para la validación de software. La validación
física de BUILD-IOT-01 sigue pendiente y pertenece a BUILD-IOT-01B.

## A. Resumen ejecutivo

El coordinador compiló para `esp32:esp32:esp32` con ESP32 core 3.3.12. La suite
C++ pasó **246 checks, 0 fallas**; la suite Python pasó **34 checks, 0 fallas**.
La compilación aislada requirió solo las cuatro bibliotecas que usan los
drivers. Frame v1, HMAC, SHA-256, vectores oficiales, contadores y configuración
de LoRa no se modificaron.

No se flasheó el ESP32, no se conectó hardware, no se habilitó GPIO 25 y no se
realizaron transmisiones LoRa.

## B. Entorno y herramientas

| Herramienta | Versión / estado |
|---|---|
| Windows | 10.0.26200.0, Win32NT |
| Arduino IDE | 2.3.11 |
| Arduino CLI | 1.6.0, incluido con Arduino IDE |
| Arduino ESP32 core | 3.3.12, ya instalado; el índice ofrece 4.0.0-alpha1, que no se usó |
| Placa de compilación | `esp32:esp32:esp32` — ESP32 Dev Module |
| GCC cruzado ESP32 | xtensa-esp-elf-g++ 14.2.0 (`esp-14.2.0_20260121`) |
| Python | 3.14.7 |
| pip | 26.2.1 |
| Compilador C++ de host | Zig 0.17.1, instalado temporalmente dentro de `.validation-iot01a` y retirado al terminar |
| g++ / clang++ | No encontrados |
| CMake | No encontrado; no es necesario para Arduino CLI ni para este proyecto |
| Git | 2.55.0.windows.5; la carpeta no contiene `.git`, por lo que `git status` no está disponible |

No existe `platformio.ini` ni una versión de core fijada en los archivos del
proyecto. Se usó el core 3.3.12 disponible y el FQBN genérico ESP32 ya usado en
las instrucciones de compilación del repositorio. La configuración Arduino
temporal apuntó a los directorios existentes sin cambiar la configuración
global.

## C. Dependencias

Las bibliotecas ya estaban instaladas en
`C:\Users\israe\OneDrive\Documentos\Arduino\libraries`; BUILD-IOT-01A no
instaló ni actualizó dependencias Arduino.

| Dependencia usada por el coordinador | Versión | Motivo |
|---|---:|---|
| Adafruit BME680 Library | 2.0.6 | `Adafruit_BME680.h` |
| DHT sensor library | 1.4.7 | `DHT.h` |
| Adafruit Unified Sensor | 1.1.15 | Incluida por el driver BME680 y declarada por DHT |
| Adafruit BusIO | 1.17.4 | Interfaz I2C usada por BME680 |

GFX 1.13.0 y SSD1306 2.5.17 aparecen en los metadatos del paquete BME680, pero
el sketch no incluye bibliotecas de pantalla. La compilación aislada se hizo
con solo las cuatro bibliotecas de la tabla y pasó.

Para las pruebas C++ se descargó Zig 0.17.1 en el directorio temporal local de
validación. No se agregó como dependencia del proyecto ni del firmware.

## D. Pruebas

| Prueba | Resultado | Evidencia |
|---|---|---|
| Conversión de temperatura ×100 | APROBADA | 25.34 → 2534; negativo y límites probados |
| Conversión de humedad ×100 | APROBADA | 63.25 → 6325 |
| Redondeo positivo y negativo | APROBADA | Verificado con `lroundf` |
| NaN e infinito | APROBADA | Ambos rechazados |
| Valores fuera de rango | APROBADA | Rechazo en límites de temperatura/humedad probados |
| Prioridad BME680 | APROBADA | BME680 se selecciona si ambos resultados son válidos |
| Respaldo DHT11 | APROBADA | DHT11 se selecciona si BME680 falla |
| Ambos sensores sin lectura válida | APROBADA | El selector devuelve `None` |
| Lectura antigua | APROBADA | El conteo se limpia; `encodeFrame()` rechaza la trama vacía |
| Contadores anti-replay | APROBADA | Suite C++ prueba arranque, incremento y agotamiento |
| Frame v1 / HMAC / vectores | APROBADA | Suite C++ y Python comprueban bytes y tag oficiales |
| Prueba Python `test_protocol.py` | APROBADA | 34 checks, 0 failures |
| Suite C++ `host_tests.cpp` | APROBADA | 246 checks, 0 failures |

Comando Python ejecutado:

```powershell
& 'C:\Users\israe\AppData\Local\Python\pythoncore-3.14-64\python.exe' test_vectors\test_protocol.py
```

Comando C++ ejecutado con el compilador temporal:

```powershell
& '.validation-iot01a\host-tools\ziglang\zig.exe' c++ -std=c++11 -Wall -Wextra `
  -I shared\src -I coordinator `
  test_vectors\host_tests.cpp shared\src\AgrotechProtocol.cpp `
  shared\src\AgrotechSha256.cpp shared\src\AgrotechBase64.cpp `
  shared\src\AgrotechMqttPayload.cpp shared\src\AgrotechCounters.cpp `
  -o .validation-iot01a\host_tests.exe
& .validation-iot01a\host_tests.exe
```

El runner `run_host_tests.ps1` busca `g++` o `clang++`; ambos faltan en este
Windows. Por eso se compiló el mismo archivo de pruebas directamente con Zig.

## E. Compilación ESP32

Comando ejecutado con una configuración y biblioteca aisladas:

```powershell
& 'C:\Program Files\Arduino IDE\resources\app\lib\backend\resources\arduino-cli.exe' `
  --config-file '.validation-iot01a\arduino-cli-isolated.yaml' compile `
  --fqbn esp32:esp32:esp32 `
  --build-path '.validation-iot01a\build-isolated' `
  --libraries (Get-Location).Path coordinator
```

**Resultado: compilación correcta.**

- Programa: 324.064 bytes de 1.310.720 (24 %).
- RAM global: 23.972 bytes de 327.680 (7 %); 303.708 bytes disponibles.
- Solo se compiló `coordinator`; no se compiló ni modificó el gateway.
- No hubo flasheo ni conexión a placa.

## F. Errores, correcciones y límites del entorno

- La configuración predeterminada de Arduino CLI apuntaba al directorio vacío
  del entorno aislado. Se usó un archivo CLI temporal que apuntó al core
  3.3.12 ya instalado y a las cuatro bibliotecas necesarias.
- La primera invocación aislada del compilador ESP32 devolvió `Illegal System
  DLL Relocation`. La compilación autorizada fuera del aislamiento del proceso
  terminó correctamente; no se requirió corregir errores de código.
- No hay metadatos Git en esta carpeta. El estado inicial/historial de Git no
  puede recuperarse; se verificaron por lectura las rutas protegidas y no se
  modificaron.
- No hubo una validación del sensor real ni del comportamiento eléctrico.

La única corrección de código de esta fase fue integrar `clearReadings()` en
el inicio de adquisición y añadir casos host de humedad, redondeo y descarte
de datos anteriores.

## G. Archivos modificados en BUILD-IOT-01A

- `coordinator/CoordinatorSensorMath.h` — limpieza de conteo antes de leer para
  que un ciclo fallido no pueda serializar mediciones anteriores.
- `coordinator/CoordinatorSensors.cpp` — usa esa limpieza al inicio de cada
  adquisición.
- `test_vectors/host_tests.cpp` — añade validaciones de humedad, redondeo y
  rechazo de tramas con lecturas antiguas.
- `docs/BUILD-IOT-01A-VALIDATION.md` — este informe.
- `docs/BUILD-IOT-01.md`, `docs/TESTS.md` y `README.md` — resultados y estado
  actualizados.

Los archivos de prueba, dependencias y compilación creados bajo
`.validation-iot01a` son temporales y se retiraron después de la validación.

## H. Integridad del protocolo y seguridad

- Frame v1 mantiene cabecera de 25 bytes, lecturas de 5 bytes y tag HMAC de 32
  bytes.
- Se mantienen big-endian, contexto HMAC y código compartido
  `encodeFrame()`/`computeTag()`.
- No se modificaron Frame v1, HMAC, SHA-256, contadores, anti-replay ni vectores
  oficiales.
- Los checks C++ y Python aprobaron el vector oficial de trama/HMAC.
- Canales 1 y 2 permanecen como en el vector/contrato; su binding físico con
  backend aún no se valida.
- La clave del repositorio sigue identificada como clave pública de pruebas; no
  se imprime por Serial.
- Inspección de `coordinator/`: sin llamadas de transmisión LoRa y sin logs de
  la clave.

## I. Estado y siguiente paso

**BUILD-IOT-01A: COMPLETADO.** Se aprobaron las pruebas de software y la
compilación ESP32 requerida por esta fase.

**BUILD-IOT-01: PARCIAL** hasta la validación física. Para BUILD-IOT-01B quedan
la detección y lectura reales de los sensores, el respaldo con hardware y la
comprobación del LED. La placa no se flasheó ni se transmitió por radio.
