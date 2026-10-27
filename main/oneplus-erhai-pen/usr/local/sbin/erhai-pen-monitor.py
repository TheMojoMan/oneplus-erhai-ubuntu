#!/usr/bin/python3
# erhai-pen-monitor.py — OnePlus Pad 3 (erhai): pen (stylus) Bluetooth support.
#
# Linux/BlueZ port of stock firmware's "OplusPen" service (org.lineageos.pen,
# android_hardware_oplus/packages/Pen in the LineageOS erhai open-source tree).
# Behaviour parity with stock:
#   * watches for OnePlus pen radios (BLE HID; Goodix OUI F8:6F:DE, Maxeye
#     OUI C0:87:06; HID vendor 0x22D9 / 0x330A — see /etc/erhai/pen.conf),
#   * auto-pairs, trusts and connects them by address (stock bonded by the
#     MAC the wireless charger reported; BlueZ discovers the radio directly),
#   * re-arms BLE discovery and re-connects while the pen is in range,
#   * watches the CPS8601 wireless-charger debugfs for the ATTACH edge and
#     starts a BLE scan + reconnect the moment the pen is placed on the coil
#     (the pen only advertises while awake, and the charger is what wakes it;
#     kernel +b9).  The coil also reports the pen's BLE MAC, which is learned
#     and reused for instant reconnects,
#   * keeps /run/erhai-pen/{state,addr,updated} fresh for other tooling,
#   * reads the pen's state of charge over BLE (GATT Battery Service,
#     0x180F / Battery Level 0x2A19) once connected and publishes it as
#     /run/erhai-pen/soc — the SAME value stock Android's charger HAL pushes
#     into the charger driver as the writable `rx_soc` attribute.  (The coil
#     driver's own `pen_soc`, from the chip's POWER packet, is only used for a
#     charge-full check and is NOT what Android displays.)
#
# The tip/position/pressure data path is NOT here: it is the NT36536 in-cell
# digitizer (touch driver), exactly like Android.  The wireless-charging/
# pen-presence uevent (pencil_status / pencil_addr from the CPS8601 charger
# chip) is stock-only for now; see /usr/share/oneplus-erhai-pen/README.md.

import os
import re
import time
import signal
import logging
import warnings
import subprocess
import sys

warnings.filterwarnings("ignore", message="GLib.unix_signal_add is deprecated")

from gi.repository import Gio, GLib

CONF = "/etc/erhai/pen.conf"
STATE_DIR = os.environ.get("ERHAI_PEN_STATE", "/run/erhai-pen")
#: Writable knob the coil driver exposes so its charge-full latch can re-arm
#: once the pen has drained (kernel oplus_cps8601 rx_soc, vendor rx_soc_store).
RX_SOC_PATH = os.environ.get(
    "ERHAI_PEN_RX_SOC", "/sys/kernel/debug/cps-wls-charger/rx_soc")
BLUEZ = "org.bluez"
BMGR = "/org/bluez"
TAG = "erhai-pen-monitor"

log = logging.getLogger(TAG)


def _unv(value):
    """Unwrap a GLib.Variant (a D-Bus 'v') if needed."""
    return value.unpack() if hasattr(value, "unpack") else value


def norm_uuid(uuid):
    """Normalise a 16-bit ('180f') or 128-bit GATT UUID to full lowercase."""
    u = str(uuid or "").strip().lower()
    if len(u) == 4:
        return "0000%s-0000-1000-8000-00805f9b34fb" % u
    return u


def write_soc(pct):
    """Publish the pen's state of charge for erhai-pen-notify / the CLI.

    Also feeds the coil driver's writable ``rx_soc`` so its charge-full latch
    can release and top the pen up again once it has drained (the pen only
    reports over the coil while charging, so the driver needs this side
    channel -- it is the same value stock's charger HAL writes).
    """
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        p = os.path.join(STATE_DIR, "soc")
        with open(p + ".tmp", "w", encoding="utf-8") as fh:
            fh.write("%d\n" % int(pct))
        os.replace(p + ".tmp", p)
        with open(os.path.join(STATE_DIR, "soc_updated"), "w",
                  encoding="utf-8") as fh:
            fh.write("%d\n" % int(time.time()))
    except (OSError, ValueError) as e:
        log.warning("write_soc failed: %s", e)
        return False
    try:
        with open(RX_SOC_PATH, "w", encoding="utf-8") as fh:
            fh.write("%d\n" % int(pct))
    except OSError:
        pass  # kernel without the knob yet, or not running as root
    return True


