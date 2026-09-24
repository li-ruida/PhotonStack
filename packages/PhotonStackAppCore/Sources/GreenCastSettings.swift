import Foundation

/// Display-only color styling, recorded separately from linear development.
public struct GreenCastSettings: Equatable, Sendable {
    public enum Method: String, CaseIterable, Sendable {
        case background
        case averageNeutral = "average-neutral"
    }
    public var method: Method = .averageNeutral
    public var amount = 0.65
    public var backgroundLimit = 0.32
    public var greenThreshold = 0.0
    public var preserveLightness = true
    public init() {}

    public var isValid: Bool {
        [amount, backgroundLimit, greenThreshold].allSatisfy { $0.isFinite && (0...1).contains($0) }
    }
    public var operationParameters: [String: String] {
        ["mode": "removeGreen", "greenMethod": method.rawValue,
         "amount": String(amount), "backgroundLimit": String(backgroundLimit),
         "greenThreshold": String(greenThreshold), "preserveLightness": String(preserveLightness)]
    }
    public init(operationParameters p: [String: String]) {
        self.init()
        // Missing keys describe the original background-weighted operation.
        method = p["greenMethod"].flatMap(Method.init(rawValue:)) ?? .background
        amount = p["amount"].flatMap(Double.init) ?? 0.65
        backgroundLimit = p["backgroundLimit"].flatMap(Double.init) ?? 0.32
        greenThreshold = p["greenThreshold"].flatMap(Double.init) ?? 0.01
        preserveLightness = p["preserveLightness"].flatMap(Bool.init) ?? false
    }
}
