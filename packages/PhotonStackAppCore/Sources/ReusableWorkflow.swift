import Foundation

public struct WorkflowError: LocalizedError, Sendable {
    public let message: String
    public init(_ message: String) { self.message = message }
    public var errorDescription: String? { message }
}

/// A recipe, not a project: no source URLs, cache outputs, asset IDs or selections.
public struct ReusableWorkflow: Identifiable, Codable, Equatable, Sendable {
    public var version = 1
    public var id = UUID()
    public var name: String
    public var createdAt = Date()
    public var steps: [WorkflowStep]
    public var parameters: ProcessingParameters
    public var exportOptions: ExportOptions
    public var notices: [String]

    public init(name: String, steps: [WorkflowStep], parameters: ProcessingParameters = .init(),
                exportOptions: ExportOptions = .init(), notices: [String] = []) {
        self.name = name.trimmingCharacters(in: .whitespacesAndNewlines)
        self.steps = steps
        self.parameters = parameters
        self.exportOptions = exportOptions
        self.notices = notices
    }

    public var combinesFrames: Bool { steps.contains { $0.isSequence } }
    public var requiredCalibration: Set<WorkflowCalibrationRole> {
        Set(steps.flatMap(\.calibration))
    }

    public func validate() throws {
        guard version == 1 else { throw WorkflowError("不支持此工作流版本 / Unsupported workflow version") }
        guard !name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty, name.count <= 160,
              !steps.isEmpty, steps.count <= 100, notices.count <= 100,
              notices.allSatisfy({ $0.count <= 2_000 }) else {
            throw WorkflowError("工作流名称或步骤数量无效 / Invalid workflow name or steps")
        }
        // A sequence step consumes a fresh folder. It must not silently discard prior edits.
        let sequenceIndices = steps.indices.filter { steps[$0].isSequence }
        guard sequenceIndices.isEmpty || sequenceIndices == [0] else {
            throw WorkflowError("堆栈必须是工作流的第一步，且只能出现一次 / Stack must be the first and only sequence step")
        }
        for step in steps { try step.validate() }
    }
}

public enum WorkflowCalibrationRole: String, Codable, CaseIterable, Sendable {
    case darks, biases, flats
    public var recipeKey: String { "calibration.\(rawValue)" }
    public var assetRole: CalibrationFrameRole {
        switch self { case .darks: .dark; case .biases: .bias; case .flats: .flat }
    }
}

public struct WorkflowStep: Codable, Equatable, Sendable {
    public var kind: ProcessingOperationKind
    public var parameters: [String: String]
    /// Present only for the engine's complete analyze/calibrate/register/stack/develop recipe.
    public var deepSkyValues: [String: String]?
    public var calibration: [WorkflowCalibrationRole]

    public init(kind: ProcessingOperationKind, parameters: [String: String] = [:],
                deepSkyValues: [String: String]? = nil, calibration: [WorkflowCalibrationRole] = []) {
        self.kind = kind
        self.parameters = parameters
        self.deepSkyValues = deepSkyValues
        self.calibration = calibration
    }

    public var isSequence: Bool { kind == .stack || kind == .drizzle }
    public static let targetSpecificRecipeKeys: Set<String> = [
        "calibration.darks", "calibration.biases", "calibration.flats",
        "selection.overrides", "registration.reference", "stack.weights", "background.exclusions",
    ]

    public static func deepSky(values: [String: String]) throws -> (step: Self, notices: [String]) {
        guard values["background.exclusions", default: ""].isEmpty else {
            throw WorkflowError("此配方含目标保护椭圆。请先移除旧目标的保护区域再保存工作流。 / Remove target-specific exclusion regions before saving.")
        }
        let roles = WorkflowCalibrationRole.allCases.filter { !values[$0.recipeKey, default: ""].isEmpty }
        var notices = ["新目标会重新分析、筛片并自动选择参考帧；不会沿用旧片的手动取舍或逐帧权重。 / New targets are analyzed and selected afresh with an automatic reference."]
        if !roles.isEmpty {
            notices.append("每个目标文件夹必须提供校准子文件夹 / Required calibration folders: " + roles.map(\.rawValue).joined(separator: ", "))
        }
        let step = Self(kind: .stack, deepSkyValues: values.filter { !targetSpecificRecipeKeys.contains($0.key) }, calibration: roles)
        try step.validate()
        return (step, notices)
    }

