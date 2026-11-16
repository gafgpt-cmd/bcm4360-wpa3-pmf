#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
test_root=$(mktemp -d "${TMPDIR:-/var/tmp}/wl-pmf-tests.XXXXXX")
read -r -a ssl_flags <<< "$(pkg-config --cflags --libs openssl)"
cc -std=c11 -Wall -Wextra -Werror -fsanitize=address,undefined -g tests/pmf.c -o "$test_root/pmf" "${ssl_flags[@]}"
"$test_root/pmf"
python3 tests/rx.py
if [[ -f tests/recovery.py ]]; then
  python3 tests/recovery.py
fi
