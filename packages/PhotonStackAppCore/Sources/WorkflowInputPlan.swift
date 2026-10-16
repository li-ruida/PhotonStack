import Foundation

public struct WorkflowInputPlan: Identifiable, Codable, Equatable, Sendable {
    public var id = UUID()
    public let folder: URL
    public let lights: [URL]
    public let calibration: [WorkflowCalibrationRole: [URL]]
    public var title: String { folder.lastPathComponent }

    public static let imageExtensions: Set<String> = [
        "nef", "arw", "cr2", "cr3", "dng", "orf", "raf", "rw2", "srw",
        "fits", "fit", "fts", "tif", "tiff", "png", "jpg", "jpeg", "heic", "heif",
    ]

    /// Directory IO must not block the desktop's main actor during batch preflight.
    public static func inspectAsync(folder: URL, workflow: ReusableWorkflow) async throws -> Self {
        let scan = Task.detached(priority: .userInitiated) { try inspect(folder: folder, workflow: workflow) }
        return try await withTaskCancellationHandler {
            let plan = try await scan.value
            try Task.checkCancellation()
            return plan
        } onCancel: {
            scan.cancel()
        }
    }

    /// Intentionally nonrecursive: adjacent targets and old output subfolders must not mix.
    public static func inspect(folder: URL, workflow: ReusableWorkflow) throws -> Self {
        try Task.checkCancellation()
        try workflow.validate()
        guard folder.isFileURL else { throw WorkflowError("请选择本地照片文件夹 / Choose a local folder") }
        let folder = folder.resolvingSymlinksInPath().standardizedFileURL
        let lightDirectory = folder.appendingPathComponent("lights", isDirectory: true)
        var isDirectory: ObjCBool = false
        let hasLights = FileManager.default.fileExists(atPath: lightDirectory.path, isDirectory: &isDirectory) && isDirectory.boolValue
        let lights = try images(in: hasLights ? lightDirectory : folder)
        guard !lights.isEmpty else { throw WorkflowError("没有可处理照片 / No images: \(folder.lastPathComponent)") }
        let minimumFrames = workflow.steps.first?.deepSkyValues == nil ? 2 : 3
        guard !workflow.combinesFrames || lights.count >= minimumFrames else {
            throw WorkflowError("堆栈至少需要 \(minimumFrames) 张照片 / Not enough frames: \(folder.lastPathComponent)")
        }
        var calibration: [WorkflowCalibrationRole: [URL]] = [:]
        for role in workflow.requiredCalibration {
            let files = try images(in: folder.appendingPathComponent(role.rawValue, isDirectory: true))
            guard !files.isEmpty else { throw WorkflowError("缺少校准帧 / Missing calibration frames: \(role.rawValue)") }
            calibration[role] = files
        }
        let allInputs = lights + calibration.values.flatMap { $0 }
        guard Set(allInputs).count == allInputs.count else { throw WorkflowError("原片与校准帧重复 / Duplicate input and calibration files") }
        return Self(folder: folder, lights: lights, calibration: calibration)
    }

    private static func images(in folder: URL) throws -> [URL] {
        let entries = try FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.isRegularFileKey], options: [.skipsHiddenFiles])
        let images = try entries.filter { url in
            try Task.checkCancellation()
            guard Self.imageExtensions.contains(url.pathExtension.lowercased()) else { return false }
            return try url.resourceValues(forKeys: [.isRegularFileKey]).isRegularFile == true
        }.map { $0.resolvingSymlinksInPath().standardizedFileURL }
        guard images.count <= 2_000 else { throw WorkflowError("单个文件夹最多处理 2000 张照片 / Maximum 2000 images per folder") }
        guard Set(images).count == images.count else { throw WorkflowError("文件夹包含重复文件链接 / Duplicate image links") }
        return images.sorted { $0.lastPathComponent.localizedStandardCompare($1.lastPathComponent) == .orderedAscending }
    }

    public func deepSkyRecipe(for step: WorkflowStep, schema: DeepSkySchema) throws -> String {
        try step.validate()
        guard let values = step.deepSkyValues else { throw WorkflowError("不是完整堆栈步骤 / Not a deep-sky step") }
        var settings = DeepSkySettings()
        settings.values = values
        settings.values["registration.reference"] = "-1"
        settings.values["stack.weights"] = ""
        settings.values["background.exclusions"] = ""
        for role in WorkflowCalibrationRole.allCases {
            let inputs = calibration[role, default: []]
            guard !step.calibration.contains(role) || !inputs.isEmpty else {
                throw WorkflowError("缺少校准帧 / Missing calibration frames: \(role.rawValue)")
            }
            settings.values[role.recipeKey] = inputs.map {
                "\"" + $0.path.replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
            }.joined(separator: " ")
        }
        return try settings.recipe(schema: schema, inputs: lights)
    }
}
