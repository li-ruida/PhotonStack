import Foundation

public struct StarLayerRecipe: Codable, Equatable, Sendable {
    public var version = 1
    public var starsEV: Double = 0
    public var nebulaEV: Double = 0
    public var nebulaGamma: Double = 1
    public var starsSaturation: Double = 1
    public var nebulaSaturation: Double = 1

    public init() {}

    public var isValid: Bool {
        version == 1 && starsEV.isFinite && (-8...4).contains(starsEV)
        && nebulaEV.isFinite && (-4...4).contains(nebulaEV)
        && nebulaGamma.isFinite && (0.25...4).contains(nebulaGamma)
        && starsSaturation.isFinite && (0...3).contains(starsSaturation)
        && nebulaSaturation.isFinite && (0...3).contains(nebulaSaturation)
    }

    public var arguments: [String] {
        ["--stars-ev", String(starsEV), "--nebula-ev", String(nebulaEV),
         "--nebula-gamma", String(nebulaGamma), "--stars-saturation", String(starsSaturation),
         "--nebula-saturation", String(nebulaSaturation)]
    }
}
