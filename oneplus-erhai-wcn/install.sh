#!/bin/bash
# Install the WCN7880 host stack into an erhai kernel tree.
#
#   ./install.sh /path/to/erhai/linux
#
# Copies the in-tree pieces (the cnss platform drivers and the OPLUS project
# stub they need), then wires them into drivers/net/wireless/{Kconfig,Makefile}.
# Idempotent -- running it twice is a no-op.
#
# qcacld-3.0 is NOT copied: it builds out-of-tree against the kernel you point
# it at. See README.md for that step.

set -eu

KERNEL=${1:-}
if [ -z "$KERNEL" ] || [ ! -f "$KERNEL/Makefile" ] || [ ! -d "$KERNEL/drivers/net/wireless" ]; then
	echo "usage: $0 /path/to/kernel/tree" >&2
	exit 1
fi

HERE=$(cd "$(dirname "$0")" && pwd)

echo "Installing cnss platform drivers into $KERNEL"
for d in cnss2 cnss_genl cnss_prealloc cnss_utils; do
	rsync -a "$HERE/drivers/net/wireless/$d" "$KERNEL/drivers/net/wireless/"
done

# Headers the port needs that upstream either never had (the cnss and OPLUS
# ones) or has since dropped -- of_gpio.h went away in b6420bd5aa0c ("gpio:
# remove of_get_named_gpio() and <linux/of_gpio.h>") and cnss2 still uses it.
echo "Installing support headers"
rsync -a "$HERE/include/" "$KERNEL/include/"

KCONFIG="$KERNEL/drivers/net/wireless/Kconfig"
if ! grep -q 'cnss2/Kconfig' "$KCONFIG"; then
	echo "Wiring up $KCONFIG"
	# The sources have to land inside the "if WLAN" block, so insert them
	# ahead of its endif rather than appending to the file.
	python3 - "$KCONFIG" <<'EOF'
import sys
path = sys.argv[1]
block = """
# Qualcomm CNSS platform driver (OnePlus Pad 3 / WCN7880)
source "drivers/net/wireless/cnss_prealloc/Kconfig"
source "drivers/net/wireless/cnss_utils/Kconfig"
source "drivers/net/wireless/cnss_genl/Kconfig"
source "drivers/net/wireless/cnss2/Kconfig"
"""
text = open(path).read()
marker = "endif # WLAN"
if marker not in text:
    sys.exit("could not find '%s' in %s" % (marker, path))
open(path, "w").write(text.replace(marker, block.lstrip("\n") + "\n" + marker, 1))
EOF
fi

MAKEFILE="$KERNEL/drivers/net/wireless/Makefile"
if ! grep -q 'cnss2/' "$MAKEFILE"; then
	echo "Wiring up $MAKEFILE"
	cat >> "$MAKEFILE" <<'EOF'

# Qualcomm CNSS platform driver (OnePlus Pad 3 / WCN7880)
obj-$(CONFIG_WCNSS_MEM_PRE_ALLOC)	+= cnss_prealloc/
obj-y					+= cnss_utils/
obj-$(CONFIG_CNSS_GENL)			+= cnss_genl/
obj-$(CONFIG_CNSS2)			+= cnss2/
EOF
fi

cat <<EOF

Installed. Now enable the drivers in your kernel config:

  cd $KERNEL
  ./scripts/config --module CNSS2 --enable CNSS2_QMI --enable CNSS2_DEBUG \\
                   --module CNSS_GENL --module CNSS_UTILS \\
                   --module CNSS_QMI_SVC --module CNSS_PLAT_IPC_QMI_SVC \\
                   --module WCNSS_MEM_PRE_ALLOC
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- olddefconfig
  make ARCH=arm64 CROSS_COMPILE=aarch64-linux-gnu- -j\$(nproc) Image.gz modules

Then build wlan.ko out-of-tree -- see README.md.
EOF
