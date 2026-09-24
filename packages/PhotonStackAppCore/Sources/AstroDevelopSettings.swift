import Foundation

/// Reproducible display development of a linear RGB integration.
/// The stellar balance is statistical, rather than catalog photometry.
public struct AstroDevelopSettings: Equatable, Sendable {
    public var stellarBalance = true
    public var brightness = 1.5
    public var background = 0.035
    public var saturation = 0.7
    public var shadowNeutralization = 0.8
    public var starExposure = 1.0
    public var starPeakThreshold = 0.0
    public var toneCurve = "asinh"
    public var toneScale = 0.0
    public var whitePoint = 0.0
    public var redGain = 1.0
    public var blueGain = 1.0

    public init() {}

    public var operationParameters: [String: String] {
        ["mode": "astro-v1", "toneCurve": toneCurve, "toneScale": String(toneScale), "whitePoint": String(whitePoint), "stellarBalance": String(stellarBalance),
         "brightness": String(brightness), "background": String(background),
         "starExposure": String(starExposure), "starPeakThreshold": String(starPeakThreshold),
         "saturation": String(saturation), "shadowNeutralization": String(shadowNeutralization),
         "redGain": String(redGain), "blueGain": String(blueGain)]
    }

    public init(operationParameters p: [String: String]) {
        self.init()
        // Records created before curve selection used the rational transfer.
        toneCurve = p["toneCurve"] ?? "rational"
        toneScale = p["toneScale"].flatMap(Double.init) ?? 0
        whitePoint = p["whitePoint"].flatMap(Double.init) ?? 0
        stellarBalance = p["stellarBalance"].flatMap(Bool.init) ?? stellarBalance
        brightness = p["brightness"].flatMap(Double.init) ?? brightness
        background = p["background"].flatMap(Double.init) ?? background
        starExposure = p["starExposure"].flatMap(Double.init) ?? 0.5
        starPeakThreshold = p["starPeakThreshold"].flatMap(Double.init) ?? 0
        saturation = p["saturation"].flatMap(Double.init) ?? saturation
        shadowNeutralization = p["shadowNeutralization"].flatMap(Double.init) ?? 0
        redGain = p["redGain"].flatMap(Double.init) ?? redGain
        blueGain = p["blueGain"].flatMap(Double.init) ?? blueGain
    }
}
