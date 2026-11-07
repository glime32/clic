#!/bin/sh
# Installe ou met à jour clic : compile et copie les commandes dans ~/.local/bin.
# Aucun service à activer : « clic » lance lui-même son démon au premier appel.
# Relancer après toute modification de src/clic.c ou des scripts.
set -e
here=$(cd "$(dirname "$0")/.." && pwd)
mkdir -p "$HOME/.local/bin"
gcc -O2 -Wall -Wextra -o "$here/.clic.new" "$here/src/clic.c" -lm
mv "$here/.clic.new" "$HOME/.local/bin/clic"
install -m 755 "$here/scripts/clic-shot" "$here/scripts/clic-daemon" "$HOME/.local/bin/"
[ -x "$HOME/.local/bin/clic" ] && "$HOME/.local/bin/clic" quit 2>/dev/null || true   # arrête l'ancien démon
"$HOME/.local/bin/clic" ping && echo "clic prêt"
