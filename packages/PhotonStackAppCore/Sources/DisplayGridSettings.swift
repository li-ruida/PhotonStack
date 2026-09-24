import Foundation

/// Optional narrow-band correction of developed display images.
public struct DisplayGridSettings: Equatable, Sendable {
    public var amount = 1.0
    public init() {}
    public var isValid: Bool { amount.isFinite && (0...1).contains(amount) }
    public var operationParameters: [String: String] {
        ["mode": "display-grid-v1", "amount": String(amount)]
    }
    public init?(operationParameters parameters: [String: String]) {
        guard parameters["mode"] == "display-grid-v1",
              let amount = parameters["amount"].flatMap(Double.init) else { return nil }
        self.amount = amount
        if !isValid { return nil }
    }
    public static func supports(_ url: URL) -> Bool {
        ["tif", "tiff", "png"].contains(url.pathExtension.lowercased())
    }
}
