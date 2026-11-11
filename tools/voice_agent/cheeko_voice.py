# /// script
# requires-python = ">=3.12,<3.14"
# dependencies = ["pyserial>=3.5", "mlx-whisper>=0.4", "numpy"]
# ///
"""Cheeko voice agent bridge: device mic (USB serial) -> Whisper -> OpenCode Go -> say -> device speaker.

  uv run tools/voice_agent/cheeko_voice.py                  # talk to the device (hold the screen)
  uv run tools/voice_agent/cheeko_voice.py --text "hi"      # no device: one turn, played on the Mac
  uv run tools/voice_agent/cheeko_voice.py --wav q.wav      # no device: transcribe a file, then reply

Needs OPENCODE_GO_API_KEY in the environment (see README.md).
"""
from __future__ import annotations

import argparse
import atexit
import base64
import glob
import json
import os
import queue
import re
import secrets
import signal
import socket
import subprocess
import sys
import tempfile
import threading
import time
import wave
from pathlib import Path

HERE = Path(__file__).resolve().parent
RATE = 16000

# Benchmarked 2026-11-11 (README.md#model-choice). Free Go models first; the
# rest are cheap fallbacks used only when the one before fails.
VOICE_MODELS = ["beam-free", "longcat-2.5-free", "gpt-6-luna"]
RESEARCH_MODELS = ["beam-free", "longcat-2.5-free", "glm-5.3-flash"]
WHISPER_MODEL = "mlx-community/whisper-large-v3-turbo"
SESSION_IDLE_S = 300  # forget the conversation after 5 quiet minutes


_LOGFILE = None


def log(msg: str) -> None:
    line = f"[{time.strftime('%H:%M:%S')}] {msg}"
    print(line, flush=True)
    if _LOGFILE:
        _LOGFILE.write(line + "\n")
        _LOGFILE.flush()


