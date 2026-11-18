# BUILD-IOT-03B-SPI — Diagnóstico SPI del SX1278

Sketch temporal e independiente para comprobar la comunicación SPI entre un ESP32
y un módulo LoRa **Ai-Thinker RA-01 (SX1278)**. Solo lee el registro `VERSION`
(`0x42`). No transmite, no configura RF, no usa Wi-Fi ni MQTT y no depende del
firmware de producción.

## Alcance

- Lee `RegVersion` (`0x42`) tras un reinicio controlado del módulo.
- Repite la lectura 10 veces para comprobar estabilidad.
- Compara con el valor esperado `0x12`.
- Imprime un resultado final: `DETECTADO` o `NO DETECTADO`.

No escribe registros de configuración, no fija frecuencia, potencia ni parámetros
de modulación, y no envía paquetes LoRa.

## Hardware

| Señal | GPIO ESP32 |
|---|---:|
| NSS | 5 |
| RESET | 14 |
| DIO0 | 26 |
| SCK | 18 |
| MOSI | 23 |
| MISO | 19 |
| VCC | 3.3 V |
| GND | GND |

El RA-01 es un módulo de 3.3 V. No alimentar desde 5 V y compartir GND con el
ESP32.

## Dependencias

- Arduino IDE con el core de **ESP32** instalado desde el Gestor de placas.
- Biblioteca `SPI`, incluida en el core de ESP32. No hace falta instalar `LoRa`
  ni ninguna otra biblioteca.

## Cómo abrirlo en Arduino IDE

1. Abrir Arduino IDE.
2. `Archivo > Abrir...` y seleccionar
   `diagnostics/BUILD-IOT-03B-SPI/BUILD-IOT-03B-SPI.ino`.
   La carpeta y el archivo tienen el mismo nombre, por lo que el IDE lo abre como
   sketch válido.
3. Si el proyecto está en otra ruta, no mover solo el `.ino`: mantenerlo dentro de
   su carpeta.

## Seleccionar la placa

1. `Herramientas > Placa > ESP32 Arduino` y elegir el **modelo exacto** del ESP32
   que se va a probar (por ejemplo `ESP32 Dev Module` solo si esa es la placa
   real). No seleccionar una placa al azar: el modelo define pines, voltaje y
   variante del core.
2. En `Herramientas > Upload Speed` puede dejarse el valor por defecto de la
   placa.

## Seleccionar el puerto COM

1. Conectar el ESP32 por USB.
2. `Herramientas > Puerto` y elegir el COM que aparece al conectar la placa.
   Si no aparece ningún puerto, revisar el cable USB (de datos, no solo de carga)
   y el driver CP210x/CH340 de la placa.

## Compilar y cargar

1. Pulsar **Verificar** (icono de check) para compilar.
2. Pulsar **Subir** (icono de flecha) para cargar.
3. Si la carga no inicia, mantener pulsado **BOOT** del ESP32 al comenzar la subida
   y soltarlo al aparecer `Connecting...`/`Writing at`.

Este sketch no se carga automáticamente: la carga la inicia el usuario desde el
IDE.

## Abrir el Monitor Serie

1. `Herramientas > Monitor Serie` (o `Ctrl+Shift+M`).
2. Seleccionar **115200 baudios** (el sketch configura `Serial.begin(115200)`).
3. Pulsar el botón **EN/RST** del ESP32 si no aparece salida.

## Salida esperada

Con el módulo conectado y alimentado correctamente:

```text
================================================
 BUILD-IOT-03B-SPI - Diagnostico SPI SX1278
 Solo lectura de registros. Sin TX LoRa, sin RF.
================================================
 NSS=5 RESET=14 DIO0=26
 SCK=18 MOSI=23 MISO=19
 VERSION esperada: 0x12

 Reinicio controlado del SX1278...
 Primer lectura tras reset: 0x12 en 3 ms (listo)

 Lecturas de VERSION (0x42), 10 intentos:
   #01 -> 0x12  valor esperado
   ...
   #10 -> 0x12  valor esperado

 Correctas: 10/10  (100%)
 Ultima lectura: 0x12
 Estabilidad: estable

------------------------------------------------
 RESULTADO: DETECTADO (VERSION = 0x12 estable)
------------------------------------------------
 Fin del diagnostico. No se transmitio por LoRa.
```

Interpretación de fallos:

| Lectura | Causa probable |
|---|---|
| `0x00` | Sin alimentación, GND/MISO abiertos o RESET no liberado |
| `0xFF` | MISO/SCK desconectados, NSS no conmutado o módulo mal soldado |
| Otro valor | SCK/MOSI/NSS mal cableados o módulo distinto |
| Mezcla de valores | Conexión inestable o alimentación insuficiente |

Un `NO DETECTADO` indica un problema de cableado, alimentación o módulo; no
implica fallo del firmware de producción. Esta prueba no valida el enlace de
radio, solo el acceso SPI al registrador del SX1278.
