#!/bin/bash
# build-mesa-usr-debs.sh (stonking / Ubuntu 26.10) -- split the 26.10 /usr
# payload staged at stonking/oneplus-erhai-mesa/usr into the six Ubuntu-named
# replacement .debs, output in stonking/debs/.
#
#   MESA_PREFIX=/usr MESA_STAGE=$PWD/oneplus-erhai-mesa MESA_NO_PULL=1 \
#     ../update-mesa.sh
#   ./build-mesa-usr-debs.sh
#
# The split itself is the same code as the resolute builder (one implementation,
# no drift); only the staging/output locations and the suite differ.  Build it
# inside the 26.10 root (build-in-series.sh or a chroot into the install on
# sda20) so the library links/floors come from that release.
set -euo pipefail

HERE="$(cd "$(dirname "$(readlink -f "${BASH_SOURCE[0]}")")" && pwd)"
export MESA_USR_STAGE="${MESA_USR_STAGE:-$HERE/oneplus-erhai-mesa}"
export MESA_USR_OUT="${MESA_USR_OUT:-$HERE/debs}"
exec "$HERE/../resolute/build-mesa-usr-debs.sh" "$@"
