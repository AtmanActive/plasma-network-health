// SPDX-License-Identifier: MIT
//
// Reading and writing a file the user picked.
//
// QML has no way to do this itself - XMLHttpRequest refuses local files in Qt 6
// and QSettings only understands INI - so both directions go through a short
// shell command. Unlike everything else in this widget these run only when
// someone clicks Export or Import, never on a timer.
import QtQuick

import org.kde.plasma.plasma5support as P5Support

import "utils.js" as Utils

Item {
    id: fileAccess

    /// Refuse anything implausible for a destination list.
    readonly property int maximumBytes: 1048576

    signal readFinished(string path, bool ok, string text, string error)
    signal writeFinished(string path, bool ok, string error)

    QtObject {
        id: priv
        property var jobs: ({})
    }

    P5Support.DataSource {
        id: shell
        engine: "executable"
        connectedSources: []

        onNewData: function (source, data) {
            disconnectSource(source);

            const job = priv.jobs[source];
            if (job === undefined) {
                return;
            }
            delete priv.jobs[source];

            const code = Number(data["exit code"]);
            const stderr = String(data["stderr"] || "").trim();

            if (job.kind === "read") {
                if (code === 0) {
                    fileAccess.readFinished(job.path, true, Qt.atob(String(data["stdout"] || "").trim()), "");
                } else if (code === 3) {
                    fileAccess.readFinished(job.path, false, "", i18n("The file is larger than %1 kB.", Math.round(fileAccess.maximumBytes / 1024)));
                } else {
                    fileAccess.readFinished(job.path, false, "", stderr.length > 0 ? stderr : i18n("The file could not be read."));
                }
            } else {
                fileAccess.writeFinished(job.path, code === 0, code === 0 ? ""
                                         : (stderr.length > 0 ? stderr : i18n("The file could not be written.")));
            }
        }
    }

    function run(kind, path, script) {
        const command = "sh -c 'echo " + Qt.btoa(script) + " | base64 -d | sh'";
        priv.jobs[command] = { "kind": kind, "path": path };
        shell.connectSource(command);
    }

    function read(path) {
        const quoted = Utils.shellQuote(path);
        run("read", path,
            "size=$(stat -c %s -- " + quoted + " 2>/dev/null) || exit 1\n"
          + "[ \"$size\" -le " + maximumBytes + " ] || exit 3\n"
          + "base64 -w 0 -- " + quoted + "\n");
    }

    function write(path, text) {
        const quoted = Utils.shellQuote(path);
        const temporary = Utils.shellQuote(path + ".part");
        run("write", path,
            "printf '%s' " + Utils.shellQuote(Qt.btoa(text)) + " | base64 -d > " + temporary + " || exit 1\n"
          + "mv -f " + temporary + " " + quoted + "\n");
    }
}
