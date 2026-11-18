# BUILD-IOT-03C-FIX — cierre de compilación

## 1. Causa exacta de las definiciones duplicadas

Arduino compila **todos** los `.cpp` que están directamente dentro de la carpeta
del sketch, además de la biblioteca `AgrotechProtocol` (instalada desde
`shared/`). El coordinador tenía copias locales de los cinco módulos compartidos,
así que cada símbolo se compilaba dos veces y el enlazador fallaba:

```text
multiple definition of `agrotech::base64EncodedLength(unsigned int)'
  shared/AgrotechBase64.cpp.o vs coordinator/AgrotechBase64.cpp.o
...
collect2.exe: error: ld returned 1 exit status
```

No era un problema de cabeceras ni de `#include`: eran dos unidades de
compilación aportando las mismas definiciones al enlace.

## 2. Copia canónica

Antes de tocar nada se comparó byte a byte cada archivo local contra
`shared/src/`. Las diez copias eran **idénticas** (SHA-256 truncado):

| Archivo | SHA-256 (inicio) | ¿Local == shared? |
|---|---|---|
| `AgrotechBase64.cpp` | `8DC14431715CF279` | Sí |
| `AgrotechBase64.h` | `63B89B7166097B56` | Sí |
| `AgrotechCounters.cpp` | `7B4345DC5BC16DE2` | Sí |
| `AgrotechCounters.h` | `99A89D183F645CE0` | Sí |
| `AgrotechMqttPayload.cpp` | `31E79533E10AC38E` | Sí |
| `AgrotechMqttPayload.h` | `6A7A934198B7AE1A` | Sí |
| `AgrotechProtocol.cpp` | `9E3BFE59F89CCB54` | Sí |
| `AgrotechProtocol.h` | `C7A0520625511419` | Sí |
| `AgrotechSha256.cpp` | `03721BB404E2CC80` | Sí |
| `AgrotechSha256.h` | `3F33C66DE9CBDC03` | Sí |

Se declara canónica la copia de `shared/src/` porque:

1. `shared/library.properties` la publica como biblioteca `AgrotechProtocol`
   (versión 0.1.0), con `includes=AgrotechProtocol.h`.
2. El gateway ya la consume como biblioteca, sin copias locales.
3. El procedimiento documentado en `README.md` instala `shared` como
   `AgrotechProtocol` en el sketchbook de Arduino IDE.
4. Es la única copia que evolucionó con los cambios de BUILD-IOT-03C (el perfil
   de radio vive en `shared/src/AgrotechRadioConfig.h`).

## 3. Acción aplicada

Se eliminaron **solo** las copias locales redundantes de `coordinator/`
(`Agrotech*.cpp` y `Agrotech*.h`, 10 archivos), cuyo contenido era idéntico al
canónico. No se eliminó código exclusivo del coordinador
(`coordinator.ino`, `Coordinator*.cpp/.h`, `CoordinatorSensorMath.h`).

Consecuencia: el coordinador y el gateway compilan y enlazan la misma
implementación shared. Frame v1, HMAC, contadores, credenciales y payload MQTT
no cambian: el código enlazado es byte a byte el mismo que antes.

## 4. Verificación

Compilado con el `arduino-cli` incluido en Arduino IDE 2.x, core ESP32 3.3.12,
FQBN `esp32:esp32:esp32`, bibliotecas del sketchbook y `shared` como
`AgrotechProtocol`, **desde las carpetas reales del repositorio y sin espejos ni
exclusiones**:

| Sketch | Resultado | Programa | RAM global |
|---|---|---:|---:|
| `coordinator/coordinator.ino` | Correcto | 330.756 B (25 %) | 24.092 B (7 %) |
| `gateway/gateway.ino` | Correcto | 566.260 B (43 %) | 31.932 B (9 %) |

`AGROTECH_LORA_TRANSPORT_ENABLED` y `AGROTECH_LORA_PROFILE_AUTHORIZED` siguen en
`false`. No se cargó firmware, no se transmitió ni recibió por LoRa y no se
conectó Wi-Fi ni MQTT.

## 5. Procedimiento reproducible en Arduino IDE

1. Instalar el core ESP32 y las bibliotecas del `README.md` (LoRa, MQTT,
   Adafruit BME680/BusIO/Unified Sensor, DHT).
2. Copiar `shared/` a `Documents\Arduino\libraries\AgrotechProtocol` y
   reiniciar el IDE.
3. `Archivo > Abrir...` → `coordinator/coordinator.ino` o
   `gateway/gateway.ino`.
4. Seleccionar el modelo exacto de ESP32 y el puerto COM.
5. `Verificar` (compilar) o `Subir`. No deben aparecer definiciones duplicadas.
