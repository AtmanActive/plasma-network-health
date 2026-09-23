// SPDX-License-Identifier: MIT
.pragma library

/// Round-trip time, in the unit that keeps it readable.
function formatRtt(rttUs, haveRtt) {
    if (!haveRtt || rttUs === undefined || rttUs === null) {
        return "—";
    }
    if (rttUs < 1000) {
        return Math.round(rttUs) + " µs";
    }
    if (rttUs < 100000) {
        return (rttUs / 1000).toFixed(2) + " ms";
    }
    return Math.round(rttUs / 1000) + " ms";
}

function formatThreshold(thresholdUs) {
    if (thresholdUs < 1000) {
        return thresholdUs + " µs";
    }
    return (thresholdUs / 1000).toFixed(thresholdUs % 1000 === 0 ? 0 : 2) + " ms";
}

function colorForState(state, goodColor, badColor, idleColor) {
    if (state === "good") {
        return goodColor;
    }
    if (state === "bad") {
        return badColor;
    }
    return idleColor;
}

function newId() {
    var id = "";
    for (var i = 0; i < 4; ++i) {
        id += Math.floor((1 + Math.random()) * 0x10000).toString(16).substring(1);
    }
    return id;
}

var ipv4Pattern = /^((25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)\.){3}(25[0-5]|2[0-4]\d|1\d\d|[1-9]?\d)$/;
var ipv6Pattern = /^[0-9A-Fa-f:.]+(%[0-9A-Za-z._-]+)?$/;
var hostPattern = /^[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?(\.[A-Za-z0-9]([A-Za-z0-9-]{0,61}[A-Za-z0-9])?)*\.?$/;

/// Cheap sanity check so the config dialog can flag obvious typos. The daemon
/// is the real authority: anything getaddrinfo() accepts will work.
function isValidAddress(text) {
    if (!text) {
        return false;
    }
    var value = String(text).trim();
    if (value.length === 0 || value.length > 255) {
        return false;
    }
    if (ipv4Pattern.test(value)) {
        return true;
    }
    if (value.indexOf(":") >= 0) {
        return ipv6Pattern.test(value) && (value.match(/:/g) || []).length >= 2;
    }
    return hostPattern.test(value);
}

/// Escapes text that is about to be interpolated into a StyledText tooltip.
function escapeHtml(text) {
    return String(text === undefined || text === null ? "" : text)
        .split("&").join("&amp;")
        .split("<").join("&lt;")
        .split(">").join("&gt;");
}

/// One canonical spelling of a destination list, so that two widgets can decide
/// whether they hold the same thing by comparing strings.
function canonicalDestinations(list) {
    if (!Array.isArray(list)) {
        return "[]";
    }
    var normalised = [];
    for (var i = 0; i < list.length; ++i) {
        var entry = list[i] || {};
        normalised.push({
            id: String(entry.id === undefined ? "" : entry.id),
            name: String(entry.name === undefined ? "" : entry.name),
            address: String(entry.address === undefined ? "" : entry.address),
            order: Number(entry.order === undefined ? i + 1 : entry.order),
            thresholdUs: Number(entry.thresholdUs === undefined ? 1000 : entry.thresholdUs),
            sensitivity: Number(entry.sensitivity === undefined ? 3 : entry.sensitivity),
            enabled: entry.enabled !== false
        });
    }
    normalised.sort(function (a, b) {
        return (a.order - b.order) || a.id.localeCompare(b.id);
    });
    return JSON.stringify(normalised);
}

function parseDestinations(text) {
    try {
        var value = JSON.parse(text);
        return Array.isArray(value) ? value : [];
    } catch (error) {
        return [];
    }
}

/// Single-quotes a value for POSIX sh.
function shellQuote(value) {
    return "'" + String(value).split("'").join("'\\''") + "'";
}

/// The document written by "Export": an envelope so the file says what it is.
function exportDocument(list) {
    return JSON.stringify({
        "application": "plasma-network-health",
        "version": 1,
        "exported": new Date().toISOString(),
        "destinations": JSON.parse(canonicalDestinations(list))
    }, null, 2) + "\n";
}

/// Accepts either an exported envelope or a bare array, and returns entries this
/// widget can actually use. Anything unusable is counted rather than guessed at.
function importDocument(text, defaultThresholdUs, defaultSensitivity, maximum) {
    var parsed;
    try {
        parsed = JSON.parse(text);
    } catch (error) {
        return { ok: false, error: String(error), destinations: [], skipped: 0, dropped: 0 };
    }

    var raw = null;
    if (Array.isArray(parsed)) {
        raw = parsed;
    } else if (parsed && Array.isArray(parsed.destinations)) {
        raw = parsed.destinations;
    }
    if (raw === null) {
        return { ok: false, error: "no destination list found", destinations: [], skipped: 0, dropped: 0 };
    }

    var out = [];
    var seen = {};
    var skipped = 0;
    var dropped = 0;

    for (var i = 0; i < raw.length; ++i) {
        var entry = raw[i] || {};
        var address = String(entry.address === undefined ? "" : entry.address).trim();
        if (address.length === 0) {
            ++skipped;
            continue;
        }
        if (out.length >= maximum) {
            ++dropped;
            continue;
        }

        var id = String(entry.id === undefined ? "" : entry.id);
        if (id.length === 0 || seen[id]) {
            id = newId();
        }
        seen[id] = true;

        var threshold = Number(entry.thresholdUs);
        if (!isFinite(threshold) || threshold < 1 || threshold > 60000000) {
            threshold = defaultThresholdUs;
        }
        var sensitivity = Number(entry.sensitivity);
        if (!isFinite(sensitivity) || sensitivity < 0 || sensitivity > 1000) {
            sensitivity = defaultSensitivity;
        }

        out.push({
            id: id,
            name: String(entry.name === undefined || String(entry.name).length === 0 ? address : entry.name),
            address: address,
            order: out.length + 1,
            thresholdUs: Math.round(threshold),
            sensitivity: Math.round(sensitivity),
            enabled: entry.enabled !== false
        });
    }

    return { ok: true, error: "", destinations: out, skipped: skipped, dropped: dropped };
}

/// "#aarrggbb" and "#RRGGBB" both settle to "#rrggbb", so that a colour never
/// compares unequal to itself just because Qt spelled it differently.
function normaliseColor(value) {
    var text = String(value === undefined || value === null ? "" : value);
    if (/^#[0-9a-fA-F]{8}$/.test(text)) {
        text = "#" + text.substring(3);
    }
    return text.toLowerCase();
}

/// The appearance settings that every Network Health widget agrees on, in a
/// fixed order so the result can be compared as a string.
function canonicalAppearance(settings) {
    return JSON.stringify({
        ledSize: Math.round(Number(settings.ledSize)),
        showNames: settings.showNames === true,
        showLatency: settings.showLatency === true,
        contentOpacity: Math.round(Number(settings.contentOpacity)),
        backgroundOpacity: Math.round(Number(settings.backgroundOpacity)),
        colorGood: normaliseColor(settings.colorGood),
        colorBad: normaliseColor(settings.colorBad),
        colorIdle: normaliseColor(settings.colorIdle),
        hideWhenHealthy: settings.hideWhenHealthy === true
    });
}
