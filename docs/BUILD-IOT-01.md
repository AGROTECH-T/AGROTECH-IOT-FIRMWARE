# BUILD-IOT-01 — adquisición de sensores y Frame v1

## Implementación

- BME680 por I2C con `Wire` en SDA GPIO 21 / SCL GPIO 22. Se buscan las
  direcciones `0x77` y `0x76`; se configuran sobremuestreo de temperatura y
  humedad y filtro IIR. Una lectura válida proporciona las dos métricas.
- DHT11 en GPIO 4 como respaldo de la pareja temperatura/humedad. Se limita su
  lectura a una llamada cada 2000 ms como mínimo; el intervalo operativo sigue
  siendo 60000 ms.
- Se rechazan NaN, infinito y valores fuera de rango. Se escala con redondeo a
  ×100. Un fallo parcial del sensor principal invalida la pareja completa y
  provoca el intento de respaldo; si el DHT11 tampoco da una pareja válida, se
  omite la trama y no se reutilizan datos anteriores.
- Las pruebas C++ de host cubren conversión ×100, NaN/infinito, límites físicos,
  prioridad BME680 > DHT11 > sin lectura y descarte de lecturas anteriores.
  BUILD-IOT-01A las ejecutó con Zig 0.17.1: 246 checks, 0 fallas.
- La procedencia (`BME680` o `DHT11`) se registra por Serial. Frame v1 no tiene
  un campo para esa procedencia, así que no se altera el formato.
- Las lecturas usan los canales 1 (temperatura) y 2 (humedad), según el vector
  oficial y `docs/PROTOCOL.md`. No se añaden presión ni gas.
- Tras una adquisición válida, el coordinador llama a `encodeFrame()` del
  código compartido con la identidad, clave de prueba y contadores existentes.
  Secuencias solo avanzan al producir una pareja válida. La clave configurada
  sigue siendo pública de prueba y no debe usarse en producción.
- El sketch solo genera y firma el frame en memoria; no inicializa ni transmite
  por LoRa en esta fase. El intervalo de adquisición conserva 60 segundos.
  GPIO 25 no se acciona y no se declara probado.

## Dependencias de compilación

Versiones seleccionadas, consultadas en metadatos oficiales de Adafruit:

- Adafruit BME680 Library 2.0.6
- DHT sensor library 1.4.7
- Adafruit Unified Sensor 1.1.15
- Adafruit BusIO 1.17.4
- Adafruit GFX Library 1.13.0
- Adafruit SSD1306 2.5.17

Adafruit BME680 declara GFX y SSD1306 en sus metadatos de paquete, pero el
coordinador no incluye sus encabezados. La compilación aislada con solo las
cuatro dependencias usadas por los drivers pasó. Las cuatro ya estaban
instaladas en el sketchbook del usuario; BUILD-IOT-01A no instaló ni actualizó
bibliotecas Arduino.

## Estado de aceptación

| Criterio | Estado | Evidencia / limitación |
|---|---|---|
| Drivers reales BME680 y DHT11 | IMPLEMENTADO SIN VALIDACIÓN FÍSICA | Compilado para ESP32 core 3.3.12; hardware no probado. |
| BME680 con lectura válida | IMPLEMENTADO SIN VALIDACIÓN FÍSICA | Direcciones, configuración y rangos codificados; no hay sensor conectado para validar. |
| Respaldo DHT11 | IMPLEMENTADO SIN VALIDACIÓN FÍSICA | Pareja completa, rate limit y descarte de lecturas inválidas; no hay sensor conectado. |
| Conversión ×100 y rangos | IMPLEMENTADO Y VALIDADO | Casos C++ de temperatura, humedad, redondeo, no finitos y límites aprobados. Lecturas físicas pendientes. |
| Trama Frame v1 | IMPLEMENTADO Y VALIDADO | Sketch compilado y encoder compartido verificado byte por byte con vectores. |
| HMAC y contadores | IMPLEMENTADO Y VALIDADO | Suite C++ y Python cubren HMAC de referencia y contadores; ambas pasaron. |
| Compatibilidad con vectores oficiales | IMPLEMENTADO Y VALIDADO | Pruebas Python y C++ aprobaron los bytes y el HMAC oficiales; formato sin cambios. |
| Pruebas automatizadas solicitadas | IMPLEMENTADO Y VALIDADO | 246 checks C++ y 34 checks Python aprobados. Detección física queda para BUILD-IOT-01B. |
| Transmisión LoRa desactivada | IMPLEMENTADO Y VALIDADO | El coordinador ya no inicializa ni llama funciones de envío LoRa. |
| Validación física / LED | PENDIENTE | No se conectó ni midió hardware. |

## Pendientes y riesgos

- Validar físicamente los sensores, sus fallos y la conmutación BME680/DHT11.
- Confirmar que los canales 1 y 2 corresponden a los bindings provisionados en
  backend; se tomaron del vector/contrato existente, no se añadieron canales.
- Confirmar rangos físicos del módulo DHT11/BME680 concretos y evaluar
  calibración/precisión en el montaje.
- La procedencia del sensor solo queda en logs porque Frame v1 no admite un
  campo para ella.
- No flasheado; no hubo emisión LoRa.

BUILD-IOT-01A de software quedó **COMPLETADO**. BUILD-IOT-01 queda **PARCIAL**
hasta completar validación física en BUILD-IOT-01B. Después puede abordarse
BUILD-IOT-02 — configuración y activación del enlace LoRa.
