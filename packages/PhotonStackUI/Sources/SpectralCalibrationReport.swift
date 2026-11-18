import Foundation

struct SpectralCalibrationSettings: Codable, Equatable, Sendable {
    var radius = 8.0, skyInner = 11.0, skyOuter = 15.0, saturation = 0.0, minSNR = 20.0
    var maxCenterOffset = 2.0, maxRelativeSpectralError = 0.1, maxApertureColorChange = 0.03, maxValidationLogError = 0.2
    var excluded: [String] = []
    var isValid: Bool {
        [radius, skyInner, skyOuter, saturation, minSNR, maxCenterOffset, maxRelativeSpectralError, maxApertureColorChange, maxValidationLogError].allSatisfy(\.isFinite) &&
        maxCenterOffset > 0 && maxCenterOffset <= 5 && maxRelativeSpectralError > 0 && maxRelativeSpectralError <= 0.5 &&
        maxApertureColorChange > 0 && maxApertureColorChange <= 0.2 && maxValidationLogError > 0 && maxValidationLogError <= 1 &&
        (3...18).contains(radius) && skyInner > radius+1 && skyOuter >= skyInner+2 && skyOuter <= 38 &&
        saturation >= 0 && minSNR >= 1 && excluded.count <= 10_000 && Set(excluded).count == excluded.count &&
        excluded.allSatisfy { !$0.isEmpty && $0.utf8.count <= 128 && $0.utf8.allSatisfy { (48...57).contains($0) || (65...90).contains($0) || (97...122).contains($0) || [95,46,58,45].contains($0) } }
    }
    var arguments: [String] {
        ["--radius", String(radius), "--sky-inner", String(skyInner), "--sky-outer", String(skyOuter),
         "--saturation", String(saturation), "--min-snr", String(minSNR), "--exclude", excluded.sorted().joined(separator: ","),
         "--max-center-offset", String(maxCenterOffset), "--max-spectral-error", String(maxRelativeSpectralError),
         "--max-aperture-color-change", String(maxApertureColorChange), "--max-validation-log-error", String(maxValidationLogError)]
    }
}

struct SpectralCalibrationReport: Decodable, Sendable {
    typealias Profile = SpectralProfileMetadata
    struct Catalog: Decodable, Sendable { var name, provenance, license, sha: String; var epoch: Double }
    struct Fit: Decodable, Sendable {
        var valid: Bool
        var reason: String
        var training, validation, rejected, regions: Int
        var gains, instrumentGains, whiteRatios, trainingScatter, validationMedian, validationP90: [Double?]
    }
    struct Photometry: Decodable, Sendable {
        var valid: Bool
        var status: String
        var x, y, area: Double
        var flux, snr, backgroundNoise: [Double?]
        var isValid: Bool {
            [x, y, area].allSatisfy(\.isFinite) && area >= 0 &&
            [flux, snr, backgroundNoise].allSatisfy(SpectralCalibrationReport.triplet) &&
            (!valid || (area > 0 && flux.allSatisfy { $0 != nil && $0! > 0 }))
        }
    }
    struct Star: Decodable, Identifiable, Sendable {
        var id, status: String
        var predictedX, predictedY: Double?
        var predictedFlux, spectralErrorBound, apertureColorChange: [Double?]
        var residual: [Double?]?
        var heldOut: Bool
        var photometry, expandedPhotometry: Photometry
        var used: Bool { status == "fit" || status == "validation" }
        var position: (Double, Double)? {
            if photometry.valid { return (photometry.x, photometry.y) }
            if let x = predictedX, let y = predictedY { return (x, y) }; return nil
        }
        var isValid: Bool {
            !id.isEmpty && id.utf8.count <= 128 && status.count <= 100 &&
            (predictedX == nil || predictedX!.isFinite) && (predictedY == nil || predictedY!.isFinite) &&
            [predictedFlux, spectralErrorBound, apertureColorChange].allSatisfy(SpectralCalibrationReport.triplet) &&
            (residual == nil || SpectralCalibrationReport.triplet(residual!)) && photometry.isValid && expandedPhotometry.isValid
        }
    }
    struct Preview: Decodable, Sendable { var resampling, transfer: String; var low, high: Double }
    var schema, width, height, detected: Int
    var algorithm, source, sourceSHA: String
    var profile: Profile
    var catalog: Catalog
    var fit: Fit
    var options: SpectralCalibrationSettings
    var stars: [Star]
    var preview: Preview
    var whiteFlux: [Double]
    var limitations: [String]
    static func triplet(_ a: [Double?]) -> Bool { a.count == 3 && a.allSatisfy { $0 == nil || $0!.isFinite } }
    static func digest(_ s: String) -> Bool { s.count == 64 && s.utf8.allSatisfy { (48...57).contains($0) || (97...102).contains($0) } }
    var isValid: Bool {
        guard schema == 1, algorithm == "PhotonStack-SPCC-1", source.hasPrefix("/"), Self.digest(sourceSHA),
              (1...100_000).contains(width), (1...100_000).contains(height), (0...100_000).contains(detected),
              profile.isValid,
              Self.digest(profile.sha), Self.digest(catalog.sha), catalog.epoch.isFinite, (1800...2200).contains(catalog.epoch),
              options.isValid, stars.count <= 10_000, Set(stars.map(\.id)).count == stars.count, stars.allSatisfy(\.isValid),
              Set(options.excluded).isSubset(of: Set(stars.map(\.id))),
              preview.resampling == "area-coverage", preview.low.isFinite, preview.high.isFinite, preview.high >= preview.low,
              whiteFlux.count == 3, whiteFlux.allSatisfy({ $0.isFinite && $0 > 0 }),
              [fit.training, fit.validation, fit.rejected].allSatisfy({ (0...stars.count).contains($0) }), (0...16).contains(fit.regions),
              [fit.gains, fit.instrumentGains, fit.whiteRatios, fit.trainingScatter, fit.validationMedian, fit.validationP90].allSatisfy(Self.triplet)
        else { return false }
        if fit.valid {
            return fit.training >= 12 && fit.validation >= 6 && fit.regions >= 4 && fit.gains[1] == 1 &&
                fit.gains.allSatisfy { $0 != nil && (0.05...20).contains($0!) } &&
                fit.validationP90.allSatisfy { $0 != nil && $0! >= 0 } &&
                fit.validationP90[0]! <= options.maxValidationLogError && fit.validationP90[2]! <= options.maxValidationLogError &&
                [fit.instrumentGains, fit.whiteRatios].allSatisfy { $0.allSatisfy { $0 != nil && $0! > 0 } } &&
                stars.filter { $0.status == "validation" }.allSatisfy(\.heldOut) &&
                stars.filter { $0.status == "fit" || $0.status == "fit-outlier" }.allSatisfy { !$0.heldOut } &&
                stars.filter { $0.status == "fit" }.count == fit.training &&
                stars.filter { $0.status == "validation" }.count == fit.validation &&
                stars.filter { $0.status == "fit-outlier" }.count == fit.rejected
        }
        return !fit.reason.isEmpty
    }
}
