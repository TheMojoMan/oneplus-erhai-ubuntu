#!/usr/bin/python3
# SPDX-License-Identifier: GPL-2.0-only
"""erhai-camera-ae.service entry point (deb: /usr/lib/oneplus-erhai-camera-ae/).

python3 -I drops the script dir from sys.path, so put the package dir back
on it and run the engine.  `camera-ae.py doctor` runs the setup check."""
import sys

sys.path.insert(0, '/usr/lib/oneplus-erhai-camera-ae')

if sys.argv[1:2] == ['doctor']:
    from camera_ae.doctor import main as doctor  # noqa: E402
    sys.exit(doctor())

from camera_ae.main import main  # noqa: E402

sys.exit(main(sys.argv[1:]))
