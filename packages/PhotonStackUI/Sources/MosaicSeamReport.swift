import Foundation

public struct MosaicSeamStatistics: Codable, Equatable, Sendable {
    public var samples: Int
    public var firstMedian, secondMedian, medianDifference, rmsDifference, p95Absolute, madDifference: [Double]
    var isValid: Bool {
        samples > 0 && [firstMedian, secondMedian, medianDifference, rmsDifference, p95Absolute, madDifference]
            .allSatisfy { $0.count == 3 && $0.allSatisfy(\.isFinite) } &&
        [rmsDifference, p95Absolute, madDifference].allSatisfy { $0.allSatisfy { $0 >= 0 } }
    }
}
public struct MosaicSeamRegion: Codable, Equatable, Sendable {
    public var bounds: [Double]
    public var candidates: Int
    public var before, after: MosaicSeamStatistics?
    func isValid(width: Int, height: Int) -> Bool {
        guard bounds.count == 4, bounds.allSatisfy(\.isFinite),
              bounds[0] >= 0, bounds[1] >= 0, bounds[2] <= Double(width), bounds[3] <= Double(height),
              bounds[2] > bounds[0], bounds[3] > bounds[1], (0...2304).contains(candidates) else { return false }
        if let before, let after {
            return before.isValid && after.isValid && before.samples == after.samples && before.samples <= candidates
        }
        return before == nil && after == nil
    }
}
public struct MosaicSeamPair: Codable, Equatable, Sendable, Identifiable {
    public var first, second: Int
    public var summary: MosaicSeamRegion
    public var cells: [MosaicSeamRegion]
    public var id: String { "\(first)-\(second)" }
}
public struct MosaicExposureCorrection: Codable, Equatable, Sendable {
    public var gains, offsets: [Double]
}
public struct MosaicSeamReport: Codable, Equatable, Sendable {
    public var version, width, height, channels, gridLimit, patchRadius: Int
    public var preview: Bool
    public var encoding: String
    public var minimumCoverage: Double
    public var inputs: [String]
    public var corrections: [MosaicExposureCorrection]
    public var pairs: [MosaicSeamPair]

    func isValid(panelCount: Int) -> Bool {
        guard version == 1, width > 0, height > 0, (1...3).contains(channels), gridLimit == 48,
              patchRadius == 1, minimumCoverage == 0.99, ["linear", "srgb", "unknown"].contains(encoding),
              inputs.count == panelCount, inputs.allSatisfy({ !$0.isEmpty }),
              corrections.count == panelCount, Set(pairs.map(\.id)).count == pairs.count else { return false }
        guard corrections.allSatisfy({ c in
            c.gains.count == 3 && c.offsets.count == 3 && c.gains.allSatisfy { $0.isFinite && $0 > 0 } && c.offsets.allSatisfy(\.isFinite)
        }) else { return false }
        return pairs.allSatisfy { pair in
            pair.first >= 0 && pair.second > pair.first && pair.second < panelCount && pair.cells.count == 16 &&
            pair.summary.isValid(width: width, height: height) && pair.cells.allSatisfy { cell in
                cell.isValid(width: width, height: height) && cell.bounds[0] >= pair.summary.bounds[0] &&
                cell.bounds[1] >= pair.summary.bounds[1] && cell.bounds[2] <= pair.summary.bounds[2] && cell.bounds[3] <= pair.summary.bounds[3]
            } && pair.cells.reduce(0, { $0 + $1.candidates }) == pair.summary.candidates &&
            pair.cells.reduce(0, { $0 + ($1.before?.samples ?? 0) }) == (pair.summary.before?.samples ?? 0)
        }
    }
}