def load_conf():
    cfg = {
        "PEN_OUIS": ["F8:6F:DE", "C0:87:06"],
        "PEN_VENDOR_IDS": ["22D9", "330A"],
        "AUTO_PAIR": "yes",
        "SCAN_SECONDS": "6",
        "TICK_SECONDS": "20",
        "SOC_ENABLE": "yes",
        "BATTERY_SERVICE_UUID": "180f",
        "BATTERY_LEVEL_UUID": "2a19",
        "CHARGER_DEBUGFS": "/sys/kernel/debug/cps-wls-charger",
        "CHARGER_POLL_SECONDS": "2",
        "CONNECT_ON_ATTACH": "yes",
    }
    try:
        with open(CONF, "r", encoding="utf-8") as fh:
            for line in fh:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                k, _, v = line.partition("=")
                cfg[k.strip()] = v.strip()
    except FileNotFoundError:
        log.warning("no %s; using defaults", CONF)
    ouis = [o.upper() for o in re.split(r"[\s,]+", cfg["PEN_OUIS"]) if o]
    if ouis:
        cfg["PEN_OUIS"] = ouis
    return cfg


def mac_match(addr, ouis):
    a = addr.replace(":", "").upper()
    return any(a.startswith(o.replace(":", "").upper()) for o in ouis)


def mac_from_u64(value):
    """Decode the CPS8601's packed pen BLE address (cps_recv_ble_mac_addr():
    decode_addr[5] is the most significant octet) into 'AA:BB:CC:DD:EE:FF'.

    Returns '' for a zero / out-of-range value."""
    try:
        v = int(value)
    except (TypeError, ValueError):
        return ""
    if v <= 0 or v > 0xFFFFFFFFFFFF:
        return ""
    return ":".join("%02X" % ((v >> (8 * i)) & 0xFF) for i in range(5, -1, -1))


def write_state(connected, addr=""):
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        for name, val in (("state", "1" if connected else "0"),
                          ("addr", addr),
                          ("updated", "%d" % int(time.time()))):
            tmp = os.path.join(STATE_DIR, name + ".tmp")
            with open(tmp, "w", encoding="utf-8") as fh:
                fh.write(val + "\n")
            os.replace(tmp, os.path.join(STATE_DIR, name))
    except OSError as e:
        log.warning("write_state failed: %s", e)


def write_kv(name, value):
    """Atomically write one state file under STATE_DIR (best effort)."""
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        tmp = os.path.join(STATE_DIR, name + ".tmp")
        with open(tmp, "w", encoding="utf-8") as fh:
            fh.write(str(value) + "\n")
        os.replace(tmp, os.path.join(STATE_DIR, name))
    except OSError:
        pass


def log_setup():
    fmt = "%(asctime)s %(name)s[%(process)d]: %(message)s"
    logging.basicConfig(level=logging.INFO, format=fmt)
    # journal catches stderr/stdout of the unit; keep the tag visible.

class BlueZ:
    """Thin GDBus wrapper around the BlueZ system-bus objects."""

    def __init__(self, bus):
        self.bus = bus

    def call(self, objpath, iface, method, args, rtype):
        try:
            return self.bus.call_sync(BLUEZ, objpath, iface, method, args, rtype,
                                      Gio.DBusCallFlags.NONE, 8000, None)
        except GLib.Error as e:
            log.debug("dbus %s.%s: %s", iface, method, e)
            return None

    def get_managed_objects(self):
        r = self.call("/", "org.freedesktop.DBus.ObjectManager",
                      "GetManagedObjects", None,
                      GLib.VariantType.new("(a{oa{sa{sv}}})"))
        if r is None:
            return {}
        return r.unpack()[0] if r else {}

    def get_prop(self, objpath, iface, prop):
        r = self.call(objpath, "org.freedesktop.DBus.Properties", "Get",
                      GLib.Variant("(ss)", (iface, prop)),
                      GLib.VariantType.new("(v)"))
        if r is None:
            return None
        return r.unpack()[0]


