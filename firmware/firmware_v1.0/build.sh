set -e
west build "$@"
UF2="build/zephyr/zephyr.uf2"
MOUNT="/Volumes/XIAO-SENSE"
if [ ! -f "$UF2" ]; then
  echo "Build output not found: $UF2" >&2
  exit 1
fi
if [ ! -d "$MOUNT" ]; then
  echo "Built $UF2 but mount not found: $MOUNT" >&2
  echo "Flash manually when the device is connected." >&2
  exit 1
fi
cp "$UF2" "$MOUNT/"
