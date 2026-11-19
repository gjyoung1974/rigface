"""Drive the real window against the simulator and check it responds.

Run from the project root:  python3 tests/smoke_gui.py   (needs a display)
                            python3 tests/smoke_gui.py qmx   (simulated QMX)
"""
import os
import sys
import tempfile

os.environ["XDG_CONFIG_HOME"] = tempfile.mkdtemp()
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT)

import gi  # noqa: E402
gi.require_version("Gtk", "4.0")
gi.require_version("Gdk", "4.0")
from gi.repository import Gdk, Gio, GLib, Gtk  # noqa: E402

from rigface import app as rbapp  # noqa: E402

failures = []


def check(name, cond):
    print(("ok   " if cond else "FAIL ") + name, flush=True)
    if not cond:
        failures.append(name)


def key(p, name):
    return p._on_key(None, Gdk.keyval_from_name(name), 0, Gdk.ModifierType(0))


def script(p):
    yield 2500
    check("streaming from simulator", p.radio.streaming and p.scope.live)
    check("spectrum arriving", p.scope.db is not None)
    check("audio path running", p.rx.rate == 192000 and p.rx.level_dbfs > -130)

    p.select_band("20")
    check("band 20 m", p.vfo["freq"] == 14_200_000 and p.vfo["mode"] == "USB")
    p.set_mode("CW"); p.cycle_filter(); p.cycle_agc(); p.cycle_step()
    check("mode/filter/agc/step", p.rx.mode == "CW" and p.cfg["agc"] == "SLOW"
          and p.cfg["step"] == 1000 and p.scope.passband == (-125, 125))
    for ch in "14.074":
        key(p, "period" if ch == "." else ch)
    check("entry shown", p.freq.entry == "14.074")
    key(p, "Return")
    check("direct entry", p.vfo["freq"] == 14_074_000 and p.freq.entry is None)
    key(p, "Right"); key(p, "Up")
    check("arrow tuning", p.vfo["freq"] == 14_085_000)
    p._on_digit_tune(-10_000, 0)
    p._on_scope_tune(p.vfo["freq"] + 2_340, True)
    check("digit and click tuning", p.vfo["freq"] == 14_077_000)
    yield 300
    check("radio follows VFO", p.radio.rx_freq == 14_077_000 == p.radio.tx_freq)

    p.select_band("40"); p.select_band("20")
    check("band stack recall", p.vfo["freq"] == 14_077_000 and p.vfo["mode"] == "CW")
    p.copy_vfo(); p.swap_vfo(); p.set_mode("USB"); p.set_freq(14_200_000)
    check("VFO B", p.cfg["cur"] == 1 and p.cfg["vfo"][0]["freq"] == 14_077_000)

    p.cycle_span()
    yield 1500
    check("span change", p.radio.rate == 384000 and p.rx.rate == 384000 and p.scope.live)

    p.set_tx(True, tune=True)
    yield 800
    import numpy as np
    peak = float(np.max(p.scope.db))
    centre = int(np.argmax(p.scope.db))
    check("TUNE carrier on scope (%.0f dBFS, bin %d)" % (peak, centre),
          p.radio.mox and abs(centre - 512) <= 1 and -26 < peak < -17)
    check("TX indication", p.b_trx.get_label() == "TX" and p.muted)
    key(p, "Escape")
    yield 500
    check("back to receive", not p.radio.mox and not p.tx_on and not p.muted)
    p.win.close()


