"""MVP-IOT-F7 - Consola serie y aprovisionamiento del gateway AGROTECH-T.

Uso (PowerShell, con pyserial instalado):
    python tools/gateway_serial_console.py COM7
    python tools/gateway_serial_console.py COM7 --uid GW-7H8D6RHUXH
    python tools/gateway_serial_console.py COM7 --reprovision

- Muestra en vivo el log del ESP32 (115200 baudios).
- Cuando el gateway pide credenciales ("[PROVISION] No valid gateway
  credentials"), solicita token Flespi y clave Wi-Fi SIN eco y envia la linea
  SET separada por TAB que espera gateway/GatewayCredentials.cpp:
      SET\t<uid>\t<token64>\t<password_vacia>\t<ssid>\t<wifi_pass>\n
- Despues funciona como consola: escriba DIAG STATUS, DIAG PUBLISH o DIAG HELP
  y pulse Enter. Ctrl+C para salir.
- --reprovision envia REPROVISION en la ventana de 2 s del arranque para
  borrar las credenciales guardadas y volver a pedirlas.

No guarda ni imprime secretos. Requiere un terminal interactivo.
"""
from __future__ import annotations

import argparse
import getpass
import re
import sys
import threading
import time

try:
    import serial  # pyserial
except ImportError:  # pragma: no cover
    sys.exit("Falta pyserial. Instalelo con:  python -m pip install pyserial")

UID_RE = re.compile(r"^GW-[ABCDEFGHJKMNPQRSTUVWXYZ23456789]{10}$")
TOKEN_RE = re.compile(r"^[A-Za-z0-9_-]{64}$")
PROMPT_MARKER = "[PROVISION] No valid gateway credentials"
REJECT_MARKER = "[PROVISION] Rejected"
STORED_MARKER = "[PROVISION] Gateway credentials stored"
PROVISION_WINDOW_S = 12.0
BAUD = 115200


def _bad_chars(value: str) -> bool:
    return any(c in value for c in ("\t", "\n", "\r", "\0"))


def ask_credentials(uid: str) -> bytes | None:
    print("\n>>> El gateway pide credenciales. Nada de lo que escriba se mostrara ni guardara.")
    token = getpass.getpass(">>> Token Flespi AGROTECH-T-GATEWAY-STAGING (64 caracteres): ").strip()
    ssid = input(">>> Nombre de la red Wi-Fi (2.4 GHz): ").strip("\r\n")
    wifi = getpass.getpass(">>> Clave de la red Wi-Fi (vacia si es abierta): ").rstrip("\r\n")
    errors = []
    if not TOKEN_RE.fullmatch(token):
        errors.append(f"el token debe tener 64 caracteres A-Z a-z 0-9 _ - (tiene {len(token)})")
    if not 1 <= len(ssid.encode("utf-8")) <= 32 or _bad_chars(ssid):
        errors.append("el SSID debe tener entre 1 y 32 bytes y sin tabuladores")
    if len(wifi.encode("utf-8")) > 64 or _bad_chars(wifi):
        errors.append("la clave Wi-Fi debe tener maximo 64 bytes y sin tabuladores")
    if errors:
        for e in errors:
            print(f">>> ERROR: {e}")
        print(">>> No se envio nada.")
        token = wifi = ""
        return None
    line = "\t".join(("SET", uid, token, "", ssid, wifi)) + "\n"
    token = wifi = ""
    return line.encode("utf-8")


def main() -> int:
    parser = argparse.ArgumentParser(description="Consola serie del gateway AGROTECH-T")
    parser.add_argument("port", help="Puerto serie, por ejemplo COM7")
    parser.add_argument("--uid", default="GW-7H8D6RHUXH", help="UID del gateway (GW-...)")
    parser.add_argument("--reprovision", action="store_true",
                        help="Borra las credenciales guardadas al arrancar y las vuelve a pedir")
    args = parser.parse_args()

    if not UID_RE.fullmatch(args.uid):
        print("ERROR: UID invalido; debe ser GW- seguido de 10 caracteres validos.")
        return 2

    try:
        port = serial.Serial(args.port, BAUD, timeout=0.2)
    except serial.SerialException as exc:
        print(f"ERROR: no se pudo abrir {args.port}: {exc}")
        print("Cierre el Monitor Serie de Arduino IDE y revise el numero de puerto.")
        return 2

    # Reinicio limpio del ESP32 (EN) para ver el arranque desde el principio.
    port.dtr = False
    port.rts = True
    time.sleep(0.1)
    port.rts = False
    print(f"Conectado a {args.port} a {BAUD}. Ctrl+C para salir.\n")

    if args.reprovision:
        time.sleep(1.0)
        port.write(b"REPROVISION\n")
        print(">>> REPROVISION enviado.")

    lock = threading.Lock()
    stop = threading.Event()
    pending_prompt = threading.Event()
    stored = threading.Event()

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
                with lock:
                    print(text)
                if PROMPT_MARKER in text or REJECT_MARKER in text:
                    pending_prompt.set()
                if STORED_MARKER in text:
                    stored.set()

    t = threading.Thread(target=reader, daemon=True)
    t.start()

    # Fase 1: aprovisionamiento. No se lee el teclado con input() aqui, para que
    # el token nunca compita con otra lectura ni aparezca en pantalla.
    deadline = time.monotonic() + PROVISION_WINDOW_S
    try:
        while not stop.is_set() and not stored.is_set():
            if pending_prompt.is_set():
                time.sleep(1.0)  # deja que el ESP32 termine de imprimir el aviso
                pending_prompt.clear()
                line = ask_credentials(args.uid)
                if line is None:
                    print(">>> Pulse Ctrl+C, reinicie el ESP32 y ejecute de nuevo la consola.")
                    break
                port.write(line)
                port.flush()
                line = b""
                print(">>> Credenciales enviadas. Esperando confirmacion del gateway...")
                deadline = time.monotonic() + PROVISION_WINDOW_S
                continue
            if time.monotonic() > deadline:
                break  # ya tenia credenciales validas: no hubo aviso
            time.sleep(0.1)

        # Fase 2: consola de comandos DIAG.
        print("\n>>> Consola lista. Escriba DIAG STATUS, DIAG PUBLISH o DIAG HELP y pulse Enter.")
        while not stop.is_set():
            cmd = input().strip()
            if not cmd:
                continue
            if not cmd.upper().startswith("DIAG "):
                print(">>> Solo se envian comandos DIAG (STATUS, PUBLISH, HELP).")
                continue
            port.write((cmd.upper() + "\n").encode("ascii", errors="ignore"))
    except (KeyboardInterrupt, EOFError):
        pass
    finally:
        stop.set()
        port.close()
        print("\nPuerto cerrado.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