    /// Only scalar controls are portable. Structural metadata never enters a workflow document.
    public static func capture(_ operation: EditOperation, redetectSelections: Bool = false) throws -> Self {
        let selectionKeys = ["selectedTrailsV1", "selectedCloudRegionsV1", "selectedIndices"]
        if selectionKeys.contains(where: { !operation.parameters[$0, default: ""].isEmpty }), !redetectSelections {
            throw WorkflowError("手工圈选不能移到新照片。请明确选择“在新照片上自动重新检测”后保存。 / Manual selections require explicit automatic re-detection.")
        }
        if operation.kind == .stack, operation.parameters["mode"] == "deepSkyRecipe" {
            throw WorkflowError("完整堆栈配方需要先解析 / Inspect the stack recipe before capture")
        }
        if operation.parameters["mode"] == "cleanSequence" {
            throw WorkflowError("逐帧清理步骤需保留序列语义，不能保存为单图后期 / Sequence cleanup is not a single-image step")
        }
        guard let keys = portableKeys[operation.kind] else {
            throw WorkflowError("此步骤需要目标专属输入，暂不能自动复用 / Step requires target-specific inputs: \(operation.kind.rawValue)")
        }
        var roles: [WorkflowCalibrationRole] = []
        if operation.kind == .stack {
            if operation.parameters["calibrationApplied"] != "false" {
                for (key, role) in [("calibrationDarkIDs", WorkflowCalibrationRole.darks), ("calibrationBiasIDs", .biases), ("calibrationFlatIDs", .flats)] {
                    if !operation.parameters[key, default: ""].isEmpty { roles.append(role) }
                }
            }
            if operation.parameters["sourceArtifactID"] != nil ||
                !["", "lights"].contains(operation.parameters["sourceArtifact", default: ""]) {
                throw WorkflowError("请从包含原片校准、配准的完整堆栈结果保存，而不是仅依赖中间配准序列。 / Save a stack with its original calibration and registration steps.")
            }
        }
        let step = Self(kind: operation.kind, parameters: operation.parameters.filter { keys.contains($0.key) }, calibration: roles)
        try step.validate()
        return step
    }

    public func validate() throws {
        guard Set(calibration).count == calibration.count, calibration.isEmpty || kind == .stack else {
            throw WorkflowError("校准绑定无效 / Invalid calibration bindings")
        }
        if let values = deepSkyValues {
            guard kind == .stack, parameters.isEmpty, !values.isEmpty, values.count <= 250,
                  Set(values.keys).isDisjoint(with: Self.targetSpecificRecipeKeys) else {
                throw WorkflowError("工作流含旧目标的配方绑定 / Workflow contains target-specific recipe bindings")
            }
            try Self.validateScalarValues(values)
        } else {
            guard let keys = Self.portableKeys[kind], Set(parameters.keys).isSubset(of: keys) else {
                throw WorkflowError("工作流包含不支持的步骤或参数 / Unsupported workflow step or parameter")
            }
            try Self.validateScalarValues(parameters)
            if let mode = parameters["mode"] {
                let modes: [ProcessingOperationKind: Set<String>] = [
                    .background: ["subtract", "divide"], .stretch: ["astro-v1"],
                    .localContrast: ["continuum-v1"], .denoise: ["display-grid-v1"],
                    .colorNeutralize: ["removeGreen"],
                ]
                guard modes[kind]?.contains(mode) == true else { throw WorkflowError("不支持的处理模式 / Unsupported processing mode") }
            }
        }
    }

    private static func validateScalarValues(_ values: [String: String]) throws {
        for (key, value) in values {
            // These recipes contain numeric/enum/curve values, never paths or commands.
            guard key.count <= 100, value.count <= 16_384,
                  !value.contains("/"), !value.contains("\\"), !value.contains("\n"),
                  !value.contains("\r"), !value.contains("\0") else {
                throw WorkflowError("工作流参数不是可移植的数值或选项 / Non-portable workflow parameter: \(key)")
            }
        }
    }

    private static let rawKeys: Set<String> = ["rawWhiteBalance", "rawTemperature", "rawTint", "rawExposureBias", "rawBlackLevel", "rawBlackValue", "rawDemosaic", "rawLinear"]
    public static let portableKeys: [ProcessingOperationKind: Set<String>] = [
        .rawDecode: rawKeys,
        .stack: rawKeys.union(["masterMethod", "stackMethod", "method", "alignment", "darkBiasState", "flatBiasState"]),
        .drizzle: ["scale", "pixfrac", "alignment", "alignTranslation"],
        .crop: ["aspect", "margin"],
        .background: ["model", "mode", "strength", "preserveBrightness", "protectBrightTargets"],
        .normalize: ["targetBackground", "targetScale"],
        .stretch: ["mode", "targetBackground", "stellarBalance", "toneCurve", "toneScale", "whitePoint", "brightness", "background", "starExposure", "starPeakThreshold", "starColorCleanup", "shadowNeutralization", "saturation", "redGain", "blueGain"],
        .curves: ["black", "midInput", "midOutput", "white", "channel", "points"],
        .localContrast: ["mode", "amount", "radius", "fineRadius", "starChroma", "points"],
        .denoise: ["mode", "amount", "chromaAmount", "radius"],
        .sharpen: ["amount", "radius"],
        .deconvolve: ["iterations", "radius", "sigma"],
        .colorNeutralize: ["mode", "strength", "greenMethod", "amount", "backgroundLimit", "greenThreshold", "preserveLightness"],
        .colorSaturate: ["amount"],
        .starReduce: ["amount", "profileAware", "edgeAware"],
        .comaReduce: ["amount", "radius", "eccentricity", "edgeAware"],
        .artifactRemove: ["removeAirplanes", "removeDrones", "removeSatellites", "removeMeteors"],
        .cloudRemove: ["strength"],
    ]
}
