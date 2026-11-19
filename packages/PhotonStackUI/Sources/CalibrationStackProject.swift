import CryptoKit
import Foundation
import PhotonStackAppCore

struct CalibrationStackPlan: Sendable {
    struct Group: Identifiable, Sendable {
        var id: Int
        var frames: [Int]
        var acquisition: CalibrationAcquisition
        var complete: Bool
    }
    let directory: URL
    let report: CalibrationJobReport
    let groups: [Group]
    let acquisitions: [Int: CalibrationAcquisition]

    func frames(in selected: Set<Int>) throws -> [Int] {
        let included = groups.filter { selected.contains($0.id) }
        guard !selected.isEmpty, included.count == selected.count, included.allSatisfy(\.complete) else {
            throw CalibrationAcquisition.failure("Select complete calibration groups.")
        }
        let frames = included.flatMap(\.frames).sorted()
        guard (3...2000).contains(frames.count), let first = acquisitions[frames[0]] else {
            throw CalibrationAcquisition.failure("A stack requires 3–2000 completed frames.")
        }
        for index in frames {
            guard let other = acquisitions[index], Self.compatible(first, other) else {
                throw CalibrationAcquisition.failure("Keep different targets, filters, exposures, sensor layouts and acquisition settings in separate stacks.")
            }
        }
        return frames
    }
    private static func compatible(_ a: CalibrationAcquisition, _ b: CalibrationAcquisition) -> Bool {
        guard a.width == b.width, a.height == b.height, a.channels == 1, b.channels == 1 else { return false }
        // UTC dates and temperatures may differ after per-frame calibration;
        // exposure is kept equal because this handoff does not rescale flux.
        for key in ["object", "filter", "camera", "cameraSerial", "readoutMode", "sampling", "opticalTrain", "units", "domain"] {
            guard let value = a.text[key], !value.isEmpty, value == b.text[key] else { return false }
        }
        for key in ["exposure", "gain", "offset", "binX", "binY", "originX", "originY"] {
            guard let value = a.numeric[key], value == b.numeric[key] else { return false }
        }
        return true
    }
}

