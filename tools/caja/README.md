# Create Ardour Session Here in Caja

The extension requires `python3-caja` and the Caja 2.0 and GTK 3.0 GI
bindings. It also needs an Ardour build from this checkout with the
`--new-session-in-folder` option.

From the Ardour checkout, run:

```sh
tools/caja/install.sh
```

The installer resolves the checkout's `gtk2_ardour/ardev` and writes
`create_ardour_session.py` under
`${XDG_DATA_HOME:-$HOME/.local/share}/caja-python/extensions/`. It accepts
the original extension with a hard-coded launcher and keeps unrelated
extensions at that filename. Run the installer again if the checkout
moves or the extension changes. Regular Ardour rebuilds use the same
launcher and need no reinstall.

After installing or updating, quit Caja with `caja --quit` and reopen it
from the desktop or application menu.

Right-click a local folder, or right-click the background of an open local
folder, and choose **Create Ardour Session Here**. Ardour opens its New
Session dialog for that folder. Confirm creation in Ardour after any
source files have finished copying. Local SSHFS mounts are supported.

To uninstall, remove the installed extension and restart Caja:

```sh
rm -- "${XDG_DATA_HOME:-$HOME/.local/share}/caja-python/extensions/create_ardour_session.py"
caja --quit
```

Reopen Caja from the desktop or application menu.
