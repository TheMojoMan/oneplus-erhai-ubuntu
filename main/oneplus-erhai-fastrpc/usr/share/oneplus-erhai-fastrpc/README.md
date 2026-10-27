# oneplus-erhai-fastrpc

The **shared FastRPC layer** of the OnePlus Pad 3 (`erhai`, SM8750): Qualcomm's
userspace for the Hexagon DSPs plus the DSP-side payload trees a FastRPC client
uploads to them.  Without this package `/dev/fastrpc-cdsp` cannot be used at all,
which is why the NPU stack depends on it.

## How erhai's Hexagon stack is split across packages

| package | owns | runs as |
|---|---|---|
| **oneplus-erhai-fastrpc** (this) | the FastRPC transport: `libcdsprpc`/`libadsprpc`/`libsdsprpc`, `cdsprpcd`, the `fastrpc` group + udev rule, the unsigned-PD env, the pay-load trees (`/usr/lib/rfsa/{adsp,cdsp}`, `/usr/share/qcom/hexagon/v79`), `fastrpc_test` | — |
| **oneplus-erhai-npu** | the NPU/HTP workload layer: the CDSP remotefs tree served to the DSP (`/var/lib/hexagonfs-cdsp`), `erhai-npu-status.sh`, `npu-llm`, the CDSP-restart helper, and the *disabled* `hexagonrpcd-cdsp.service` fallback | — |
| **oneplus-erhai-sensors** | the **ADSP** path: `hexagonrpcd-erhai` (+ its drop-in) and `/var/lib/hexagonfs-erhai`, patched iio-sensor-proxy, `erhai-alsd` | `User=fastrpc` |

Rules that keep the boundary stable:
* **transport vs workload.** Anything a *client* needs in order to talk to a DSP
  (libraries, the group, the device permissions, the daemon, the DSP payload
  trees) belongs to **fastrpc**. Anything specific to running a *graph/model* on
  the compute DSP belongs to **npu**. Anything the **ADSP** serves the sensors
  with belongs to **sensors**.
* the `fastrpc` group is declared here (upstream's `sysusers.d/fastrpc.conf`) AND
  created defensively by both other packages' postinsts, so every package stays
  installable on its own.
* neither `npu` nor `sensors` ships a second daemon for a DSP that the other
  already serves: `cdsprpcd` (here) is the CDSP daemon, `hexagonrpcd-erhai`
  (sensors) is the ADSP one.

## What is installed

```
/usr/local/lib/libcdsprpc.so{,.1,.1.0.0}          CDSP client library (SONAME libcdsprpc.so.1)
/usr/local/lib/libadsprpc.so*  libsdsprpc.so*     ADSP / Sensor-DSP client libraries
/usr/local/lib/lib{cdsp,adsp,sdsp}_default_listener.so*
/usr/local/sbin/cdsprpcd                          remotefs daemon for the COMPUTE DSP
/usr/local/bin/{fastrpc_test,fastrpc-healthcheck} bring-up / diagnostic tools
/usr/local/include/fastrpc/*.h                    client headers
/etc/systemd/system/cdsprpcd.service{,.d/erhai.conf}
/etc/udev/rules.d/60-fastrpc.rules                OWNER/GROUP/MODE + dma_heap ACL
/usr/lib/sysusers.d/fastrpc.conf                  the fastrpc group
/etc/environment.d/57-erhai-fastrpc-unsigned.conf FASTRPC_UNSIGNED_PD=1
/usr/lib/rfsa/adsp/   /usr/lib/rfsa/cdsp/         DSP payload trees (shells, C++ runtime,
/usr/share/qcom/hexagon/v79/                      QNN HTP v79 skel, HTP runtime, ggml-htp skels)
/usr/share/doc/oneplus-erhai-fastrpc/             this README + the build recipe
```

The unit is `ConditionPathExists=`-gated on `/dev/fastrpc-cdsp`, so installing
this package on a kernel without the CDSP is safe (the unit is *skipped*, not
failed).

## After installing

```sh
# the group is what grants access to /dev/fastrpc-cdsp (root:fastrpc 0640):
id -nG | tr ' ' '\n' | grep -qx fastrpc && echo OK || echo "re-login needed"
systemctl status cdsprpcd                 # active once the CDSP is up
fastrpc_test -d 3                         # client -> DSP round trip (see the warning)
sudo /usr/local/sbin/erhai-npu-status.sh  # full chain report (oneplus-erhai-npu)
```

**`fastrpc_test` warning (measured 2026-09-20/21):** its
`hap_example_mem_dmahandle` skel always fails here (DSP error `0x4e`), and 10
iterations of that tool are enough to exhaust the CDSP FastRPC mapping space, so
the NEXT model load dies with `failed to map buffer` / `-100201 Model loading
failed`.  Recovery is a CDSP restart (`/usr/local/sbin/erhai-cdsp-restart.sh`,
shipped by oneplus-erhai-npu).  Use `fastrpc_test` to prove the transport, not as
a soak test.

## Provenance

* `libcdsprpc`/`cdsprpcd` etc. are built from <https://github.com/qualcomm/fastrpc>
  (`./gitcompile --prefix=/usr/local && make install DESTDIR=<stage>`); the exact
  script used is shipped as `build-fastrpc.sh` and the pristine staged tree is
  `tmp/qnn/fastrpc-stage` in the erhai workspace.
* the DSP payload trees come from the device's own OxygenOS 16.0.9.400 `dsp.img`
  (`/cdsp`), the public Qualcomm QNN/QAIRT 2.47/2.48 releases (the HTP v79 skel
  must match the client's build id) and Qualcomm's public PyPI `geniex` /
  `qai_appbuilder` tooling (the `libggml-htp-v7x.so` HTP backends) -- DSP-side
  Qualcomm binaries, not built here.

## Deliberately not shipped

* `adsprpcd`, `sdsprpcd`, `gdsprpcd` and their units: the ADSP/SDSP paths on
  erhai are served by **oneplus-erhai-sensors**' `hexagonrpcd-erhai`.  Two daemons
  for one DSP is exactly what this split exists to prevent.
* the libtool `.la` files (Debian policy: they leak build paths).
* the manual backup copies of the QNN skel (`libQnnHtpV79Skel.so.previous`,
  `libQnnHtpV79Skel.so.qai_appbuilder`, ~20 MB): nothing loads a file with those
  names; they stay on this device but are not part of a clean install.
