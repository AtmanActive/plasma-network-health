// SPDX-License-Identifier: MIT
import org.kde.plasma.configuration

ConfigModel {
    ConfigCategory {
        name: i18nc("@title", "Destinations")
        icon: "network-server-symbolic"
        source: "configDestinations.qml"
    }
    ConfigCategory {
        name: i18nc("@title", "Appearance")
        icon: "preferences-desktop-theme"
        source: "configAppearance.qml"
    }
    ConfigCategory {
        name: i18nc("@title", "Monitoring")
        icon: "preferences-system-network"
        source: "configMonitoring.qml"
    }
}
