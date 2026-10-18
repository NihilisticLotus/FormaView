import QtQuick
import QtQuick.Controls
SpinBox {
    id: root
    readonly property real realValue: value / 100
    from: -3200; to: 3200; stepSize: 25; editable: true
    validator: DoubleValidator { bottom: -32; top: 32; decimals: 2; locale: root.locale.name }
    textFromValue: (value, locale) => Number(value / 100).toLocaleString(locale, "f", 2)
    valueFromText: (text, locale) => Math.round(Number.fromLocaleString(locale, text) * 100)
}