extension CalibrationLibrary {
    func stackPlan(directory: URL, expected: CalibrationJobReport) async throws -> CalibrationStackPlan {
        let report = try await inspectCalibration(directory)
        guard report.jobSHA == expected.jobSHA else { throw fail("Calibration task changed; reopen it before preparing a stack.") }
        let frames = report.frames.filter(\.complete)
        var acquisitions: [Int: CalibrationAcquisition] = [:]
        for start in stride(from: 0, to: frames.count, by: 256) {
            let batch = Array(frames[start..<min(start + 256, frames.count)])
            let urls = batch.map { directory.appendingPathComponent($0.directory).appendingPathComponent("calibrated.fits") }
            for (frame, acquisition) in zip(batch, try await CalibrationAcquisition.inspect(urls, service: service)) {
                guard acquisition.issues.isEmpty, acquisition.channels == 1, acquisition.text["calibration"] == "sensor-v1",
                      acquisition.text["biasState"] == "removed", acquisition.text["domain"] == "linear",
                      acquisition.sha == frame.products["calibrated.fits"] else { throw fail("Invalid or changed calibrated sensor output.") }
                acquisitions[frame.index] = acquisition
            }
        }
        let groups = Dictionary(grouping: report.frames, by: \.group).keys.sorted().compactMap { group -> CalibrationStackPlan.Group? in
            let members = report.frames.filter { $0.group == group }
            guard let first = members.compactMap({ acquisitions[$0.index] }).first else { return nil }
            return .init(id: group, frames: members.map(\.index), acquisition: first, complete: members.allSatisfy(\.complete))
        }
        return CalibrationStackPlan(directory: directory, report: report, groups: groups, acquisitions: acquisitions)
    }
    static func fileDigest(_ url: URL) throws -> String {
        let handle = try FileHandle(forReadingFrom: url); defer { try? handle.close() }
        var hash = SHA256()
        while let data = try handle.read(upToCount: 1024 * 1024), !data.isEmpty {
            try Task.checkCancellation(); hash.update(data: data)
        }
        return hash.finalize().map { String(format: "%02x", $0) }.joined()
    }
    func createStackProject(_ plan: CalibrationStackPlan, groups: Set<Int>, name: String, destination: URL) async throws -> URL {
        guard Self.validLabel(name) else { throw fail("Choose a project name.") }
        let indices = try plan.frames(in: groups)
        let current = try await inspectCalibration(plan.directory)
        guard current.jobSHA == plan.report.jobSHA else { throw fail("Calibration task changed before project creation.") }
        for i in indices {
            guard current.frames[i].complete, current.frames[i].products == plan.report.frames[i].products else {
                throw fail("Selected calibrated products changed; reopen the task.")
            }
        }
        let target = destination.standardizedFileURL.resolvingSymlinksInPath()
        guard !fm.fileExists(atPath: target.path) else { throw fail("Choose a new stack project folder.") }
        let stage = target.deletingLastPathComponent().appendingPathComponent(".calibration-stack-" + UUID().uuidString)
        try fm.createDirectory(at: stage, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: stage) }
        for dir in ["sources", "calibration-record"] { try fm.createDirectory(at: stage.appendingPathComponent(dir), withIntermediateDirectories: false) }
        var project = PhotonStackProject(name: name, template: .deepSky)
        var settings = DeepSkySettings()
        settings.values["calibration.darks"] = ""; settings.values["calibration.biases"] = ""; settings.values["calibration.flats"] = ""
        settings.values["calibration.sensor-pattern"] = "off"
        settings.values["decode.debayer"] = "on"
        project.deepSkySettings = settings
        var records: [ProcessingArtifactFrame] = []
        for i in indices {
            try Task.checkCancellation()
            let frame = current.frames[i], source = plan.directory.appendingPathComponent(frame.directory)
            let originalName = URL(fileURLWithPath: frame.source).deletingPathExtension().lastPathComponent
            let filename = String(format: "%06d-", i + 1) + String(decoding: originalName.utf8.prefix(180), as: UTF8.self) + ".fits"
            let relative = "sources/" + filename
            try fm.copyItem(at: source.appendingPathComponent("calibrated.fits"), to: stage.appendingPathComponent(relative))
            guard try Self.fileDigest(stage.appendingPathComponent(relative)) == frame.products["calibrated.fits"] else { throw fail("Calibrated science changed while copying.") }
            let recordDir = stage.appendingPathComponent("calibration-record/" + frame.directory)
            try fm.createDirectory(at: recordDir, withIntermediateDirectories: false)
            for name in ["valid.fits", "report.json"] {
                try fm.copyItem(at: source.appendingPathComponent(name), to: recordDir.appendingPathComponent(name))
                guard try Self.fileDigest(recordDir.appendingPathComponent(name)) == frame.products[name] else { throw fail("Calibration diagnostics changed while copying.") }
            }
            let output = target.appendingPathComponent(relative)
            project.addAssets(from: [output], role: .light)
            records.append(.init(sourceURL: URL(fileURLWithPath: frame.source), outputURL: output,
                metrics: ["frame": String(i), "sourceSHA": frame.sourceSHA, "scienceSHA": frame.products["calibrated.fits"]!, "group": String(frame.group),
                          "mask": target.appendingPathComponent("calibration-record/\(frame.directory)/valid.fits").path,
                          "maskSHA": frame.products["valid.fits"]!, "reportSHA": frame.products["report.json"]!]))
        }
        for name in ["job.txt", "job.sha256"] { try fm.copyItem(at: plan.directory.appendingPathComponent(name), to: stage.appendingPathComponent("calibration-record/" + name)) }
        guard try Self.fileDigest(stage.appendingPathComponent("calibration-record/job.txt")) == current.jobSHA else { throw fail("Calibration configuration changed while copying.") }
        let provenance = stage.appendingPathComponent("calibration-record/source-task.json")
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
        try encoder.encode(current).write(to: provenance, options: .atomic)
        project.appendArtifact(.init(kind: .calibratedSequence, name: "Calibrated sensor inputs", operationKind: .calibrate,
            outputDirectory: target.appendingPathComponent("sources"), outputURLs: records.compactMap(\.outputURL), frames: records,
            parameters: ["mode": "calibration-job-copy", "jobSHA": current.jobSHA, "originJobDirectory": plan.directory.path,
                         "sourceTaskReport": target.appendingPathComponent("calibration-record/source-task.json").path,
                         "sourceTaskReportSHA": try Self.fileDigest(provenance)], metrics: ["frames": String(indices.count)]))
        // Store final paths, but publish the complete project only after every
        // copied science/mask/report has matched its verified receipt digest.
        try await ProjectRepository().save(project, to: stage)
        try Task.checkCancellation()
        try publish(stage, to: target)
        return target
    }

    func verifyStackProject(_ project: PhotonStackProject) throws {
        let active = Set(project.assets.filter { $0.role == .light }.map { PhotonStackProject.assetIdentityPath(for: $0.originalURL) })
        for artifact in project.artifacts where artifact.parameters["mode"] == "calibration-job-copy" {
            guard let path = artifact.parameters["sourceTaskReport"], let digest = artifact.parameters["sourceTaskReportSHA"] else {
                throw fail("Missing calibration source record.")
            }
            let file = URL(fileURLWithPath: path); try regular(file)
            guard (try file.resourceValues(forKeys: [.fileSizeKey]).fileSize ?? Int.max) <= 16 * 1024 * 1024,
                  try Self.fileDigest(file) == digest else { throw fail("Calibration source record changed.") }
            let report = try CalibrationJobReport.decode(String(contentsOf: file, encoding: .utf8))
            guard report.jobSHA == artifact.parameters["jobSHA"] else { throw fail("Calibration task identity changed.") }
            for frame in artifact.frames {
                guard let output = frame.outputURL, let index = frame.metrics["frame"].flatMap(Int.init), report.frames.indices.contains(index),
                      report.frames[index].complete, report.frames[index].sourceSHA == frame.metrics["sourceSHA"],
                      report.frames[index].products["calibrated.fits"] == frame.metrics["scienceSHA"] else { throw fail("Invalid calibration frame record.") }
                if active.contains(PhotonStackProject.assetIdentityPath(for: output)) {
                    try regular(output)
                    guard try Self.fileDigest(output) == frame.metrics["scienceSHA"] else { throw fail("A calibrated input changed; create a new stack project from the verified calibration task.") }
                }
            }
        }
    }
}