def script_qmx(p):
    import numpy as np
    yield 3000
    r = p.radio
    check("streaming from simulated QMX", r.kind == "qmx" and r.streaming and p.scope.live)
    check("48 kHz I/Q, 12 kHz IF", p.rate == 48000 == p.rx.rate and p.rx.offset == 12000
          and p.scope.mark == 12000 and p.scope.center == p.vfo["freq"] - 12000)
    check("status names the radio", "QMX" in p.status and "1.04.003" in p.status)
    check("panel fitted to the radio", not p.mode_keys["FM"].get_sensitive()
          and not p.sliders["power"].get_sensitive() and not p.k_span.get_sensitive()
          and p.band_keys["6"].get_sensitive() and p.mode_keys["CW"].get_sensitive())
    check("audio path running", p.rx.level_dbfs > -130)

    p.select_band("20")
    p._on_scope_tune(14_201_300, True)
    yield 600
    check("tuning reaches the radio", r.rx_freq == 14_201_300 == r._sent_freq)
    p.set_mode("CW")
    yield 900
    check("CW offset followed", p.rx.offset == 12700 and p.scope.mark == 12700 and r.mode == "CW")

    fd = os.open(r.label, os.O_RDWR | os.O_NOCTTY)     # the knob on the radio
    os.write(fd, b"FA14030000;MD2;")
    os.close(fd)
    yield 1500
    check("follows the radio's own knob", p.vfo["freq"] == 14_030_000 and p.vfo["mode"] == "USB"
          and p.freq.freq == 14_030_000 and p.rx.mode == "USB")

    p.set_tx(True, tune=True)
    yield 1200
    k = int(np.argmax(p.scope.db))
    f = p.scope.center + (k + 0.5 - 512) * 48000 / 1024
    check("TUNE carrier on the dial (%.0f Hz off, %.0f dBFS)" % (f - p.vfo["freq"], p.scope.db[k]),
          abs(f - p.vfo["freq"]) < 60 and -24 < p.scope.db[k] < -17)
    check("measured power on the meter", p.meter.po_text == "4.5 W   SWR 1.21" and p.meter.tx)
    key(p, "Escape")
    yield 900
    check("back to receive", not r.mox and not p.tx_on and r._tx is None and r.mode == "USB")
    p.set_tx(True)
    yield 700
    check("voice transmit keys the radio", r._tx == "voice" and r._out is not None)
    p.set_tx(False)
    yield 500

    s = p.qmx_settings()
    check("QMX settings defaults", s["port"] == "" and s["usb_tx_audio"] and not s["iq_swap"])
    s.update(iq_swap=True, smeter_cal=-30.0, mic_gain=6.5)
    p.apply_qmx_settings(s)
    check("QMX settings applied live", p.radio is r and p.rx.iq_swap and r.smeter_cal == -30.0
          and r.mic_gain == 6.5 and p.cfg["qmx_iq_swap"])
    s["iq_swap"] = False
    p.apply_qmx_settings(s)
    p.show_qmx_settings()
    check("QMX settings window opens", p.settings_win is not None)
    yield 1500
    check("still streaming with the window open", r.streaming)
    p.close_qmx_settings()
    check("QMX settings window closes", p.settings_win is None)

    p.start_demo()                                     # and back to an HPSDR radio
    yield 2500
    check("switch to HPSDR", p.radio.kind == "hpsdr" and p.radio.streaming and p.rx.offset == 0
          and p.scope.mark == 0 and p.mode_keys["FM"].get_sensitive()
          and not p.band_keys["6"].get_sensitive() and p.rate == 192000)
    p.win.close()


def main():
    qmx = sys.argv[1:] == ["qmx"]
    application = Gtk.Application(application_id="io.github.rigface.smoke",
                                  flags=Gio.ApplicationFlags.NON_UNIQUE)

    def activate(a):
        p = rbapp.Panel(a, rbapp.argparse.Namespace(
            radio=None, demo="qmx" if qmx else "hpsdr", rate=192000, audio_sink="fakesink"))
        p.win.present()
        steps = (script_qmx if qmx else script)(p)

        def advance():
            try:
                GLib.timeout_add(next(steps), advance)
            except StopIteration:
                pass
            except Exception as e:
                failures.append(repr(e))
                import traceback
                traceback.print_exc()
                p.win.close()
            return False

        advance()

    application.connect("activate", activate)
    application.run(None)
    print("FAILED: %s" % failures if failures else "gui smoke test passed")
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
