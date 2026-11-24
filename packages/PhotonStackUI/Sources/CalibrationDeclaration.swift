import Foundation

enum CalibrationDeclaration {
    struct Field: Sendable { let key, zh, en: String }
    static let fields: [Field] = [
        .init(key: "CAM-SER", zh: "相机编号", en: "Camera serial"), .init(key: "INSTRUME", zh: "相机型号", en: "Camera model"),
        .init(key: "FILTER", zh: "滤镜", en: "Filter"), .init(key: "PSOPTICS", zh: "光路标识", en: "Optical train"),
        .init(key: "READMODE", zh: "读出模式", en: "Readout mode"), .init(key: "GAIN", zh: "增益", en: "Gain"),
        .init(key: "OFFSET", zh: "偏置设定", en: "Offset setting"), .init(key: "EXPTIME", zh: "曝光（秒）", en: "Exposure (s)"),
        .init(key: "CCD-TEMP", zh: "实测温度（°C）", en: "Measured temperature (°C)"),
        .init(key: "XBINNING", zh: "横向 binning", en: "Horizontal binning"), .init(key: "YBINNING", zh: "纵向 binning", en: "Vertical binning"),
        .init(key: "XORGSUBF", zh: "传感器起点 X", en: "Sensor origin X"), .init(key: "YORGSUBF", zh: "传感器起点 Y", en: "Sensor origin Y"),
        .init(key: "OBJECT", zh: "目标", en: "Object"), .init(key: "DATE-OBS", zh: "拍摄时间（UTC）", en: "Observation time (UTC)")]
}

extension CalibrationLibrary {
    func declareAcquisition(_ inputs: [URL], fields: [String: String], reason: String, to destination: URL) async throws -> [URL] {
        guard !inputs.isEmpty, inputs.count <= 4096, !fields.isEmpty, Self.validLabel(reason) else { throw fail("Choose inputs, declarations and a reason.") }
        var manifest = "PhotonStack-AcquisitionDeclaration-1\nreason \(try CalibrationJobDraft.quote(reason))\n"
        for (key, value) in fields.sorted(by: { $0.key < $1.key }) {
            guard CalibrationDeclaration.fields.contains(where: { $0.key == key }) else { throw fail("Unsupported declaration field.") }
            manifest += "set \(key) \(try CalibrationJobDraft.quote(value))\n"
        }
        var snapshots: [CalibrationAcquisition] = []
        for start in stride(from: 0, to: inputs.count, by: 256) {
            snapshots += try await CalibrationAcquisition.inspect(Array(inputs[start..<min(start + 256, inputs.count)]), service: service)
        }
        for snapshot in snapshots { manifest += "source \(try CalibrationJobDraft.quote(snapshot.path)) \(snapshot.sha)\n" }
        let scratch = fm.temporaryDirectory.appendingPathComponent("AcquisitionDeclaration-" + UUID().uuidString)
        try fm.createDirectory(at: scratch, withIntermediateDirectories: false); defer { try? fm.removeItem(at: scratch) }
        let file = scratch.appendingPathComponent("declarations.txt")
        try manifest.write(to: file, atomically: true, encoding: .utf8)
        let response = try await service.runCLI(arguments: ["calibration-declare", "--manifest", file.path, "--output-dir", destination.path])
        struct Report: Decodable {
            struct Frame: Decodable { var source, sourceSHA, output, outputSHA: String }
            var type, command, reason: String; var schema: Int; var manualDeclarations: Bool
            var frames: [Frame]; var declarations: [String: String]
        }
        let report = try JSONDecoder().decode(Report.self, from: Data(response.standardOutput.utf8))
        guard report.schema == 1, report.type == "complete", report.command == "calibration-declare", report.manualDeclarations,
              report.frames.count == snapshots.count, report.declarations == fields, report.reason == reason else { throw fail("Invalid declaration report.") }
        return try report.frames.enumerated().map { i, frame in
            guard frame.source == snapshots[i].path, frame.sourceSHA == snapshots[i].sha,
                  frame.output == String(format: "frame-%06d.fits", i + 1) else { throw fail("Declaration output does not match its source.") }
            let url = destination.appendingPathComponent(frame.output); try regular(url)
            guard try Self.fileDigest(url) == frame.outputSHA else { throw fail("Declared copy changed.") }
            return url
        }
    }
}
