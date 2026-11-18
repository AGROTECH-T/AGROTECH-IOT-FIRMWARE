# BUILD-IOT-03A — configuración lógica de hardware

## Configuración compartida

`shared/src/AgrotechRadioConfig.h` es la única fuente de pines LoRa y perfil RF
que consumen coordinador y gateway. `AGROTECH_LORA_TRANSPORT_ENABLED` y
`AGROTECH_MQTT_TRANSPORT_ENABLED` permanecen en `false`.

| Parámetro | Valor de referencia | Estado |
|---|---:|---|
| Frecuencia | 433 MHz en firmware antiguo | Sin configurar; falta frecuencia central compatible con BW y límites de banda/emisiones |
| SF | SF7 | Referencia histórica compartida, no aprobación de producción |
| BW | 125 kHz | Referencia histórica compartida; verificar emisión/borde de banda |
| Coding rate | 4/5 (denominador 5) | Referencia histórica compartida |
| Sync word | `0xA6` | Referencia histórica compartida |
| TX power | 17 dBm histórico | **Sin configurar**; no trasladado al código operativo |
| Preamble | No especificado en los sketches antiguos | Sin configurar |
| CRC LoRa | No especificado explícitamente | Sin configurar |
| SPI pins | SCK 18, MISO 19, MOSI 23, NSS 5 | Referencia histórica en configuración compartida; falta comprobación física |
| Control LoRa | RESET 14, DIO0 26 | Referencia histórica en configuración compartida; falta comprobación física |

Los pines coordinador/gateway ahora toman sus valores de la cabecera común y
coinciden. La referencia de alimentación del RA-01 es 3.3 V; revisar la placa
portadora concreta, su alimentación y tierra al cablear.

## Pines del coordinador

| Función | GPIO | Polaridad/estado |
|---|---:|---|
| BME680 SDA | 21 | I2C |
| BME680 SCL | 22 | I2C |
| DHT11 DATA | 4 | Digital |
| LED testigo | 25 | Activo HIGH: HIGH encendido, LOW apagado; lógica ya existente |
| LoRa NSS / RESET / DIO0 | 5 / 14 / 26 | Valores de referencia común |
| LoRa SCK / MOSI / MISO | 18 / 23 / 19 | Valores de referencia común |

Se conserva el LED de estado actual del coordinador y su polaridad activa HIGH.
No se modifica la adquisición ni Frame v1/HMAC/contadores.

## LED del gateway

GPIO25 se documentó y usó en el firmware antiguo. Ese código expresaba encendido
con HIGH y apagado con LOW. El firmware actual adopta esos niveles lógicos, pero
no hay evidencia de continuidad/polaridad física del montaje actual; verificar
antes de energizarlo. El LED indica:

- Inicio: dos pulsos breves.
- Error/configuración deshabilitada o conectividad fallida: tres pulsos cada
  dos segundos.
- Espera con transports habilitados y sin MQTT conectado: parpadeo lento.
- MQTT conectado: encendido fijo.
- Recepción de una trama Frame v1 válida: destello breve.

LoRa y MQTT deshabilitados por configuración se muestran como error/configuración
pendiente. El gateway no toma como evidencia que haya recepción física.

## Regulación y potencia RF en Colombia

La tabla de ANE para **telemetría y telecontrol inalámbricos** publica el rango
433–434.79 MHz con máximo de **10 mW P.R.A.**. El portal oficial explica que el
uso libre aplica bajo las condiciones de esa tabla y que no da protección frente
a interferencias. [Portal ANE de acceso IoT y bandas de uso libre](https://portalespectro.ane.gov.co/Portal/mecanismos-acceso-iot.aspx),
[CNABF/Resolución ANE 105 compilada](https://www.ane.gov.co/Sliders/archivos/GestionTecnica/cnabf/Resolucion000105.pdf).

La P.R.A. se calcula con la potencia a la entrada de antena y la ganancia
respecto a un dipolo de media onda. Por tanto, potencia conducida del módulo,
ganancia de antena y pérdidas de alimentador/conectores son factores distintos.
La relación útil es `PRA(dBm) = P_conducida(dBm) + G_antena(dBd) - pérdidas(dB)`.
El valor histórico de 17 dBm es potencia conducida configurada en el driver, no
P.R.A. Por ejemplo, con ganancia neta de antena+línea de 0 dBd, equivaldría a
unos 50 mW P.R.A., por encima del máximo tabulado de 10 mW. No se puede validar
17 dBm sin ganancia real, pérdidas y medición/cálculo del sistema radiante.

Además, el centro histórico 433.000 MHz cae en el borde inferior del rango y
BW nominal 125 kHz requiere revisar frecuencia central y máscara/emisión fuera
de banda. La tabla no convierte automáticamente cualquier configuración LoRa
433 MHz en conforme. La frecuencia exacta y potencia deben definirse con el
módulo/antena y regulación aplicable. No se activaron.

## Funciones antiguas excluidas

No se importaron funciones de botones direccionales, botón de pelota, eventos,
AG1 ASCII, motores/actuadores ni topics MQTT de botones. El camino actual solo
usa mediciones en Frame v1 binario firmado.

## Verificación

- Arduino CLI 1.6.0, ESP32 core 3.3.12, FQBN `esp32:esp32:esp32`:
  coordinador **correcto**, 330552 bytes de programa y 24092 bytes de RAM
  global; gateway **correcto**, 565968 bytes y 31932 bytes respectivamente.
- `test_protocol.py`: 34 checks, 0 fallos.
- `test_coordinator_led.py`: 6 simulaciones controladas, 0 fallos.
- Comprobación estática: pines compartidos/flags/sensores coinciden; no se
  encontraron funciones antiguas de botones, AG1 o actuadores en los sketches.
- Advertencias: LoRa 0.8.0 usa los literales deprecados `B111` y `B1000` con
  el ESP32 core 3.3.12. Sin fallos de compilación.
- No se flashea, transmite ni conecta al broker.
