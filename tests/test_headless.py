"""Headless checks: DSP on synthetic I/Q, then protocol + simulator end to end.

Run from the project root:  python3 tests/test_headless.py
"""
import os
import socket
import subprocess
import sys
import time

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

from rigface import dsp, protocol, qmx, sim  # noqa: E402


def tone_peak(audio):
    spec = np.abs(np.fft.rfft(audio * np.hanning(len(audio))))
    return np.argmax(spec) * dsp.FS / len(audio), 20 * np.log10(np.sqrt(np.mean(audio ** 2)) + 1e-12)


def run_rx(mode, lo, hi, offset, rate=192000, blocks=30, agc="OFF"):
    rx = dsp.Receiver()
    rx.agc = agc
    rx.volume = 1.0
    rx.set_mode(mode, lo, hi)
    n = 1008 * rate // 48000
    out = []
    for b in range(blocks):
        t = (b * n + np.arange(n)) / rate
        iq = (1e-3 * np.exp(2j * np.pi * offset * t)).astype(np.complex64)
        out.append(rx.process(rate, dsp.unpack_iq(sim.pack24(iq))))
    return np.concatenate(out[10:]), rx


def test_dsp():
    for rate in (48000, 192000, 384000):
        a, rx = run_rx("USB", 100, 3000, 1000, rate)
        f, lvl = tone_peak(a)
        assert abs(f - 1000) < 15, (rate, f)
        assert abs(rx.level_dbfs + 60) < 1.0, rx.level_dbfs
        a2, _ = run_rx("LSB", -3000, -100, 1000, rate)
        rej = lvl - tone_peak(a2)[1]
        assert rej > 60, (rate, rej)
    a, _ = run_rx("LSB", -3000, -100, -1500)
    assert abs(tone_peak(a)[0] - 1500) < 15
    a, _ = run_rx("CW", -250, 250, 100)
    assert abs(tone_peak(a)[0] - (dsp.CW_PITCH + 100)) < 15
    a, _ = run_rx("USB", 100, 3000, 1000, agc="MID")
    assert 0.2 < np.max(np.abs(a)) < 0.3, np.max(np.abs(a))
    # spectrum: tone at +10 kHz lands in the right bin at -60 dBFS
    _, rx = run_rx("USB", 100, 3000, 10000)
    p = rx.spectrum()
    k = np.argmax(p)
    assert abs((k - dsp.NFFT // 2) * 192000 / dsp.NFFT - 10000) < 100
    assert abs(10 * np.log10(p[k]) + 60) < 1.5, 10 * np.log10(p[k])
    print("dsp ok")


def test_tx():
    t = np.arange(48000) / 48000
    mic = 0.1 * np.sin(2 * np.pi * 1000 * t).astype(np.float32)
    for mode, want in (("USB", 1000), ("LSB", -1000)):
        tx = dsp.Transmitter()
        tx.set_mode(mode)
        raw = b"".join(tx.process(mic[i:i + 480]) for i in range(0, 48000, 480))
        s = np.frombuffer(raw, ">i2").reshape(-1, 4)
        iq = (s[:, 2] + 1j * s[:, 3])[24000:] / 32768
        assert np.max(np.abs(iq)) <= 0.91
        sp = np.abs(np.fft.fft(iq * np.hanning(len(iq))))
        fr = np.fft.fftfreq(len(iq), 1 / 48000)
        assert abs(fr[np.argmax(sp)] - want) < 5, (mode, fr[np.argmax(sp)])
        image = sp[np.argmin(np.abs(fr + want))]
        assert 20 * np.log10(sp.max() / image) > 60
    print("tx ok")


def test_end_to_end():
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    env = dict(os.environ, PYTHONPATH=ROOT)
    proc = subprocess.Popen([sys.executable, "-m", "rigface.sim", "--host", "127.0.0.1",
                             "--port", str(port)], env=env)
    try:
        time.sleep(0.8)
        found = protocol.discover(["127.0.0.1"], port)
        assert found and found[0]["board"] == 6, found

        # find a CW station in the simulated 40 m band and tune 700 Hz below it
        kind, freq, _ = next(x for x in sim.band_signals(7.0e6, 7.05e6) if x[0] == "cw")
        rx = dsp.Receiver()
        rx.set_mode("USB", 100, 3000)
        count = [0, 0]

        def on_iq(rate, iq):
            count[0] += len(iq)
            count[1] += 1
            rx.process(rate, iq)

        r = protocol.Radio()
        r.rate = 96000
        r.rx_freq = r.tx_freq = freq - 20000
        r.on_iq = on_iq
        r.open("127.0.0.1", port)
        time.sleep(2.0)
        assert r.streaming
        got = count[0] / 2.0
        assert 0.85 * 96000 < got < 1.1 * 96000, got
        assert r.lost == 0, r.lost

        r.set_rate(192000)
        time.sleep(1.0)
        count[0] = 0
        time.sleep(1.5)
        assert 0.85 * 192000 < count[0] / 1.5 < 1.1 * 192000, count[0] / 1.5

        # transmit a carrier: the simulator loops it back at -20 dBFS on centre
        r.tx_carrier = True
        r.mox = True
        time.sleep(0.6)
        p = rx.spectrum()
        k = int(np.argmax(p))
        db = 10 * np.log10(p[k])
        assert abs(k - dsp.NFFT // 2) <= 1 and abs(db + 21) < 2, (k, db)

        # transmit SSB audio through the mic path
        r.tx_carrier = False
        tx = dsp.Transmitter()
        t = np.arange(480) / 48000
        for i in range(60):
            r.tx_push(tx.process(0.2 * np.sin(2 * np.pi * 1500 * (t + i * 0.01)).astype(np.float32)))
            time.sleep(0.01)
        p = rx.spectrum()
        f = (int(np.argmax(p)) - dsp.NFFT // 2) * 192000 / dsp.NFFT
        assert abs(f - 1500) < 100, f
        r.mox = False
        r.close()
        print("end-to-end ok (%d blocks)" % count[1])
    finally:
        proc.terminate()


def test_offset():
    """A radio with an IF: the wanted signal is `offset` Hz off the I/Q centre."""
    rx = dsp.Receiver()
    rx.agc, rx.volume, rx.offset = "OFF", 1.0, 12000.0
    rx.set_mode("USB", 100, 3000)
    out = []
    for b in range(30):
        t = (b * 1024 + np.arange(1024)) / 48000
        out.append(rx.process(48000, (1e-3 * np.exp(2j * np.pi * 13000 * t)).astype(np.complex64)))
    assert abs(tone_peak(np.concatenate(out[10:]))[0] - 1000) < 15
    assert abs(rx.level_dbfs + 60) < 1.0, rx.level_dbfs
    k = int(np.argmax(rx.spectrum()))             # the scope still shows the raw I/Q
    assert abs((k - dsp.NFFT // 2) * 48000 / dsp.NFFT - 13000) < 30
    print("offset ok")


def wait_for(cond, timeout=4.0):
    end = time.monotonic() + timeout
    while time.monotonic() < end and not cond():
        time.sleep(0.05)
    return cond()


def test_qmx():
    """CAT + I/Q against the simulated QMX (pty and UDP instead of USB)."""
    assert app_parse("qmx") == ("qmx", None) and app_parse("10.0.0.5") == ("hpsdr", "10.0.0.5")
    assert app_parse("qmx:/dev/ttyACM1") == app_parse("/dev/ttyACM1") == ("qmx", "/dev/ttyACM1")
    s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    s.bind(("127.0.0.1", 0))
    port = s.getsockname()[1]
    s.close()
    proc = subprocess.Popen([sys.executable, "-m", "rigface.sim", "--qmx", "--iq-port", str(port)],
                            env=dict(os.environ, PYTHONPATH=ROOT), stdout=subprocess.PIPE, text=True)
    r = qmx.Radio()
    try:
        tty = proc.stdout.readline().strip()
        kind, freq, _ = next(x for x in sim.band_signals(7.0e6, 7.05e6) if x[0] == "cw")
        rx = dsp.Receiver()
        rx.agc, rx.volume = "OFF", 1.0
        rx.set_mode("USB", 100, 3000)
        blocks, panel = [], []

        def on_iq(rate, iq):
            rx.offset = r.if_offset
            blocks.append(len(iq))
            rx.process(rate, iq)

        r.rx_freq, r.mode, r.on_iq = 7_100_000, "USB", on_iq
        r.on_panel = lambda f, m: panel.append((f, m))
        r.open(tty, audio_out="fakesink", audio_in=(
            "udpsrc address=127.0.0.1 port=%d caps=audio/x-raw,format=F32LE,rate=48000,"
            "channels=2,layout=interleaved" % port))
        assert wait_for(lambda: r.streaming), r.error
        assert r.info == {"name": "QMX", "fw": "1.04.003"}, r.info
        assert r.if_offset == 12000 and set(blocks) == {qmx.BLOCK}
        n0, t0 = sum(blocks), time.monotonic()
        time.sleep(1.5)
        got = (sum(blocks) - n0) / (time.monotonic() - t0)
        assert 0.9 * 48000 < got < 1.1 * 48000, got

        # CW moves the radio's LO a further 700 Hz; we must hear of it
        r.mode = "CW"
        assert wait_for(lambda: r.if_offset == 12700), r.if_offset
        assert not panel, panel                       # our own changes are not "the knob"

        # TUNE from a voice mode: carrier exactly on the dial, power and SWR read back
        r.mode = "USB"
        assert wait_for(lambda: r.if_offset == 12000)
        r.tx_carrier = r.mox = True
        assert wait_for(lambda: r.fwd_power == 4.5 and r.swr == 1.21), (r.fwd_power, r.swr)
        time.sleep(0.4)
        p = rx.spectrum()
        k = int(np.argmax(p))
        f = (k - dsp.NFFT // 2) * 48000 / dsp.NFFT
        assert abs(f - 12700) < 30 and abs(10 * np.log10(p[k]) + 20) < 2, (f, 10 * np.log10(p[k]))
        r.mox = False
        assert wait_for(lambda: r.fwd_power is None and r.if_offset == 12000)
        assert not panel, panel                       # borrowed CW mode was put back quietly

        # somebody turns the knob on the radio itself
        fd = os.open(tty, os.O_RDWR | os.O_NOCTTY)
        os.write(fd, b"FA7030000;MD1;")
        os.close(fd)
        assert wait_for(lambda: panel), panel
        assert panel == [(7_030_000, "LSB")] and r.rx_freq == 7_030_000 and r.mode == "LSB", panel

        r.close()
        assert not r.streaming
        print("qmx ok (%d blocks)" % len(blocks))
    finally:
        r.close()
        proc.terminate()


if __name__ == "__main__":
    from rigface.app import parse_radio as app_parse
    test_dsp()
    test_tx()
    test_offset()
    test_end_to_end()
    test_qmx()
