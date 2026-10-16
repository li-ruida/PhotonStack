import Foundation
import Testing
@testable import PhotonStackAppCore

private func stackSchema() -> DeepSkySchema {
    let defaults = ["stack.method": "sigma", "stack.median-mad": "on", "stack.rejection-maps": "on",
                    "stack.weights": "", "registration.mode": "similarity", "registration.refine-centroids": "on",
                    "stack.normalize-background": "on", "selection.apply": "on", "denoise.noise-model": "scene", "denoise.h": "0.4"]
    return DeepSkySchema(version: 1, fields: defaults.map { key, value in
        DeepSkySchema.Field(key: key, label: key, chineseLabel: key, group: "stack", type: "text", defaultValue: value, minimum: 0, maximum: 0, choices: [])
    })
}

@Test func stackModesRoundTripAndHaveDistinctLocalizedDescriptions() throws {
    #expect(StackMethod.allCases.count == 8)
    for method in StackMethod.allCases {
        #expect(try JSONDecoder().decode(StackMethod.self, from: JSONEncoder().encode(method)) == method)
        #expect(!method.explanation(chinese: true).isEmpty)
        #expect(!method.explanation(chinese: false).isEmpty)
    }
    #expect(AppLanguage.simplifiedChinese.stackMethodName(.maximum).contains("最大值"))
    #expect(AppLanguage.english.stackMethodName(.minimum).contains("Minimum"))
}

@Test func stackModesDisableAndClearOnlyIncompatibleDependencies() {
    let schema = stackSchema()
    for method in StackMethod.allCases {
        var settings = DeepSkySettings()
        settings.values = ["stack.weights": "1,2,3", "denoise.noise-model": "independent-luminance", "develop.saturation": "0.72"]
        settings.setCompatibleValue(method.rawValue, for: "stack.method", schema: schema)
        #expect(settings.effectiveValue("stack.median-mad", schema: schema) == (method == .sigma ? "on" : "off"))
        #expect(settings.fieldIsApplicable("stack.rejection-maps", schema: schema) == method.supportsRejectionMaps)
        #expect(settings.fieldIsApplicable("stack.weights", schema: schema) == !method.isExtrema)
        #expect(settings.fieldIsApplicable("stack.sigma-high", schema: schema) == [.sigma, .winsorized].contains(method))
        #expect(settings.fieldIsApplicable("stack.percentile-low", schema: schema) == (method == .percentile))
        #expect(settings.values["develop.saturation"] == "0.72")
        #expect(settings.effectiveValue("registration.mode", schema: schema) == "similarity") // Selecting maximum alone doesn't disable alignment.
        if method != .sigma { #expect(settings.values["denoise.noise-model"] == "scene") }
        if !method.supportsRejectionMaps { #expect(settings.values["stack.rejection-maps"] == "off") }
        if method.isExtrema { #expect(settings.values["stack.weights"] == "") }
    }
}

@Test func stackStarTrailCompositionRetainsManualDecisionsAndFinishing() throws {
    let schema = stackSchema()
    var settings = DeepSkySettings()
    settings.overrides["/original.fits"] = "reject"
    settings.values["develop.saturation"] = "0.72"
    settings.configureStarTrailComposition(schema: schema)
    for (key, value) in ["stack.method": "maximum", "registration.mode": "none", "registration.refine-centroids": "off",
                         "selection.apply": "off", "stack.normalize-background": "off", "stack.median-mad": "off", "stack.rejection-maps": "off"] {
        #expect(settings.values[key] == value)
    }
    #expect(settings.overrides["/original.fits"] == "reject")
    #expect(settings.values["develop.saturation"] == "0.72")
    #expect(!settings.fieldIsApplicable("registration.minimum-matches", schema: schema))
    #expect(try JSONDecoder().decode(DeepSkySettings.self, from: JSONEncoder().encode(settings)) == settings)
}

@Test func stackRegistrationAndNoiseModulesKeepCompatibleSettings() {
    let schema = stackSchema()
    var settings = DeepSkySettings()
    settings.setCompatibleValue("translation", for: "registration.mode", schema: schema)
    #expect(settings.values["registration.refine-centroids"] == "off")
    #expect(!settings.fieldIsApplicable("registration.refine-centroids", schema: schema))
    settings.setCompatibleValue("independent-luminance", for: "denoise.noise-model", schema: schema)
    #expect(settings.values["denoise.h"] == "0")
    #expect(!settings.fieldIsApplicable("denoise.h", schema: schema))
    settings.setCompatibleValue("maximum", for: "stack.method", schema: schema)
    #expect(settings.fieldIsApplicable("denoise.h", schema: schema))
    settings.setCompatibleValue("independent-luminance", for: "denoise.noise-model", schema: schema)
    #expect(settings.values["denoise.noise-model"] == "scene")
}
