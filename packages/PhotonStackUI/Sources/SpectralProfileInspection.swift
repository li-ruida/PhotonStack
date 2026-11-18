import Foundation
import PhotonStackProcessing

struct SpectralNarrowbandModel: Codable, Equatable, Sendable {
    var layout: String
    var baseSHA: String
    var model = "trapezoid-edge-1e-4"
    var centers: [Double]
    var widths: [Double]
    var isValid: Bool {
        guard ["mono", "osc-dual", "extracted-hoo"].contains(layout),
              model == "trapezoid-edge-1e-4", SpectralCalibrationReport.digest(baseSHA),
              centers.count == 3, widths.count == 3 else { return false }
        for c in 0..<3 {
            guard centers[c].isFinite, widths[c].isFinite, (3...100).contains(widths[c]),
                  centers[c] - widths[c]/2 >= 336, centers[c] + widths[c]/2 <= 1020 else { return false }
        }
        return layout == "mono" || (centers[1] == centers[2] && widths[1] == widths[2] &&
            abs(centers[0] - centers[1]) > (widths[0] + widths[1])/2)
    }
    func title(chinese: Bool) -> String {
        switch layout {
        case "mono": chinese ? "单色滤镜合色" : "Mono filter composite"
        case "osc-dual": chinese ? "OSC 双窄带 RGB" : "OSC dual-band RGB"
        default: chinese ? "提取后的 HOO" : "Extracted HOO"
        }
    }
}

struct SpectralProfileMetadata: Codable, Equatable, Sendable {
    var name, mode, quality, provenance, license, sha: String
    var whiteName, whiteProvenance, whiteLicense: String
    var narrowband: SpectralNarrowbandModel?
    var isValid: Bool {
        ((mode == "broadband" && narrowband == nil && ["measured", "approximate", "synthetic"].contains(quality)) ||
         (mode == "narrowband" && quality == "approximate" && narrowband?.isValid == true)) &&
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
