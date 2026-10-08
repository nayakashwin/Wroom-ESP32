#!/usr/bin/env bash
# Scaffold a new ESP32 project from the _project template.
# usage: ./new_project.sh <name>
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
NAME="${1:?usage: ./new_project.sh <name>}"

if [ -e "$ROOT/$NAME" ]; then
  echo "error: $ROOT/$NAME already exists" >&2
  exit 1
fi

cp -r "$ROOT/_project" "$ROOT/$NAME"
sed -i "s/<project-name>/$NAME/" "$ROOT/$NAME/AGENTS.md"
echo "created $ROOT/$NAME"
echo "next: cd $ROOT/$NAME && pio run -e default -t upload"
echo "note: _project's build_flags already has -I../_secrets; if the new"
echo "      project talks to WiFi/Telegram, #include \"secrets.h\" as in"
echo "      README.md section 2 (values live in _secrets/secrets.h)."
