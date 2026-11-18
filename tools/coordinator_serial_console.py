"""MVP-IOT-F8 - Consola serie y aprovisionamiento del coordinador AGROTECH-T.

Uso (PowerShell, con pyserial instalado):
    python tools/coordinator_serial_console.py COM8
    python tools/coordinator_serial_console.py COM8 --reprovision

- Muestra en vivo el log del ESP32 (115200 baudios).
- Cuando el coordinador pide credenciales ("[PROVISION] Enter SET"), solicita
  SIN eco la linea completa  SET <CO-UID> <version> <clave_base64>  generada
  por el backend, valida su formato y la envia.
- --reprovision envia REPROVISION en la ventana de 2 s del arranque para
  borrar la credencial guardada y volver a pedirla.
- Ctrl+C para salir. No guarda ni imprime secretos.
"""
from __future__ import annotations

import argparse
import base64
import binascii
import getpass
import re
import sys
import threading
import time

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    sys.exit("Falta pyserial. Instalelo con:  python -m pip install pyserial")

BAUD = 115200
PROMPT_MARKER = "[PROVISION] Enter SET"
REJECT_MARKER = "[PROVISION] Rejected"
STORED_MARKER = "[PROVISION] Credentials stored"
SET_RE = re.compile(
    r"^SET (CO-[ABCDEFGHJKMNPQRSTUVWXYZ23456789]{10}) ([0-9]{1,5}) ([A-Za-z0-9+/]{43}=)$"
)


def ask_set_line() -> bytes | None:
    print("\n>>> El coordinador pide su credencial. Lo que pegue NO se mostrara.")
    line = getpass.getpass(">>> Pegue la linea completa 'SET CO-... 1 ...=' y pulse Enter: ").strip()
    m = SET_RE.fullmatch(line)
    ok = False
    if m and 1 <= int(m.group(2)) <= 65535:
        try:
            ok = len(base64.b64decode(m.group(3), validate=True)) == 32
        except (binascii.Error, ValueError):
            ok = False
    if not ok:
        print(">>> ERROR: formato invalido. Debe ser: SET CO-XXXXXXXXXX <1..65535> <Base64 de 44 caracteres>.")
        print(">>> No se envio nada.")
        line = ""
        return None
    data = (line + "\n").encode("ascii")
    line = ""
    return data


def main() -> int:
    parser = argparse.ArgumentParser(description="Consola serie del coordinador AGROTECH-T")
    parser.add_argument("port", help="Puerto serie, por ejemplo COM8")
    parser.add_argument("--reprovision", action="store_true",
                        help="Borra la credencial guardada al arrancar y la vuelve a pedir")
    args = parser.parse_args()

    try:
        port = serial.Serial(args.port, BAUD, timeout=0.2)
    except serial.SerialException as exc:
        print(f"ERROR: no se pudo abrir {args.port}: {exc}")
        print("Cierre el Monitor Serie de Arduino IDE y revise el numero de puerto.")
        return 2

    # Reinicio limpio del ESP32 (EN) para ver el arranque completo.
    port.dtr = False
    port.rts = True
    time.sleep(0.1)
    port.rts = False
    print(f"Conectado a {args.port} a {BAUD}. Ctrl+C para salir.\n")
    if args.reprovision:
        time.sleep(1.0)
        port.write(b"REPROVISION\n")
        print(">>> REPROVISION enviado.")

    stop = threading.Event()
    prompt = threading.Event()

    def reader():
        buf = b""
        while not stop.is_set():
            try:
                chunk = port.read(256)
            except serial.SerialException:
                print("\n>>> Puerto desconectado.")
                stop.set()
                return
            if not chunk:
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                text = raw.decode("utf-8", errors="replace").rstrip("\r")
                print(text)
                if PROMPT_MARKER in text or REJECT_MARKER in text:
                    prompt.set()

    threading.Thread(target=reader, daemon=True).start()
    try:
        while not stop.is_set():
            if prompt.is_set():
                time.sleep(0.5)
                prompt.clear()
                data = ask_set_line()
                if data is None:
                    print(">>> Pulse Ctrl+C y ejecute de nuevo la consola.")
                    continue
                port.write(data)
                port.flush()
                data = b""
                print(">>> Linea enviada. Espere '[PROVISION] Credentials stored in ESP32 NVS.'")
            time.sleep(0.1)
    except KeyboardInterrupt:
        pass
    finally:
        stop.set()
        port.close()
        print("\nPuerto cerrado.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
