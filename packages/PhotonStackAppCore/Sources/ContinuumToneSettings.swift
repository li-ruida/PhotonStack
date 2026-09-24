import Foundation

/// Display tone and band contrast, with a separately estimated diffuse continuum.
public struct ContinuumToneSettings: Equatable, Sendable {
    public static let defaultCurve = "0:0,0.12:0.12,0.18:0.18,0.4:0.47,0.6:0.7,0.8:0.86,1:1"
    public var amount = 0.95
    public var fineRadius = 3
    public var radius = 12
    public var starChroma = 0.7
    public var curvePoints = Self.defaultCurve
    public init() {}

    public var isValid: Bool {
        guard amount.isFinite, (0...2).contains(amount), starChroma.isFinite,
              (0...1).contains(starChroma), (2...64).contains(radius),
              fineRadius > 0, fineRadius < radius else { return false }
        let pairs = curvePoints.split(separator: ",", omittingEmptySubsequences: false)
        guard pairs.count >= 2 else { return false }
        var previousInput: Float = -1
        var previousOutput: Float = -1
        for (index, pair) in pairs.enumerated() {
            let parts = pair.split(separator: ":", omittingEmptySubsequences: false)
            guard parts.count == 2, let x = Float(parts[0]), let y = Float(parts[1]),
                  x.isFinite, y.isFinite, (0...1).contains(x), (0...1).contains(y),
                  x - previousInput >= 1e-6, y >= previousOutput else { return false }
            if index == 0 && (x != 0 || y != 0) { return false }
            if index == pairs.count - 1 && (x != 1 || y != 1) { return false }
            previousInput = x; previousOutput = y
        }
        return true
    }

    /// UI strength interpolates the fixed curve toward identity; saved history
    /// keeps the actual points, so replay never depends on a future preset.
    public mutating func setCurveStrength(_ strength: Double) {
        guard strength.isFinite, (0...1).contains(strength) else { curvePoints = ""; return }
        let knots: [(Double, Double)] = [(0,0),(0.12,0.12),(0.18,0.18),(0.4,0.47),(0.6,0.7),(0.8,0.86),(1,1)]
        curvePoints = knots.map { x, y in "\(x):\(x + strength * (y - x))" }.joined(separator: ",")
    }

    public var operationParameters: [String: String] {
        ["mode": "continuum-v1", "amount": String(amount), "fineRadius": String(fineRadius),
         "radius": String(radius), "starChroma": String(starChroma), "points": curvePoints]
    }

    /// Incomplete or corrupt new records must fail, never fall back to legacy
    /// unsharp masking or silently acquire different processing parameters.
    public init?(operationParameters p: [String: String]) {
        guard p["mode"] == "continuum-v1", let amount = p["amount"].flatMap(Double.init),
              let fine = p["fineRadius"].flatMap(Int.init), let radius = p["radius"].flatMap(Int.init),
              let chroma = p["starChroma"].flatMap(Double.init), let points = p["points"] else { return nil }
        self.init(); self.amount = amount; fineRadius = fine; self.radius = radius
        starChroma = chroma; curvePoints = points
        if !isValid { return nil }
    }
}
