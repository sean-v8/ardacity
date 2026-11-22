#!/bin/sh
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd -P)
repo_dir=$(CDPATH= cd -- "$script_dir/../.." && pwd -P)
launcher="$repo_dir/gtk2_ardour/ardev"
extensions="${XDG_DATA_HOME:-"$HOME/.local/share"}/caja-python/extensions"

if [ ! -f "$launcher" ] || [ ! -x "$launcher" ]; then
    printf 'Ardour launcher is missing or not executable: %s\n' "$launcher" >&2
    exit 1
fi

python3 - "$script_dir/create_ardour_session.py" "$launcher" "$extensions" <<'PY'
import ast
import os
from pathlib import Path
import sys
import tempfile


def extension_signature(source):
    """Compare extension code while allowing the installed launcher to differ."""
    tree = ast.parse(source)
    for statement in tree.body:
        if (
            isinstance(statement, ast.Assign)
            and len(statement.targets) == 1
            and isinstance(statement.targets[0], ast.Name)
            and statement.targets[0].id == "LAUNCHER"
            and isinstance(statement.value, ast.Constant)
        ):
            statement.value = ast.Constant(value=None)
            break
    return ast.dump(tree)


template, launcher, extensions = map(Path, sys.argv[1:])
destination = extensions / template.name
source = template.read_text(encoding="utf-8")
installed = source.replace(
    "LAUNCHER = None  # Filled in by tools/caja/install.sh.",
    f"LAUNCHER = {ascii(str(launcher))}",
    1,
)
compile(installed, str(destination), "exec")

if destination.is_symlink() or (destination.exists() and not destination.is_file()):
    raise SystemExit(f"Refusing to replace {destination}")

if destination.exists():
    try:
        current = destination.read_text(encoding="utf-8")
        previous_source = source.replace('            icon="/usr/share/pixmaps/ardour.svg",\n', "", 1)
        recognized = extension_signature(current) in (
            extension_signature(source),
            extension_signature(previous_source),
        )
    except (SyntaxError, UnicodeError):
        recognized = False

    if not recognized:
        raise SystemExit(f"Keeping unrecognized extension at {destination}")
    if current == installed:
        print(f"Already installed at {destination}")
        sys.exit(0)

extensions.mkdir(parents=True, exist_ok=True)
with tempfile.NamedTemporaryFile(
    mode="w", encoding="utf-8", dir=extensions, delete=False
) as temporary:
    temporary_path = Path(temporary.name)
    try:
        temporary.write(installed)
        temporary.close()
        os.replace(temporary_path, destination)
    finally:
        temporary_path.unlink(missing_ok=True)

print(f"Installed {destination}")
print("Restart Caja to load the extension.")
PY
