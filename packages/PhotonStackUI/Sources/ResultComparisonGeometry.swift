import AppKit
import Foundation
import CryptoKit
import PhotonStackAppCore

struct ResultComparisonSources: Sendable {
    let currentReport: URL
    let referenceReport: URL
}

enum ResultComparisonGeometryError: Error {
    case unavailable, unrelated, inconsistent, noOverlap, changedOutput
}

struct ResultComparisonGeometry: Sendable {
    let current: CGRect
    let reference: CGRect
    let common: CGRect

    /// Integer crops only: never resample or register two developed pictures.
    static func overlap(current: CGRect, reference: CGRect) throws -> Self {
        for rect in [current, reference] {
            guard [rect.minX, rect.minY, rect.width, rect.height].allSatisfy({
                $0.isFinite && $0 >= 0 && $0.rounded() == $0 && $0 <= 1_000_000
            }), rect.width > 0, rect.height > 0 else { throw ResultComparisonGeometryError.inconsistent }
        }
        let common = current.intersection(reference)
        guard !common.isNull, !common.isEmpty else { throw ResultComparisonGeometryError.noOverlap }
        return Self(current: common.offsetBy(dx: -current.minX, dy: -current.minY),
                    reference: common.offsetBy(dx: -reference.minX, dy: -reference.minY), common: common)
    }

    func crop(current: CGImage, reference: CGImage) throws -> ResultComparisonPair {
        guard CGRect(x: 0, y: 0, width: current.width, height: current.height).contains(self.current),
              CGRect(x: 0, y: 0, width: reference.width, height: reference.height).contains(self.reference),
              let a = current.cropping(to: self.current), let b = reference.cropping(to: self.reference) else {
            throw ResultComparisonGeometryError.inconsistent
        }
        return ResultComparisonPair(current: a, reference: b)
    }
}

/// Report IO stays off the UI actor. A legacy report without an immutable master
/// digest cannot prove shared geometry; retain the explicit manual-file route.
actor ResultComparisonProvenanceLoader {
    func load(_ sources: ResultComparisonSources, currentURL: URL, referenceURL: URL,
              currentSize: CGSize, referenceSize: CGSize) throws -> ResultComparisonGeometry {
        func read(_ url: URL, output: URL, size: CGSize) throws -> DeepSkyReport {
            guard let data = try? Data(contentsOf: url),
                  let report = try? JSONDecoder().decode(DeepSkyReport.self, from: data),
                  (try? report.validate()) != nil, report.success, !report.analysisOnly,
                  let crop = report.crop, let digest = report.masterSHA256,
                  digest.count == 64, digest.allSatisfy({ $0.isHexDigit }),
                  let outputDigest = report.developedSHA256, outputDigest.count == 64,
                  URL(fileURLWithPath: report.developed).standardizedFileURL == output.standardizedFileURL else {
                throw ResultComparisonGeometryError.unavailable
            }
            guard Double(crop.width) == size.width, Double(crop.height) == size.height else {
                throw ResultComparisonGeometryError.inconsistent
            }
            let file = try FileHandle(forReadingFrom: output)
            defer { try? file.close() }
            var hash = SHA256()
            while let data = try file.read(upToCount: 1_048_576), !data.isEmpty {
                try Task.checkCancellation()
                hash.update(data: data)
            }
            guard hash.finalize().map({ String(format: "%02x", $0) }).joined() == outputDigest else {
                throw ResultComparisonGeometryError.changedOutput
            }
            return report
        }
        try Task.checkCancellation()
        let a = try read(sources.currentReport, output: currentURL, size: currentSize)
        let b = try read(sources.referenceReport, output: referenceURL, size: referenceSize)
        guard a.masterSHA256 == b.masterSHA256 else { throw ResultComparisonGeometryError.unrelated }
        guard let ac = a.crop, let bc = b.crop else { throw ResultComparisonGeometryError.unavailable }
        return try ResultComparisonGeometry.overlap(
            current: CGRect(x: ac.x, y: ac.y, width: ac.width, height: ac.height),
            reference: CGRect(x: bc.x, y: bc.y, width: bc.width, height: bc.height))
    }
}
