#!/usr/bin/env bash
DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
if [ ! -x "$DIR/bin/Kestral" ]; then
  echo "Kestral is not built yet. Run ./build.sh first."
  exit 1
fi
exec "$DIR/bin/Kestral" "$@"
