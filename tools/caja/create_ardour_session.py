import os

import gi

gi.require_version("Caja", "2.0")
gi.require_version("Gtk", "3.0")
from gi.repository import Caja, Gio, GLib, GObject, Gtk

LAUNCHER = None  # Filled in by tools/caja/install.sh.


class ArdourSessionMenu(GObject.GObject, Caja.MenuProvider):
    def get_file_items(self, window, files):
        if len(files) != 1:
            return []
        return self._items(window, files[0])

    def get_background_items(self, window, folder):
        return self._items(window, folder)

    def _items(self, window, folder):
        if not folder.is_directory() or folder.get_uri_scheme() != "file":
            return []
        path = folder.get_location().get_path()
        if not path or not os.path.isabs(path):
            return []
        item = Caja.MenuItem(
            name="ArdourSessionMenu::create",
            label="Create Ardour Session Here",
            icon="/usr/share/pixmaps/ardour.svg",
            tip="Open the Ardour New Session dialog for this folder",
        )
        item.connect("activate", self._activate, window, path)
        return [item]

    def _activate(self, item, window, path):
        try:
            Gio.Subprocess.new(
                [LAUNCHER, "--new-session-in-folder", path],
                Gio.SubprocessFlags.NONE,
            )
        except GLib.Error as error:
            dialog = Gtk.MessageDialog(
                transient_for=window,
                message_type=Gtk.MessageType.ERROR,
                buttons=Gtk.ButtonsType.CLOSE,
                text="Could not launch Ardour",
            )
            dialog.format_secondary_text(str(error))
            dialog.connect(
                "response", lambda widget, response: widget.destroy()
            )
            dialog.show()
