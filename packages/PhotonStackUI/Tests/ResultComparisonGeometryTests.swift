import AppKit
import Foundation
import CryptoKit
import PhotonStackAppCore
import Testing
@testable import PhotonStackUI

@Test func resultComparisonFinishingUsesMatchingBranchAndRejectsStaleInputs() throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let input = root.appendingPathComponent("before.tiff")
    let output = root.appendingPathComponent("after.tiff")
    let unrelated = root.appendingPathComponent("other.tiff")
    for url in [input, output, unrelated] { try Data([0]).write(to: url) }
    let kinds: [(ProcessingOperationKind, String)] = [
        (.localContrast, "continuum-v1"), (.denoise, "display-grid-v1"), (.colorNeutralize, "removeGreen")
    ]
    for (kind, mode) in kinds {
        var adjustment = EditOperation(kind: kind, parameters: [
            "mode": mode, "input": input.path, "output": output.path
        ])
        let otherBranch = EditOperation(kind: kind, parameters: [
            "mode": mode, "input": output.path, "output": unrelated.path
        ])
        #expect(ResultComparisonHistory.finishingInput(current: output, operations: [adjustment, otherBranch]) == input)
        adjustment.isEnabled = false
        #expect(ResultComparisonHistory.finishingInput(current: output, operations: [adjustment]) == nil)
        adjustment.isEnabled = true
        for invalidInput in ["", output.path, root.appendingPathComponent("missing.tiff").path] {
            adjustment.parameters["input"] = invalidInput
            #expect(ResultComparisonHistory.finishingInput(current: output, operations: [adjustment]) == nil)
        }
    }
    let raw = root.appendingPathComponent("linear.fits")
    try Data([0]).write(to: raw)
    let scientific = EditOperation(kind: .localContrast, parameters: [
        "mode": "continuum-v1", "input": raw.path, "output": output.path
    ])
    #expect(ResultComparisonHistory.finishingInput(current: output, operations: [scientific]) == nil)
    let unsupported = EditOperation(kind: .localContrast, parameters: ["input": input.path, "output": output.path])
    #expect(ResultComparisonHistory.finishingInput(current: output, operations: [unsupported]) == nil)
    #expect(ResultComparisonHistory.finishingInput(current: nil, operations: [unsupported]) == nil)
}

@Test func resultComparisonUsesCommonCoordinatesWithoutResampling() throws {
    var pixels = [UInt8]()
    for y in 0..<8 { for x in 0..<10 { pixels += [UInt8(x * 10), UInt8(y * 10), 70, 255] } }
    let provider = try #require(CGDataProvider(data: Data(pixels) as CFData))
    let canvas = try #require(CGImage(width: 10, height: 8, bitsPerComponent: 8, bitsPerPixel: 32,
        bytesPerRow: 40, space: CGColorSpaceCreateDeviceRGB(), bitmapInfo: CGBitmapInfo(rawValue: CGImageAlphaInfo.last.rawValue),
        provider: provider, decode: nil, shouldInterpolate: false, intent: .defaultIntent))
    let a = CGRect(x: 1, y: 2, width: 6, height: 5), b = CGRect(x: 3, y: 1, width: 6, height: 5)
    let geometry = try ResultComparisonGeometry.overlap(current: a, reference: b)
    #expect(geometry.common == CGRect(x: 3, y: 2, width: 4, height: 4))
    let pair = try geometry.crop(current: #require(canvas.cropping(to: a)), reference: #require(canvas.cropping(to: b)))
    #expect(pair.current.width == 4 && pair.reference.height == 4)
    let current = NSBitmapImageRep(cgImage: pair.current), reference = NSBitmapImageRep(cgImage: pair.reference)
    for y in 0..<4 { for x in 0..<4 {
        var ac = [Int](repeating: 0, count: 4), bc = ac
        current.getPixel(&ac, atX: x, y: y); reference.getPixel(&bc, atX: x, y: y)
        #expect(ac == bc)
        #expect(ac[0] == (x + 3) * 10 && ac[1] == (y + 2) * 10)
    } }
    // Equal dimensions alone never imply that source coordinates coincide.
    #expect(geometry.current.origin != geometry.reference.origin)
}

