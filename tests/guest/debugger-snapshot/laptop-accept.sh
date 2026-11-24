#!/bin/bash
set -eu
if [ "$#" -ne 3 ]; then
    echo "usage: laptop-accept.sh /path/to/stock/lldb /path/to/hao /path/to/target-arm" >&2
    exit 2
fi
fixture=$(cd "$(dirname "$0")" && pwd)
exec python3 "$fixture/session.py" --lldb "$1" "$2" "$3"
