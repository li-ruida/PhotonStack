import Foundation

struct CalibrationQualityReport: Codable, Sendable {
    struct Phase: Codable, Sendable {
        var median, madSigma, minimum, maximum: Double
        var finite, invalid, low, high: Int
    }
    var schema, width, height, rejected: Int
    var type, command, source, sourceSHA, role, backend: String
    var candidateOnly: Bool
    var sigma, absoluteTolerance, flatMin, flatMax: Double
    var phases: [Phase]
    var products: [String: String]
    var isValid: Bool {
        guard schema == 1, type == "complete", command == "sensor-quality", candidateOnly, backend == "cpu",
              width > 0, height > 0, width <= 100000, height <= 100000, rejected >= 0, rejected <= width * height,
              [1,4].contains(phases.count), SpectralCalibrationReport.digest(sourceSHA),
              Set(products.keys) == ["bad-pixels.fits", "preview.fits", "preview.png"], products.values.allSatisfy(SpectralCalibrationReport.digest),
              [sigma, absoluteTolerance, flatMin, flatMax].allSatisfy(\.isFinite) else { return false }
        return phases.allSatisfy { p in
            [p.median,p.madSigma,p.minimum,p.maximum].allSatisfy(\.isFinite) && p.madSigma >= 0 && p.minimum <= p.median && p.median <= p.maximum &&
            p.finite > 0 && p.finite <= width * height && (0...width * height).contains(p.invalid) &&
            (0...p.finite).contains(p.low) && (0...p.finite).contains(p.high) && p.low + p.high <= p.finite
        } && phases.reduce(0) { $0 + $1.finite + $1.invalid } == width * height &&
        phases.reduce(0) { $0 + $1.invalid + $1.low + $1.high } == rejected
    }
}

extension CalibrationLibrary {
    func inspectBadPixels(_ url: URL) async throws -> CalibrationAcquisition {
        try await CalibrationAcquisition.inspect([url], service: service)[0]
    }
    func inspectQuality(_ entry: Entry, sigma: Double, tolerance: Double, flatMin: Double, flatMax: Double,
                        to destination: URL) async throws -> CalibrationQualityReport {
        let acquisition = try await verify(entry)
        let response = try await service.runCLI(arguments: ["sensor-quality", "--input", acquisition.path, "--expected-sha", acquisition.sha,
            "--sigma", String(sigma), "--absolute-tolerance", String(tolerance), "--flat-min", String(flatMin), "--flat-max", String(flatMax), "--output-dir", destination.path])
        let result = try JSONDecoder().decode(CalibrationQualityReport.self, from: Data(response.standardOutput.utf8))
        guard result.isValid, result.source == acquisition.path, result.sourceSHA == acquisition.sha,
              result.width == acquisition.width, result.height == acquisition.height,
              result.sigma == sigma, result.absoluteTolerance == tolerance, result.flatMin == flatMin, result.flatMax == flatMax else { throw fail("Invalid quality report.") }
        for (name, sha) in result.products {
            let file = destination.appendingPathComponent(name); try regular(file)
            guard try Self.fileDigest(file) == sha else { throw fail("Quality product changed.") }
        }
        return result
    }
}