@Test func resultComparisonRejectsInvalidOrDisjointCrops() throws {
    let valid = CGRect(x: 0, y: 0, width: 20, height: 20)
    for rect in [CGRect(x: 20, y: 0, width: 20, height: 20), CGRect(x: -1, y: 0, width: 20, height: 20),
                 CGRect(x: 0.5, y: 0, width: 10, height: 10), CGRect(x: 0, y: 0, width: 0, height: 2),
                 CGRect(x: 2_000_000, y: 0, width: 2, height: 2)] {
        #expect(throws: ResultComparisonGeometryError.self) {
            try ResultComparisonGeometry.overlap(current: valid, reference: rect)
        }
    }
}

private func comparisonReport(output: URL, digest: String?, x: Int = 0) throws -> Data {
    var object: [String: Any] = ["version": 1, "success": true, "analysisOnly": false, "inputCount": 0,
        "selectedCount": 0, "referenceIndex": 0, "recipe": "", "errorCode": "", "message": "",
        "master": "/same-path/master.fits", "backgroundMaster": "", "developed": output.path,
        "rejectionLow": "", "rejectionHigh": "", "frames": [],
        "crop": ["x": x, "y": 2, "width": 20, "height": 30]]
    object["developedSHA256"] = SHA256.hash(data: try Data(contentsOf: output)).map { String(format: "%02x", $0) }.joined()
    if let digest { object["masterSHA256"] = digest }
    return try JSONSerialization.data(withJSONObject: object)
}

@Test func resultComparisonRequiresMatchingMasterIdentityAndOutputGeometry() async throws {
    let root = FileManager.default.temporaryDirectory.appendingPathComponent(UUID().uuidString)
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let a = root.appendingPathComponent("a.json"), b = root.appendingPathComponent("b.json")
    let au = root.appendingPathComponent("a.tiff"), bu = root.appendingPathComponent("b.tiff")
    try Data("current output".utf8).write(to: au)
    try Data("previous output".utf8).write(to: bu)
    let sources = ResultComparisonSources(currentReport: a, referenceReport: b)
    let loader = ResultComparisonProvenanceLoader(), size = CGSize(width: 20, height: 30)
    let digest = String(repeating: "a", count: 64)
    try comparisonReport(output: au, digest: digest, x: 4).write(to: a)
    try comparisonReport(output: bu, digest: digest).write(to: b)
    let geometry = try await loader.load(sources, currentURL: au, referenceURL: bu, currentSize: size, referenceSize: size)
    #expect(geometry.common == CGRect(x: 4, y: 2, width: 16, height: 30))
    // Same master path with different content identities must not auto-align.
    try comparisonReport(output: bu, digest: String(repeating: "b", count: 64)).write(to: b)
    await #expect(throws: ResultComparisonGeometryError.self) {
        try await loader.load(sources, currentURL: au, referenceURL: bu, currentSize: size, referenceSize: size)
    }
    // Legacy data still decodes, but cannot assert verified geometry.
    let legacy = try comparisonReport(output: bu, digest: nil)
    #expect(try JSONDecoder().decode(DeepSkyReport.self, from: legacy).masterSHA256 == nil)
    try legacy.write(to: b)
    await #expect(throws: ResultComparisonGeometryError.self) {
        try await loader.load(sources, currentURL: au, referenceURL: bu, currentSize: size, referenceSize: size)
    }
    try comparisonReport(output: bu, digest: digest).write(to: b)
    await #expect(throws: ResultComparisonGeometryError.self) {
        try await loader.load(sources, currentURL: au, referenceURL: bu, currentSize: CGSize(width: 21, height: 30), referenceSize: size)
    }
    await #expect(throws: ResultComparisonGeometryError.self) {
        try await loader.load(sources, currentURL: bu, referenceURL: au, currentSize: size, referenceSize: size)
    }
    try Data("modified output".utf8).write(to: bu)
    await #expect(throws: ResultComparisonGeometryError.self) {
        try await loader.load(sources, currentURL: au, referenceURL: bu, currentSize: size, referenceSize: size)
    }
}
