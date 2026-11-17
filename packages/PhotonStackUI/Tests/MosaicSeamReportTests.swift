import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

private func seamFixture() -> MosaicSeamReport {
    let stats = MosaicSeamStatistics(samples: 1, firstMedian: [10,20,30], secondMedian: [11,22,33],
        medianDifference: [1,2,3], rmsDifference: [1,2,3], p95Absolute: [1,2,3], madDifference: [0,0,0])
    let region = MosaicSeamRegion(bounds: [0,0,16,16], candidates: 16,
        before: .init(samples: 16, firstMedian: stats.firstMedian, secondMedian: stats.secondMedian,
                      medianDifference: stats.medianDifference, rmsDifference: stats.rmsDifference,
                      p95Absolute: stats.p95Absolute, madDifference: stats.madDifference),
        after: .init(samples: 16, firstMedian: stats.firstMedian, secondMedian: stats.secondMedian,
                     medianDifference: stats.medianDifference, rmsDifference: stats.rmsDifference,
                     p95Absolute: stats.p95Absolute, madDifference: stats.madDifference))
    let cells: [MosaicSeamRegion] = (0..<16).map { i in
        let x = Double(i % 4) * 4
        let y = Double(i / 4) * 4
        return MosaicSeamRegion(bounds: [x, y, x + 4, y + 4], candidates: 1, before: stats, after: stats)
    }
    return MosaicSeamReport(version: 1, width: 16, height: 16, channels: 3, gridLimit: 48, patchRadius: 1,
        preview: false, encoding: "linear", minimumCoverage: 0.99, inputs: ["/tmp/east.fits", "/tmp/west.fits"],
        corrections: [.init(gains: [1,1,1], offsets: [0,0,0]),.init(gains: [1,1,1], offsets: [0,0,0])],
        pairs: [.init(first: 0, second: 1, summary: region, cells: cells)])
}
private func mosaicObject() -> [String: Any] {
    ["type":"complete", "command":"mosaic", "autoAligned": false, "matches": 0, "fallbackPanels": 0,
     "exposureMatched": false, "blend": "feather", "placements": (0..<2).map { i -> [String: Any] in
         ["index":i, "x":0, "y":0, "a":1, "b":0, "c":0, "d":1, "dx":0, "dy":0, "model":"manual", "matches":0,
          "references":0, "autoAligned":false, "fallback":false, "reducedModel":false, "coarseAlignment":false]
     }]
}
private func decodeMosaic(_ object: [String: Any]) throws -> MosaicQualityReport {
    try WorkspaceReportDecoder.parseMosaicQualityReport(.init(command: ["mosaic"], exitCode: 0,
        standardOutput: String(decoding: try JSONSerialization.data(withJSONObject: object), as: UTF8.self),
        standardError: ""), expectedPanelCount: 2)
}
@Test func mosaicSeamReportRoundTripsWithProjectAndAcceptsLegacyAbsence() throws {
    let legacy = try decodeMosaic(mosaicObject()); #expect(legacy.seams == nil)
    var object = mosaicObject()
    let fixture = seamFixture()
    object["seams"] = try JSONSerialization.jsonObject(with: JSONEncoder().encode(fixture))
    let decoded = try decodeMosaic(object)
    #expect(decoded.seams == fixture)
    let saved = try JSONEncoder().encode(decoded)
    #expect(try JSONDecoder().decode(MosaicQualityReport.self, from: saved) == decoded)
    // Old project JSON also lacks the new optional property.
    var old = try #require(JSONSerialization.jsonObject(with: JSONEncoder().encode(legacy)) as? [String: Any])
    old.removeValue(forKey: "seams")
    #expect(try JSONDecoder().decode(MosaicQualityReport.self, from: JSONSerialization.data(withJSONObject: old)).seams == nil)
}
@Test func mosaicSeamReportRejectsCorruptIndicesCountsBoundsAndNonfiniteData() throws {
    let valid = seamFixture(); #expect(valid.isValid(panelCount: 2))
    var variants: [MosaicSeamReport] = []
    var bad = valid; bad.pairs[0].second = 2; variants.append(bad)
    bad = valid; bad.pairs.append(bad.pairs[0]); variants.append(bad)
    bad = valid; bad.pairs[0].cells.removeLast(); variants.append(bad)
    bad = valid; bad.pairs[0].cells[0].before?.samples = 2; variants.append(bad)
    bad = valid; bad.pairs[0].cells[0].bounds[2] = 20; variants.append(bad)
    bad = valid; bad.pairs[0].summary.before?.medianDifference = [1]; variants.append(bad)
    bad = valid; bad.corrections[0].gains = [1]; variants.append(bad)
    for report in variants {
        #expect(!report.isValid(panelCount: 2))
        var object = mosaicObject(); object["seams"] = try JSONSerialization.jsonObject(with: JSONEncoder().encode(report))
        do { _ = try decodeMosaic(object); Issue.record("Malformed seam report accepted") } catch {}
    }
    bad = valid; bad.pairs[0].summary.after?.rmsDifference[0] = .nan
    #expect(!bad.isValid(panelCount: 2))
    bad = valid; bad.pairs[0].summary.after?.rmsDifference[0] = -1
    #expect(!bad.isValid(panelCount: 2))
}
