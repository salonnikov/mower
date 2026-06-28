#!/usr/bin/env python3
# Оркестратор: принимает соединения от mower-auto (data:7777, ctrl:7778),
# сам снимает дамп прошивки косилки (esptool через мост), затем переходит в сниф.
# Контейнер сам различает дамп/сниф: сперва ставит P=LOW и ловит загрузчик (дамп),
# не поймал — переходит в сниф (P release, пишет sniff.log).
import socket, os, pty, select, threading, subprocess, time, re

DATA_PORT = int(os.environ.get("DATA_PORT", "7777"))
CTRL_PORT = int(os.environ.get("CTRL_PORT", "7778"))
OUT = os.environ.get("OUT", "/out")
os.makedirs(OUT, exist_ok=True)

def log(*a): print("[orch]", *a, flush=True)

def listen(port):
    s = socket.socket(); s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    s.bind(("0.0.0.0", port)); s.listen(1); return s

ls_data, ls_ctrl = listen(DATA_PORT), listen(CTRL_PORT)
log(f"жду ESP: data:{DATA_PORT}, ctrl:{CTRL_PORT}")

def handle():
    log("жду data-соединение от ESP..."); data, _ = ls_data.accept(); log("data ESP подключился")
    log("жду ctrl-соединение от ESP..."); ctrl, _ = ls_ctrl.accept(); log("ctrl ESP подключился")
    mfd, sfd = pty.openpty(); slave = os.ttyname(sfd)
    stop = threading.Event(); sniff = threading.Event()
    sf = open(f"{OUT}/sniff.log", "ab", buffering=0)

    def bridge():
        data.setblocking(False)
        while not stop.is_set():
            try: r, _, _ = select.select([data, mfd], [], [], 0.2)
            except Exception: break
            if data in r:
                try: d = data.recv(4096)
                except Exception: d = b''
                if d == b'': break
                if d: os.write(mfd, d)
            if mfd in r:
                try: d = os.read(mfd, 4096)
                except OSError: d = b''
                if d:
                    try: data.sendall(d)
                    except Exception: break
                    if sniff.is_set():
                        sf.write(("%d %s\n" % (int(time.time()*1000), " ".join("%02X" % b for b in d))).encode())
        stop.set()
    threading.Thread(target=bridge, daemon=True).start()

    # --- фаза ДАМП ---
    ctrl.sendall(b'0'); log("P -> LOW (готов к дампу). >>> ПЕРЕДЁРНИ ПИТАНИЕ КОСИЛКИ <<<")
    ts = time.strftime("%Y%m%d-%H%M%S"); dump = f"{OUT}/mower-esp-{ts}.bin"; ok = False
    for i in range(40):
        if stop.is_set(): break
        log(f"esptool sync {i+1}/40 (если тихо — передёрни питание косилки)")
        r = subprocess.run(["esptool", "--before", "no_reset", "--after", "no_reset",
                            "--chip", "esp32", "--connect-attempts", "2", "--port", slave,
                            "read_flash", "0", "0x400000", dump], capture_output=True, text=True)
        if r.returncode == 0 and os.path.exists(dump) and os.path.getsize(dump) > 1_000_000:
            ok = True; log("*** ДАМП СНЯТ:", dump); break
        time.sleep(2)
    if ok:
        d = open(dump, "rb").read(); n = len(d); ff = d.count(0xff)*100//n
        st = len(re.findall(rb'[ -~]{6,}', d))
        v = "НОРМАЛЬНАЯ прошивка (есть протокол!)" if (st > 300 and ff < 90) else ("почти пусто" if ff > 95 else "ВОЗМОЖНО ЗАШИФРОВАНА")
        log(f"вердикт: {v}  (size={n} FF={ff}% strings={st})")
    else:
        log("дамп не снят — перехожу в СНИФ.")
    # --- фаза СНИФ ---
    ctrl.sendall(b'1'); log("P release. СНИФ -> sniff.log (для норм. работы косилки передёрни питание ещё раз)")
    sniff.set()
    while not stop.is_set(): time.sleep(1)
    log("ESP отключился.")
    for x in (data, ctrl, sf):
        try: x.close()
        except Exception: pass

while True:
    try: handle()
    except Exception as e: log("ошибка:", e); time.sleep(2)
