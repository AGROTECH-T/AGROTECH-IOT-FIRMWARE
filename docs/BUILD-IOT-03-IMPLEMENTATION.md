# BUILD-IOT-03 — transporte LoRa preparado

## Estado

El coordinador puede enviar la trama Frame v1 binaria firmada y el gateway
puede recibirla y verificar su estructura antes de pasar los mismos bytes al
payload existente. El envío asíncrono usa el callback `onTxDone` de LoRa 0.8.0.
El gateway no altera la serialización ni presenta la comprobación estructural
como autenticación HMAC; el backend sigue siendo el verificador de identidad,
versión de clave y firma.

Por seguridad operativa, `AGROTECH_LORA_TRANSPORT_ENABLED` y
`AGROTECH_MQTT_TRANSPORT_ENABLED` permanecen en `false`. BUILD-IOT-03A fijó
los pines compartidos como referencias históricas y compartió SF7, BW125 kHz,
CR4/5 y sync word `0xA6`. Frecuencia, potencia, preámbulo y CRC siguen sin
configurar (sentinelas 0/-1). Falta aprobación de frecuencia y potencia que
considere la normativa y cadena de antena. Con esos valores pendientes el
código no inicializa SPI/LoRa, Wi-Fi ni MQTT, y no transmite ni publica.

Para la futura activación habrá que establecer en
`shared/src/AgrotechRadioConfig.h` parámetros RF aprobados, verificar físicamente
los pines compartidos, y habilitar el transporte solo después de revisar
requisitos regionales y de hardware. La conexión MQTT
requiere además host, puerto, CA y las credenciales individuales provisionadas
en NVS. No colocar credenciales en este repositorio.

## Flujo de transporte preparado

1. El coordinador lee su UID, clave HMAC y versión individuales desde NVS.
2. Genera una trama Frame v1 firmada mediante la biblioteca compartida, sin
   cambiar su estructura ni sus bytes.
3. Si radio habilitada y configuración completa, transmite esos bytes en modo
   asíncrono; mientras haya transmisión pendiente descarta un ciclo de envío
   nuevo sin detener adquisición y contadores.
4. El gateway limita el tamaño, requiere lectura completa y acepta únicamente
   una estructura Frame v1 válida. El HMAC se valida en el backend contra el
   UID y la versión registrados.
5. MQTT solo se configura/conecta/publica con su bandera habilitada y
   configuración completa. El esquema TLS conserva verificación por CA.

No hay ACK ni reintentos LoRa en esta implementación. La entrega continúa
siendo best effort.

## Verificación de software

- Arduino CLI 1.6.0, ESP32 core 3.3.12, FQBN `esp32:esp32:esp32`.
- Coordinador: 330552 bytes de programa y 24092 bytes de RAM global.
- Gateway tras añadir LED de estado: 565968 bytes de programa y 31932 bytes de
  RAM global.
- `test_vectors/test_protocol.py`: 34 checks, 0 fallos. Comprueba el contrato
  de serialización Frame v1, tamaños, orden de bytes, HMAC vector y payload.
- `test_vectors/run_host_tests.ps1`: no ejecutó los tests C++ porque no hay
  `g++` ni `clang++` host instalado en este entorno.
- No se conectó a hardware ni a broker, no hubo transmisión, publicación,
  escritura en base de datos ni flasheo.

El vector HMAC de pruebas sigue existiendo solo como vector de conformidad;
no es una credencial operativa y el radio permanece deshabilitado.

## Restricciones del hardware

El objetivo compila para ESP32 sin asumir secure element. Las credenciales se
guardan en NVS y la clave de firmware no queda protegida contra extracción por
un atacante con acceso físico y herramientas adecuadas. La protección depende
de la configuración de secure boot/flash encryption de producción, que no se
verificó ni se habilitó durante esta fase. No constituye protección física
absoluta.