# --------------------------------------------------------------------------- OpenCode
class OpenCode:
    """A private `opencode serve` (isolated data dir, env API key) plus `opencode run --server` calls."""

    def __init__(self, port: int = 0):
        if not os.environ.get("OPENCODE_GO_API_KEY"):
            sys.exit("OPENCODE_GO_API_KEY is not set in this shell.\n"
                     "  export OPENCODE_GO_API_KEY=...   (OpenCode Go key from opencode.ai/console)")
        if not port:  # pick a free port so a stale server can't block startup
            with socket.socket() as sock:
                sock.bind(("127.0.0.1", 0))
                port = sock.getsockname()[1]
        self.url = f"http://127.0.0.1:{port}"
        data = HERE / ".ocdata"
        data.mkdir(exist_ok=True)
        # Isolated XDG_DATA_HOME: a stale key in ~/.local/share/opencode/auth.json
        # would otherwise win over opencode.json's {env:OPENCODE_GO_API_KEY}.
        # PWD too: opencode resolves its project (opencode.json, agents) from
        # $PWD, which subprocess cwd= does not update.
        self.env = {**os.environ, "PWD": str(HERE), "XDG_DATA_HOME": str(data),
                    "OPENCODE_SERVER_PASSWORD": secrets.token_urlsafe(24)}
        self.proc = subprocess.Popen(
            ["opencode", "serve", "--hostname", "127.0.0.1", "--port", str(port)],
            cwd=HERE, env=self.env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        atexit.register(self.close)  # never leave an orphaned server behind
        startup = []
        deadline = time.time() + 30
        while time.time() < deadline:
            line = self.proc.stdout.readline()
            startup.append(line)
            if "listening" in line:
                break
            if not line and self.proc.poll() is not None:
                sys.exit("opencode serve exited during startup:\n" + "".join(startup[-10:]))
        threading.Thread(target=lambda: [None for _ in self.proc.stdout], daemon=True).start()
        self.voice_session: str | None = None
        self.last_turn = 0.0
        log(f"opencode server up at {self.url}")

    def close(self) -> None:
        if self.proc.poll() is None:
            self.proc.terminate()
            try:
                self.proc.wait(5)
            except subprocess.TimeoutExpired:
                self.proc.kill()

    def run(self, agent: str, models: list[str], message: str, session: str | None = None,
            timeout: float = 90) -> tuple[str, str | None, str]:
        """Return (reply, session_id, model). Falls through `models` on errors."""
        last_err = ""
        for model in models:
            cmd = ["opencode", "run", "--server", self.url, "--agent", agent,
                   "-m", f"opencode-go/{model}", "--format", "json"]
            if session:
                cmd += ["--session", session]
            t0 = time.time()
            try:
                out = subprocess.run(cmd + [message], cwd=HERE, env=self.env, capture_output=True, stdin=subprocess.DEVNULL,
                                     text=True, timeout=timeout).stdout
            except subprocess.TimeoutExpired:
                last_err = f"{model}: timeout"
                continue
            text, sid, err, tools = [], None, None, []
            for line in out.splitlines():
                try:
                    ev = json.loads(line)
                except ValueError:
                    continue
                sid = ev.get("sessionID", sid)
                if ev.get("type") == "text":
                    text.append(ev["part"].get("text", ""))
                elif ev.get("type") in ("tool_use", "tool"):
                    tools.append(ev.get("part", {}).get("tool"))
                elif ev.get("type") == "error":
                    err = (ev.get("error") or {}).get("message", "error")
            reply = "".join(text).strip()
            if reply and not err:
                log(f"{agent} via {model} in {time.time() - t0:.1f}s" + (f" tools={tools}" if tools else ""))
                return reply, sid, model
            last_err = f"{model}: {err or 'empty reply'}"
            log(f"{agent} fallback: {last_err[:140]}")
            session = None  # a session is tied to its first model's context; start fresh
        raise RuntimeError(last_err)

    def answer(self, question: str, on_research=None) -> str:
        """Voice agent first; hand off to the research agent when it asks for the web."""
        if time.time() - self.last_turn > SESSION_IDLE_S:
            self.voice_session = None
        self.last_turn = time.time()
        reply, sid, _ = self.run("cheeko-voice", VOICE_MODELS, question, self.voice_session, timeout=40)
        self.voice_session = sid or self.voice_session
        m = re.match(r"\s*RESEARCH:\s*(.+)", reply, re.S)
        if not m:
            return reply
        query = m.group(1).strip().splitlines()[0]
        log(f"research: {query}")
        if on_research:
            on_research(query)
        today = time.strftime("%A, %B %-d, %Y")
        reply, _, _ = self.run("cheeko-research", RESEARCH_MODELS,
                               f"Today is {today}. Spoken question: {question}\nSuggested search: {query}",
                               timeout=90)
        return reply


# --------------------------------------------------------------------------- speech
class Ears:
    def __init__(self):
        import mlx_whisper  # noqa: F401  (slow import; do it once)
        import numpy as np
        self.np, self.whisper = np, mlx_whisper
        log("loading whisper (first run downloads ~1.6 GB)...")
        self.transcribe(np.zeros(RATE // 2, dtype=np.int16).tobytes())
        log("whisper ready")

    def transcribe(self, pcm: bytes) -> str:
        audio = self.np.frombuffer(pcm, dtype=self.np.int16).astype(self.np.float32) / 32768.0
        r = self.whisper.transcribe(audio, path_or_hf_repo=WHISPER_MODEL, language="en",
                                    initial_prompt="Cheeko the duck.", condition_on_previous_text=False)
        text = r.get("text", "").strip()
        # Whisper hallucinates these on silence / tones.
        if re.fullmatch(r"(?i)[\s.]*(thank you\.?|thanks for watching!?|you|cheeko the duck\.?)?[\s.]*", text):
            return ""
        return text


def clean_for_speech(text: str) -> str:
    text = re.sub(r"https?://\S+", "", text)
    text = re.sub(r"[*_`#>|\[\]]", "", text)
    text = re.sub(r"\s+", " ", text)
    return text.strip()


def sentences(text: str) -> list[str]:
    parts = re.split(r"(?<=[.!?])\s+", text)
    return [p for p in (s.strip() for s in parts) if p]


def tts(text: str, voice: str | None, rate: int) -> bytes:
    """macOS `say` straight to 16 kHz s16le WAV; returns raw PCM."""
    with tempfile.NamedTemporaryFile(suffix=".wav") as f:
        cmd = ["say", "-r", str(rate), "-o", f.name, "--data-format=LEI16@16000"]
        if voice:
            cmd += ["-v", voice]
        subprocess.run(cmd + [text], check=True)
        with wave.open(f.name) as w:
            return w.readframes(w.getnframes())


# --------------------------------------------------------------------------- device link
class Device:
    def __init__(self, port: str, logfile: Path):
        self.port = port
        self.ser = self._open()
        self.logf = open(logfile, "a", buffering=1)
        self.wlock = threading.Lock()
        self.events: queue.Queue[tuple[str, bytes | str]] = queue.Queue()
        self.hello = threading.Event()
        threading.Thread(target=self._reader, daemon=True).start()

    def _open(self):
        import serial
        return serial.Serial(self.port, 115200, timeout=0.2, write_timeout=5)

    def _reopen(self) -> None:
        """USB vanished (power-cycle/reset): wait for it and reopen."""
        with self.wlock:
            try:
                self.ser.close()
            except Exception:
                pass
            self.hello.clear()
            log("device disconnected; waiting for it to come back...")
            while True:
                time.sleep(1.0)
                self.port = find_port()
                try:
                    self.ser = self._open()
                    log(f"reopened {self.port}")
                    return
                except Exception:
                    continue

    def send(self, line: str) -> None:
        with self.wlock:
            self.ser.write((line + "\n").encode())

    def _reader(self) -> None:
        buf = b""
        while True:
            try:
                chunk = self.ser.read(8192)
            except Exception as e:  # USB re-enumeration after reset
                log(f"serial read error: {e}")
                self._reopen()
                buf = b""
                continue
            buf += chunk
            while b"\n" in buf:
                raw, buf = buf.split(b"\n", 1)
                line = raw.rstrip(b"\r")
                if line.startswith(b"@@MIC "):
                    try:
                        self.events.put(("mic", base64.b64decode(line[6:])))
                    except ValueError:
                        pass
                elif line.startswith(b"@@BEGIN"):
                    self.events.put(("begin", b""))
                elif line.startswith(b"@@END"):
                    self.events.put(("end", b""))
                elif line.startswith(b"@@HELLO"):
                    if not self.hello.is_set():
                        log(f"device linked ({line[8:].decode(errors='replace')})")
                    self.hello.set()
                else:
                    self.logf.write(line.decode("utf-8", "replace") + "\n")

    def attach(self) -> None:
        # Heartbeat forever: the device treats the link as down 6 s after the last @@HI.
        def beat():
            while True:
                try:
                    self.send("@@HI")
                except Exception:
                    pass  # reader thread handles reconnects
                time.sleep(2.0)
        threading.Thread(target=beat, daemon=True).start()
        while not self.hello.wait(1.0):
            pass
        # Terminate any partial line the device buffered while booting, so the
        # first real command isn't glued onto it ("bad PCM base64").
        self.send("")

    def state(self, s: str) -> None:
        self.send(f"@@STATE {s}")

    def text(self, s: str) -> None:
        self.send("@@TEXT " + s.replace("\n", " ")[:900])

    LEAD_S = 0.8  # how far ahead of playback we may send

    def play(self, pcm: bytes, cancel: threading.Event, clock: list[float]) -> float:
        """Stream PCM paced to real time; returns its duration.

        The device's USB RX ring buffer drops bytes when full (seen as "bad PCM
        base64"), so never run more than LEAD_S ahead of playback. `clock` is
        [playback start time, seconds already queued] shared across sentences.
        """
        step = 2048  # 1024 samples (64 ms) per line
        for i in range(0, len(pcm), step):
            if cancel.is_set():
                break
            if clock[0] == 0.0 or time.time() > clock[0] + clock[1]:
                clock[0], clock[1] = time.time(), 0.0  # speaker went idle: restart clock
            ahead = clock[0] + clock[1] - time.time()
            if ahead > self.LEAD_S:
                time.sleep(ahead - self.LEAD_S)
            chunk = pcm[i:i + step]
            self.send("@@PCM " + base64.b64encode(chunk).decode())
            clock[1] += len(chunk) / 2 / RATE
        self.send("@@PCMEND")
        return len(pcm) / 2 / RATE


def describe_audio(pcm: bytes) -> str:
    import array
    a = array.array("h", pcm[: len(pcm) // 2 * 2])
    if not a:
        return "0.0s audio"
    peak = max(abs(x) for x in a)
    rms = (sum(x * x for x in a) / len(a)) ** 0.5
    zeros = sum(1 for x in a if x == 0) / len(a)
    return f"{len(a) / RATE:.1f}s audio, peak {peak}, rms {rms:.0f}, zeros {zeros:.0%}"


def save_turn(pcm: bytes) -> Path:
    """Keep the last 10 turns' mic audio for debugging (.ocdata/turns/)."""
    d = HERE / ".ocdata" / "turns"
    d.mkdir(parents=True, exist_ok=True)
    path = d / time.strftime("%H%M%S.wav")
    with wave.open(str(path), "wb") as w:
        w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(pcm)
    for old in sorted(d.glob("*.wav"))[:-10]:
        old.unlink()
    return path


def find_port() -> str:
    """Wait for the Cheeko's USB port; it vanishes briefly while power-cycling."""
    waiting = False
    while True:
        ports = sorted(glob.glob("/dev/cu.usbmodem*"))
        if ports:
            if waiting:
                time.sleep(1.5)  # let it finish enumerating/booting
            return ports[0]
        if not waiting:
            log("waiting for the Cheeko on USB (/dev/cu.usbmodem*)...")
            waiting = True
        time.sleep(0.5)


class Bridge:
    def __init__(self, args):
        self.args = args
        self.oc = OpenCode(args.opencode_port)
        self._ears: Ears | None = None
        self.speak_lock = threading.Lock()
        self.dev: Device | None = None
        self.cancel = threading.Event()
        self.busy = threading.Lock()

    @property
    def ears(self) -> Ears:
        if self._ears is None:
            self._ears = Ears()
        return self._ears

    def speak(self, text: str) -> None:
        with self.speak_lock:
            self._speak(clean_for_speech(text))

    def _speak(self, text: str) -> None:
        if self.dev is None:
            pcm = b"".join(tts(s, self.args.voice, self.args.rate) for s in sentences(text))
            with tempfile.NamedTemporaryFile(suffix=".wav") as f:
                with wave.open(f.name, "wb") as w:
                    w.setnchannels(1); w.setsampwidth(2); w.setframerate(RATE); w.writeframes(pcm)
                subprocess.run(["afplay", f.name])
            return
        self.dev.state("speaking")
        clock = [0.0, 0.0]
        for s in sentences(text):  # synth per sentence so playback starts sooner
            if self.cancel.is_set():
                break
            self.dev.play(tts(s, self.args.voice, self.args.rate), self.cancel, clock)
        # Lines are sent up to LEAD_S early; wait out the speaker before going idle.
        remaining = clock[0] + clock[1] + 0.4 - time.time()
        if remaining > 0:
            self.cancel.wait(remaining)

    def turn(self, question: str) -> None:
        t0 = time.time()
        log(f"you: {question}")
        dev = self.dev
        if dev:
            dev.text("you: " + question)
            dev.state("thinking")

        def researching(_query: str) -> None:
            if dev:
                dev.state("researching")
            threading.Thread(target=self.speak, args=("Ooh, let me look that up.",), daemon=True).start()

        try:
            reply = self.oc.answer(question, on_research=researching)
        except Exception as e:
            log(f"agent failed: {e}")
            if dev:
                dev.state("error")
            return
        log(f"cheeko ({time.time() - t0:.1f}s): {reply}")
        if dev:
            dev.text(reply)
        self.speak(reply)
        if dev:
            dev.state("idle")

    def handle_audio(self, pcm: bytes) -> None:
        secs = len(pcm) / 2 / RATE
        if secs < 0.35:
            self.dev.state("idle")
            return
        self.dev.state("thinking")
        t0 = time.time()
        text = self.ears.transcribe(pcm)
        log(f"stt {secs:.1f}s audio in {time.time() - t0:.2f}s")
        if not text:
            self.dev.text("(didn't catch that)")
            self.speak("Sorry, I didn't catch that.")
            self.dev.state("idle")
            return
        self.turn(text)

    def mic_test(self, seconds: float = 4.0) -> None:
        """Record from the device mic while the Mac speaks a phrase out loud."""
        port = self.args.port or find_port()
        self.dev = Device(port, HERE / ".ocdata" / "device.log")
        self.dev.attach()
        phrase = "Hey Cheeko, tell me a duck joke."
        self.dev.send(f"@@REC {int(seconds * 1000)}")
        time.sleep(0.5)
        subprocess.run(["say", phrase])  # out loud through the Mac speakers
        pcm = bytearray()
        deadline = time.time() + seconds + 3
        while time.time() < deadline:
            try:
                kind, data = self.dev.events.get(timeout=0.5)
            except queue.Empty:
                continue
            if kind == "mic":
                pcm += data
            elif kind == "end":
                break
        audio = bytes(pcm)
        log(f"mic test: {describe_audio(audio)} -> {save_turn(audio)}")
        log(f"mic test transcript: {self.ears.transcribe(audio)!r} (played: {phrase!r})")

    def run_device(self) -> None:
        port = self.args.port or find_port()
        log(f"opening {port} (device may reboot)")
        self.dev = Device(port, HERE / ".ocdata" / "device.log")
        self.ears  # load whisper before the first turn
        self.dev.attach()
        log("ready: hold the screen and talk, release to send")
        pcm = bytearray()
        while True:
            kind, data = self.dev.events.get()
            if kind == "begin":
                log("turn: listening")
                self.cancel.set()  # barge-in: stop any reply still streaming
                pcm.clear()
            elif kind == "mic":
                pcm += data
            elif kind == "end":
                audio = bytes(pcm)
                pcm.clear()
                log(f"turn: released, {describe_audio(audio)} -> {save_turn(audio).name}")
                if not self.busy.acquire(blocking=False):
                    log("turn: still busy with the previous one, dropped")
                    self.dev.state("idle")
                    continue
                self.cancel.clear()

                def work(a=audio):
                    try:
                        self.handle_audio(a)
                    except Exception as e:
                        log(f"turn failed: {e!r}")
                        self.dev.state("error")
                    finally:
                        self.busy.release()
                threading.Thread(target=work, daemon=True).start()


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--port", help="serial port (default: first /dev/cu.usbmodem*)")
    ap.add_argument("--text", help="one text turn without the device")
    ap.add_argument("--wav", help="one audio turn from a WAV file without the device")
    ap.add_argument("--voice", default=os.environ.get("CHEEKO_VOICE"), help="macOS say voice (say -v '?')")
    ap.add_argument("--rate", type=int, default=195, help="speech rate, words per minute")
    ap.add_argument("--say", help="speak this text through the device speaker (playback test)")
    ap.add_argument("--mic-test", action="store_true", help="record the device mic while the Mac speaks")
    ap.add_argument("--opencode-port", type=int, default=0, help="default: any free port")
    args = ap.parse_args()
    # SIGTERM/SIGHUP (kill, closed terminal) -> normal exit so atexit stops opencode serve.
    for sig in (signal.SIGTERM, signal.SIGHUP):
        signal.signal(sig, lambda *_: sys.exit(0))
    global _LOGFILE
    (HERE / ".ocdata").mkdir(exist_ok=True)
    _LOGFILE = open(HERE / ".ocdata" / "bridge.log", "a")
    log(f"--- cheeko_voice start (pid {os.getpid()})")
    b = Bridge(args)
    try:
        if args.say:
            b.dev = Device(args.port or find_port(), HERE / ".ocdata" / "device.log")
            b.dev.attach()
            t0 = time.time()
            b.speak(args.say)
            b.dev.state("idle")
            log(f"say: done in {time.time() - t0:.1f}s")
            time.sleep(0.5)
        elif args.mic_test:
            b.mic_test()
        elif args.text or args.wav:
            q = args.text
            if args.wav:
                with wave.open(args.wav) as w:
                    assert w.getframerate() == RATE and w.getnchannels() == 1, "need 16 kHz mono WAV"
                    q = b.ears.transcribe(w.readframes(w.getnframes()))
            b.turn(q)
        else:
            b.run_device()
    except KeyboardInterrupt:
        pass
    finally:
        b.oc.close()


if __name__ == "__main__":
    main()