class Pen:
    """A BlueZ device we believe is the OnePlus pen (matched by OUI)."""

    def __init__(self, path, props):
        self.path = path
        self.props = dict(props or {})
        self.address = str(self.props.get("Address", "") or "")
        self.name = str(self.props.get("Name", "") or props.get("Alias", "") or "")

    def get(self, key, default=None):
        return self.props.get(key, default)

    @property
    def paired(self):
        return bool(self.props.get("Paired", False))

    @property
    def trusted(self):
        return bool(self.props.get("Trusted", False))

    @property
    def connected(self):
        return bool(self.props.get("Connected", False))

    def reload(self, bluez):
        objs = bluez.get_managed_objects()
        entry = objs.get(self.path)
        if entry and "org.bluez.Device1" in entry:
            self.props = entry["org.bluez.Device1"]
        else:
            log.info("pen %s disappeared from BlueZ", self.address)
            return False
        return True

    def describe(self):
        n = self.get("Alias") or self.get("Name") or "?"
        return "%s (%s)" % (self.address, n)

class Watcher:
    def __init__(self, cfg):
        self.cfg = cfg
        self.pens = {}            # path -> Pen
        self.last_seen = {}       # addr -> monotonic time
        self.connected_addr = ""
        self.stopping = False
        self._logged_idle = False
        self.bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
        self.bluez = BlueZ(self.bus)
        self._subs = []
        self._tick_id = None
        self.soc_enable = cfg.get("SOC_ENABLE", "yes").lower() == "yes"
        self.battery_svc = norm_uuid(cfg.get("BATTERY_SERVICE_UUID", "180f"))
        self.battery_chr = norm_uuid(cfg.get("BATTERY_LEVEL_UUID", "2a19"))
        self._char_subs = []
        self._notifying = set()
        self._gatt_logged = set()
        # Wireless-charger (CPS8601) attach trigger: the coil wakes the pen,
        # so its attach edge is when a BLE scan is most likely to find it.
        self.charger_dir = cfg.get("CHARGER_DEBUGFS",
                                   "/sys/kernel/debug/cps-wls-charger")
        self.connect_on_attach = cfg.get("CONNECT_ON_ATTACH", "yes").lower() == "yes"
        self._charger_id = None
        self._last_present = None
        self._mac_attempts = 0
        self._mac_retry_id = None
        self.known_addrs = set()
        try:
            with open(os.path.join(STATE_DIR, "known_addrs"),
                      encoding="utf-8") as fh:
                for line in fh:
                    a = line.strip()
                    if a:
                        self.known_addrs.add(a)
        except OSError:
            pass

    # ----- low-level helpers -------------------------------------------------

    def btctl(self, args, timeout):
        try:
            p = subprocess.run(["bluetoothctl", "-t", "20"] + args,
                               capture_output=True, text=True, timeout=timeout)
            return p.returncode, (p.stdout or "") + (p.stderr or "")
        except subprocess.TimeoutExpired:
            return -9, "timeout"
        except OSError as e:
            return -1, str(e)

    def scan_once(self):
        n = max(1, min(30, int(self.cfg.get("SCAN_SECONDS", "6"))))
        try:
            subprocess.Popen(["bluetoothctl", "-t", str(n), "scan", "on"],
                             stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        except OSError as e:
            log.warning("scan launch failed: %s", e)

    # ----- wireless-charger trigger (auto-connect on attach) ----------------

    def _charger_int(self, name, base=0):
        """Read an integer from the charger debugfs.  Decimal attributes
        (pen_present/soc/state) use base 0; `ble_mac_addr` is printed by the
        driver as bare hex (`%llx`), so it must be read with base 16."""
        try:
            with open(os.path.join(self.charger_dir, name),
                      encoding="utf-8") as fh:
                text = fh.read().strip().split()[0]
        except (OSError, IndexError):
            return None
        try:
            return int(text, base)
        except ValueError:
            try:
                return int(text, 16)
            except ValueError:
                return None

    def _remember_addr(self, addr):
        """Track a pen's BLE address and mirror it to the state dir, so a
        later attach can reconnect it without waiting for re-discovery."""
        if not addr or addr in self.known_addrs:
            return
        self.known_addrs.add(addr)
        try:
            os.makedirs(STATE_DIR, exist_ok=True)
            with open(os.path.join(STATE_DIR, "known_addrs"), "w",
                      encoding="utf-8") as fh:
                for a in sorted(self.known_addrs):
                    fh.write(a + "\n")
        except OSError as e:
            log.debug("write known_addrs failed: %s", e)
        log.info("remembered pen BLE address %s", addr)

    def _charger_tick(self):
        if self.stopping:
            return GLib.SOURCE_REMOVE
        present = self._charger_int("pen_present")
        if present is None:
            return GLib.SOURCE_CONTINUE
        if self._last_present is None:
            self._last_present = present
            write_kv("charger_present", present)
            if present == 1:
                # The pen was ALREADY on the charger when we started (the normal
                # case: the service comes up while the pen is charging).  Take
                # startup as the attach edge - otherwise the auto-connect never
                # fires for that pen, because no CHANGE is ever seen.
                log.info("pen already on the charger at startup - treating as attach")
                self._on_attach()
            return GLib.SOURCE_CONTINUE
        if present != self._last_present:
            write_kv("charger_present", present)
            if present == 1:
                self._on_attach()
            else:
                log.info("pen lifted off the charger")
            self._last_present = present
        return GLib.SOURCE_CONTINUE

    def _on_attach(self):
        """The pen was just placed on the wireless charger: it is awake and
        advertising, so start a BLE scan and connect it right away (kernel
        +b9's coils report the attach edge and the pen's BLE MAC)."""
        addr = self._read_coil_mac()
        log.info("pen placed on the charger%s",
                 (" (coil reports BLE MAC %s)" % addr) if addr else "")
        write_kv("charger_mac", addr)
        if addr:
            self._remember_addr(addr)
        else:
            # The coil publishes the pen's BLE MAC a few seconds AFTER the
            # attach edge (it arrives with the Qi handshake) - measured 3 s
            # late on Marcos' pad (2026-10-03), so a single read at the edge
            # always missed it.  Retry in the background.
            self._mac_attempts = 0
            self._mac_retry_id = GLib.timeout_add_seconds(2, self._mac_retry)
        if not self.connect_on_attach:
            return
        try:
            self.scan_once()
        except OSError as e:
            log.debug("attach scan failed: %s", e)
        # A pen BlueZ already knows (paired) can be paged directly without
        # waiting for discovery.
        for a in sorted(self.known_addrs):
            self.btctl(["connect", a], 12)
        try:
            self._sweep(force_scan=True)
        except GLib.Error as e:
            log.debug("attach sweep failed: %s", e)
        # Discovery needs a few seconds to land; keep sweeping briefly.
        for delay in (3, 6, 10):
            GLib.timeout_add_seconds(delay, self._attach_retry)

    def _attach_retry(self):
        if self.stopping:
            return GLib.SOURCE_REMOVE
        try:
            self._sweep(force_scan=True)
        except GLib.Error as e:
            log.debug("attach retry failed: %s", e)
        return GLib.SOURCE_REMOVE

    def _read_coil_mac(self):
        """The pen's BLE identity address as the CPS8601 coil reports it."""
        raw = self._charger_int("ble_mac_addr", 16)
        return mac_from_u64(raw) if raw else ""

    def _mac_retry(self):
        """Poll the coil for the pen's BLE MAC a few seconds after the attach
        edge (the chip only reports it once the pen has finished its Qi
        handshake).  Learn it, then page the pen directly."""
        if self.stopping:
            return GLib.SOURCE_REMOVE
        addr = self._read_coil_mac()
        if addr:
            write_kv("charger_mac", addr)
            self._remember_addr(addr)
            log.info("coil reported the pen BLE MAC late: %s", addr)
            if self.connect_on_attach:
                self.btctl(["connect", addr], 12)
            return GLib.SOURCE_REMOVE
        self._mac_attempts += 1
        if self._mac_attempts >= 8:      # ~16 s after the attach edge
            log.debug("coil never reported a pen BLE MAC")
            return GLib.SOURCE_REMOVE
        return GLib.SOURCE_CONTINUE

    # ----- BLE state of charge (GATT battery service) -----------------------

    def _drop_char_subs(self):
        for sid in self._char_subs:
            try:
                self.bus.signal_unsubscribe(sid)
            except Exception:
                pass
        self._char_subs = []
        self._notifying.clear()

    def _gatt_objects(self, pen):
        """Return ({service path: uuid}, [(char path, props)]) for the pen."""
        svcs, chars = {}, []
        try:
            objs = self.bluez.get_managed_objects()
        except Exception as e:
            log.debug("gatt enumerate failed: %s", e)
            return svcs, chars
        prefix = pen.path + "/"
        for path, ifaces in objs.items():
            if not path.startswith(prefix):
                continue
            svc = ifaces.get("org.bluez.GattService1")
            if svc:
                svcs[path] = str(_unv(svc.get("UUID", "")) or "").lower()
            chr_ = ifaces.get("org.bluez.GattCharacteristic1")
            if chr_:
                chars.append((path, chr_))
        return svcs, chars

    def _read_char(self, path):
        """ReadValue() of a GATT characteristic -> first byte, or None."""
        r = self.bluez.call(path, "org.bluez.GattCharacteristic1", "ReadValue",
                            GLib.Variant("(a{sv})", ({},)),
                            GLib.VariantType.new("(ay)"))
        if r is None:
            return None
        raw = _unv(r)[0]
        if not isinstance(raw, (bytes, bytearray)):
            raw = bytes(bytearray(raw))
        return raw[0] if len(raw) else None

    def _on_char_changed(self, connection, sender, path, iface, name, params):
        try:
            obj, changed, _ = params.unpack()
        except Exception:
            return
        if obj != "org.bluez.GattCharacteristic1" or "Value" not in changed:
            return
        raw = _unv(changed["Value"])
        if not isinstance(raw, (bytes, bytearray)):
            try:
                raw = bytes(bytearray(raw))
            except (TypeError, ValueError):
                return
        if raw:
            log.info("pen SoC %d%% (BLE notify)", raw[0])
            write_soc(raw[0])

    def _update_soc(self, pen):
        """Find the pen's battery characteristic, read it, subscribe if able."""
        svcs, chars = self._gatt_objects(pen)
        if pen.address not in self._gatt_logged and (svcs or chars):
            self._gatt_logged.add(pen.address)
            log.info("pen GATT: %d service(s), %d characteristic(s)",
                     len(svcs), len(chars))
            for _, u in sorted(svcs.items()):
                log.info("  svc %s", u)
            for _, c in chars:
                log.info("  chr %s flags=%s", _unv(c.get("UUID", "")),
                         _unv(c.get("Flags")))
        target = None
        for path, c in chars:
            if norm_uuid(_unv(c.get("UUID", ""))) == self.battery_chr:
                target = (path, c)
                break
        if target is None and self.battery_svc:
            for path, c in chars:
                svc = svcs.get(str(_unv(c.get("Service", ""))), "")
                if norm_uuid(svc) == self.battery_svc:
                    target = (path, c)
                    break
        if target is None:
            log.info("pen SoC: no battery characteristic found")
            return
        path, c = target
        val = self._read_char(path)
        if val is not None:
            log.info("pen SoC %d%% (BLE read)", val)
            write_soc(val)
        flags = [str(_unv(f)).lower() for f in (_unv(c.get("Flags")) or [])]
        if "notify" in flags and path not in self._notifying:
            self._char_subs.append(self.bus.signal_subscribe(
                BLUEZ, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                path, None, Gio.DBusSignalFlags.NONE, self._on_char_changed))
            self._notifying.add(path)
            try:
                self.bus.call_sync(BLUEZ, path,
                                   "org.bluez.GattCharacteristic1", "StartNotify",
                                   None, GLib.VariantType.new("()"),
                                   Gio.DBusCallFlags.NONE, 8000, None)
                log.info("pen SoC notifications enabled")
            except GLib.Error as e:
                log.debug("StartNotify failed: %s", e)

    # ----- discovery / state machine ----------------------------------------

    def _sweep(self, force_scan=False):
        objs = self.bluez.get_managed_objects()
        found = {}
        for path, ifaces in objs.items():
            if "org.bluez.Device1" not in ifaces:
                continue
            addr = str(ifaces["org.bluez.Device1"].get("Address", "") or "")
            if not addr or not mac_match(addr, self.cfg["PEN_OUIS"]):
                continue
            pen = self.pens.get(path) or Pen(path, ifaces["org.bluez.Device1"])
            pen.props = ifaces["org.bluez.Device1"]
            found[path] = pen
            self.pens[path] = pen
            self.last_seen[addr] = time.monotonic()
        # forget pens no longer in BlueZ
        for path in [p for p in self.pens if p not in found]:
            del self.pens[path]
        if force_scan and not found:
            self.scan_once()
        acted = False
        for pen in sorted(self.pens.values(), key=lambda p: p.address):
            if self.ensure_pen(pen):
                acted = True
        self._refresh_state()
        if self.soc_enable:
            for pen in self.pens.values():
                if pen.connected:
                    try:
                        self._update_soc(pen)
                    except Exception as e:      # never let GATT kill the watcher
                        log.debug("soc update failed: %s", e)
        if not acted and not found and not self.last_seen and not self._logged_idle:
            self._logged_idle = True
            log.info("no pen in range")
        return found

    def ensure_pen(self, pen, force=False):
        """pair -> trust -> connect, one step per call (re-entrant)."""
        if not pen.address:
            return False
        if not pen.reload(self.bluez):
            self.pens.pop(pen.path, None)
            return False
        auto = self.cfg.get("AUTO_PAIR", "yes").lower() == "yes"
        if not pen.paired:
            if not auto:
                log.info("pen %s seen, not paired (AUTO_PAIR=no)", pen.describe())
                return False
            log.info("pairing pen %s", pen.describe())
            rc, out = self.btctl(["pair", pen.address], 25)
            if rc != 0:
                log.warning("pair %s failed rc=%s: %s", pen.address, rc,
                            out.strip().replace("\n", " | ")[:200])
                return False
            if not pen.reload(self.bluez):
                return False
        if not pen.trusted:
            rc, out = self.btctl(["trust", pen.address], 15)
            if rc != 0:
                log.warning("trust %s failed rc=%s: %s", pen.address, rc,
                            out.strip().replace("\n", " | ")[:160])
                return False
            if not pen.reload(self.bluez):
                return False
        if not pen.connected:
            stale = time.monotonic() - self.last_seen.get(pen.address, 0) > 60
            if stale:
                return False  # sighting is stale: pen is likely asleep
            rc, out = self.btctl(["connect", pen.address], 20)
            if rc != 0:
                log.warning("connect %s failed rc=%s: %s", pen.address, rc,
                            out.strip().replace("\n", " | ")[:160])
                return False
            pen.reload(self.bluez)
        if pen.connected:
            log.info("pen %s connected", pen.describe())
            self.connected_addr = pen.address
            self._remember_addr(pen.address)
            if self.soc_enable:
                try:
                    self._update_soc(pen)
                except Exception as e:
                    log.debug("soc update failed: %s", e)
            return True
        return False

    def _refresh_state(self):
        connected = self.connected_addr
        if connected:
            pen = next((p for p in self.pens.values()
                        if p.address == connected and p.connected), None)
            if pen is None:
                connected = ""
        self.connected_addr = connected
        write_state(bool(connected), connected)

# ----- BlueZ signals -----------------------------------------------------

    def _on_interfaces_added(self, connection, sender, path, iface, name, params):
        try:
            objpath, ifaces = params.unpack()
        except Exception:
            return
        if "org.bluez.Device1" not in ifaces:
            return
        addr = str(ifaces["org.bluez.Device1"].get("Address", "") or "")
        if addr and mac_match(addr, self.cfg["PEN_OUIS"]):
            log.info("pen radio seen: %s (%s)", addr,
                     ifaces["org.bluez.Device1"].get("Name", "") or "?")
            self.pens[objpath] = Pen(objpath, ifaces["org.bluez.Device1"])
            self.last_seen[addr] = time.monotonic()
            self.ensure_pen(self.pens[objpath])
            self._refresh_state()

    def _on_props_changed(self, connection, sender, path, iface, name, params):
        try:
            obj, changed, _ = params.unpack()
        except Exception:
            return
        if obj != "org.bluez.Device1":
            return
        pen = self.pens.get(path)
        if pen is None:
            return
        pen.props.update(changed)
        if "Connected" in changed:
            if bool(changed["Connected"]):
                log.info("pen %s connected", pen.describe())
                self.connected_addr = pen.address
                self._remember_addr(pen.address)
            else:
                log.info("pen %s disconnected", pen.describe())
                self._drop_char_subs()
                if self.connected_addr == pen.address:
                    self.connected_addr = ""
            self._refresh_state()

    # ----- tick --------------------------------------------------------------

    def _tick(self):
        if self.stopping:
            return GLib.SOURCE_REMOVE
        try:
            self._sweep(force_scan=True)
        except GLib.Error as e:
            log.warning("tick failed: %s", e)
        return GLib.SOURCE_CONTINUE

    # ----- lifecycle ---------------------------------------------------------

    def start(self):
        self._subs.append(
            self.bus.signal_subscribe(
                BLUEZ, "org.freedesktop.DBus.ObjectManager", "InterfacesAdded",
                BMGR, None, Gio.DBusSignalFlags.NONE, self._on_interfaces_added))
        self._subs.append(
            self.bus.signal_subscribe(
                BLUEZ, "org.freedesktop.DBus.Properties", "PropertiesChanged",
                None, None, Gio.DBusSignalFlags.NONE, self._on_props_changed))
        self._sweep(force_scan=True)
        tick = max(5, min(300, int(self.cfg.get("TICK_SECONDS", "20"))))
        self._tick_id = GLib.timeout_add_seconds(tick, self._tick)
        if self.connect_on_attach and os.path.isdir(self.charger_dir):
            poll = max(1, min(60, int(self.cfg.get("CHARGER_POLL_SECONDS", "2"))))
            self._charger_id = GLib.timeout_add_seconds(poll, self._charger_tick)
            log.info("charger attach trigger armed (%s, poll=%ss)",
                     self.charger_dir, poll)
        elif self.connect_on_attach:
            log.info("no charger debugfs at %s; attach trigger disabled",
                     self.charger_dir)
        log.info("erhai-pen monitor started (ouis=%s, tick=%ss)",
                 ",".join(self.cfg["PEN_OUIS"]), tick)

    def stop(self):
        self.stopping = True
        if self._tick_id:
            GLib.source_remove(self._tick_id)
        if self._charger_id:
            GLib.source_remove(self._charger_id)
        self._drop_char_subs()
        write_state(False, "")


LOOP = None


def _on_signal(*_a):
    if LOOP is not None and LOOP.is_running():
        LOOP.quit()


def dump_gatt():
    """Print BlueZ's GATT tree (services + characteristics).  Dev aid."""
    bus = Gio.bus_get_sync(Gio.BusType.SYSTEM, None)
    objs = BlueZ(bus).get_managed_objects()
    n = 0
    for path, ifaces in sorted(objs.items()):
        svc = ifaces.get("org.bluez.GattService1")
        chr_ = ifaces.get("org.bluez.GattCharacteristic1")
        if svc:
            print("SVC  %s  %s" % (svc.get("UUID", "?"), path))
            n += 1
        elif chr_:
            print("  CHR %s  flags=%s  %s" % (chr_.get("UUID", "?"),
                                              chr_.get("Flags", []), path))
            n += 1
    print("(%d GATT object(s))" % n)
    return 0


def main():
    global LOOP
    log_setup()
    if sys.argv[1:2] == ["--gatt"]:
        return dump_gatt()
    cfg = load_conf()
    watcher = Watcher(cfg)

    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGTERM, _on_signal)
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGINT, _on_signal)
    GLib.unix_signal_add(GLib.PRIORITY_DEFAULT, signal.SIGHUP, _on_signal)

    write_state(False, "")
    watcher.start()
    LOOP = GLib.MainLoop.new(None, False)
    try:
        LOOP.run()
    finally:
        watcher.stop()
    return 0


if __name__ == "__main__":
    main()