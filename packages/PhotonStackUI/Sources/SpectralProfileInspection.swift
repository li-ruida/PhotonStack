import Foundation
import PhotonStackProcessing

struct SpectralProfileMetadata: Codable, Equatable, Sendable {
    var name, mode, quality, provenance, license, sha: String
    var whiteName, whiteProvenance, whiteLicense: String
    var isValid: Bool {
        mode == "broadband" && ["measured", "approximate", "synthetic"].contains(quality) &&
        SpectralCalibrationReport.digest(sha) &&
        [name, provenance, license, whiteName, whiteProvenance, whiteLicense].allSatisfy {
            !$0.isEmpty && $0.utf8.count <= 4096 && !$0.unicodeScalars.contains { $0.value < 32 || $0.value == 127 }
        }
    }
}

struct SpectralProfileInspection: Decodable, Sendable {
    struct Band: Decodable, Sendable {
        var components: Int
        var support: [Double]
        var meanWavelength, rmsBandwidth, whiteFlux: Double
        var samples: [[Double]]
        var isValid: Bool {
            (1...8).contains(components) && support.count == 2 && support.allSatisfy(\.isFinite) &&
            support[0] > 0 && support[0] < support[1] && support[1] <= 1e6 &&
            meanWavelength.isFinite && (support[0]...support[1]).contains(meanWavelength) &&
            rmsBandwidth.isFinite && rmsBandwidth >= 0 && rmsBandwidth <= support[1] - support[0] &&
            whiteFlux.isFinite && whiteFlux > 0 && SpectralProfileInspection.validSamples(samples, response: true) &&
            samples.first?.first == support[0] && samples.last?.first == support[1]
        }
    }
    var type, command, plotMethod: String
    var schema: Int
    var profile: SpectralProfileMetadata
    var bands: [Band]
    var whiteNormalization: Double
    var whiteSamples: [[Double]]
    static func validSamples(_ samples: [[Double]], response: Bool) -> Bool {
        guard (2...2048).contains(samples.count) else { return false }
        var previous = 0.0
        for p in samples {
            guard p.count == 2, p.allSatisfy(\.isFinite), p[0] > previous, p[0] <= 1e6,
                  (response ? 0.0 : -1.0)...1.0 ~= p[1] else { return false }
            previous = p[0]
        }
        return true
    }
    var isValid: Bool {
        type == "complete" && command == "spcc-profile-inspect" && schema == 1 &&
        plotMethod == "sampled-minmax-512" && profile.isValid && bands.count == 3 && bands.allSatisfy(\.isValid) &&
        whiteNormalization.isFinite && whiteNormalization > 0 && Self.validSamples(whiteSamples, response: false)
    }
    static func read(_ url: URL, expectedSHA: String? = nil, service: any ProcessingService) async throws -> Self {
        var args = ["spcc-profile-inspect", "--profile", url.path]
        if let expectedSHA { args += ["--expected-sha", expectedSHA] }
        let response = try await service.runCLI(arguments: args)
        let decoded = try JSONDecoder().decode(Self.self, from: Data(response.standardOutput.utf8))
        guard decoded.isValid, expectedSHA == nil || expectedSHA == decoded.profile.sha else {
            throw ProcessingServiceError.invalidReport(command: "spcc-profile-inspect")
        }
        try Task.checkCancellation()
        return decoded
    }
}
