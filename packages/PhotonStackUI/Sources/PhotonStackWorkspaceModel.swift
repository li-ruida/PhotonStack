import Foundation
import CoreImage
import CoreText
import ImageIO
import OSLog
import PhotonStackAppCore
import PhotonStackProcessing
import SwiftUI
import UniformTypeIdentifiers

private let curveTelemetryLogger = Logger(subsystem: "dev.photonstack.app", category: "curves")

public struct MosaicPlacementReport: Identifiable, Equatable, Sendable {
    public var id: Int { index }
    public var index: Int
    public var x: Double
    public var y: Double
    public var a: Double
    public var b: Double
    public var c: Double
    public var d: Double
    public var dx: Double
    public var dy: Double
    public var model: String
    public var matches: Int
    public var references: Int
    public var autoAligned: Bool
    public var fallback: Bool
    public var reducedModel: Bool
    public var coarseAlignment: Bool

    public var scale: Double {
        (hypot(a, c) + hypot(b, d)) * 0.5
    }

    public var rotationDegrees: Double {
        atan2(c, a) * 180.0 / .pi
    }
}

public struct MosaicQualityReport: Equatable, Sendable {
    public var autoAligned: Bool
    public var matches: Int
    public var fallbackPanels: Int
    public var exposureMatched: Bool
    public var blend: String
    public var placements: [MosaicPlacementReport]
}

public struct RegistrationReport: Equatable, Sendable {
    public var referenceName: String
    public var movingName: String
    public var mode: AlignmentMethod
    public var dx: Double
    public var dy: Double
    public var scale: Double
    public var rotationDegrees: Double
    public var matches: Int
    public var outputURL: URL
}

private struct ParsedRegistrationMetrics {
    var mode: AlignmentMethod
    var dx: Double
    var dy: Double
    var scale: Double
    var rotationRadians: Double
    var matches: Int
    var usedFallback: Bool
}

struct ArtifactMarkerLabelPlacement: Equatable {
    var rect: CGRect
    var anchor: CGPoint
    var drawsLeader: Bool
}

struct MarkerLabelLayout: Equatable {
    var placements: [Int: ArtifactMarkerLabelPlacement]
    var omittedCount: Int
    var overflowPlacement: ArtifactMarkerLabelPlacement?

    var overflowText: String? {
        guard omittedCount > 0, overflowPlacement != nil else {
            return nil
        }
        return "+\(omittedCount)"
    }
}

private struct MarkerLabelRequest {
    var index: Int
    var anchor: CGPoint
    var size: CGSize
}

public struct ArtifactTrailPathPoint: Equatable, Sendable {
    public var x: Double
    public var y: Double
}

public struct ArtifactTrailItem: Identifiable, Equatable, Sendable {
    public var id: Int { index }
    public var displayIndex: Int { index + 1 }
    public var displayLabel: String { "#\(displayIndex)" }
    public var index: Int
    public var kind: String
    public var confidence: Double
    public var x1: Double
    public var y1: Double
    public var x2: Double
    public var y2: Double
    public var length: Double
    public var width: Double
    public var meanBrightness: Double
    public var weight: Double
    public var peakPosition: Double
    public var taperScore: Double
    public var path: [ArtifactTrailPathPoint] = []
}

public struct ArtifactTrailReport: Equatable, Sendable {
    public var trails: Int
    public var removedTrails: Int
    public var protectedMeteors: Int
    public var items: [ArtifactTrailItem]
    public var sourceURL: URL? = nil
    public var imageWidth: Int? = nil
    public var imageHeight: Int? = nil

    public var topItems: [ArtifactTrailItem] {
        Array(items.sorted { left, right in
            let leftWeight = left.weight.isFinite ? left.weight : -.infinity
            let rightWeight = right.weight.isFinite ? right.weight : -.infinity
            if leftWeight != rightWeight {
                return leftWeight > rightWeight
            }
            let leftConfidence = left.confidence.isFinite ? left.confidence : -.infinity
            let rightConfidence = right.confidence.isFinite ? right.confidence : -.infinity
            if leftConfidence != rightConfidence {
                return leftConfidence > rightConfidence
            }
            return left.index < right.index
        }.prefix(20))
    }
}

public struct CloudMaskRectItem: Equatable, Sendable {
    public var x: Double
    public var y: Double
    public var width: Double
    public var height: Double
}

public struct CloudRegionItem: Identifiable, Equatable, Sendable {
    public var id: Int { index }
    public var displayIndex: Int { index + 1 }
    public var displayLabel: String { "#\(displayIndex)" }
    public var index: Int
    public var confidence: Double
    public var x: Double
    public var y: Double
    public var width: Double
    public var height: Double
    public var coverage: Double
    public var meanLuminance: Double
    public var backgroundLuminance: Double
    public var temporalSupport: Int = 0
    public var maskRects: [CloudMaskRectItem]
}

public struct CloudRegionReport: Equatable, Sendable {
    public var clouds: Int
    public var removedClouds: Int
    public var items: [CloudRegionItem]
    public var sourceURL: URL? = nil
    public var imageWidth: Int? = nil
    public var imageHeight: Int? = nil
    public var temporalFramesUsed: Int = 0
    public var temporallyConfirmedClouds: Int = 0

    public var topItems: [CloudRegionItem] {
        Array(items.sorted { left, right in
            let leftConfidence = left.confidence.isFinite ? left.confidence : -.infinity
            let rightConfidence = right.confidence.isFinite ? right.confidence : -.infinity
            if leftConfidence != rightConfidence {
                return leftConfidence > rightConfidence
            }
            return left.index < right.index
        }.prefix(20))
    }
}

public enum MaskBrushMode: String, Codable, CaseIterable, Equatable, Sendable {
    case hide
    case reveal
}

public struct MaskBrushPoint: Codable, Equatable, Sendable {
    public var x: Double
    public var y: Double

    public init(x: Double, y: Double) {
        self.x = x.isFinite ? min(max(x, 0), 1) : 0
        self.y = y.isFinite ? min(max(y, 0), 1) : 0
    }

    private enum CodingKeys: String, CodingKey {
        case x
        case y
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        self.init(
            x: try container.decode(Double.self, forKey: .x),
            y: try container.decode(Double.self, forKey: .y)
        )
    }
}

public struct MaskBrushStroke: Codable, Equatable, Sendable {
    public var mode: MaskBrushMode
    public var points: [MaskBrushPoint]
    public var diameterFraction: Double
    public var opacity: Double

    public init(
        mode: MaskBrushMode,
        points: [MaskBrushPoint],
        diameterFraction: Double,
        opacity: Double = 1
    ) {
        self.mode = mode
        self.points = points
        self.diameterFraction = diameterFraction.isFinite
            ? min(max(diameterFraction, 0.0001), 1)
            : 0.0001
        self.opacity = opacity.isFinite ? min(max(opacity, 0), 1) : 1
    }

    private enum CodingKeys: String, CodingKey {
        case mode
        case points
        case diameterFraction
        case opacity
    }

    public init(from decoder: Decoder) throws {
        let container = try decoder.container(keyedBy: CodingKeys.self)
        self.init(
            mode: try container.decode(MaskBrushMode.self, forKey: .mode),
            points: try container.decode([MaskBrushPoint].self, forKey: .points),
            diameterFraction: try container.decode(Double.self, forKey: .diameterFraction),
            opacity: try container.decodeIfPresent(Double.self, forKey: .opacity) ?? 1
        )
    }
}

public enum PendingProjectTransition: Equatable, Sendable {
    case create(template: ProjectTemplate, name: String?, importAfterCreate: Bool)
    case open(directory: URL)
    case closeWindow
    case terminateApplication
}

public enum PendingProcessingInterruption: Equatable, Sendable {
    case closeWindow
    case terminateApplication
}

public struct PreviewCacheCleanupPlan: Equatable, Sendable {
    public let fileCount: Int
    public let totalBytes: Int64
    fileprivate let fileURLs: [URL]

    fileprivate init(fileURLs: [URL], totalBytes: Int64) {
        self.fileURLs = fileURLs
        self.fileCount = fileURLs.count
        self.totalBytes = totalBytes
    }
}

public enum RawDecodeBackend: String, Codable, Equatable, Sendable {
    case appleRaw = "apple-raw"
    case imageIO = "imageio"
}

public struct RawDecodeStatus: Codable, Equatable, Sendable {
    public var backend: RawDecodeBackend
    public var usedFallback: Bool
    public var fallbackErrorCode: String
    public var fallbackMessage: String
}

private struct PreviewCacheReceipt: Codable {
    var schema: String
    var buildIdentity: String
    var outputPath: String
    var outputSignature: String
    var rawDecodeStatus: RawDecodeStatus?
}

private final class NotificationObserverToken: @unchecked Sendable {
    private let token: NSObjectProtocol

    init(_ token: NSObjectProtocol) {
        self.token = token
    }

    deinit {
        NotificationCenter.default.removeObserver(token)
    }
}

enum EditOperationParameterEditor: Equatable {
    case decimal(ClosedRange<Double>)
    case integer(ClosedRange<Int>)
    case toggle
    case options([String])
    case text
}

struct EditableEditOperationParameter: Identifiable, Equatable {
    var id: String { key }

    let key: String
    let value: String
    let editor: EditOperationParameterEditor
}

private enum EditOperationParameterRule {
    case decimal(ClosedRange<Double>)
    case integer(ClosedRange<Int>)
    case toggle
    case options([String])
    case aspectRatio
    case curvePoints

    var editor: EditOperationParameterEditor {
        switch self {
        case let .decimal(range):
            return .decimal(range)
        case let .integer(range):
            return .integer(range)
        case .toggle:
            return .toggle
        case let .options(values):
            return .options(values)
        case .aspectRatio, .curvePoints:
            return .text
        }
    }

    func normalizedValue(_ value: String) -> String? {
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        switch self {
        case let .decimal(range):
            guard let number = Double(trimmed), number.isFinite, range.contains(number) else {
                return nil
            }
            return String(number)
        case let .integer(range):
            guard let number = Int(trimmed), range.contains(number) else {
                return nil
            }
            return String(number)
        case .toggle:
            switch trimmed.lowercased() {
            case "true", "1", "yes", "on":
                return "true"
            case "false", "0", "no", "off":
                return "false"
            default:
                return nil
            }
        case let .options(values):
            return values.contains(trimmed) ? trimmed : nil
        case .aspectRatio:
            let parts = trimmed.split(separator: ":", omittingEmptySubsequences: false)
            guard parts.count == 2,
                  let width = Double(parts[0]), width.isFinite, width > 0,
                  let height = Double(parts[1]), height.isFinite, height > 0
            else {
                return nil
            }
            return trimmed
        case .curvePoints:
            let rawPoints = trimmed.split(separator: ",", omittingEmptySubsequences: false)
            guard rawPoints.count >= 2 else {
                return nil
            }
            var previousInput = -Double.infinity
            for rawPoint in rawPoints {
                let parts = rawPoint.split(separator: ":", omittingEmptySubsequences: false)
                guard parts.count == 2,
                      let input = Double(parts[0]), input.isFinite, (0...1).contains(input),
                      let output = Double(parts[1]), output.isFinite, (0...1).contains(output),
                      input > previousInput
                else {
                    return nil
                }
                previousInput = input
            }
            return trimmed
        }
    }
}

@MainActor
public final class PhotonStackWorkspaceModel: ObservableObject {
    @Published public private(set) var project: PhotonStackProject
    @Published public private(set) var selectedAssetID: PhotonStackAsset.ID?
    @Published public private(set) var parameters: ProcessingParameters
    @Published public var language: AppLanguage
    @Published public var selectedTemplate: ProjectTemplate
    @Published public var autosaveEnabled: Bool
    @Published public private(set) var exportOptions: ExportOptions
    @Published public private(set) var recentProjects: [RecentProject]
    @Published public private(set) var customCurvePresets: [CustomCurvePreset]
    @Published public private(set) var currentProjectDirectory: URL?
    @Published public private(set) var previewURL: URL?
    @Published public private(set) var previewOperationID: EditOperation.ID?
    @Published public private(set) var editGraphNeedsReplay = false
    @Published public private(set) var canvasMode: WorkspaceCanvasMode = .sourcePreview
    @Published public private(set) var histogram: HistogramSnapshot?
    @Published public private(set) var curveReferenceHistogram: HistogramSnapshot?
    @Published public private(set) var jobs: [ProcessingJob] = []
    @Published public private(set) var batchQueue: [BatchQueueItem] = []
    @Published public private(set) var batchRegistrationSelection: Set<PhotonStackAsset.ID> = []
    @Published public private(set) var pendingAssetImportCount = 0
    @Published public private(set) var activeLayerID: ProcessingLayer.ID?
    @Published public private(set) var isBatchRunning = false
    @Published public private(set) var isBatchPaused = false
    @Published public private(set) var commandOutput: String = ""
    @Published public private(set) var isProcessing = false
    @Published public private(set) var progressFraction: Double?
    @Published public private(set) var progressMessage: String?
    @Published public private(set) var errorMessage: String?
    @Published public private(set) var canCancel = false
    @Published public private(set) var canStepBack = false
    @Published public private(set) var canUndoEditGraph = false
    @Published public private(set) var canRedoEditGraph = false
    @Published public private(set) var isRawPreviewUpdating = false
    @Published public private(set) var rawDecodeStatus: RawDecodeStatus?
    @Published public private(set) var mosaicQualityReport: MosaicQualityReport?
    @Published public private(set) var registrationReport: RegistrationReport?
    @Published public private(set) var artifactTrailReport: ArtifactTrailReport?
    @Published public private(set) var artifactMarkedPreviewURL: URL?
    @Published public private(set) var cloudRegionReport: CloudRegionReport?
    @Published public private(set) var detectedStarCount: Int?
    @Published public private(set) var pendingAssetRemovalIDs: [PhotonStackAsset.ID] = []
    @Published public private(set) var pendingArtifactRemovalID: ProcessingArtifact.ID?
    @Published public private(set) var pendingPreviewCacheCleanupPlan: PreviewCacheCleanupPlan?
    @Published public private(set) var pendingProcessingInterruption: PendingProcessingInterruption?
    @Published public private(set) var pendingProjectTransition: PendingProjectTransition?
    @Published public private(set) var hasUnsavedChanges = false
    @Published public private(set) var hasPendingUserSettingsPersistence = false

    private let processingService: any ProcessingService
    private let projectRepository: ProjectRepository
    private let settingsRepository: any AppSettingsStoring
    private var previewDirectory: URL
    private let usesManagedPreviewDirectory: Bool
    private var activeTask: Task<Void, Never>?
    private var batchTask: Task<Void, Never>?
    private var rawPreviewTask: Task<Void, Never>?
    private var curvePreviewTask: Task<Void, Never>?
    private var histogramRefreshTask: Task<Void, Never>?
    private var metadataRefreshTask: Task<Void, Never>?
    private var autosaveTask: Task<Void, Never>?
    private var settingsPersistenceTask: Task<Void, Never>?
    private var initialUserSettingsSnapshot: AppSettings?
    private var settingsPersistenceErrorMessage: String?
    private var editParameterValidationErrorMessage: String?
    private var hasLoadedUserSettings = false
    private var rawPreviewGeneration = 0
    private var curvePreviewGeneration = 0
    private var histogramRefreshGeneration = 0
    private var metadataRefreshGeneration = 0
    private var foregroundGeneration = 0
    private var batchGeneration = 0
    private var autosaveGeneration = 0
    private var settingsPersistenceGeneration = 0
    private var projectChangeGeneration = 0
    @Published public private(set) var foregroundTaskRequested = false
    private var previewHistory: [PreviewHistoryState] = []
    private var pendingLayerOpacityAdjustment: LayerAdjustmentSession?
    private var pendingLayerMaskAdjustment: LayerAdjustmentSession?
    private var editGraphUndoStack: [EditGraphHistoryState] = []
    private var editGraphRedoStack: [EditGraphHistoryState] = []
    private var pendingAssetImportURLs: [URL] = []
    private var batchRegistrationCandidateIDs: Set<PhotonStackAsset.ID> = []
    private var progressObserver: NotificationObserverToken?
    private var activeProgressJobID: UUID?
    private var activeBatchItemIDs: Set<BatchQueueItem.ID> = []
    private var batchProgressJobItems: [UUID: BatchQueueItem.ID] = [:]
    private var batchItemProgress: [BatchQueueItem.ID: Double] = [:]
    private var replayStackSourceInputs: [URL]?
    private var replayRegisteredSequence: ReplayRegisteredSequence?
    private var replayNeedsMeteorSequenceSources = false
    private var replayTemporaryDirectories: Set<URL> = []
    private var mosaicQualityReportSourceURL: URL?
    private var curvePreviewBaseInputURL: URL?
    private var curvePreviewReferenceHistogram: HistogramSnapshot?
    private var pendingCurvePreviewPoints: [CurveEditorPoint]?
    private var curvePreviewLoopID: UUID?
    private let cacheBuildIdentity: String
    private let previewCacheSchemaVersion = "preview-cache-v4"
    private let previewCacheReceiptSchemaVersion = "preview-cache-receipt-v2"
    private let artifactRemovalCacheRevision = "satellite-trails-v6-path-shape"
    private let artifactMarkerRendererRevision = "labels-v12-global-declutter-overflow-summary"
    private let cloudMarkerRendererRevision = "labels-v3-global-declutter-overflow-summary"
    static let maximumCurvePresetImportBytes: UInt64 = 4 * 1_024 * 1_024
    static let maximumImportedCurvePresetCount = 1_000

    public init(
        project: PhotonStackProject = PhotonStackProject(name: "PhotonStack"),
        parameters: ProcessingParameters = ProcessingParameters(),
        language: AppLanguage = .systemDefault,
        selectedTemplate: ProjectTemplate = .deepSky,
        autosaveEnabled: Bool = true,
        exportOptions: ExportOptions = ExportOptions(),
        recentProjects: [RecentProject] = [],
        customCurvePresets: [CustomCurvePreset] = [],
        processingService: any ProcessingService,
        projectRepository: ProjectRepository = ProjectRepository(),
        settingsRepository: any AppSettingsStoring = AppSettingsRepository(),
        previewDirectory: URL? = nil,
        cacheBuildIdentity: String? = nil
    ) {
        self.project = project
        self.canvasMode = project.workspaceState?.canvasMode
            ?? (project.layers.contains(where: { $0.kind != .mask }) ? .layerComposite : .sourcePreview)
        self.parameters = Self.normalizedProcessingParameters(parameters)
        self.language = language
        self.selectedTemplate = selectedTemplate
        self.autosaveEnabled = autosaveEnabled
        self.exportOptions = exportOptions
        self.recentProjects = AppSettings(recentProjects: recentProjects).recentProjects
        self.customCurvePresets = AppSettings(customCurvePresets: customCurvePresets).customCurvePresets
        self.batchQueue = Self.restoredBatchQueue(from: project.batchQueue)
        self.batchRegistrationCandidateIDs = Self.batchRegistrationCandidateIDs(in: project)
        self.batchRegistrationSelection = self.batchRegistrationCandidateIDs
        self.processingService = processingService
        self.projectRepository = projectRepository
        self.settingsRepository = settingsRepository
        self.usesManagedPreviewDirectory = previewDirectory == nil
        self.previewDirectory = previewDirectory ?? Self.managedPreviewDirectory(for: project.id)
        self.cacheBuildIdentity = cacheBuildIdentity
            ?? PhotonStackBuildIdentity.current.cacheIdentity
        self.initialUserSettingsSnapshot = currentUserSettingsSnapshot()
        let progressObserver = NotificationCenter.default.addObserver(
            forName: .photonStackProcessingProgress,
            object: nil,
            // Pipe readers must not wait for SwiftUI layout on the main queue.
            // The bounded progress stream hops to the main actor below.
            queue: nil
        ) { [weak self] notification in
            guard let event = notification.object as? ProcessingProgressEvent else {
                return
            }
            Task { @MainActor in
                self?.handleProcessingProgress(event)
            }
        }
        self.progressObserver = NotificationObserverToken(progressObserver)
        restoreWorkspaceState(from: project)
        if synchronizeOperationLayerVisibilityWithEditGraph() {
            editGraphNeedsReplay = true
        }
        restorePersistedRawOptions(for: selectedAsset)
    }

    private static func restoredBatchQueue(from queue: [BatchQueueItem]) -> [BatchQueueItem] {
        queue.map { item in
            guard item.status == .running else {
                return item
            }

            var restored = item
            restored.status = .queued
            restored.message = ""
            return restored
        }
    }

    public var selectedAsset: PhotonStackAsset? {
        guard let selectedAssetID else {
            return project.assets.first
        }
        return project.assets.first { $0.id == selectedAssetID }
    }

    public var currentInputURL: URL? {
        switch canvasMode {
        case .sourcePreview:
            return previewURL ?? selectedAsset?.originalURL
        case .layerComposite:
            return activeLayerURL
        }
    }

    private var currentInputIsMaskLayer: Bool {
        guard activeLayer?.kind == .mask,
              let activeLayerURL,
              let currentInputURL
        else {
            return false
        }
        return Self.urlsReferenceSameFile(activeLayerURL, currentInputURL)
    }

    public func isAssetMissing(_ asset: PhotonStackAsset) -> Bool {
        var isDirectory: ObjCBool = false
        return FileManager.default.fileExists(atPath: asset.originalURL.path, isDirectory: &isDirectory) == false
            || isDirectory.boolValue
    }

    public var selectedAssetIsMissing: Bool {
        selectedAsset.map(isAssetMissing) ?? false
    }

    public var canResetPreviewToOriginal: Bool {
        selectedAsset.map { firstMissingAsset(in: [$0]) == nil } == true
    }

    public var currentInputIsMissing: Bool {
        guard let currentInputURL else {
            return false
        }
        var isDirectory: ObjCBool = false
        return FileManager.default.fileExists(atPath: currentInputURL.path, isDirectory: &isDirectory) == false
            || isDirectory.boolValue
    }

    public var missingAssetCount: Int {
        project.assets.lazy.filter(isAssetMissing).count
    }

    public var canRunStackWorkflow: Bool {
        lightAssets.isEmpty == false &&
            firstMissingAsset(in: lightAssets + darkAssets + biasAssets + flatAssets) == nil &&
            calibrationConfigurationWarning == nil
    }

    public var calibrationConfigurationWarning: String? {
        if flatAssets.isEmpty == false,
           biasAssets.isEmpty,
           parameters.workflowFlatBiasState == .included {
            return localized(.flatBiasMissingWarning)
        }
        return nil
    }

    public func canRegister(reference: PhotonStackAsset?, moving: PhotonStackAsset?) -> Bool {
        guard let reference,
              let moving,
              reference.id != moving.id,
              project.assets.contains(where: { $0.id == reference.id }),
              project.assets.contains(where: { $0.id == moving.id })
        else {
            return false
        }
        return firstMissingAsset(in: [reference, moving]) == nil
    }

    public func canRegisterBatch(reference: PhotonStackAsset?) -> Bool {
        guard let reference,
              project.assets.contains(where: { $0.id == reference.id }),
              isAssetMissing(reference) == false
        else {
            return false
        }
        let movingFrames = selectedBatchRegistrationMovingFrames(reference: reference)
        return movingFrames.isEmpty == false && firstMissingAsset(in: movingFrames) == nil
    }

    public var canRunMosaic: Bool {
        mosaicAssets.isEmpty == false && firstMissingAsset(in: mosaicAssets) == nil
    }

    public var canRunDrizzle: Bool {
        lightAssets.isEmpty == false && firstMissingAsset(in: lightAssets) == nil
    }

    public var canCleanTimelapseArtifactSequence: Bool {
        timelapseArtifactSequenceFrames.isEmpty == false &&
            firstMissingAsset(in: timelapseArtifactSequenceFrames) == nil
    }

    public var canProcessCurrentInput: Bool {
        guard let currentInputURL else {
            return false
        }
        guard currentInputIsMaskLayer == false else {
            return false
        }
        guard inputFileExists(currentInputURL) else {
            return false
        }
        guard canvasMode == .layerComposite else {
            return true
        }
        guard let activeLayer else {
            return false
        }
        return isLayerInputAvailable(activeLayer) && isLayerMaskInputAvailable(activeLayer)
    }

    public var canRestoreMeteorsFromSelectedAsset: Bool {
        canProcessCurrentInput && selectedAsset.map { firstMissingAsset(in: [$0]) == nil } == true
    }

    public var canEnqueueCurrentBatchItem: Bool {
        canProcessCurrentInput
    }

    public var canEnqueueAllAssets: Bool {
        project.assets.isEmpty == false && firstMissingAsset(in: project.assets) == nil
    }

    public var canModifyProcessingConfiguration: Bool {
        activeTask == nil &&
            batchTask == nil &&
            foregroundTaskRequested == false &&
            isProcessing == false &&
            isBatchRunning == false
    }

    public var canModifyWorkspaceProducts: Bool {
        canModifyProcessingConfiguration &&
            rawPreviewTask == nil &&
            isRawPreviewUpdating == false
    }

    public var hasActiveProcessing: Bool {
        activeTask != nil ||
            batchTask != nil ||
            rawPreviewTask != nil ||
            foregroundTaskRequested ||
            isProcessing ||
            isBatchRunning ||
            isRawPreviewUpdating
    }

    public func setProcessingParameters(_ updatedParameters: ProcessingParameters) {
        guard canModifyProcessingConfiguration else {
            return
        }
        parameters = Self.normalizedProcessingParameters(updatedParameters)
    }

    public var batchQueueInputsAvailable: Bool {
        let queuedItems = batchQueue.filter { $0.status == .queued }
        return queuedItems.isEmpty == false && queuedItems.allSatisfy { item in
            inputFileExists(item.inputURL)
        }
    }

    private func firstMissingAsset(in assets: [PhotonStackAsset]) -> PhotonStackAsset? {
        guard processingService.requiresExistingInputFiles else {
            return nil
        }
        return assets.first(where: isAssetMissing)
    }

    private func inputFileExists(_ url: URL) -> Bool {
        guard processingService.requiresExistingInputFiles else {
            return true
        }
        return workspaceFileExists(url)
    }

    private func workspaceFileExists(_ url: URL) -> Bool {
        var isDirectory: ObjCBool = false
        return FileManager.default.fileExists(atPath: url.path, isDirectory: &isDirectory) &&
            isDirectory.boolValue == false
    }

    private func rejectMissingAsset(in assets: [PhotonStackAsset]) -> Bool {
        guard let missingAsset = firstMissingAsset(in: assets) else {
            return false
        }
        errorMessage = "\(localized(.missingAssetProcessingError)): \(missingAsset.displayName)"
        return true
    }

    private func validatedCurrentInputURL() -> URL? {
        guard let currentInputURL else {
            if canvasMode == .layerComposite {
                errorMessage = localized(.missingWorkspaceSourceError)
            }
            return nil
        }
        guard currentInputIsMaskLayer == false else {
            errorMessage = localized(.imageProcessingRequiresImageLayer)
            return nil
        }
        guard inputFileExists(currentInputURL) else {
            errorMessage = "\(localized(.missingAssetProcessingError)): \(currentInputURL.lastPathComponent)"
            return nil
        }
        if canvasMode == .layerComposite, let activeLayer,
           (isLayerInputAvailable(activeLayer) == false || isLayerMaskInputAvailable(activeLayer) == false) {
            errorMessage = "\(localized(.missingWorkspaceSourceError)): \(activeLayer.name)"
            return nil
        }
        return currentInputURL
    }

    private var timelapseArtifactSequenceFrames: [PhotonStackAsset] {
        lightAssets.isEmpty ? project.assets : lightAssets
    }

    public var canExportCurrentImage: Bool {
        exportCanvasInputsAvailable &&
            isProcessing == false &&
            editGraphNeedsReplay == false &&
            hasAmbiguousEditGraphProvenance == false &&
            exportBlockingMissingAsset == nil &&
            exportBlockingWorkspaceSourceName == nil
    }

    public var canReplayEditGraph: Bool {
        isProcessing == false &&
            project.editGraph.operations.isEmpty == false &&
            (previewEditGraphReplayPlan() != nil || selectedAsset != nil)
    }

    public var activeLayer: ProcessingLayer? {
        guard let activeLayerID else {
            return nil
        }
        return project.layers.first { $0.id == activeLayerID }
    }

    private var activeLayerURL: URL? {
        activeLayer?.inputURL
    }

    private var emptyLayerStackSourceLayer: ProcessingLayer? {
        project.layers.first { layer in
            layer.kind != .mask && isLayerInputAvailable(layer)
        }
    }

    private var exportCanvasInputsAvailable: Bool {
        switch canvasMode {
        case .sourcePreview:
            if canReplayFullResolutionForExport,
               let replayPlan = fullResolutionExportReplayPlan(),
               inputFileExists(replayPlan.input) {
                return true
            }
            return currentInputURL.map(inputFileExists) ?? false
        case .layerComposite:
            let visibleLayers = visiblePreviewLayers
            if visibleLayers.isEmpty {
                guard let sourceLayer = emptyLayerStackSourceLayer else {
                    return false
                }
                return isLayerReplaySourceAvailable(sourceLayer)
            }
            return visibleLayers.allSatisfy { layer in
                isLayerInputAvailable(layer) &&
                    isLayerMaskInputAvailable(layer) &&
                    isLayerReplaySourceAvailable(layer)
            }
        }
    }

    private var exportBlockingWorkspaceSourceName: String? {
        switch canvasMode {
        case .sourcePreview:
            if canReplayFullResolutionForExport,
               let replayPlan = fullResolutionExportReplayPlan(),
               inputFileExists(replayPlan.input) {
                return nil
            }
            guard let currentInputURL, inputFileExists(currentInputURL) == false else {
                return nil
            }
            return currentInputURL.lastPathComponent
        case .layerComposite:
            if visiblePreviewLayers.isEmpty,
               let sourceLayer = emptyLayerStackSourceLayer,
               isLayerReplaySourceAvailable(sourceLayer) == false {
                return sourceLayer.name
            }
            for layer in visiblePreviewLayers {
                if isLayerInputAvailable(layer) == false {
                    return layer.name
                }
                if isLayerMaskInputAvailable(layer) == false {
                    return layer.maskArtifactID.flatMap { maskID in
                        project.artifacts.first(where: { $0.id == maskID })?.name
                    } ?? layer.name
                }
                if isLayerReplaySourceAvailable(layer) == false {
                    return layer.name
                }
            }
            return nil
        }
    }

    private var exportBlockingMissingAsset: PhotonStackAsset? {
        let requiredAssetIDs = exportRequiredAssetIDs()
        return project.assets.first { asset in
            requiredAssetIDs.contains(asset.id) && isAssetMissing(asset)
        }
    }

    private func exportRequiredAssetIDs() -> Set<PhotonStackAsset.ID> {
        var traversal = ExportDependencyTraversal()
        let visibleLayers = visiblePreviewLayers
        if canvasMode == .layerComposite {
            if visibleLayers.isEmpty {
                if let sourceLayer = emptyLayerStackSourceLayer {
                    collectExportDependencies(from: sourceLayer, traversal: &traversal)
                }
            } else {
                for layer in visibleLayers {
                    collectExportDependencies(from: layer, traversal: &traversal)
                }
            }
        } else if let plan = fullResolutionExportReplayPlan() {
            collectExportDependency(at: plan.input, traversal: &traversal)
            for operation in plan.operations {
                collectExportDependencies(from: operation, traversal: &traversal)
            }
        } else if let currentInputURL {
            collectExportDependency(at: currentInputURL, traversal: &traversal)
        } else if let selectedAsset {
            traversal.assetIDs.insert(selectedAsset.id)
        }
        return traversal.assetIDs
    }

    private func collectExportDependencies(
        from layer: ProcessingLayer,
        traversal: inout ExportDependencyTraversal
    ) {
        guard traversal.layerIDs.insert(layer.id).inserted else {
            return
        }
        if let inputURL = layer.inputURL {
            collectExportDependency(at: inputURL, traversal: &traversal)
        }
        collectExportDependencies(from: layer.parameters, traversal: &traversal)
        if let sourceArtifactID = layer.sourceArtifactID {
            collectExportDependencies(fromArtifactID: sourceArtifactID, traversal: &traversal)
        }
        if let maskArtifactID = layer.maskArtifactID {
            collectExportDependencies(fromArtifactID: maskArtifactID, traversal: &traversal)
        }
        guard let operation = editOperation(for: layer) else {
            return
        }
        collectExportDependencies(from: operation, traversal: &traversal)
        if let plan = editGraphReplayPlan(
            finalOperationID: operation.id,
            finalOperationIsEligible: { _ in true }
        ) {
            collectExportDependency(at: plan.input, traversal: &traversal)
            for operation in plan.operations {
                collectExportDependencies(from: operation, traversal: &traversal)
            }
        }
    }

    private func collectExportDependencies(
        from operation: EditOperation,
        traversal: inout ExportDependencyTraversal
    ) {
        guard traversal.operationIDs.insert(operation.id).inserted else {
            return
        }
        collectExportDependencies(from: operation.parameters, traversal: &traversal)
    }

    private func collectExportDependencies(
        fromArtifactID artifactID: ProcessingArtifact.ID,
        traversal: inout ExportDependencyTraversal
    ) {
        guard traversal.artifactIDs.insert(artifactID).inserted,
              let artifact = project.artifacts.first(where: { $0.id == artifactID })
        else {
            return
        }
        for sourceArtifactID in artifact.sourceArtifactIDs {
            collectExportDependencies(fromArtifactID: sourceArtifactID, traversal: &traversal)
        }
        collectExportDependencies(from: artifact.parameters, traversal: &traversal)
        collectExportDependencies(from: artifact.metrics, traversal: &traversal)
        for outputURL in artifact.outputURLs {
            collectExportDependency(at: outputURL, traversal: &traversal)
        }
        for frame in artifact.frames {
            collectExportDependencies(from: frame.metrics, traversal: &traversal)
            if let sourceURL = frame.sourceURL {
                collectExportDependency(at: sourceURL, traversal: &traversal)
            }
            if let outputURL = frame.outputURL {
                collectExportDependency(at: outputURL, traversal: &traversal)
            }
        }
    }

    private func collectExportDependencies(
        from parameters: [String: String],
        traversal: inout ExportDependencyTraversal
    ) {
        let singleAssetIDKeys = ["assetID", "referenceID", "movingID", "sourceAssetID"]
        let assetIDListKeys = [
            "calibrationBiasIDs",
            "calibrationDarkIDs",
            "calibrationFlatIDs",
            "calibrationLightIDs",
            "lightIDs",
            "movingFrameIDs",
            "panelIDs",
            "selectedMovingFrameIDs",
            "sourceFrameIDs",
            "sourceLightIDs",
        ]
        for key in singleAssetIDKeys {
            if let assetID = parameters[key].flatMap(UUID.init(uuidString:)) {
                traversal.assetIDs.insert(assetID)
            }
        }
        for key in assetIDListKeys {
            guard let value = parameters[key] else {
                continue
            }
            for token in value.split(separator: ",") {
                if let assetID = UUID(uuidString: token.trimmingCharacters(in: .whitespacesAndNewlines)) {
                    traversal.assetIDs.insert(assetID)
                }
            }
        }

        for key in ["artifactID", "sourceArtifactID", "sourceMaskID"] {
            if let artifactID = parameters[key].flatMap(UUID.init(uuidString:)) {
                collectExportDependencies(fromArtifactID: artifactID, traversal: &traversal)
            }
        }
        for key in ["layerID", "sourceLayerID"] {
            if let layerID = parameters[key].flatMap(UUID.init(uuidString:)),
               let layer = project.layers.first(where: { $0.id == layerID }) {
                collectExportDependencies(from: layer, traversal: &traversal)
            }
        }
        if let operationID = parameters["operationID"].flatMap(UUID.init(uuidString:)),
           let operation = project.editGraph.operations.first(where: { $0.id == operationID }) {
            collectExportDependencies(from: operation, traversal: &traversal)
        }

        for path in parameters.values {
            collectExportDependency(at: URL(fileURLWithPath: path), traversal: &traversal)
        }
    }

    private func collectExportDependency(
        at url: URL,
        traversal: inout ExportDependencyTraversal
    ) {
        let identityPath = Self.workspaceFileIdentityPath(for: url)
        if let asset = project.assets.first(where: {
            Self.workspaceFileIdentityPath(for: $0.originalURL) == identityPath
        }) {
            traversal.assetIDs.insert(asset.id)
        }
    }

    public var latestJob: ProcessingJob? {
        jobs.last
    }

    public func localized(_ key: LocalizedTextKey) -> String {
        language.text(key)
    }

    public func roleName(_ role: CalibrationFrameRole) -> String {
        language.roleName(role)
    }

    public func calibrationBiasStateName(_ state: CalibrationBiasState) -> String {
        language.calibrationBiasStateName(state)
    }

    public func stackMethodName(_ method: StackMethod) -> String {
        language.stackMethodName(method)
    }

    public func alignmentMethodName(_ method: AlignmentMethod) -> String {
        language.alignmentMethodName(method)
    }

    public func artifactKindName(_ kind: ProcessingArtifactKind) -> String {
        language.artifactKindName(kind)
    }

    public func layerKindName(_ kind: ProcessingLayerKind) -> String {
        language.layerKindName(kind)
    }

    public func layerBlendModeName(_ mode: ProcessingLayerBlendMode) -> String {
        language.layerBlendModeName(mode)
    }

    public func projectTemplateName(_ template: ProjectTemplate) -> String {
        language.projectTemplateName(template)
    }

    public func curvePresetName(_ preset: CurvePreset) -> String {
        language.curvePresetName(preset)
    }

    public func curveChannelName(_ channel: CurveChannel) -> String {
        language.curveChannelName(channel)
    }

    public func batchOutputFormatName(_ format: BatchOutputFormat) -> String {
        language.batchOutputFormatName(format)
    }

    public func exportBitDepthName(_ bitDepth: ExportBitDepth) -> String {
        language.exportBitDepthName(bitDepth)
    }

    public func exportColorSpaceName(_ colorSpace: ExportColorSpace) -> String {
        language.exportColorSpaceName(colorSpace)
    }

    public func fitsValueModeName(_ mode: FITSValueMode) -> String {
        language.fitsValueModeName(mode)
    }

    public func rawWhiteBalanceModeName(_ mode: RawWhiteBalanceMode) -> String {
        language.rawWhiteBalanceModeName(mode)
    }

    public func rawBlackLevelModeName(_ mode: RawBlackLevelMode) -> String {
        language.rawBlackLevelModeName(mode)
    }

    public func rawDemosaicQualityName(_ quality: RawDemosaicQuality) -> String {
        language.rawDemosaicQualityName(quality)
    }

    public func mosaicProjectionName(_ projection: MosaicProjection) -> String {
        language.mosaicProjectionName(projection)
    }

    public func mosaicLayoutName(_ layout: MosaicLayoutMode) -> String {
        language.mosaicLayoutName(layout)
    }

    public func mosaicAlignmentName(_ alignment: MosaicAlignmentMode) -> String {
        language.mosaicAlignmentName(alignment)
    }

    public func mosaicBlendModeName(_ blendMode: MosaicBlendMode) -> String {
        language.mosaicBlendModeName(blendMode)
    }

    public func curveParameters(for operation: EditOperation) -> ProcessingParameters? {
        guard operation.kind == .curves else {
            return nil
        }

        var curveParameters = parameters
        curveParameters.curveBlackPoint = doubleParameter("black", in: operation, defaultValue: curveParameters.curveBlackPoint)
        curveParameters.curveMidInput = doubleParameter("midInput", in: operation, defaultValue: curveParameters.curveMidInput)
        curveParameters.curveMidOutput = doubleParameter("midOutput", in: operation, defaultValue: curveParameters.curveMidOutput)
        curveParameters.curveWhitePoint = doubleParameter("white", in: operation, defaultValue: curveParameters.curveWhitePoint)
        curveParameters.curveChannel = CurveChannel(rawValue: operation.parameters["channel"] ?? "") ?? .rgb
        if let points = curvePointsParameter(in: operation) {
            curveParameters.curvePoints = points
        }
        return curveParameters
    }

    public func matchingCustomCurvePreset(for parameters: ProcessingParameters) -> CustomCurvePreset? {
        customCurvePresets.first { $0.matches(parameters) }
    }

    public func editOperationPresetName(_ operation: EditOperation) -> String? {
        guard let rawValue = operation.parameters["preset"] else {
            return nil
        }
        if let preset = CurvePreset(rawValue: rawValue) {
            return curvePresetName(preset)
        }
        let customPrefix = "custom:"
        if rawValue.hasPrefix(customPrefix) {
            return String(rawValue.dropFirst(customPrefix.count))
        }
        return rawValue
    }

    public var lightAssets: [PhotonStackAsset] {
        assets(with: .light)
    }

    public var batchRegistrationAssets: [PhotonStackAsset] {
        Self.batchRegistrationAssets(in: project)
    }

    public var hasVisiblePreviewLayers: Bool {
        visiblePreviewLayers.isEmpty == false
    }

    public var canPreviewVisibleLayerStack: Bool {
        let layers = visiblePreviewLayers
        return layers.isEmpty == false && layers.allSatisfy { layer in
            isLayerInputAvailable(layer) && isLayerMaskInputAvailable(layer)
        }
    }

    public var applicationCommandSnapshot: PhotonStackApplicationCommandSnapshot {
        let reference = batchRegistrationAssets.first
        return PhotonStackApplicationCommandSnapshot(
            language: language,
            isWorkspaceActive: true,
            canRunBatchRegistration: isProcessing == false && canRegisterBatch(reference: reference),
            canPreviewLayerStack: isProcessing == false && canPreviewVisibleLayerStack
        )
    }

    public var hasAvailableLayerSource: Bool {
        project.assets.contains { firstMissingAsset(in: [$0]) == nil } ||
            project.artifacts.contains(where: isArtifactPreviewAvailable)
    }

    public func isArtifactPreviewAvailable(_ artifact: ProcessingArtifact) -> Bool {
        artifact.previewURL.map(inputFileExists) ?? false
    }

    public func isLayerInputAvailable(_ layer: ProcessingLayer) -> Bool {
        layer.inputURL.map(inputFileExists) ?? false
    }

    private func isLayerReplaySourceAvailable(_ layer: ProcessingLayer) -> Bool {
        var resolvingLayerIDs: Set<ProcessingLayer.ID> = []
        var resolvingOperationIDs: Set<EditOperation.ID> = []
        return isLayerReplaySourceAvailable(
            layer,
            resolvingLayerIDs: &resolvingLayerIDs,
            resolvingOperationIDs: &resolvingOperationIDs
        )
    }

    private func isLayerReplaySourceAvailable(
        _ layer: ProcessingLayer,
        resolvingLayerIDs: inout Set<ProcessingLayer.ID>,
        resolvingOperationIDs: inout Set<EditOperation.ID>
    ) -> Bool {
        guard resolvingLayerIDs.insert(layer.id).inserted else {
            return false
        }
        defer {
            resolvingLayerIDs.remove(layer.id)
        }

        let operation = editOperation(for: layer)
        switch layer.parameters["source"] {
        case "operation":
            guard operation != nil else {
                return false
            }
        case "asset":
            guard let assetID = layer.parameters["assetID"].flatMap(UUID.init(uuidString:)),
                  project.assets.contains(where: { $0.id == assetID })
            else {
                return false
            }
        case "artifact":
            guard let artifactID = layer.sourceArtifactID,
                  project.artifacts.contains(where: { $0.id == artifactID })
            else {
                return false
            }
        default:
            break
        }

        if let operation {
            return isOperationReplaySourceAvailable(
                operation,
                sourceLayerID: layer.parameters["sourceLayerID"].flatMap(UUID.init(uuidString:)),
                resolvingLayerIDs: &resolvingLayerIDs,
                resolvingOperationIDs: &resolvingOperationIDs
            )
        }
        if let assetID = layer.parameters["assetID"].flatMap(UUID.init(uuidString:)) {
            return project.assets.contains { $0.id == assetID }
        }
        return layer.inputURL.map(inputFileExists) ?? (selectedAsset != nil)
    }

    private func isOperationReplaySourceAvailable(
        _ operation: EditOperation,
        sourceLayerID: ProcessingLayer.ID?,
        resolvingLayerIDs: inout Set<ProcessingLayer.ID>,
        resolvingOperationIDs: inout Set<EditOperation.ID>
    ) -> Bool {
        guard resolvingOperationIDs.insert(operation.id).inserted else {
            return false
        }
        defer {
            resolvingOperationIDs.remove(operation.id)
        }

        if operation.kind == .rawDecode {
            if let rawAssetID = operation.parameters["assetID"] {
                guard let assetID = UUID(uuidString: rawAssetID) else {
                    return false
                }
                return project.assets.contains { $0.id == assetID && $0.kind == .raw }
            }
            return project.assets.filter { $0.kind == .raw }.count == 1
        }

        if let sourceLayerID {
            guard let sourceLayer = project.layers.first(where: { $0.id == sourceLayerID }) else {
                return false
            }
            return isLayerReplaySourceAvailable(
                sourceLayer,
                resolvingLayerIDs: &resolvingLayerIDs,
                resolvingOperationIDs: &resolvingOperationIDs
            )
        }

        if let inputPath = operation.parameters["input"] {
            return isReplaySourceAvailable(
                matching: URL(fileURLWithPath: inputPath),
                resolvingLayerIDs: &resolvingLayerIDs,
                resolvingOperationIDs: &resolvingOperationIDs
            )
        }
        return selectedAsset != nil
    }

    private func isReplaySourceAvailable(
        matching sourceURL: URL,
        resolvingLayerIDs: inout Set<ProcessingLayer.ID>,
        resolvingOperationIDs: inout Set<EditOperation.ID>
    ) -> Bool {
        let sourceIdentity = Self.workspaceFileIdentityPath(for: sourceURL)
        if project.assets.contains(where: {
            Self.workspaceFileIdentityPath(for: $0.originalURL) == sourceIdentity
        }) {
            return true
        }

        let sourceLayers = project.layers.filter {
            $0.kind != .mask && $0.inputURL.map {
                Self.workspaceFileIdentityPath(for: $0) == sourceIdentity
            } == true
        }
        guard sourceLayers.count <= 1 else {
            return false
        }
        if let sourceLayer = sourceLayers.first {
            return isLayerReplaySourceAvailable(
                sourceLayer,
                resolvingLayerIDs: &resolvingLayerIDs,
                resolvingOperationIDs: &resolvingOperationIDs
            )
        }

        let sourceOperations = project.editGraph.operations.filter {
            $0.parameters["output"].map {
                Self.workspaceFileIdentityPath(for: URL(fileURLWithPath: $0)) == sourceIdentity
            } ?? false
        }
        guard sourceOperations.count <= 1 else {
            return false
        }
        if let sourceOperation = sourceOperations.first {
            return isOperationReplaySourceAvailable(
                sourceOperation,
                sourceLayerID: nil,
                resolvingLayerIDs: &resolvingLayerIDs,
                resolvingOperationIDs: &resolvingOperationIDs
            )
        }
        return inputFileExists(sourceURL)
    }

    public func canCreateLayer(from artifact: ProcessingArtifact) -> Bool {
        guard let previewURL = artifact.previewURL,
              isArtifactPreviewAvailable(artifact)
        else {
            return false
        }
        guard artifact.kind == .mask else {
            return true
        }
        return project.layers.contains(where: { layer in
            layer.kind == .mask &&
                (layer.sourceArtifactID == artifact.id || layer.inputURL.map {
                    Self.urlsReferenceSameFile($0, previewURL)
                } == true)
        }) == false
    }

    private var visiblePreviewLayers: [ProcessingLayer] {
        project.layers.filter { $0.isVisible && $0.kind != .mask }
    }

    public var availableMaskArtifacts: [ProcessingArtifact] {
        project.artifacts.filter { $0.kind == .mask && isArtifactPreviewAvailable($0) }
    }

    public func isLayerMaskInputAvailable(_ layer: ProcessingLayer) -> Bool {
        guard let maskArtifactID = layer.maskArtifactID else {
            return true
        }
        guard let artifact = project.artifacts.first(where: { $0.id == maskArtifactID }) else {
            return false
        }
        return isArtifactPreviewAvailable(artifact)
    }

    private func rejectUnavailableWorkspaceURL(_ url: URL?, name: String) -> Bool {
        guard let url, inputFileExists(url) else {
            errorMessage = "\(localized(.missingWorkspaceSourceError)): \(name)"
            return true
        }
        return false
    }

    public var darkAssets: [PhotonStackAsset] {
        assets(with: .dark)
    }

    public var biasAssets: [PhotonStackAsset] {
        assets(with: .bias)
    }

    public var flatAssets: [PhotonStackAsset] {
        assets(with: .flat)
    }

    public var mosaicAssets: [PhotonStackAsset] {
        let panels = assets(with: .mosaic)
        var orderIndex: [PhotonStackAsset.ID: Int] = [:]
        for (offset, assetID) in project.mosaicPanelOrder.enumerated()
            where orderIndex[assetID] == nil {
            orderIndex[assetID] = offset
        }
        return panels.enumerated().sorted { left, right in
            let leftOrder = orderIndex[left.element.id] ?? Int.max
            let rightOrder = orderIndex[right.element.id] ?? Int.max
            if leftOrder == rightOrder {
                return left.offset < right.offset
            }
            return leftOrder < rightOrder
        }
        .map(\.element)
    }

    public var mosaicPlan: MosaicPlanSummary {
        MosaicPlanSummary(
            assets: mosaicAssets,
            overlapPixels: parameters.mosaicOverlapPixels,
            layout: parameters.mosaicLayout,
            columns: parameters.mosaicColumns
        )
    }

    public func startSaveProject(to directory: URL) {
        guard hasActiveProcessing == false else {
            return
        }
        launch { await self.saveProject(to: directory) }
    }

    public func startLoadProject(from directory: URL) {
        guard hasActiveProcessing == false else {
            return
        }
        clearPendingAssetImports()
        launch { await self.loadProject(from: directory) }
    }

    public func startInspectSelectedAsset() {
        launch { await self.inspectSelectedAsset() }
    }

    public func startBuildPreview() {
        launch { await self.buildPreview() }
    }

    public func startAutoStretchPreview() {
        launch { await self.autoStretchPreview() }
    }

    public func startLocalContrastPreview() {
        launch { await self.localContrastPreview() }
    }

    public func startDenoisePreview() {
        launch { await self.denoisePreview() }
    }

    public func startSharpenPreview() {
        launch { await self.sharpenPreview() }
    }

    public func startReduceStarsPreview() {
        launch { await self.reduceStarsPreview() }
    }

    public func startReduceComaPreview() {
        launch { await self.reduceComaPreview() }
    }

    public func startDetectStars(sigmaThreshold: Double, minPeak: Double, maxStars: Int = 50_000) {
        launch { await self.detectStars(sigmaThreshold: sigmaThreshold, minPeak: minPeak, maxStars: maxStars) }
    }

    public func startCreateStarMask(
        radius: Int,
        largeRadius: Int,
        layered: Bool,
        sigmaThreshold: Double,
        minPeak: Double
    ) {
        launch {
            await self.createStarMask(
                radius: radius,
                largeRadius: largeRadius,
                layered: layered,
                sigmaThreshold: sigmaThreshold,
                minPeak: minPeak
            )
        }
    }

    public func startDetectArtifactTrails() {
        launch { await self.detectArtifactTrails() }
    }

    public func startRemoveArtifactTrails(selectedIndices: [Int]? = nil, removeMeteors: Bool = false) {
        launch { await self.removeArtifactTrails(selectedIndices: selectedIndices, removeMeteors: removeMeteors) }
    }

    public func startCleanTimelapseArtifactSequence() {
        launch { await self.cleanTimelapseArtifactSequence() }
    }

    public func startExtractMeteorLayer() {
        launch { await self.extractMeteorLayer() }
    }

    public func startRestoreMeteorsFromSelectedAsset() {
        launch { await self.restoreMeteorsFromSelectedAsset() }
    }

    public func startBackgroundPreview(
        model: String,
        mode: String,
        strength: Double,
        preserveBrightness: Bool = true,
        protectBrightTargets: Bool = true
    ) {
        launch {
            await self.backgroundPreview(
                model: model,
                mode: mode,
                strength: strength,
                preserveBrightness: preserveBrightness,
                protectBrightTargets: protectBrightTargets
            )
        }
    }

    public func startDetectClouds() {
        launch { await self.detectClouds() }
    }

    public func startDetectCloudsTemporally() {
        launch { await self.detectCloudsTemporally() }
    }

    public var temporalCloudReferenceCount: Int {
        guard let input = selectedAsset?.originalURL else {
            return 0
        }
        return temporalCloudReferenceURLs(for: input).count
    }

    public func startRemoveClouds(selectedIndices: [Int]? = nil, strength: Double? = nil) {
        launch { await self.removeClouds(selectedIndices: selectedIndices, strength: strength ?? self.parameters.cloudRemovalStrength) }
    }

    public func startNormalizePreview(targetBackground: Double, targetScale: Double) {
        launch { await self.normalizePreview(targetBackground: targetBackground, targetScale: targetScale) }
    }

    public func startColorNeutralizePreview(strength: Double) {
        launch { await self.colorNeutralizePreview(strength: strength) }
    }

    public func startColorSaturatePreview(amount: Double) {
        launch { await self.colorSaturatePreview(amount: amount) }
    }

    public func startDeconvolvePreview(iterations: Int, radius: Int, sigma: Double) {
        launch { await self.deconvolvePreview(iterations: iterations, radius: radius, sigma: sigma) }
    }

    public func startRunDrizzle(scale: Int, pixfrac: Double = 1.0, alignment: AlignmentMethod) {
        launch { await self.runDrizzle(scale: scale, pixfrac: pixfrac, alignment: alignment) }
    }

    public func startExportCurrentImage(to output: URL) {
        guard editGraphNeedsReplay == false else {
            errorMessage = localized(.editGraphNeedsReplay)
            return
        }
        guard hasAmbiguousEditGraphProvenance == false else {
            errorMessage = localized(.editGraphAmbiguousProvenance)
            return
        }
        if let missingAsset = exportBlockingMissingAsset {
            errorMessage = "\(localized(.missingAssetExportError)): \(missingAsset.displayName)"
            return
        }
        launch { await self.exportCurrentImage(to: output) }
    }

    public func startExportProductManifestToDesktop() {
        launch { await self.exportProductManifest(to: self.defaultProductManifestURL()) }
    }

    public func startPreviewLayerStack() {
        launch { await self.previewLayerStack() }
    }

    public func startRunConsoleCommand(_ text: String) {
        launch { await self.runConsoleCommand(text) }
    }

    public func startRegister(reference: PhotonStackAsset, moving: PhotonStackAsset, alignment: AlignmentMethod) {
        launch { await self.register(reference: reference, moving: moving, alignment: alignment) }
    }

    public func startRegisterBatch(reference: PhotonStackAsset, alignment: AlignmentMethod) {
        launch { await self.registerBatch(reference: reference, alignment: alignment) }
    }

    public func isBatchRegistrationFrameSelected(_ asset: PhotonStackAsset) -> Bool {
        batchRegistrationSelection.contains(asset.id)
    }

    public func setBatchRegistrationFrame(_ asset: PhotonStackAsset, isSelected: Bool) {
        guard canModifyProcessingConfiguration,
              batchRegistrationCandidateIDs.contains(asset.id)
        else {
            return
        }
        if isSelected {
            batchRegistrationSelection.insert(asset.id)
        } else {
            batchRegistrationSelection.remove(asset.id)
        }
    }

    public func selectAllBatchRegistrationFrames() {
        guard canModifyProcessingConfiguration else {
            return
        }
        batchRegistrationSelection = batchRegistrationCandidateIDs
    }

    public func clearBatchRegistrationFrames() {
        guard canModifyProcessingConfiguration else {
            return
        }
        batchRegistrationSelection.removeAll()
    }

    public func selectOnlyBatchRegistrationFrame(_ asset: PhotonStackAsset) {
        guard canModifyProcessingConfiguration,
              batchRegistrationCandidateIDs.contains(asset.id)
        else {
            return
        }
        batchRegistrationSelection = [asset.id]
    }

    public func batchRegistrationSelectedMovingCount(reference: PhotonStackAsset?) -> Int {
        selectedBatchRegistrationMovingFrames(reference: reference).count
    }

    public func startRefreshHistogram() {
        launch { await self.refreshHistogram() }
    }

    public func startApplyCurve() {
        cancelCurvePreviewTask()
        let inputOverride = curvePreviewBaseInputURL
        let referenceHistogramOverride = curvePreviewReferenceHistogram
        curvePreviewBaseInputURL = nil
        curvePreviewReferenceHistogram = nil
        launch { await self.applyCurvePreview(inputOverride: inputOverride, referenceHistogramOverride: referenceHistogramOverride) }
    }

    public func startCurvePreview() {
        scheduleCurvePreview(points: parameters.curveEditorPoints)
    }

    public func scheduleCurvePreview(points: [CurveEditorPoint]) {
        guard canProcessCurrentInput,
              activeTask == nil,
              batchTask == nil,
              isBatchRunning == false,
              activeProgressJobID == nil
        else {
            return
        }
        guard let input = curvePreviewBaseInputURL ?? validatedCurrentInputURL() else {
            return
        }
        if curvePreviewBaseInputURL == nil {
            curvePreviewBaseInputURL = input
            curvePreviewReferenceHistogram = curveReferenceHistogram ?? histogram
        }

        curvePreviewGeneration += 1
        pendingCurvePreviewPoints = points
        guard curvePreviewTask == nil else {
            return
        }
        let loopID = UUID()
        curvePreviewLoopID = loopID
        curvePreviewTask = Task { @MainActor in
            defer {
                if self.curvePreviewLoopID == loopID {
                    self.curvePreviewLoopID = nil
                    self.curvePreviewTask = nil
                }
            }
            await self.runInteractiveCurvePreviewLoop(input: input)
        }
    }

    private func runInteractiveCurvePreviewLoop(input: URL) async {
        while Task.isCancelled == false {
            do {
                // Coalesce pointer events into a steady ~20 fps render cadence.
                try await Task.sleep(for: .milliseconds(50))
                try Task.checkCancellation()
                guard let normalizedPoints = pendingCurvePreviewPoints else {
                    return
                }
                pendingCurvePreviewPoints = nil

                let generation = curvePreviewGeneration
                let startedAt = Date()
                let rawOptions = parameters.rawProcessingOptions
                let channel = parameters.curveChannel
                let interactiveWidth = interactiveCurvePreviewWidth(for: parameters.previewWidth)
                let operationParameters = curveOperationParameters(
                    points: normalizedPoints,
                    channel: channel,
                    previewWidth: interactiveWidth
                )
                let output = makeCachedPreviewOutput(
                    prefix: "curves-interactive",
                    input: input,
                    parameters: operationParameters
                        .merging(rawDecodeParameters(from: rawOptions), uniquingKeysWith: { _, new in new }),
                    includeEditGraph: false
                )

                // Interactive previews bypass runProcessingJob, whose normal setup
                // creates this directory. A fresh unsaved project therefore needs
                // the same setup before the native bridge opens its PNG destination.
                try FileManager.default.createDirectory(
                    at: previewDirectory,
                    withIntermediateDirectories: true
                )

                var executionPath = "processing-service"
                let result = try await runValidatedCachedStep(output: output, command: "curves interactive") {
                    let pointPairs = normalizedPoints.map { ($0.input, $0.output) }
                    if let bridged = try await EngineCurvePreview.runIfAvailableAsync(
                        using: processingService,
                        input: input,
                        output: output,
                        width: interactiveWidth,
                        rawOptions: rawOptions,
                        points: pointPairs,
                        channel: channel,
                        retainDecodedSource: true
                    ) {
                        executionPath = "engine-bridge"
                        return bridged
                    }
                    executionPath = "processing-service"
                    return try await processingService.makeCurvePreview(
                        input: input,
                        output: output,
                        width: interactiveWidth,
                        rawOptions: rawOptions,
                        points: pointPairs,
                        channel: channel
                    )
                }
                try Task.checkCancellation()
                guard generation == curvePreviewGeneration,
                      curvePreviewBaseInputURL.map({ Self.urlsReferenceSameFile($0, input) }) == true
                else {
                    continue
                }
                logCurveTelemetry(
                    event: "interactive-preview-complete",
                    startedAt: startedAt,
                    source: isCacheHitMessage(result.standardOutput) ? "cache" : executionPath,
                    width: interactiveWidth,
                    points: normalizedPoints.count
                )
                setPreview(
                    output,
                    trackHistory: false,
                    resetCurveReferenceHistogram: false
                )
                if let referenceHistogram = curvePreviewReferenceHistogram {
                    curveReferenceHistogram = referenceHistogram
                }
                commandOutput = "Curve preview"
                errorMessage = nil
            } catch is CancellationError {
                return
            } catch {
                guard Task.isCancelled == false else {
                    return
                }
                errorMessage = error.localizedDescription
            }
        }
    }

    @discardableResult
    public func cancelCurvePreview(restoreBasePreview: Bool = false) -> Bool {
        let baseInput = curvePreviewBaseInputURL
        let referenceHistogram = curvePreviewReferenceHistogram
        let hadPreview = baseInput != nil
        cancelCurvePreviewTask()
        curvePreviewBaseInputURL = nil
        curvePreviewReferenceHistogram = nil
        Task {
            await EngineCurvePreview.clearCachedSourceAsync()
        }
        guard restoreBasePreview, let baseInput else {
            return hadPreview
        }
        setPreview(baseInput, trackHistory: false, resetCurveReferenceHistogram: false)
        if let referenceHistogram {
            curveReferenceHistogram = referenceHistogram
            histogram = referenceHistogram
        }
        return true
    }

    public func resetCurveEditor() {
        guard canModifyWorkspaceProducts else {
            return
        }
        let restoredInteractivePreview = cancelCurvePreview(restoreBasePreview: true)
        parameters.resetCurve()
        if restoreConsecutiveAppliedCurves(command: "Curve reset") {
            return
        }
        if restoredInteractivePreview {
            commandOutput = "Curve reset"
            errorMessage = nil
            return
        }
        if restorePreviousPreviewState(command: "Curve reset") {
            return
        }
        if project.editGraph.operations.last?.kind == .curves {
            recordEditGraphUndoSnapshot()
            project.editGraph.operations.removeLast()
            triggerAutosave()
        }
        commandOutput = "Curve reset"
        errorMessage = nil
    }

    /// Restores the state before a run of curve operations produced by the old
    /// drag-to-apply behavior. Each history state must be the exact graph prefix
    /// of the current state so unrelated edits are never unwound.
    private func restoreConsecutiveAppliedCurves(command: String) -> Bool {
        var restored = false
        while project.editGraph.operations.last?.kind == .curves,
              let previous = previewHistory.last,
              isSingleCurveHistoryTransition(from: previous.editGraph, to: project.editGraph) {
            guard restorePreviousPreviewState(command: command) else {
                break
            }
            restored = true
        }
        return restored
    }

    private func isSingleCurveHistoryTransition(from previous: EditGraph, to current: EditGraph) -> Bool {
        guard current.operations.count == previous.operations.count + 1,
              current.operations.last?.kind == .curves
        else {
            return false
        }
        return zip(previous.operations, current.operations).allSatisfy { prior, latest in
            prior.id == latest.id
        }
    }

    public func startReplayEditGraph() {
        launch { await self.replayEditGraph() }
    }

    public func startRunMosaic() {
        launch { await self.runMosaic() }
    }

    public func startRunMosaicPreview() {
        launch { await self.runMosaicPreview() }
    }

    public func loadUserSettings() async {
        do {
            let baseline = hasLoadedUserSettings
                ? currentUserSettingsSnapshot()
                : initialUserSettingsSnapshot ?? currentUserSettingsSnapshot()
            let settings = try await settingsRepository.load()
            let current = currentUserSettingsSnapshot()

            if current.recentProjects == baseline.recentProjects {
                recentProjects = settings.recentProjects
            } else {
                recentProjects = AppSettings(
                    recentProjects: current.recentProjects + settings.recentProjects
                ).recentProjects
            }
            customCurvePresets = current.customCurvePresets == baseline.customCurvePresets
                ? settings.customCurvePresets
                : current.customCurvePresets
            autosaveEnabled = current.autosaveEnabled == baseline.autosaveEnabled
                ? settings.autosaveEnabled
                : current.autosaveEnabled
            selectedTemplate = current.selectedTemplate == baseline.selectedTemplate
                ? settings.selectedTemplate
                : current.selectedTemplate

            var mergedExportOptions = settings.exportOptions
            if current.exportOptions.bitDepth != baseline.exportOptions.bitDepth {
                mergedExportOptions.bitDepth = current.exportOptions.bitDepth
            }
            if current.exportOptions.colorSpace != baseline.exportOptions.colorSpace {
                mergedExportOptions.colorSpace = current.exportOptions.colorSpace
            }
            if current.exportOptions.fitsValueMode != baseline.exportOptions.fitsValueMode {
                mergedExportOptions.fitsValueMode = current.exportOptions.fitsValueMode
            }
            if current.exportOptions.jpegQuality != baseline.exportOptions.jpegQuality {
                mergedExportOptions.jpegQuality = current.exportOptions.jpegQuality
            }
            exportOptions = mergedExportOptions

            if current.language != baseline.language {
                language = current.language ?? language
            } else if let savedLanguage = settings.language {
                language = savedLanguage
            }

            hasLoadedUserSettings = true
            let mergedSettings = currentUserSettingsSnapshot()
            initialUserSettingsSnapshot = mergedSettings
            scheduleUserSettingsPersistence(mergedSettings)
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    public func persistUserSettings() {
        scheduleUserSettingsPersistence(currentUserSettingsSnapshot())
    }

    func waitForPendingUserSettingsPersistence() async {
        while hasPendingUserSettingsPersistence, let task = settingsPersistenceTask {
            await task.value
        }
    }

    private func currentUserSettingsSnapshot() -> AppSettings {
        AppSettings(
            recentProjects: recentProjects,
            autosaveEnabled: autosaveEnabled,
            selectedTemplate: selectedTemplate,
            language: language,
            customCurvePresets: customCurvePresets,
            exportOptions: exportOptions
        )
    }

    private func scheduleUserSettingsPersistence(_ settings: AppSettings) {
        settingsPersistenceGeneration += 1
        let generation = settingsPersistenceGeneration
        let previousTask = settingsPersistenceTask
        let repository = settingsRepository
        hasPendingUserSettingsPersistence = true
        let task = Task { @MainActor [weak self] in
            await previousTask?.value
            let persistenceError: (any Error)?
            do {
                try await repository.save(settings)
                persistenceError = nil
            } catch {
                persistenceError = error
            }
            guard let self, self.settingsPersistenceGeneration == generation else {
                return
            }
            if let persistenceError {
                let message = "\(self.localized(.settingsSaveFailed)): \(persistenceError.localizedDescription)"
                self.settingsPersistenceErrorMessage = message
                self.errorMessage = message
            } else {
                if self.errorMessage == self.settingsPersistenceErrorMessage {
                    self.errorMessage = nil
                }
                self.settingsPersistenceErrorMessage = nil
            }
            self.hasPendingUserSettingsPersistence = false
        }
        settingsPersistenceTask = task
        PhotonStackApplicationActivity.trackSettingsPersistence(task)
    }

    public func setAutosaveEnabled(_ isEnabled: Bool) {
        autosaveEnabled = isEnabled
        persistUserSettings()
        if isEnabled {
            scheduleAutosaveIfNeeded()
        }
    }

    public func setExportBitDepth(_ bitDepth: ExportBitDepth) {
        guard canModifyProcessingConfiguration else {
            return
        }
        exportOptions.bitDepth = bitDepth
        persistUserSettings()
    }

    public func setExportColorSpace(_ colorSpace: ExportColorSpace) {
        guard canModifyProcessingConfiguration else {
            return
        }
        exportOptions.colorSpace = colorSpace
        persistUserSettings()
    }

    public func setFITSValueMode(_ mode: FITSValueMode) {
        guard canModifyProcessingConfiguration else {
            return
        }
        exportOptions.fitsValueMode = mode
        persistUserSettings()
    }

    public func setExportJPEGQuality(_ quality: Double) {
        guard canModifyProcessingConfiguration else {
            return
        }
        exportOptions.jpegQuality = Self.normalizedFinite(
            quality,
            in: 0...1,
            fallback: exportOptions.jpegQuality
        )
        persistUserSettings()
    }

    public func createProjectFromSelectedTemplate() {
        _ = requestCreateProject(template: selectedTemplate)
    }

    @discardableResult
    public func requestCreateProject(
        template: ProjectTemplate,
        name: String? = nil,
        importAfterCreate: Bool = false
    ) -> Bool {
        guard hasActiveProcessing == false else {
            return false
        }
        guard hasUnsavedChanges else {
            createProject(template: template, name: name)
            return true
        }
        pendingProjectTransition = .create(
            template: template,
            name: name,
            importAfterCreate: importAfterCreate
        )
        return false
    }

    public func requestOpenProject(from directory: URL) {
        guard hasActiveProcessing == false else {
            return
        }
        guard hasUnsavedChanges else {
            startLoadProject(from: directory)
            return
        }
        pendingProjectTransition = .open(directory: directory)
    }

    @discardableResult
    public func requestApplicationTermination() -> Bool {
        if hasActiveProcessing {
            pendingProcessingInterruption = .terminateApplication
            return false
        }
        guard hasUnsavedChanges else {
            return true
        }
        pendingProjectTransition = .terminateApplication
        return false
    }

    @discardableResult
    public func requestWindowClose() -> Bool {
        if hasActiveProcessing {
            pendingProcessingInterruption = .closeWindow
            return false
        }
        guard hasUnsavedChanges else {
            return true
        }
        pendingProjectTransition = .closeWindow
        return false
    }

    public func cancelPendingProcessingInterruption() {
        pendingProcessingInterruption = nil
    }

    @discardableResult
    public func confirmPendingProcessingInterruption(
        _ interruption: PendingProcessingInterruption
    ) -> PendingProjectTransition? {
        guard pendingProcessingInterruption == interruption else {
            return nil
        }
        pendingProcessingInterruption = nil
        interruptActiveTaskIfNeeded()
        let transition: PendingProjectTransition = switch interruption {
        case .closeWindow:
            .closeWindow
        case .terminateApplication:
            .terminateApplication
        }
        guard hasUnsavedChanges else {
            return transition
        }
        pendingProjectTransition = transition
        return nil
    }

    public func cancelPendingProjectTransition() {
        pendingProjectTransition = nil
        importPendingAssetsIfPossible()
    }

    @discardableResult
    public func discardChangesAndPerformPendingProjectTransition() -> PendingProjectTransition? {
        guard hasActiveProcessing == false, let transition = pendingProjectTransition else {
            return nil
        }
        pendingProjectTransition = nil
        performPendingProjectTransitionWithoutSaving(transition)
        return transition
    }

    @discardableResult
    public func saveChangesAndPerformPendingProjectTransition(
        to directory: URL
    ) async -> PendingProjectTransition? {
        guard let transition = beginSavingPendingProjectTransition() else {
            return nil
        }
        return await saveChangesAndPerformProjectTransition(transition, to: directory)
    }

    public func beginSavingPendingProjectTransition() -> PendingProjectTransition? {
        guard hasActiveProcessing == false, let transition = pendingProjectTransition else {
            return nil
        }
        pendingProjectTransition = nil
        return transition
    }

    @discardableResult
    public func saveChangesAndPerformProjectTransition(
        _ transition: PendingProjectTransition,
        to directory: URL
    ) async -> PendingProjectTransition? {
        guard hasActiveProcessing == false else {
            return nil
        }
        await saveProject(to: directory)
        guard hasUnsavedChanges == false else {
            return nil
        }
        switch transition {
        case let .create(template, name, _):
            createProject(template: template, name: name)
        case let .open(directory):
            await loadProject(from: directory)
        case .closeWindow:
            break
        case .terminateApplication:
            break
        }
        return transition
    }

    private func performPendingProjectTransitionWithoutSaving(_ transition: PendingProjectTransition) {
        switch transition {
        case let .create(template, name, _):
            createProject(template: template, name: name)
        case let .open(directory):
            startLoadProject(from: directory)
        case .closeWindow:
            break
        case .terminateApplication:
            break
        }
    }

    public func createProject(template: ProjectTemplate, name: String? = nil) {
        guard hasActiveProcessing == false else {
            return
        }
        cancelMetadataRefresh()
        clearPendingAssetImports()
        let trimmedName = name?.trimmingCharacters(in: .whitespacesAndNewlines)
        clearProjectTransientState()
        project = template.makeProject(name: trimmedName?.isEmpty == false ? trimmedName : nil)
        if usesManagedPreviewDirectory {
            previewDirectory = Self.managedPreviewDirectory(for: project.id)
        }
        selectedTemplate = template
        selectedAssetID = nil
        activeLayerID = nil
        currentProjectDirectory = nil
        previewURL = nil
        previewOperationID = nil
        editGraphNeedsReplay = false
        canvasMode = .sourcePreview
        jobs.removeAll()
        batchQueue.removeAll()
        project.replaceBatchQueue([])
        resetBatchRegistrationSelection(selectAll: true)
        commandOutput = ""
        clearPreviewHistory()
        clearEditGraphHistory()
        projectChangeGeneration += 1
        hasUnsavedChanges = true
        persistUserSettings()
    }

    public func openRecentProject(_ recentProject: RecentProject) {
        requestOpenProject(from: recentProject.directory)
    }

    public func saveCurrentCurvePreset() {
        let preset = CustomCurvePreset(
            name: nextCustomCurvePresetName(),
            parameters: CurvePresetParameters(parameters: parameters)
        )
        customCurvePresets.insert(preset, at: 0)
        persistUserSettings()
    }

    public func renameCustomCurvePreset(_ presetID: CustomCurvePreset.ID, name: String) {
        guard let index = customCurvePresets.firstIndex(where: { $0.id == presetID }) else {
            return
        }

        let uniqueName = uniqueCustomCurvePresetName(preferred: name, excluding: presetID)
        customCurvePresets[index].name = uniqueName
        persistUserSettings()
    }

    public func applyCustomCurvePreset(_ preset: CustomCurvePreset) {
        guard canModifyProcessingConfiguration else {
            return
        }
        parameters.curveBlackPoint = preset.parameters.black
        parameters.curveMidInput = preset.parameters.midInput
        parameters.curveMidOutput = preset.parameters.midOutput
        parameters.curveWhitePoint = preset.parameters.white
    }

    public func deleteCustomCurvePreset(_ presetID: CustomCurvePreset.ID) {
        customCurvePresets.removeAll { $0.id == presetID }
        persistUserSettings()
    }

    public func importCustomCurvePresets(from url: URL) {
        do {
            let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
            if let fileSize = (attributes[.size] as? NSNumber)?.uint64Value,
               fileSize > Self.maximumCurvePresetImportBytes {
                errorMessage = localized(.curvePresetImportTooLarge)
                return
            }
            let data = try Data(contentsOf: url)
            guard UInt64(data.count) <= Self.maximumCurvePresetImportBytes else {
                errorMessage = localized(.curvePresetImportTooLarge)
                return
            }
            let decoder = JSONDecoder()
            decoder.dateDecodingStrategy = .iso8601
            let library = try decoder.decode(CustomCurvePresetLibrary.self, from: data)
            guard library.presets.count <= Self.maximumImportedCurvePresetCount else {
                errorMessage = localized(.curvePresetImportTooMany)
                return
            }
            var existingNames = Set(customCurvePresets.map(\.name))
            let importedPresets = library.presets.map { preset in
                let name = uniqueCustomCurvePresetName(preferred: preset.name, existingNames: existingNames)
                existingNames.insert(name)
                return CustomCurvePreset(
                    id: UUID(),
                    name: name,
                    parameters: preset.parameters,
                    createdAt: Date()
                )
            }
            customCurvePresets.insert(contentsOf: importedPresets, at: 0)
            persistUserSettings()
            errorMessage = nil
            commandOutput = "Imported \(importedPresets.count) curve preset(s)."
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    public func exportCustomCurvePresets(to url: URL) {
        do {
            let library = CustomCurvePresetLibrary(presets: customCurvePresets)
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.prettyPrinted, .sortedKeys]
            encoder.dateEncodingStrategy = .iso8601
            let data = try encoder.encode(library)
            try data.write(to: url, options: .atomic)
            commandOutput = "Exported \(customCurvePresets.count) curve preset(s)."
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    public func enqueueCurrent(_ kind: ProcessingOperationKind) {
        guard isBatchRunning == false,
              batchTask == nil,
              let input = validatedCurrentInputURL()
        else {
            return
        }
        guard Self.supportedBatchOperationKinds.contains(kind) else {
            errorMessage = BatchQueueError.unsupportedOperation.localizedDescription
            return
        }

        let item = BatchQueueItem(title: kind.rawValue, kind: kind, inputURL: input)
        batchQueue.append(item)
        persistBatchQueue()
    }

    public func enqueueAllAssets(_ kind: ProcessingOperationKind) {
        guard isBatchRunning == false,
              batchTask == nil,
              project.assets.isEmpty == false,
              rejectMissingAsset(in: project.assets) == false
        else {
            return
        }
        guard Self.supportedBatchOperationKinds.contains(kind) else {
            errorMessage = BatchQueueError.unsupportedOperation.localizedDescription
            return
        }
        let items = project.assets.map { asset in
            BatchQueueItem(title: "\(kind.rawValue) \(asset.displayName)", kind: kind, inputURL: asset.originalURL)
        }
        batchQueue.append(contentsOf: items)
        persistBatchQueue()
    }

    public func startBatchQueue() {
        guard isBatchRunning == false, isProcessing == false, activeTask == nil else {
            return
        }
        let queuedItems = batchQueue.filter { $0.status == .queued }
        guard queuedItems.isEmpty == false else {
            return
        }
        guard let missingInput = queuedItems.first(where: { inputFileExists($0.inputURL) == false }) else {
            startAvailableBatchQueue()
            return
        }
        errorMessage = "\(localized(.missingAssetProcessingError)): \(missingInput.inputURL.lastPathComponent)"
    }

    private func startAvailableBatchQueue() {

        rawPreviewGeneration += 1
        rawPreviewTask?.cancel()
        rawPreviewTask = nil
        isRawPreviewUpdating = false
        batchGeneration += 1
        let generation = batchGeneration
        isBatchRunning = true
        isBatchPaused = false
        isProcessing = true
        canCancel = true
        activeBatchItemIDs = Set(batchQueue.lazy.filter { $0.status == .queued }.map(\.id))
        batchProgressJobItems.removeAll()
        batchItemProgress = Dictionary(uniqueKeysWithValues: activeBatchItemIDs.map { ($0, 0) })
        updateBatchAggregateProgress()
        batchTask = Task { @MainActor in
            await self.runBatchQueue(generation: generation)
        }
    }

    public func pauseBatchQueue() {
        guard isBatchRunning else {
            return
        }
        isBatchPaused = true
    }

    public func resumeBatchQueue() {
        guard isBatchRunning else {
            return
        }
        isBatchPaused = false
    }

    public func retryFailedBatchItems() {
        guard isBatchRunning == false, batchTask == nil else {
            return
        }
        for index in batchQueue.indices where batchQueue[index].status == .failed || batchQueue[index].status == .cancelled {
            batchQueue[index].status = .queued
            batchQueue[index].message = ""
        }
        persistBatchQueue()
    }

    public func clearBatchQueue() {
        guard isBatchRunning == false, batchTask == nil else {
            return
        }
        batchQueue.removeAll()
        persistBatchQueue()
    }

    public func makePreviewCacheCleanupPlan() -> PreviewCacheCleanupPlan? {
        guard isProcessing == false,
              isBatchRunning == false,
              isRawPreviewUpdating == false,
              activeTask == nil,
              foregroundTaskRequested == false
        else {
            return nil
        }

        do {
            errorMessage = nil
            return try buildPreviewCacheCleanupPlan()
        } catch {
            errorMessage = error.localizedDescription
            return nil
        }
    }

    public func requestPreviewCacheCleanup() {
        guard let plan = makePreviewCacheCleanupPlan() else {
            return
        }
        guard plan.fileCount > 0 else {
            clearPreviewCache(plan)
            return
        }
        pendingPreviewCacheCleanupPlan = plan
    }

    public func cancelPreviewCacheCleanup() {
        pendingPreviewCacheCleanupPlan = nil
    }

    public func confirmPreviewCacheCleanup(_ plan: PreviewCacheCleanupPlan) {
        pendingPreviewCacheCleanupPlan = nil
        clearPreviewCache(plan)
    }

    public func clearPreviewCache(_ requestedPlan: PreviewCacheCleanupPlan) {
        guard isProcessing == false,
              isBatchRunning == false,
              isRawPreviewUpdating == false,
              activeTask == nil,
              foregroundTaskRequested == false
        else {
            return
        }

        do {
            let currentPlan = try buildPreviewCacheCleanupPlan()
            let requestedPaths = Set(requestedPlan.fileURLs.map { $0.standardizedFileURL.path })
            for url in currentPlan.fileURLs
            where requestedPaths.contains(url.standardizedFileURL.path) {
                try FileManager.default.removeItem(at: url)
            }
            try pruneEmptyManagedPreviewDirectories()
            errorMessage = nil
            commandOutput = localized(.previewCacheCleared)
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    public func clearPreviewCache() {
        guard let plan = makePreviewCacheCleanupPlan() else {
            return
        }
        clearPreviewCache(plan)
    }

    private func buildPreviewCacheCleanupPlan() throws -> PreviewCacheCleanupPlan {
        guard FileManager.default.fileExists(atPath: previewDirectory.path) else {
            return PreviewCacheCleanupPlan(fileURLs: [], totalBytes: 0)
        }

        let protectedPaths = previewCacheProtectedPaths()
        let resourceKeys: Set<URLResourceKey> = [
            .isDirectoryKey,
            .isRegularFileKey,
            .isSymbolicLinkKey,
            .fileSizeKey,
        ]
        let contents = try FileManager.default.contentsOfDirectory(
            at: previewDirectory,
            includingPropertiesForKeys: Array(resourceKeys),
            options: []
        )
        var fileURLs: [URL] = []
        var totalBytes: Int64 = 0
        for url in contents.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            let standardizedURL = url.standardizedFileURL
            let values = try standardizedURL.resourceValues(forKeys: resourceKeys)
            guard values.isSymbolicLink != true else {
                continue
            }
            if values.isDirectory == true {
                guard Self.isManagedPreviewDirectoryName(standardizedURL.lastPathComponent) else {
                    continue
                }
                try collectPreviewCacheFiles(
                    in: standardizedURL,
                    protectedPaths: protectedPaths,
                    resourceKeys: resourceKeys,
                    fileURLs: &fileURLs,
                    totalBytes: &totalBytes
                )
                continue
            }
            guard values.isRegularFile == true,
                  Self.isManagedPreviewFileName(standardizedURL.lastPathComponent),
                  protectedPaths.contains(standardizedURL.path) == false
            else {
                continue
            }
            fileURLs.append(standardizedURL)
            totalBytes += Int64(values.fileSize ?? 0)
        }
        return PreviewCacheCleanupPlan(fileURLs: fileURLs, totalBytes: totalBytes)
    }

    private func collectPreviewCacheFiles(
        in directory: URL,
        protectedPaths: Set<String>,
        resourceKeys: Set<URLResourceKey>,
        fileURLs: inout [URL],
        totalBytes: inout Int64
    ) throws {
        let contents = try FileManager.default.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: Array(resourceKeys),
            options: []
        )
        for url in contents.sorted(by: { $0.lastPathComponent < $1.lastPathComponent }) {
            let standardizedURL = url.standardizedFileURL
            let values = try standardizedURL.resourceValues(forKeys: resourceKeys)
            guard values.isSymbolicLink != true else {
                continue
            }
            if values.isDirectory == true {
                try collectPreviewCacheFiles(
                    in: standardizedURL,
                    protectedPaths: protectedPaths,
                    resourceKeys: resourceKeys,
                    fileURLs: &fileURLs,
                    totalBytes: &totalBytes
                )
            } else if values.isRegularFile == true,
                      protectedPaths.contains(standardizedURL.path) == false {
                fileURLs.append(standardizedURL)
                totalBytes += Int64(values.fileSize ?? 0)
            }
        }
    }

    private func pruneEmptyManagedPreviewDirectories() throws {
        guard FileManager.default.fileExists(atPath: previewDirectory.path) else {
            return
        }
        let contents = try FileManager.default.contentsOfDirectory(
            at: previewDirectory,
            includingPropertiesForKeys: [.isDirectoryKey, .isSymbolicLinkKey],
            options: []
        )
        for directory in contents where Self.isManagedPreviewDirectoryName(directory.lastPathComponent) {
            let values = try directory.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            guard values.isDirectory == true, values.isSymbolicLink != true else {
                continue
            }
            try pruneManagedPreviewDirectoryIfEmpty(directory)
        }
    }

    private func pruneManagedPreviewDirectoryIfEmpty(_ directory: URL) throws {
        let contents = try FileManager.default.contentsOfDirectory(
            at: directory,
            includingPropertiesForKeys: [.isDirectoryKey, .isSymbolicLinkKey],
            options: []
        )
        for child in contents {
            let values = try child.resourceValues(forKeys: [.isDirectoryKey, .isSymbolicLinkKey])
            if values.isDirectory == true, values.isSymbolicLink != true {
                try pruneManagedPreviewDirectoryIfEmpty(child)
            }
        }
        if try FileManager.default.contentsOfDirectory(atPath: directory.path).isEmpty {
            try FileManager.default.removeItem(at: directory)
        }
    }

    private static func isManagedPreviewFileName(_ name: String) -> Bool {
        [
            "cache-",
            "artifact-marked-",
            "cloud-marked-",
            "export-layer-stack-",
            "layer-stack-",
            "mask-brush-",
            "meteor-layer-",
            "replay-",
            "restore-meteors-",
        ].contains { name.hasPrefix($0) }
    }

    private static func isManagedPreviewDirectoryName(_ name: String) -> Bool {
        [
            "clean-sequence-",
            "mosaic-",
            "registered-batch-",
            "replay-",
            "workflow-",
        ].contains { name.hasPrefix($0) }
    }

    private func previewCacheProtectedPaths() -> Set<String> {
        var paths: Set<String> = []

        func add(_ url: URL?) {
            if let url {
                let standardizedURL = url.standardizedFileURL
                let identityURL = URL(fileURLWithPath: PhotonStackProject.assetIdentityPath(for: standardizedURL))
                    .standardizedFileURL
                for candidate in [standardizedURL, identityURL] {
                    paths.insert(candidate.path)
                    if isManagedPreviewCacheOutput(candidate) {
                        paths.insert(previewCacheReceiptURL(for: candidate).standardizedFileURL.path)
                    }
                }
            }
        }

        func add(_ graph: EditGraph) {
            for operation in graph.operations {
                add(operation.parameters["output"].map(URL.init(fileURLWithPath:)))
                add(operation.parameters["previewOutput"].map(URL.init(fileURLWithPath:)))
            }
        }

        func add(_ layers: [ProcessingLayer]) {
            for layer in layers {
                add(layer.inputURL)
            }
        }

        add(previewURL)
        add(artifactMarkedPreviewURL)
        add(registrationReport?.outputURL)
        add(project.workspaceState?.previewURL)
        add(project.editGraph)
        add(project.layers)
        for artifact in project.artifacts {
            artifact.outputURLs.forEach { add($0) }
            artifact.frames.forEach { add($0.outputURL) }
        }
        for item in batchQueue {
            add(item.inputURL)
            add(item.outputURL)
        }
        for state in previewHistory {
            add(state.previewURL)
            add(state.editGraph)
            add(state.layers)
        }
        for state in editGraphUndoStack + editGraphRedoStack {
            add(state.editGraph)
            add(state.layers)
        }
        return paths
    }

    public func setBatchOutputDirectory(_ directory: URL) {
        guard isBatchRunning == false, batchTask == nil else {
            return
        }
        _ = directory.startAccessingSecurityScopedResource()
        project.setBatchOutputDirectory(directory)
        triggerAutosave()
    }

    public func clearBatchOutputDirectory() {
        guard isBatchRunning == false, batchTask == nil else {
            return
        }
        project.setBatchOutputDirectory(nil)
        triggerAutosave()
    }

    public func setBatchOutputFormat(_ format: BatchOutputFormat) {
        guard isBatchRunning == false, batchTask == nil else {
            return
        }
        project.setBatchOutputFormat(format)
        triggerAutosave()
    }

    public func setBatchMaxConcurrentTasks(_ count: Int) {
        guard isBatchRunning == false, batchTask == nil else {
            return
        }
        project.setBatchMaxConcurrentTasks(count)
        triggerAutosave()
    }

    public func setRawWhiteBalanceMode(_ mode: RawWhiteBalanceMode) {
        guard canModifyProcessingConfiguration else {
            return
        }
        parameters.rawProcessingOptions.whiteBalanceMode = mode
        scheduleRawPreviewUpdate()
    }

    public func setRawManualWhiteBalanceTemperature(_ temperature: Double) {
        guard canModifyProcessingConfiguration else {
            return
        }
        parameters.rawProcessingOptions.manualWhiteBalanceTemperature = Self.normalizedFinite(
            temperature,
            in: 2000...50000,
            fallback: parameters.rawProcessingOptions.manualWhiteBalanceTemperature
        )
        scheduleRawPreviewUpdate()
    }

    public func setRawManualWhiteBalanceTint(_ tint: Double) {
        guard canModifyProcessingConfiguration else {
            return
        }
        parameters.rawProcessingOptions.manualWhiteBalanceTint = Self.normalizedFinite(
            tint,
            in: -150...150,
            fallback: parameters.rawProcessingOptions.manualWhiteBalanceTint
        )
        scheduleRawPreviewUpdate()
    }

    public func setRawExposureBias(_ bias: Double) {
        guard canModifyProcessingConfiguration else {
            return
        }
        parameters.rawProcessingOptions.exposureBias = Self.normalizedFinite(
            bias,
            in: -2...2,
            fallback: parameters.rawProcessingOptions.exposureBias
        )
        scheduleRawPreviewUpdate()
    }

    public func setRawBlackLevelMode(_ mode: RawBlackLevelMode) {
        guard canModifyProcessingConfiguration else {
            return
        }
        parameters.rawProcessingOptions.blackLevelMode = mode
        scheduleRawPreviewUpdate()
    }

    public func setRawManualBlackLevel(_ level: Double) {
        guard canModifyProcessingConfiguration else {
            return
        }
        parameters.rawProcessingOptions.manualBlackLevel = Self.normalizedFinite(
            level,
            in: 0...1,
            fallback: parameters.rawProcessingOptions.manualBlackLevel
        )
        scheduleRawPreviewUpdate()
    }

    public func setRawDemosaicQuality(_ quality: RawDemosaicQuality) {
        guard canModifyProcessingConfiguration else {
            return
        }
        parameters.rawProcessingOptions.demosaicQuality = quality
        scheduleRawPreviewUpdate()
    }

    public func setRawLinearOutput(_ enabled: Bool) {
        guard canModifyProcessingConfiguration else {
            return
        }
        parameters.rawProcessingOptions.linearOutput = enabled
        scheduleRawPreviewUpdate()
    }

    public func cancelCurrentTask() {
        activeTask?.cancel()
        batchTask?.cancel()
        cancelRawPreviewUpdate()
        commandOutput = localized(.cancelledTask)
    }

    private func interruptActiveTaskIfNeeded() {
        guard activeTask != nil || batchTask != nil || rawPreviewTask != nil ||
                isProcessing || isBatchRunning || isRawPreviewUpdating
        else {
            return
        }

        activeTask?.cancel()
        batchTask?.cancel()
        cancelRawPreviewUpdate()
        if isBatchRunning || batchTask != nil {
            markRunningBatchItemsCancelled()
        }
        foregroundGeneration += 1
        batchGeneration += 1
        activeTask = nil
        batchTask = nil
        activeProgressJobID = nil
        activeBatchItemIDs.removeAll()
        batchProgressJobItems.removeAll()
        batchItemProgress.removeAll()
        isProcessing = false
        foregroundTaskRequested = false
        isBatchRunning = false
        isBatchPaused = false
        canCancel = false
        progressFraction = nil
        progressMessage = nil
        commandOutput = localized(.cancelledTask)

        for index in jobs.indices where jobs[index].status == .running {
            jobs[index].status = .cancelled
            jobs[index].finishedAt = Date()
            jobs[index].message = "Cancelled"
        }
    }

    public var canImportAssetsImmediately: Bool {
        activeTask == nil &&
            batchTask == nil &&
            foregroundTaskRequested == false &&
            isProcessing == false &&
            isBatchRunning == false
    }

    @discardableResult
    public func importAssets(from urls: [URL]) -> Bool {
        var seenPaths = Set(project.assets.map {
            PhotonStackProject.assetIdentityPath(for: $0.originalURL)
        })
        seenPaths.formUnion(pendingAssetImportURLs.map {
            PhotonStackProject.assetIdentityPath(for: $0)
        })
        let readableURLs = expandedImportAssetURLs(from: urls).filter { url in
            seenPaths.insert(PhotonStackProject.assetIdentityPath(for: url)).inserted
        }.map { url in
            _ = url.startAccessingSecurityScopedResource()
            return url
        }
        guard readableURLs.isEmpty == false else {
            return false
        }

        guard canImportAssetsImmediately else {
            pendingAssetImportURLs.append(contentsOf: readableURLs)
            pendingAssetImportCount = pendingAssetImportURLs.count
            commandOutput = String(format: localized(.assetImportQueued), pendingAssetImportCount)
            return false
        }

        cancelRawPreviewUpdate()
        return importReadableAssets(from: readableURLs)
    }

    @discardableResult
    private func importReadableAssets(from readableURLs: [URL]) -> Bool {
        let shouldAdoptImportedSelection = project.assets.isEmpty || selectedAsset == nil

        let existingAssetIDs = Set(project.assets.map(\.id))
        project.addAssets(
            from: readableURLs,
            role: project.template?.defaultImportedAssetRole ?? .light
        )
        let importedAssets = project.assets.filter { existingAssetIDs.contains($0.id) == false }
        guard importedAssets.isEmpty == false else {
            return false
        }
        syncBatchRegistrationSelection(selectNewCandidates: true)
        recordRawSequenceArtifact(for: importedAssets)
        applyAstroStackingDefaultsIfNeeded(for: importedAssets)
        if shouldAdoptImportedSelection {
            selectedAssetID = importedAssets.first?.id ?? project.assets.first?.id
            activeLayerID = nil
            previewURL = selectedAsset?.originalURL
            previewOperationID = nil
            editGraphNeedsReplay = false
            canvasMode = .sourcePreview
            artifactTrailReport = nil
            cloudRegionReport = nil
            detectedStarCount = nil
            replaceMarkedPreview(nil)
            histogram = nil
            curveReferenceHistogram = nil
            clearPreviewHistory()
        }
        errorMessage = nil
        commandOutput = String(format: localized(.assetsImported), importedAssets.count)
        refreshMetadataIfNeeded(for: selectedAsset)
        triggerAutosave()
        return true
    }

    private func importPendingAssetsIfPossible() {
        guard canImportAssetsImmediately, pendingAssetImportURLs.isEmpty == false else {
            return
        }
        let urls = pendingAssetImportURLs
        clearPendingAssetImports()
        cancelRawPreviewUpdate()
        _ = importReadableAssets(from: urls)
    }

    private func clearPendingAssetImports() {
        pendingAssetImportURLs.removeAll()
        pendingAssetImportCount = 0
    }

    private func expandedImportAssetURLs(from urls: [URL]) -> [URL] {
        let fileManager = FileManager.default
        var expanded: [URL] = []
        var seen = Set<String>()

        func appendIfSupported(_ url: URL) {
            guard AssetKind.detect(from: url) != .unknown else {
                return
            }
            guard seen.insert(PhotonStackProject.assetIdentityPath(for: url)).inserted else {
                return
            }
            expanded.append(url)
        }

        for url in urls {
            var isDirectory: ObjCBool = false
            if fileManager.fileExists(atPath: url.path, isDirectory: &isDirectory), isDirectory.boolValue {
                guard let enumerator = fileManager.enumerator(
                    at: url,
                    includingPropertiesForKeys: [.isRegularFileKey],
                    options: [.skipsHiddenFiles, .skipsPackageDescendants]
                ) else {
                    continue
                }
                for case let child as URL in enumerator {
                    let resourceValues = try? child.resourceValues(forKeys: [.isRegularFileKey])
                    if resourceValues?.isRegularFile == true {
                        appendIfSupported(child)
                    }
                }
            } else {
                appendIfSupported(url)
            }
        }

        return expanded.sorted { $0.path.localizedStandardCompare($1.path) == .orderedAscending }
    }

    private func applyAstroStackingDefaultsIfNeeded(for importedAssets: [PhotonStackAsset]) {
        let importedRawCount = importedAssets
            .filter { $0.kind == .raw && $0.role == .light }
            .count
        let lightRawCount = project.assets
            .filter { $0.kind == .raw && $0.role == .light }
            .count

        guard importedRawCount >= 3 || lightRawCount >= 3 else {
            return
        }

        selectedTemplate = .deepSky
        parameters.workflowStackMethod = .winsorized
        parameters.workflowAlignment = .distortion
        parameters.workflowRestoreMeteors = false
        parameters.rawProcessingOptions.blackLevelMode = .auto
        parameters.stretchTargetBackground = 0.16
        parameters.localContrastAmount = 0.18
        parameters.localContrastRadius = 18
        parameters.denoiseAmount = 0.30
        parameters.denoiseChromaAmount = 0.55
        parameters.denoiseRadius = 2
        parameters.starReductionAmount = 0.30
        parameters.profileAwareStarReduction = true
        parameters.edgeAwareStarReduction = true
        parameters.comaReductionAmount = 0.85
        parameters.comaReductionRadius = 10
        parameters.comaReductionEccentricity = 0.20
        parameters.edgeAwareComaReduction = true
    }

    public func select(_ asset: PhotonStackAsset) {
        guard canModifyProcessingConfiguration,
              project.assets.contains(where: { $0.id == asset.id })
        else {
            return
        }
        cancelMetadataRefresh()
        cancelRawPreviewUpdate()
        activeLayerID = nil
        selectedAssetID = asset.id
        restorePersistedRawOptions(for: asset)
        previewURL = asset.originalURL
        previewOperationID = nil
        editGraphNeedsReplay = false
        canvasMode = .sourcePreview
        artifactTrailReport = nil
        cloudRegionReport = nil
        detectedStarCount = nil
        replaceMarkedPreview(nil)
        histogram = nil
        curveReferenceHistogram = nil
        clearPreviewHistory()
        commandOutput = ""
        errorMessage = nil
        refreshMetadataIfNeeded(for: asset)
        triggerAutosave()
    }

    public func removeAsset(_ asset: PhotonStackAsset) {
        guard canModifyProcessingConfiguration,
              project.assets.contains(where: { $0.id == asset.id })
        else {
            return
        }
        pendingAssetRemovalIDs.removeAll { $0 == asset.id }
        cancelMetadataRefresh()
        cancelRawPreviewUpdate()
        let preservedRawOptions = asset.kind == .raw
            ? (persistedRawOptions(for: asset) ?? parameters.rawProcessingOptions)
            : nil
        let removedWasSelected = selectedAssetID == nil || selectedAssetID == asset.id
        let previousLayerIDs = Set(project.layers.map(\.id))
        let previousLayers = project.layers
        let previousOperations = project.editGraph.operations
        let previousArtifacts = project.artifacts

        project.removeAsset(asset.id)

        let remainingLayerIDs = Set(project.layers.map(\.id))
        let removedLayerIDs = previousLayerIDs.subtracting(remainingLayerIDs)
        let removedOperationIDs = Set(previousOperations.map(\.id))
            .subtracting(project.editGraph.operations.map(\.id))
        let removedArtifactIDs = Set(previousArtifacts.map(\.id))
            .subtracting(project.artifacts.map(\.id))
        var invalidatedPaths: Set<String> = [
            PhotonStackProject.assetIdentityPath(for: asset.originalURL),
        ]
        for layer in previousLayers where removedLayerIDs.contains(layer.id) {
            if let inputURL = layer.inputURL {
                invalidatedPaths.insert(PhotonStackProject.assetIdentityPath(for: inputURL))
            }
        }
        for operation in previousOperations where removedOperationIDs.contains(operation.id) {
            for key in ["output", "previewOutput"] {
                if let path = operation.parameters[key] {
                    invalidatedPaths.insert(
                        PhotonStackProject.assetIdentityPath(for: URL(fileURLWithPath: path))
                    )
                }
            }
        }
        for artifact in previousArtifacts where removedArtifactIDs.contains(artifact.id) {
            invalidatedPaths.formUnion(artifact.outputURLs.map {
                PhotonStackProject.assetIdentityPath(for: $0)
            })
            invalidatedPaths.formUnion(artifact.frames.compactMap(\.outputURL).map {
                PhotonStackProject.assetIdentityPath(for: $0)
            })
        }
        let removedWasPreviewed = previewURL.map {
            invalidatedPaths.contains(PhotonStackProject.assetIdentityPath(for: $0))
        } == true
            || activeLayerID.map(removedLayerIDs.contains) == true
            || previewOperationID.map(removedOperationIDs.contains) == true

        batchQueue.removeAll { item in
            invalidatedPaths.contains(PhotonStackProject.assetIdentityPath(for: item.inputURL)) ||
                item.outputURL.map {
                    invalidatedPaths.contains(PhotonStackProject.assetIdentityPath(for: $0))
                } == true
        }
        syncProjectBatchQueue()
        batchRegistrationSelection.remove(asset.id)
        batchRegistrationCandidateIDs.remove(asset.id)
        if let currentActiveLayerID = activeLayerID, removedLayerIDs.contains(currentActiveLayerID) {
            activeLayerID = nil
        }

        if removedWasSelected {
            selectedAssetID = project.assets.first?.id
        }
        if previewOperationID.map(removedOperationIDs.contains) == true {
            previewOperationID = nil
        }
        if removedWasPreviewed || (canvasMode == .sourcePreview && currentInputURL == nil) {
            previewURL = selectedAsset?.originalURL
            previewOperationID = nil
            editGraphNeedsReplay = false
            canvasMode = .sourcePreview
        }
        if project.assets.isEmpty {
            selectedAssetID = nil
            if canvasMode == .sourcePreview {
                previewURL = nil
                previewOperationID = nil
                editGraphNeedsReplay = false
            }
        }

        if let preservedRawOptions,
           let remainingRawAsset = selectedAsset.flatMap({ $0.kind == .raw ? $0 : nil })
                ?? project.assets.first(where: { $0.kind == .raw }) {
            parameters.rawProcessingOptions = preservedRawOptions
            upsertRawDecodeOperation(for: remainingRawAsset, options: preservedRawOptions, outputURL: nil)
        } else {
            restorePersistedRawOptions(for: selectedAsset)
        }

        artifactTrailReport = nil
        cloudRegionReport = nil
        detectedStarCount = nil
        replaceMarkedPreview(nil)
        histogram = nil
        curveReferenceHistogram = nil
        commandOutput = ""
        clearPreviewHistory()
        clearEditGraphHistory()
        reconcileLayerCompositeAfterProjectMutation(layerStackChanged: previousLayers != project.layers)
        syncBatchRegistrationSelection(selectNewCandidates: false)
        triggerAutosave()
        refreshMetadataIfNeeded(for: selectedAsset)
    }

    public var assetsPendingRemoval: [PhotonStackAsset] {
        pendingAssetRemovalIDs.compactMap { pendingID in
            project.assets.first { $0.id == pendingID }
        }
    }

    public func requestAssetRemoval(_ asset: PhotonStackAsset) {
        requestAssetRemoval([asset])
    }

    public func requestAssetRemoval(_ assets: [PhotonStackAsset]) {
        guard canModifyProcessingConfiguration else {
            return
        }
        let existingIDs = Set(project.assets.map(\.id))
        pendingAssetRemovalIDs = assets.reduce(into: []) { ids, asset in
            guard existingIDs.contains(asset.id), ids.contains(asset.id) == false else {
                return
            }
            ids.append(asset.id)
        }
    }

    public func cancelAssetRemoval() {
        pendingAssetRemovalIDs.removeAll()
    }

    public func confirmAssetRemoval(_ requestedAssets: [PhotonStackAsset]? = nil) {
        guard canModifyProcessingConfiguration else {
            return
        }
        let assets = requestedAssets ?? assetsPendingRemoval
        pendingAssetRemovalIDs.removeAll()
        for asset in assets {
            if project.assets.contains(where: { $0.id == asset.id }) {
                removeAsset(asset)
            }
        }
    }

    public var artifactPendingRemoval: ProcessingArtifact? {
        pendingArtifactRemovalID.flatMap { pendingID in
            project.artifacts.first { $0.id == pendingID }
        }
    }

    public func requestArtifactRemoval(_ artifact: ProcessingArtifact) {
        guard canModifyWorkspaceProducts,
              project.artifacts.contains(where: { $0.id == artifact.id })
        else {
            return
        }
        pendingArtifactRemovalID = artifact.id
    }

    public func cancelArtifactRemoval() {
        pendingArtifactRemovalID = nil
    }

    public func confirmArtifactRemoval(_ requestedArtifact: ProcessingArtifact? = nil) {
        guard canModifyWorkspaceProducts,
              let artifact = requestedArtifact ?? artifactPendingRemoval,
              project.artifacts.contains(where: { $0.id == artifact.id })
        else {
            return
        }
        pendingArtifactRemovalID = nil
        removeArtifact(artifact)
    }

    public func removeArtifact(_ artifact: ProcessingArtifact) {
        guard canModifyWorkspaceProducts,
              project.artifacts.contains(where: { $0.id == artifact.id })
        else {
            return
        }
        pendingArtifactRemovalID = nil
        let previousArtifacts = project.artifacts
        let previousLayers = project.layers
        project.removeArtifact(artifact.id)

        let remainingArtifactIDs = Set(project.artifacts.map(\.id))
        let removedArtifactIDs = Set(previousArtifacts.map(\.id)).subtracting(remainingArtifactIDs)
        guard removedArtifactIDs.isEmpty == false else {
            return
        }
        let remainingLayerIDs = Set(project.layers.map(\.id))
        let removedLayerIDs = Set(previousLayers.map(\.id)).subtracting(remainingLayerIDs)
        var invalidatedPaths = Set(previousArtifacts
            .filter { removedArtifactIDs.contains($0.id) }
            .flatMap(\.outputURLs)
            .map { PhotonStackProject.assetIdentityPath(for: $0) })
        invalidatedPaths.formUnion(previousArtifacts
            .filter { removedArtifactIDs.contains($0.id) }
            .flatMap { $0.frames.compactMap(\.outputURL) }
            .map { PhotonStackProject.assetIdentityPath(for: $0) })

        let removedWasActive = activeLayerID.map(removedLayerIDs.contains) == true
        batchQueue.removeAll { item in
            invalidatedPaths.contains(PhotonStackProject.assetIdentityPath(for: item.inputURL)) ||
                item.outputURL.map {
                    invalidatedPaths.contains(PhotonStackProject.assetIdentityPath(for: $0))
                } == true
        }
        syncProjectBatchQueue()
        if removedWasActive {
            activeLayerID = nil
        }
        let removedWasPreviewed = previewURL.map {
            invalidatedPaths.contains(PhotonStackProject.assetIdentityPath(for: $0))
        } == true ||
            removedWasActive
        if removedWasPreviewed || (canvasMode == .sourcePreview && currentInputURL == nil) {
            previewURL = selectedAsset?.originalURL
            previewOperationID = nil
            editGraphNeedsReplay = false
            canvasMode = .sourcePreview
        }

        artifactTrailReport = nil
        cloudRegionReport = nil
        detectedStarCount = nil
        replaceMarkedPreview(nil)
        histogram = nil
        curveReferenceHistogram = nil
        commandOutput = ""
        clearPreviewHistory()
        clearEditGraphHistory()
        reconcileLayerCompositeAfterProjectMutation(layerStackChanged: previousLayers != project.layers)
        triggerAutosave()
    }

    @discardableResult
    public func relinkAsset(_ asset: PhotonStackAsset, to replacementURL: URL) -> Bool {
        guard canModifyProcessingConfiguration else {
            return false
        }

        var isDirectory: ObjCBool = false
        let fileExists = FileManager.default.fileExists(
            atPath: replacementURL.path,
            isDirectory: &isDirectory
        )
        guard fileExists,
              isDirectory.boolValue == false,
              AssetKind.detect(from: replacementURL) != .unknown
        else {
            errorMessage = localized(.relinkAssetInvalid)
            return false
        }
        guard project.assets.contains(where: {
            $0.id != asset.id &&
                PhotonStackProject.assetIdentityPath(for: $0.originalURL) ==
                    PhotonStackProject.assetIdentityPath(for: replacementURL)
        }) == false else {
            errorMessage = localized(.relinkAssetDuplicate)
            return false
        }

        let oldURL = asset.originalURL
        let replayPlan = fullResolutionExportReplayPlan()
        let previewUsesAsset = replayPlan.map {
            Self.urlsReferenceSameFile($0.input, oldURL)
        } == true || replayPlan?.operations.contains(where: {
            $0.parameters["assetID"] == asset.id.uuidString ||
                $0.parameters.values.contains { Self.storedPath($0, references: oldURL) }
        }) == true

        _ = replacementURL.startAccessingSecurityScopedResource()
        cancelMetadataRefresh()
        cancelRawPreviewUpdate()
        guard project.relinkAsset(asset.id, to: replacementURL) else {
            errorMessage = localized(.relinkAssetDuplicate)
            return false
        }

        for index in batchQueue.indices {
            if Self.urlsReferenceSameFile(batchQueue[index].inputURL, oldURL) {
                batchQueue[index].inputURL = replacementURL
            }
            if batchQueue[index].outputURL.map({
                Self.urlsReferenceSameFile($0, oldURL)
            }) == true {
                batchQueue[index].outputURL = replacementURL
            }
        }
        syncProjectBatchQueue()

        if previewURL.map({ Self.urlsReferenceSameFile($0, oldURL) }) == true ||
            (previewURL == nil && selectedAsset?.id == asset.id) {
            previewURL = replacementURL
            previewOperationID = nil
            editGraphNeedsReplay = false
        } else if previewUsesAsset {
            editGraphNeedsReplay = true
        }

        artifactTrailReport = nil
        cloudRegionReport = nil
        detectedStarCount = nil
        replaceMarkedPreview(nil)
        histogram = nil
        curveReferenceHistogram = nil
        clearPreviewHistory()
        errorMessage = nil
        commandOutput = "\(localized(.relinkAssetSuccess)): \(replacementURL.path)"
        triggerAutosave()
        refreshMetadataIfNeeded(for: project.assets.first { $0.id == asset.id })
        return true
    }

    public func setRole(_ role: CalibrationFrameRole, for asset: PhotonStackAsset) {
        guard canModifyProcessingConfiguration,
              project.assets.contains(where: { $0.id == asset.id })
        else {
            return
        }
        project.updateRole(for: asset.id, role: role)
        synchronizeSourceArtifactRole(for: asset.id, role: role)
        syncBatchRegistrationSelection(selectNewCandidates: true)
        triggerAutosave()
    }

    public func moveMosaicPanel(_ assetID: PhotonStackAsset.ID, direction: EditOperationMoveDirection) {
        guard canModifyProcessingConfiguration else {
            return
        }
        var orderedIDs = mosaicAssets.map(\.id)
        guard let index = orderedIDs.firstIndex(of: assetID) else {
            return
        }

        let targetIndex: Int
        switch direction {
        case .up:
            targetIndex = index - 1
        case .down:
            targetIndex = index + 1
        }

        guard orderedIDs.indices.contains(targetIndex) else {
            return
        }

        orderedIDs.swapAt(index, targetIndex)
        project.mosaicPanelOrder = orderedIDs
        triggerAutosave()
    }

    public func moveMosaicPanel(_ assetID: PhotonStackAsset.ID, before targetID: PhotonStackAsset.ID) {
        guard canModifyProcessingConfiguration else {
            return
        }
        var orderedIDs = mosaicAssets.map(\.id)
        guard let sourceIndex = orderedIDs.firstIndex(of: assetID),
              let targetIndex = orderedIDs.firstIndex(of: targetID),
              sourceIndex != targetIndex
        else {
            return
        }

        let moved = orderedIDs.remove(at: sourceIndex)
        let adjustedTargetIndex = sourceIndex < targetIndex ? targetIndex - 1 : targetIndex
        orderedIDs.insert(moved, at: adjustedTargetIndex)
        project.mosaicPanelOrder = orderedIDs
        triggerAutosave()
    }

    public func saveProject(to directory: URL) async {
        await runLocalJob(title: "Save Project", kind: .export) {
            await self.waitForPendingAutosave()
            self.syncProjectBatchQueue()
            self.syncProjectWorkspaceState()
            let generation = self.projectChangeGeneration
            let projectSnapshot = self.project
            try await self.projectRepository.save(projectSnapshot, to: directory)
            try Task.checkCancellation()
            self.currentProjectDirectory = directory
            self.markRecentProject(directory: directory)
            if self.projectChangeGeneration == generation {
                self.hasUnsavedChanges = false
            }
            return "\(self.localized(.savedProject)): \(directory.path)"
        }
    }

    public func loadProject(from directory: URL) async {
        await runLocalJob(title: "Open Project", kind: .inspect) {
            self.cancelMetadataRefresh()
            self.clearPendingAssetImports()
            await self.waitForPendingAutosave()
            let loaded = try await self.projectRepository.load(from: directory)
            try Task.checkCancellation()
            self.clearProjectTransientState()
            self.project = loaded
            if self.usesManagedPreviewDirectory {
                self.previewDirectory = Self.managedPreviewDirectory(for: loaded.id)
            }
            self.batchQueue = Self.restoredBatchQueue(from: loaded.batchQueue)
            self.resetBatchRegistrationSelection(selectAll: true)
            self.restoreWorkspaceState(from: loaded)
            let repairedOperationLayerVisibility = self.synchronizeOperationLayerVisibilityWithEditGraph()
            if repairedOperationLayerVisibility {
                self.editGraphNeedsReplay = true
                self.project.updatedAt = Date()
            }
            self.restorePersistedRawOptions(for: self.selectedAsset)
            self.currentProjectDirectory = directory
            self.clearPreviewHistory()
            self.clearEditGraphHistory()
            self.markRecentProject(directory: directory)
            self.projectChangeGeneration += 1
            self.hasUnsavedChanges = repairedOperationLayerVisibility
            if repairedOperationLayerVisibility {
                self.scheduleAutosaveIfNeeded()
            }
            return "\(self.localized(.openedProject)): \(directory.path)"
        }
    }

    private func restoreWorkspaceState(from loaded: PhotonStackProject) {
        if let workspaceState = loaded.workspaceState {
            selectedAssetID = workspaceState.selectedAssetID.flatMap { selectedID in
                loaded.assets.contains(where: { $0.id == selectedID }) ? selectedID : nil
            } ?? loaded.assets.first?.id
            if let savedActiveLayerID = workspaceState.activeLayerID {
                activeLayerID = loaded.layers.first(where: {
                    $0.id == savedActiveLayerID && $0.inputURL.map(workspaceFileExists) == true
                })?.id
            } else {
                activeLayerID = nil
            }
            previewOperationID = workspaceState.previewOperationID.flatMap { operationID in
                loaded.editGraph.operations.contains(where: { $0.id == operationID }) ? operationID : nil
            }
            let savedPreview = workspaceState.previewURL.flatMap { url in
                workspaceFileExists(url) ? url : nil
            }
            previewURL = savedPreview
                ?? activeLayer?.inputURL.flatMap { workspaceFileExists($0) ? $0 : nil }
                ?? (selectedAsset?.originalURL).flatMap { workspaceFileExists($0) ? $0 : nil }
                ?? loaded.layers.last(where: {
                    $0.inputURL.map(workspaceFileExists) == true
                })?.inputURL
            if previewOperationID == nil, let recordedPreview = workspaceState.previewURL {
                previewOperationID = Self.uniqueEditOperationID(
                    for: recordedPreview,
                    in: loaded.editGraph.operations
                )
            }
            editGraphNeedsReplay = workspaceState.editGraphNeedsReplay == true ||
                (previewOperationID != nil && savedPreview == nil)
            canvasMode = workspaceState.canvasMode
                ?? inferredLegacyCanvasMode(savedPreview: savedPreview)
        } else {
            selectedAssetID = loaded.assets.first?.id
            activeLayerID = nil
            previewURL = (loaded.assets.first?.originalURL).flatMap { workspaceFileExists($0) ? $0 : nil }
            previewOperationID = previewURL.flatMap { candidateURL in
                Self.uniqueEditOperationID(for: candidateURL, in: loaded.editGraph.operations)
            }
            editGraphNeedsReplay = false
            canvasMode = loaded.layers.contains(where: { $0.kind != .mask }) ? .layerComposite : .sourcePreview
        }
    }

    public func inspectSelectedAsset() async {
        guard let selectedAsset else {
            return
        }
        guard rejectMissingAsset(in: [selectedAsset]) == false else {
            return
        }

        await runProcessingJob(title: "Inspect \(selectedAsset.displayName)", kind: .inspect) {
            let result = try await self.processingService.inspect(selectedAsset)
            try Task.checkCancellation()
            let metadata = try self.parseInspectMetadata(result)
            self.project.updateMetadata(for: selectedAsset.id, metadata: metadata)
            self.triggerAutosave()
            return result
        }
    }

    private func clearProjectTransientState() {
        pendingAssetRemovalIDs.removeAll()
        pendingArtifactRemovalID = nil
        pendingPreviewCacheCleanupPlan = nil
        pendingProcessingInterruption = nil
        pendingProjectTransition = nil
        histogram = nil
        curveReferenceHistogram = nil
        mosaicQualityReport = nil
        mosaicQualityReportSourceURL = nil
        registrationReport = nil
        artifactTrailReport = nil
        replaceMarkedPreview(nil)
        cloudRegionReport = nil
        detectedStarCount = nil
        errorMessage = nil
    }

    private func refreshMetadataIfNeeded(for asset: PhotonStackAsset?) {
        guard let asset, asset.metadata == nil else {
            return
        }

        cancelMetadataRefresh()
        let generation = metadataRefreshGeneration
        let projectID = project.id
        let assetID = asset.id
        let assetIdentity = Self.workspaceFileIdentityPath(for: asset.originalURL)
        metadataRefreshTask = Task { @MainActor in
            defer {
                if generation == self.metadataRefreshGeneration {
                    self.metadataRefreshTask = nil
                }
            }
            do {
                let result = try await processingService.inspect(asset)
                try Task.checkCancellation()
                guard generation == metadataRefreshGeneration,
                      project.id == projectID,
                      let currentAsset = project.assets.first(where: { $0.id == assetID }),
                      Self.workspaceFileIdentityPath(for: currentAsset.originalURL) == assetIdentity,
                      let metadata = try? parseInspectMetadata(result)
                else {
                    return
                }
                project.updateMetadata(for: assetID, metadata: metadata)
                triggerAutosave()
            } catch {
                // Metadata is nice-to-have; keep the import/select flow quiet if ImageIO cannot inspect a file.
            }
        }
    }

    private func cancelMetadataRefresh() {
        metadataRefreshGeneration += 1
        metadataRefreshTask?.cancel()
        metadataRefreshTask = nil
    }

    public func buildPreview() async {
        guard let selectedAsset else {
            return
        }
        guard rejectMissingAsset(in: [selectedAsset]) == false else {
            return
        }

        let operationParameters = ["width": String(parameters.previewWidth)]
            .merging(rawDecodeParameters(), uniquingKeysWith: { _, new in new })
        let output = makeCachedPreviewOutput(prefix: "preview", input: selectedAsset.originalURL, parameters: operationParameters)
        await runProcessingJob(title: "Build Preview", kind: .preview, outputURL: output) {
            if let cached = self.cachedResultIfAvailable(output: output, command: "preview") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.preview, parameters: operationParameters, outputURL: output)
                return cached
            }
            let result = try await self.processingService.makePreview(
                input: selectedAsset.originalURL,
                output: output,
                width: self.parameters.previewWidth,
                rawOptions: self.parameters.rawProcessingOptions
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.preview, parameters: operationParameters, outputURL: output)
            return result
        }
    }

    private func scheduleRawPreviewUpdate() {
        guard let selectedAsset, selectedAsset.kind == .raw else {
            return
        }
        guard firstMissingAsset(in: [selectedAsset]) == nil else {
            return
        }
        let asset = selectedAsset
        let rawOptions = parameters.rawProcessingOptions
        upsertRawDecodeOperation(for: asset, options: rawOptions, outputURL: nil)
        guard activeTask == nil, batchTask == nil, isBatchRunning == false else {
            return
        }

        rawPreviewGeneration += 1
        let generation = rawPreviewGeneration
        let previewOptions = visualRawPreviewOptions(from: rawOptions)
        let width = min(max(parameters.previewWidth, 320), 1200)
        let operationParameters = rawDecodeParameters(from: rawOptions)
            .merging(["width": String(width)], uniquingKeysWith: { _, new in new })
        let output = makeCachedPreviewOutput(prefix: "raw-preview", input: asset.originalURL, parameters: operationParameters)

        rawPreviewTask?.cancel()
        isRawPreviewUpdating = true

        rawPreviewTask = Task { @MainActor in
            defer {
                if generation == self.rawPreviewGeneration {
                    self.isRawPreviewUpdating = false
                    self.rawPreviewTask = nil
                }
            }

            do {
                try await Task.sleep(for: .milliseconds(300))
                try Task.checkCancellation()

                try FileManager.default.createDirectory(at: self.previewDirectory, withIntermediateDirectories: true)
                let baseOptions = RawProcessingOptions(
                    whiteBalanceMode: .camera,
                    manualWhiteBalanceTemperature: rawOptions.manualWhiteBalanceTemperature,
                    manualWhiteBalanceTint: rawOptions.manualWhiteBalanceTint,
                    exposureBias: 0,
                    blackLevelMode: .camera,
                    manualBlackLevel: rawOptions.manualBlackLevel,
                    demosaicQuality: rawOptions.demosaicQuality,
                    linearOutput: false
                )
                let baseOutput = self.makeCachedPreviewOutput(
                    prefix: "raw-decode-base",
                    input: asset.originalURL,
                    parameters: [
                        "width": String(width),
                        "rawDemosaic": rawOptions.demosaicQuality.rawValue,
                    ],
                    includeEditGraph: false
                )
                var decodeStatus = self.cachedRawDecodeStatus(for: baseOutput)
                if self.cachedResultIfAvailable(output: output, command: "raw preview") == nil {
                    if self.cachedResultIfAvailable(output: baseOutput, command: "raw decode base") == nil {
                        let baseResult = try await self.processingService.makePreview(
                            input: asset.originalURL,
                            output: baseOutput,
                            width: width,
                            rawOptions: baseOptions
                        )
                        decodeStatus = try self.parseRawDecodeStatus(baseResult)
                        self.recordValidatedCacheOutput(baseOutput, rawDecodeStatus: decodeStatus)
                    }
                    try Task.checkCancellation()
                    guard self.foregroundTaskRequested == false else {
                        return
                    }
                    _ = try await self.runValidatedCachedStep(output: output, command: "raw preview") {
                        try await self.processingService.adjustRawPreview(
                            input: baseOutput,
                            output: output,
                            rawOptions: previewOptions
                        )
                    }
                }
                try Task.checkCancellation()
                guard generation == self.rawPreviewGeneration,
                      self.project.assets.contains(where: {
                          $0.id == asset.id &&
                              Self.urlsReferenceSameFile($0.originalURL, asset.originalURL)
                      }),
                      self.selectedAsset?.id == asset.id
                else {
                    return
                }

                try await self.commitPreviewAfterHistogramRefresh(output, trackHistory: false)
                self.rawDecodeStatus = decodeStatus
                self.upsertRawDecodeOperation(for: asset, options: rawOptions, outputURL: output)
                self.commandOutput = self.localized(.rawAutoPreview)
            } catch is CancellationError {
                // A newer RAW parameter edit superseded this preview.
            } catch {
                guard generation == self.rawPreviewGeneration else {
                    return
                }
                self.errorMessage = error.localizedDescription
            }
        }
    }

    private func cancelRawPreviewUpdate() {
        rawPreviewGeneration += 1
        rawPreviewTask?.cancel()
        rawPreviewTask = nil
        isRawPreviewUpdating = false
        rawDecodeStatus = nil
    }

    private func visualRawPreviewOptions(from options: RawProcessingOptions) -> RawProcessingOptions {
        RawProcessingOptions(
            whiteBalanceMode: options.whiteBalanceMode,
            manualWhiteBalanceTemperature: options.manualWhiteBalanceTemperature,
            manualWhiteBalanceTint: options.manualWhiteBalanceTint,
            exposureBias: options.exposureBias,
            blackLevelMode: options.blackLevelMode,
            manualBlackLevel: options.manualBlackLevel,
            demosaicQuality: options.demosaicQuality,
            linearOutput: false
        )
    }

    public func startAstroDevelop(settings: AstroDevelopSettings) {
        launch { await self.astroDevelopPreview(settings: settings) }
    }

    @Published public private(set) var deepSkySchema: DeepSkySchema?

    public var deepSkySettings: DeepSkySettings {
        var settings = project.deepSkySettings ?? DeepSkySettings()
        for (key, assets) in [("calibration.darks", darkAssets), ("calibration.biases", biasAssets), ("calibration.flats", flatAssets)] {
            if settings.values[key] == nil, !assets.isEmpty {
                settings.values[key] = assets.map { asset in
                    "\"" + PhotonStackProject.assetIdentityPath(for: asset.originalURL)
                        .replacingOccurrences(of: "\\", with: "\\\\").replacingOccurrences(of: "\"", with: "\\\"") + "\""
                }.joined(separator: " ")
            }
        }
        return settings
    }

    public func loadDeepSkySchema() async {
        guard deepSkySchema == nil else { return }
        do { deepSkySchema = try await processingService.deepSkySchema() }
        catch { errorMessage = error.localizedDescription }
    }

    public func setDeepSkyValue(_ value: String, for key: String) {
        guard !isProcessing else { return }
        var settings = deepSkySettings
        settings.values[key] = value
        project.deepSkySettings = settings
        project.updatedAt = Date()
        triggerAutosave()
    }

    public func setDeepSkyOverride(_ value: String, path: String) {
        updateDeepSkyOverrides([path: value])
    }

    public func updateDeepSkyOverrides(_ changes: [String: String]) {
        guard !isProcessing else { return }
        var settings = deepSkySettings
        let paths = Set(lightAssets.map { PhotonStackProject.assetIdentityPath(for: $0.originalURL) })
        let unusable = Set((settings.lastReport?.frames ?? []).filter { !$0.usable }.map(\.input))
        for (path, value) in changes where paths.contains(path) && ["auto", "keep", "reject"].contains(value) {
            guard value != "keep" || !unusable.contains(path) else { continue }
            if value == "auto" { settings.overrides.removeValue(forKey: path) }
            else { settings.overrides[path] = value }
        }
        project.deepSkySettings = settings
        project.updatedAt = Date()
        triggerAutosave()
    }

    var deepSkyReviewDecodeKey: String {
        "\(deepSkySettings.values["decode.debayer"] ?? "on")|\(deepSkySettings.values["decode.demosaic"] ?? "malvar")"
    }

    /// Independent review preview. Never replaces the editor's stack or adds history.
    func makeDeepSkyReviewPreview(input: URL, output: URL) async throws {
        let demosaic = deepSkySettings.values["decode.demosaic"] ?? "malvar"
        let debayer = deepSkySettings.values["decode.debayer"] ?? "on"
        _ = try await processingService.runCLI(arguments: ["review-preview", "--input", input.path,
            "--output", output.path, "--fits-debayer", debayer, "--fits-demosaic", demosaic])
        try Task.checkCancellation()
    }

    public func resetDeepSkySettings() {
        guard !isProcessing else { return }
        var settings = deepSkySettings
        settings.values = [:]
        project.deepSkySettings = settings
        project.updatedAt = Date()
        triggerAutosave()
    }

    public func deepSkyRecipeText() throws -> String {
        guard let schema = deepSkySchema else { throw DeepSkyRecipeError.unsupportedVersion }
        return try deepSkySettings.recipe(schema: schema, inputs: lightAssets.map(\.originalURL))
    }

    public func exportDeepSkyRecipe(to url: URL) {
        guard !isProcessing else { return }
        do { try deepSkyRecipeText().write(to: url, atomically: true, encoding: .utf8) }
        catch { errorMessage = error.localizedDescription }
    }

    public func importDeepSkyRecipe(from url: URL) async {
        guard !isProcessing else { return }
        do {
            let parsed = try await processingService.inspectDeepSkyRecipe(url)
            guard parsed.version == 1 else { throw DeepSkyRecipeError.unsupportedVersion }
            var settings = DeepSkySettings()
            settings.values = parsed.values
            let overrides = (parsed.values["selection.overrides"] ?? "").split(separator: ",").map(String.init)
            if !overrides.isEmpty {
                guard overrides.count == parsed.inputs.count else { throw DeepSkyRecipeError.invalidReport }
                for (path, override) in zip(parsed.inputs, overrides) {
                    settings.overrides[PhotonStackProject.assetIdentityPath(for: URL(fileURLWithPath: path))] = override
                }
            }
            // An imported recipe describes its own exact input sequence. Avoid
            // silently mixing those inputs with a different open project's lights.
            let current = lightAssets.map { PhotonStackProject.assetIdentityPath(for: $0.originalURL) }
            let imported = parsed.inputs.map { PhotonStackProject.assetIdentityPath(for: URL(fileURLWithPath: $0)) }
            if !imported.isEmpty, current != imported {
                guard current.isEmpty else {
                    throw EditGraphReplayError(message: "配方中的亮场顺序与当前项目不同。请在空项目中导入，以保留准确的参考帧和权重对应关系。")
                }
                project.addAssets(from: parsed.inputs.map { URL(fileURLWithPath: $0) }, role: .light)
            }
            if current == imported { settings.lastReport = deepSkySettings.lastReport }
            project.deepSkySettings = settings
            project.updatedAt = Date()
            triggerAutosave()
        } catch { errorMessage = error.localizedDescription }
    }

    public var latestDeepSkyResultURL: URL? {
        guard let operation = project.editGraph.operations.last(where: {
            $0.isEnabled && $0.parameters["mode"] == "deepSkyRecipe"
        }), let path = operation.parameters["output"], !path.isEmpty,
              FileManager.default.fileExists(atPath: path) else { return nil }
        return URL(fileURLWithPath: path)
    }

    public func showLatestDeepSkyResult() {
        guard canModifyWorkspaceProducts, let url = latestDeepSkyResultURL else { return }
        activeLayerID = nil
        setPreview(url, canvasMode: .sourcePreview)
        triggerAutosave()
    }

    public var canRefinishDeepSkyMaster: Bool {
        guard canModifyWorkspaceProducts, let previous = deepSkySettings.lastReport,
              previous.success, !previous.analysisOnly, !previous.master.isEmpty,
              FileManager.default.fileExists(atPath: previous.master) else { return false }
        return canReuseDeepSkyNoise(previous) && DeepSkyReview.sameStackRecipe(try? deepSkyRecipeText(), previous.recipe)
    }

    private func canReuseDeepSkyNoise(_ previous: DeepSkyReport) -> Bool {
        deepSkySettings.values["denoise.noise-model"] != "independent-luminance" ||
            (previous.noiseReferencePaths.count == 3 && previous.noiseReferencePaths.allSatisfy { FileManager.default.fileExists(atPath: $0) })
    }

    public func startRefinishDeepSkyMaster() {
        launch { await self.refinishDeepSkyMaster() }
    }

    public func refinishDeepSkyMaster() async {
        // launch() marks a foreground task pending, so test recipe validity here
        // independently of the UI's idle-only availability check.
        guard let previous = deepSkySettings.lastReport, previous.success, !previous.analysisOnly,
              canReuseDeepSkyNoise(previous),
              DeepSkyReview.sameStackRecipe(try? deepSkyRecipeText(), previous.recipe),
              FileManager.default.fileExists(atPath: previous.master) else { return }
        await runJob(title: "Deep Sky Background & Denoise", kind: .stack) {
            let root = self.currentProjectDirectory?.appendingPathComponent("processing", isDirectory: true) ?? self.previewDirectory
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
            let directory = root.appendingPathComponent("deep-sky-finish-\(UUID().uuidString)")
            let recipeURL = root.appendingPathComponent("recipe-\(UUID().uuidString).txt")
            let recipe = try self.deepSkyRecipeText()
            try recipe.write(to: recipeURL, atomically: true, encoding: .utf8)
            defer { try? FileManager.default.removeItem(at: recipeURL) }
            let report = try await self.processingService.finishDeepSky(recipe: recipeURL,
                master: URL(fileURLWithPath: previous.master), outputDirectory: directory, previous: previous)
            try Task.checkCancellation()
            var settings = self.deepSkySettings; settings.lastReport = report
            self.project.deepSkySettings = settings
            self.project.appendArtifact(ProcessingArtifact(kind: .stackMaster, name: "Deep Sky Reprocessed Background",
                operationKind: .stack, outputDirectory: directory,
                outputURLs: ([report.backgroundMaster] + report.noiseReferencePaths).map { URL(fileURLWithPath: $0) }, parameters: ["recipe": recipe],
                metrics: ["frames": String(report.selectedCount)]))
            let output = URL(fileURLWithPath: report.developed)
            try await self.commitPreviewAfterHistogramRefresh(output)
            // A replay may rebuild the full recipe if the cached result is gone.
            // This keeps saved project export independent of a hidden temp master.
            self.recordOperation(.stack, parameters: ["mode": "deepSkyRecipe", "recipe": report.recipe,
                "report": directory.appendingPathComponent("app-report.json").path,
                "sourceMaster": previous.master, "masterReused": "true"], outputURL: output, promoteToLayer: true)
            self.project.updatedAt = Date(); self.triggerAutosave()
            return "Updated background, denoising and development from \(previous.master)"
        }
    }

    public func startDeepSkyWorkflow(analyzeOnly: Bool) {
        launch { await self.runDeepSkyWorkflow(analyzeOnly: analyzeOnly) }
    }

    public func runDeepSkyWorkflow(analyzeOnly: Bool) async {
        guard !rejectMissingAsset(in: lightAssets) else { return }
        await runJob(title: analyzeOnly ? "Deep Sky Frame Assessment" : "Deep Sky Recipe", kind: .stack) {
            if self.deepSkySchema == nil { self.deepSkySchema = try await self.processingService.deepSkySchema() }
            let recipe = try self.deepSkyRecipeText()
            let root = self.currentProjectDirectory?.appendingPathComponent("processing", isDirectory: true) ?? self.previewDirectory
            try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
            let directory = root.appendingPathComponent("deep-sky-\(UUID().uuidString)", isDirectory: true)
            let recipeURL = root.appendingPathComponent("recipe-\(UUID().uuidString).txt")
            try recipe.write(to: recipeURL, atomically: true, encoding: .utf8)
            defer { try? FileManager.default.removeItem(at: recipeURL) }
            let report: DeepSkyReport
            do {
                report = try await self.processingService.deepSky(recipe: recipeURL, outputDirectory: directory, analyzeOnly: analyzeOnly)
            } catch {
                // Even selection safeguards return a reviewable report. Never
                // interpret process failure as a successful stack.
                if let data = try? Data(contentsOf: directory.appendingPathComponent("report.json")),
                   let partial = try? JSONDecoder().decode(DeepSkyReport.self, from: data),
                   (try? partial.validate()) != nil {
                    var settings = self.deepSkySettings
                    settings.lastReport = partial
                    self.project.deepSkySettings = settings
                    self.triggerAutosave()
                }
                throw error
            }
            try Task.checkCancellation()
            var settings = self.deepSkySettings
            settings.lastReport = report
            self.project.deepSkySettings = settings
            if !analyzeOnly {
                let output = URL(fileURLWithPath: report.developed)
                let masters = ([report.master, report.backgroundMaster, report.rejectionLow, report.rejectionHigh] + report.noiseReferencePaths)
                    .filter { !$0.isEmpty }.map { URL(fileURLWithPath: $0) }
                self.project.appendArtifact(ProcessingArtifact(
                    kind: .stackMaster, name: "Deep Sky Linear Masters", operationKind: .stack,
                    outputDirectory: directory, outputURLs: masters,
                    parameters: ["recipe": report.recipe], metrics: ["frames": String(report.selectedCount)]))
                try await self.commitPreviewAfterHistogramRefresh(output)
                var recorded = ["mode": "deepSkyRecipe", "recipe": report.recipe,
                    "report": directory.appendingPathComponent("app-report.json").path,
                    "calibrationLightIDs": self.lightAssets.map(\.id.uuidString).joined(separator: ","),
                    "calibrationDarkIDs": self.darkAssets.map(\.id.uuidString).joined(separator: ","),
                    "calibrationBiasIDs": self.biasAssets.map(\.id.uuidString).joined(separator: ","),
                    "calibrationFlatIDs": self.flatAssets.map(\.id.uuidString).joined(separator: ",")]
                for (index, frame) in report.frames.enumerated() { recorded["inputPath.\(index)"] = frame.input }
                self.recordOperation(.stack, parameters: recorded, outputURL: output, promoteToLayer: true)
            }
            self.project.updatedAt = Date()
            self.triggerAutosave()
            return "Deep sky: \(report.selectedCount)/\(report.inputCount) frames. Report: \(directory.appendingPathComponent("report.json").path)"
        }
    }

    public func astroDevelopPreview(settings: AstroDevelopSettings) async {
        guard let input = validatedCurrentInputURL() else { return }
        let operationParameters = settings.operationParameters
        let output = makeCachedPreviewOutput(prefix: "astro-develop-v1", input: input, parameters: operationParameters, extension: "tiff")
        await runProcessingJob(title: "Deep Sky Development", kind: .stretch, outputURL: output) {
            let result: ProcessingCommandResult
            if let cached = self.cachedResultIfAvailable(output: output, command: "develop") {
                result = cached
            } else {
                result = try await self.processingService.develop(input: input, output: output, settings: settings)
            }
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.stretch, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func autoStretchPreview() async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let operationParameters = ["targetBackground": String(parameters.stretchTargetBackground)]
        let output = makeCachedPreviewOutput(prefix: "stretch", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Auto Stretch", kind: .stretch, outputURL: output) {
            if let cached = self.cachedResultIfAvailable(output: output, command: "stretch") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.stretch, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result = try await self.processingService.autoStretch(
                input: input,
                output: output,
                targetBackground: self.parameters.stretchTargetBackground
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.stretch, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func localContrastPreview() async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let operationParameters = ["amount": String(parameters.localContrastAmount), "radius": String(parameters.localContrastRadius)]
        let output = makeCachedPreviewOutput(prefix: "contrast", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Local Contrast", kind: .localContrast, outputURL: output) {
            if let cached = self.cachedResultIfAvailable(output: output, command: "local-contrast") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.localContrast, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result = try await self.processingService.localContrast(
                input: input,
                output: output,
                amount: self.parameters.localContrastAmount,
                radius: self.parameters.localContrastRadius
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.localContrast, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func denoisePreview() async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let operationParameters = [
            "amount": String(parameters.denoiseAmount),
            "chromaAmount": String(parameters.denoiseChromaAmount),
            "radius": String(parameters.denoiseRadius),
        ]
        let output = makeCachedPreviewOutput(prefix: "denoise", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Denoise", kind: .denoise, outputURL: output) {
            if let cached = self.cachedResultIfAvailable(output: output, command: "denoise") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.denoise, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result = try await self.processingService.denoise(
                input: input,
                output: output,
                amount: self.parameters.denoiseAmount,
                chromaAmount: self.parameters.denoiseChromaAmount,
                radius: self.parameters.denoiseRadius
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.denoise, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func sharpenPreview() async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let operationParameters = ["amount": String(parameters.sharpenAmount), "radius": String(parameters.sharpenRadius)]
        let output = makeCachedPreviewOutput(prefix: "sharpen", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Sharpen", kind: .sharpen, outputURL: output) {
            if let cached = self.cachedResultIfAvailable(output: output, command: "sharpen") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.sharpen, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result = try await self.processingService.sharpen(
                input: input,
                output: output,
                amount: self.parameters.sharpenAmount,
                radius: self.parameters.sharpenRadius
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.sharpen, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func reduceStarsPreview() async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let operationParameters = [
            "amount": String(parameters.starReductionAmount),
            "profileAware": String(parameters.profileAwareStarReduction),
            "edgeAware": String(parameters.edgeAwareStarReduction),
        ]
        let output = makeCachedPreviewOutput(prefix: "stars", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Reduce Stars", kind: .starReduce, outputURL: output) {
            if let cached = self.cachedResultIfAvailable(output: output, command: "stars reduce") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.starReduce, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result = try await self.processingService.reduceStars(
                input: input,
                output: output,
                amount: self.parameters.starReductionAmount,
                profileAware: self.parameters.profileAwareStarReduction,
                edgeAware: self.parameters.edgeAwareStarReduction
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.starReduce, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func reduceComaPreview() async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let operationParameters = [
            "amount": String(parameters.comaReductionAmount),
            "radius": String(parameters.comaReductionRadius),
            "eccentricity": String(parameters.comaReductionEccentricity),
            "edgeAware": String(parameters.edgeAwareComaReduction),
        ]
        let output = makeCachedPreviewOutput(prefix: "coma", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Reduce Coma", kind: .comaReduce, outputURL: output) {
            if let cached = self.cachedResultIfAvailable(output: output, command: "stars coma") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.comaReduce, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result = try await self.processingService.reduceComa(
                input: input,
                output: output,
                amount: self.parameters.comaReductionAmount,
                radius: self.parameters.comaReductionRadius,
                eccentricity: self.parameters.comaReductionEccentricity,
                edgeAware: self.parameters.edgeAwareComaReduction
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.comaReduce, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func detectStars(sigmaThreshold: Double, minPeak: Double, maxStars: Int = 50_000) async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let threshold = Self.normalizedFinite(sigmaThreshold, in: 0.1...20, fallback: 3)
        let peak = Self.normalizedFinite(minPeak, in: 0...1, fallback: 0.05)
        let limit = min(max(maxStars, 1), 100_000)
        let operationParameters = [
            "sigmaThreshold": String(threshold),
            "minPeak": String(peak),
            "maxStars": String(limit),
        ]
        await runProcessingJob(title: "Detect Stars", kind: .starDetect, outputURL: nil) {
            let result = try await self.processingService.detectStars(
                input: input,
                sigmaThreshold: threshold,
                minPeak: peak,
                maxStars: limit
            )
            try Task.checkCancellation()
            self.detectedStarCount = try self.parseStarCount(result, expectedCommand: "stars detect")
            self.recordOperation(.starDetect, parameters: operationParameters, outputURL: nil)
            return result
        }
    }

    public func createStarMask(
        radius: Int,
        largeRadius: Int,
        layered: Bool,
        sigmaThreshold: Double,
        minPeak: Double
    ) async {
        guard let input = validatedCurrentInputURL() else {
            return
        }
        let targetLayerID = activeLayer.flatMap { $0.kind == .mask ? nil : $0.id }

        let maskRadius = min(max(radius, 1), 32)
        let largeStarRadius = min(max(largeRadius, maskRadius), 64)
        let threshold = Self.normalizedFinite(sigmaThreshold, in: 0.1...20, fallback: 3)
        let peak = Self.normalizedFinite(minPeak, in: 0...1, fallback: 0.05)
        let operationParameters = [
            "radius": String(maskRadius),
            "largeRadius": String(largeStarRadius),
            "layered": String(layered),
            "sigmaThreshold": String(threshold),
            "minPeak": String(peak),
            "maxStars": "50000",
            "input": input.path,
        ]
        let output = makeCachedPreviewOutput(prefix: "star-mask", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Create Star Mask", kind: .starMask, outputURL: output) {
            let result: ProcessingCommandResult
            var metrics: [String: String] = [:]
            let cachedArtifacts = self.project.artifacts.filter {
                $0.kind == .mask && $0.previewURL.map {
                    Self.urlsReferenceSameFile($0, output)
                } == true
            }
            if cachedArtifacts.count == 1,
               let artifact = cachedArtifacts.first,
               let storedMetrics = self.validatedStoredStarMaskMetrics(artifact.metrics),
               let cached = self.cachedResultIfAvailable(output: output, command: "stars mask") {
                result = cached
                metrics = storedMetrics
            } else {
                result = try await self.processingService.createStarMask(
                    input: input,
                    output: output,
                    radius: maskRadius,
                    largeRadius: largeStarRadius,
                    layered: layered,
                    sigmaThreshold: threshold,
                    minPeak: peak,
                    maxStars: 50_000
                )
                try Task.checkCancellation()
                metrics = try self.parseStarMaskMetrics(result)
            }

            try Task.checkCancellation()
            self.setPreview(output)
            self.detectedStarCount = metrics["stars"].flatMap(Int.init)
            let artifact: ProcessingArtifact
            let matchingArtifacts = self.project.artifacts.filter {
                $0.kind == .mask && $0.previewURL.map {
                    Self.urlsReferenceSameFile($0, output)
                } == true
            }
            if matchingArtifacts.count == 1, let existingArtifact = matchingArtifacts.first {
                artifact = existingArtifact
            } else {
                artifact = self.appendDerivedArtifactAndLayer(
                    kind: .mask,
                    name: "Star Mask",
                    operationKind: .starMask,
                    outputURLs: [output],
                    parameters: operationParameters,
                    metrics: metrics,
                    layerKind: .mask,
                    isVisible: false,
                    makeActive: false
                )
            }
            let resolvedTargetLayerID = targetLayerID ?? self.ensureImageLayer(for: input)
            self.applyLayerMask(resolvedTargetLayerID, artifactID: artifact.id)
            self.recordOperation(
                .starMask,
                parameters: operationParameters,
                outputURL: output,
                sourceURL: input
            )
            return result
        }
    }

    public func detectArtifactTrails() async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        await runProcessingJob(title: "Detect Trails", kind: .artifactDetect, outputURL: nil) {
            let result = try await self.processingService.detectArtifacts(input: input)
            try Task.checkCancellation()
            var report = try self.parseArtifactTrailReport(result)
            report.sourceURL = input.standardizedFileURL
            if report.imageWidth == nil || report.imageHeight == nil,
               let size = self.imagePixelSize(at: input) {
                report.imageWidth = Int(size.width)
                report.imageHeight = Int(size.height)
            }
            self.artifactTrailReport = report
            let selected = Set(report.topItems.filter { $0.kind != "meteor" }.prefix(8).map(\.index))
            self.updateArtifactMarkedPreview(
                input: input,
                report: report,
                selectedIndices: selected
            )
            return result
        }
    }

    public func refreshArtifactMarkedPreview(selectedIndices: Set<Int>) {
        guard canModifyWorkspaceProducts,
              let input = validatedCurrentInputURL(),
              let report = artifactTrailReport,
              report.sourceURL.map({ Self.urlsReferenceSameFile($0, input) }) == true
        else {
            return
        }
        updateArtifactMarkedPreview(
            input: input,
            report: report,
            selectedIndices: selectedIndices
        )
    }

    public func removeArtifactTrails(selectedIndices: [Int]? = nil, removeMeteors: Bool = false) async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let selected = selectedIndices?.sorted()
        var operationParameters = [
            "algorithmRevision": artifactRemovalCacheRevision,
            "selectedIndices": selected?.map(String.init).joined(separator: ",") ?? "",
            "removeAirplanes": "true",
            "removeDrones": "true",
            "removeSatellites": "true",
            "removeMeteors": String(removeMeteors),
        ]
        let encodedSelection: String? = if let selected,
                                           selected.isEmpty == false,
                                           let report = artifactTrailReport,
                                           report.sourceURL.map({ Self.urlsReferenceSameFile($0, input) }) == true {
            encodedArtifactTrailSelection(selected, report: report)
        } else {
            nil
        }
        if let encodedSelection {
            operationParameters["selectedTrailsV1"] = encodedSelection
        }
        let detectedTrailsV1: String? = if let report = artifactTrailReport,
                                           report.sourceURL.map({ Self.urlsReferenceSameFile($0, input) }) == true {
            encodedArtifactTrailRemovalHints(report)
        } else {
            nil
        }
        let output = makeCachedPreviewOutput(prefix: "artifacts", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Remove Trails", kind: .artifactRemove, outputURL: output) {
            if selected != nil, encodedSelection == nil {
                throw ProcessingServiceError.invalidSelection(command: "artifacts remove")
            }
            if let cached = self.cachedResultIfAvailable(output: output, command: "artifacts remove") {
                try Task.checkCancellation()
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.artifactRemove, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result: ProcessingCommandResult
            if let service = self.processingService as? CLIProcessingService,
               let detectedTrailsV1 {
                result = try await service.removeArtifacts(
                    input: input,
                    output: output,
                    selectedIndices: selected,
                    removeAirplanes: true,
                    removeDrones: true,
                    removeSatellites: true,
                    removeMeteors: removeMeteors,
                    detectedTrailsV1: detectedTrailsV1
                )
            } else {
                result = try await self.processingService.removeArtifacts(
                    input: input,
                    output: output,
                    selectedIndices: selected,
                    removeAirplanes: true,
                    removeDrones: true,
                    removeSatellites: true,
                    removeMeteors: removeMeteors
                )
            }
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.artifactRemove, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func cleanTimelapseArtifactSequence() async {
        let frames = timelapseArtifactSequenceFrames
        guard frames.isEmpty == false else {
            errorMessage = localized(.workflowNoLights)
            return
        }
        guard rejectMissingAsset(in: frames) == false else {
            return
        }

        let baseOutputDirectory = project.batchOutputDirectory ?? previewDirectory
        let outputDirectory = baseOutputDirectory.appendingPathComponent("clean-sequence-\(UUID().uuidString)", isDirectory: true)
        let outputFormat = project.batchOutputFormat == .tiff ? "tiff" : "png"
        let sourceArtifactIDs = sourceArtifactIDs(for: frames)
        var operationParameters = [
            "mode": "cleanSequence",
            "frames": String(frames.count),
            "source": lightAssets.isEmpty ? "allAssets" : "lights",
            "sourceFrameIDs": frames.map(\.id.uuidString).joined(separator: ","),
            "outputFormat": outputFormat,
            "removeAirplanes": "true",
            "removeDrones": "true",
            "removeSatellites": "true",
            "preserveMeteors": "true",
            "minWeight": "0.3",
            "sequenceMinLength": "36",
            "sequenceMinBrightness": "0.02",
            "recurrenceThreshold": "2",
        ]
        for (index, frame) in frames.enumerated() {
            operationParameters["inputPath.\(index)"] = frame.originalURL.path
        }

        await runProcessingJob(title: localized(.cleanTimelapseTrails), kind: .artifactRemove, outputURL: outputDirectory) {
            let result = try await self.processingService.cleanArtifactSequence(
                inputs: frames.map(\.originalURL),
                outputDirectory: outputDirectory,
                outputFormat: outputFormat,
                minWeight: 0.30,
                recurrenceThreshold: 2
            )
            try Task.checkCancellation()
            let report = try self.validatedBatchReport(
                result,
                expectedCommand: "artifacts clean-sequence",
                expectedInputs: frames.map(\.originalURL),
                outputDirectory: outputDirectory,
                expectedOutputFormat: outputFormat
            )
            let artifact = self.cleanedTimelapseSequenceArtifact(
                from: report.object,
                items: report.items,
                sourceFrames: frames,
                outputDirectory: outputDirectory,
                sourceArtifactIDs: sourceArtifactIDs
            )
            let previewURL = artifact.previewURL
            let previewHistogram: HistogramSnapshot?
            if let previewURL {
                previewHistogram = try await self.bestEffortHistogram(for: previewURL)
            } else {
                previewHistogram = nil
            }
            try Task.checkCancellation()
            self.project.appendArtifact(artifact)
            if let previewURL {
                self.setPreview(previewURL)
                self.histogram = previewHistogram
                self.project.appendLayer(
                    ProcessingLayer(
                        name: artifact.name,
                        kind: .baseImage,
                        sourceArtifactID: artifact.id,
                        inputURL: previewURL,
                        parameters: ["source": "cleanedTimelapseSequence"]
                    )
                )
                self.recordOperation(.artifactRemove, parameters: operationParameters, outputURL: previewURL)
            } else {
                self.recordOperation(.artifactRemove, parameters: operationParameters, outputURL: outputDirectory)
            }
            return result
        }
    }

    public func extractMeteorLayer() async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let output = makePreviewOutput(prefix: "meteor-layer", extension: "png")
        await runProcessingJob(title: "Extract Meteor Layer", kind: .meteorRestore, outputURL: output) {
            let result = try await self.processingService.extractMeteors(input: input, output: output)
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.appendDerivedArtifactAndLayer(
                kind: .meteorLayer,
                name: "Meteor Layer",
                operationKind: .meteorRestore,
                sourceArtifactIDs: self.activeLayer?.sourceArtifactID.map { [$0] } ?? [],
                outputURLs: [output],
                parameters: ["mode": "extract"],
                layerKind: .meteors,
                blendMode: .screen
            )
            self.recordOperation(.meteorRestore, parameters: ["mode": "extract"], outputURL: output)
            return result
        }
    }

    public func restoreMeteorsFromSelectedAsset() async {
        guard let base = validatedCurrentInputURL(), let sourceAsset = selectedAsset else {
            return
        }
        guard rejectMissingAsset(in: [sourceAsset]) == false else {
            return
        }
        let source = sourceAsset.originalURL

        let output = makePreviewOutput(prefix: "restore-meteors", extension: "tiff")
        await runProcessingJob(title: "Restore Meteors", kind: .meteorRestore, outputURL: output) {
            let result = try await self.processingService.restoreMeteors(base: base, source: source, output: output)
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(
                .meteorRestore,
                parameters: [
                    "mode": "restore",
                    "source": source.lastPathComponent,
                    "sourceAssetID": sourceAsset.id.uuidString,
                    "sourcePath": source.path,
                ],
                outputURL: output,
                promoteToLayer: true,
                sourceURL: base
            )
            return result
        }
    }

    public func backgroundPreview(
        model: String,
        mode: String,
        strength: Double,
        preserveBrightness: Bool = true,
        protectBrightTargets: Bool = true
    ) async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let normalizedModel = ["global", "grid"].contains(model) ? model : "grid"
        let normalizedMode = ["subtract", "divide"].contains(mode) ? mode : "subtract"
        let clampedStrength = Self.normalizedFinite(strength, in: 0...1, fallback: 0.35)
        let operationParameters = [
            "model": normalizedModel,
            "mode": normalizedMode,
            "strength": String(clampedStrength),
            "preserveBrightness": String(preserveBrightness),
            "protectBrightTargets": String(protectBrightTargets),
        ]
        let output = makeCachedPreviewOutput(prefix: "background", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Background", kind: .background, outputURL: output) {
            if let cached = self.cachedResultIfAvailable(output: output, command: "background") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.background, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result = try await self.processingService.background(
                input: input,
                output: output,
                model: normalizedModel,
                mode: normalizedMode,
                strength: clampedStrength,
                preserveBrightness: preserveBrightness,
                protectBrightTargets: protectBrightTargets
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.background, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func detectClouds() async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        await runProcessingJob(title: "Detect Clouds", kind: .cloudDetect, outputURL: nil) {
            let result = try await self.processingService.detectClouds(input: input)
            try Task.checkCancellation()
            var report = try self.parseCloudRegionReport(result)
            report.sourceURL = input.standardizedFileURL
            if report.imageWidth == nil || report.imageHeight == nil,
               let size = self.imagePixelSize(at: input) {
                report.imageWidth = Int(size.width)
                report.imageHeight = Int(size.height)
            }
            self.cloudRegionReport = report
            self.updateCloudMarkedPreview(
                input: input,
                report: report,
                selectedIndices: []
            )
            return result
        }
    }

    public func detectCloudsTemporally() async {
        guard let input = validatedCurrentInputURL() else {
            return
        }
        let references = temporalCloudReferenceURLs(for: input)
        guard references.count >= 2 else {
            errorMessage = "Temporal cloud detection needs at least two other loaded frames."
            return
        }
        await runProcessingJob(title: "Detect Clouds Across Frames", kind: .cloudDetect, outputURL: nil) {
            var arguments = ["clouds", "temporal-detect", "--input", input.path]
            for reference in references {
                arguments.append(contentsOf: ["--reference", reference.path])
            }
            let result = try await self.processingService.runCLI(arguments: arguments)
            try Task.checkCancellation()
            var report = try self.parseCloudRegionReport(result)
            report.sourceURL = input.standardizedFileURL
            if report.imageWidth == nil || report.imageHeight == nil,
               let size = self.imagePixelSize(at: input) {
                report.imageWidth = Int(size.width)
                report.imageHeight = Int(size.height)
            }
            self.cloudRegionReport = report
            self.updateCloudMarkedPreview(input: input, report: report, selectedIndices: [])
            return result
        }
    }

    public func refreshCloudMarkedPreview(selectedIndices: Set<Int>) {
        guard canModifyWorkspaceProducts,
              let input = validatedCurrentInputURL(),
              let report = cloudRegionReport,
              report.sourceURL.map({ Self.urlsReferenceSameFile($0, input) }) == true
        else {
            return
        }
        updateCloudMarkedPreview(
            input: input,
            report: report,
            selectedIndices: selectedIndices
        )
    }

    public func removeClouds(selectedIndices: [Int]? = nil, strength: Double) async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let clampedStrength = Self.normalizedFinite(
            strength,
            in: 0...1,
            fallback: parameters.cloudRemovalStrength
        )
        let selected = selectedIndices?.sorted()
        let usesTemporalDetection: Bool
        let temporalReferences: [URL]
        if let report = cloudRegionReport,
           report.temporalFramesUsed > 0,
           report.sourceURL.map({ Self.urlsReferenceSameFile($0, input) }) == true {
            usesTemporalDetection = true
            temporalReferences = temporalCloudReferenceURLs(for: input)
        } else {
            usesTemporalDetection = false
            temporalReferences = []
        }
        let command = usesTemporalDetection ? "clouds temporal-remove" : "clouds remove"
        var operationParameters = [
            "selectedIndices": selected?.map(String.init).joined(separator: ",") ?? "",
            "strength": String(clampedStrength),
            "temporalReferences": temporalReferences.map(\.path).joined(separator: "|"),
        ]
        let encodedSelection: String? = if let selected,
                                           selected.isEmpty == false,
                                           let report = cloudRegionReport,
                                           report.sourceURL.map({ Self.urlsReferenceSameFile($0, input) }) == true {
            encodedCloudRegionSelection(selected, report: report)
        } else {
            nil
        }
        if let encodedSelection {
            operationParameters["selectedCloudRegionsV1"] = encodedSelection
        }
        let output = makeCachedPreviewOutput(prefix: "clouds", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Remove Clouds", kind: .cloudRemove, outputURL: output) {
            if selected != nil, encodedSelection == nil {
                throw ProcessingServiceError.invalidSelection(command: command)
            }
            if usesTemporalDetection, temporalReferences.count < 2 {
                throw ProcessingServiceError.invalidSelection(command: command)
            }
            if let cached = self.cachedResultIfAvailable(output: output, command: command) {
                try Task.checkCancellation()
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.parameters.cloudRemovalStrength = clampedStrength
                self.recordOperation(.cloudRemove, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result: ProcessingCommandResult
            if usesTemporalDetection {
                var arguments = ["clouds", "temporal-remove", "--input", input.path, "--output", output.path, "--strength", String(clampedStrength)]
                if let selected, selected.isEmpty == false {
                    arguments.append(contentsOf: ["--selected-indices", selected.map(String.init).joined(separator: ",")])
                }
                for reference in temporalReferences {
                    arguments.append(contentsOf: ["--reference", reference.path])
                }
                result = try await self.processingService.runCLI(arguments: arguments)
            } else {
                result = try await self.processingService.removeClouds(
                    input: input,
                    output: output,
                    selectedIndices: selected,
                    strength: clampedStrength
                )
            }
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.parameters.cloudRemovalStrength = clampedStrength
            self.recordOperation(.cloudRemove, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func normalizePreview(targetBackground: Double, targetScale: Double) async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let normalizedBackground = Self.normalizedFinite(targetBackground, in: 0.02...0.6, fallback: 0.25)
        let normalizedScale = Self.normalizedFinite(targetScale, in: 0.5...2, fallback: 1)
        let operationParameters = [
            "targetBackground": String(normalizedBackground),
            "targetScale": String(normalizedScale),
        ]
        let output = makeCachedPreviewOutput(prefix: "normalize", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Normalize", kind: .normalize, outputURL: output) {
            if let cached = self.cachedResultIfAvailable(output: output, command: "normalize") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.normalize, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result = try await self.processingService.normalize(
                input: input,
                output: output,
                targetBackground: normalizedBackground,
                targetScale: normalizedScale
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.normalize, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func startContinuumTonePreview(settings: ContinuumToneSettings) {
        launch { await self.continuumTonePreview(settings: settings) }
    }

    public func continuumTonePreview(settings: ContinuumToneSettings) async {
        guard let input = validatedCurrentInputURL() else { return }
        guard settings.isValid else { errorMessage = ContinuumToneError.invalidSettings.localizedDescription; return }
        guard AssetKind.detect(from: input) != .fits else {
            errorMessage = ContinuumToneError.displayImageRequired.localizedDescription; return
        }
        let operationParameters = settings.operationParameters
        let output = makeCachedPreviewOutput(prefix: "continuum-tone-v1", input: input,
            parameters: operationParameters, extension: "tiff")
        await runProcessingJob(title: "Diffuse Tone", kind: .localContrast, outputURL: output) {
            let result: ProcessingCommandResult
            if let cached = self.cachedResultIfAvailable(output: output, command: "local-contrast") {
                result = cached
            } else {
                result = try await self.processingService.continuumTone(input: input, output: output, settings: settings)
            }
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.localContrast, parameters: operationParameters, outputURL: output,
                promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func startDisplayGridPreview(settings: DisplayGridSettings) {
        launch { await self.displayGridPreview(settings: settings) }
    }

    public func displayGridPreview(settings: DisplayGridSettings) async {
        guard let input = validatedCurrentInputURL() else { return }
        guard settings.isValid else { errorMessage = DisplayGridError.invalidSettings.localizedDescription; return }
        guard DisplayGridSettings.supports(input) else {
            errorMessage = DisplayGridError.displayImageRequired.localizedDescription; return
        }
        let operationParameters = settings.operationParameters
        let output = makeCachedPreviewOutput(prefix: "display-grid-v1", input: input,
            parameters: operationParameters, extension: "tiff")
        await runProcessingJob(title: "Display Grid Reduction", kind: .denoise, outputURL: output) {
            let result: ProcessingCommandResult
            if let cached = self.cachedResultIfAvailable(output: output, command: "suppress-grid") {
                result = cached
            } else {
                result = try await self.processingService.suppressDisplayGrid(input: input, output: output, settings: settings)
            }
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.denoise, parameters: operationParameters, outputURL: output,
                promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func startGreenCastPreview(settings: GreenCastSettings) {
        launch { await self.greenCastPreview(settings: settings) }
    }

    public func greenCastPreview(settings: GreenCastSettings) async {
        guard let input = validatedCurrentInputURL() else { return }
        var normalized = settings
        normalized.amount = Self.normalizedFinite(settings.amount, in: 0...1, fallback: 0.65)
        normalized.backgroundLimit = Self.normalizedFinite(settings.backgroundLimit, in: 0...1, fallback: 0.32)
        normalized.greenThreshold = Self.normalizedFinite(settings.greenThreshold, in: 0...1, fallback: 0)
        let appliedSettings = normalized
        let operationParameters = appliedSettings.operationParameters
        let output = makeCachedPreviewOutput(prefix: "green-cast-v1", input: input,
            parameters: operationParameters, extension: "tiff")
        await runProcessingJob(title: "Green Cast Adjustment", kind: .colorNeutralize, outputURL: output) {
            let result: ProcessingCommandResult
            if let cached = self.cachedResultIfAvailable(output: output, command: "color remove-green") {
                result = cached
            } else {
                result = try await self.processingService.removeGreenCast(input: input, output: output, settings: appliedSettings)
            }
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.colorNeutralize, parameters: operationParameters, outputURL: output,
                promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func colorNeutralizePreview(strength: Double) async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let normalizedStrength = Self.normalizedFinite(strength, in: 0...1, fallback: 1)
        let operationParameters = ["strength": String(normalizedStrength)]
        let output = makeCachedPreviewOutput(prefix: "neutralize", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Color Neutralize", kind: .colorNeutralize, outputURL: output) {
            if let cached = self.cachedResultIfAvailable(output: output, command: "color neutralize") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.colorNeutralize, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result = try await self.processingService.colorNeutralize(
                input: input,
                output: output,
                strength: normalizedStrength
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.colorNeutralize, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func colorSaturatePreview(amount: Double) async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let normalizedAmount = Self.normalizedFinite(amount, in: -0.5...1.5, fallback: 0.2)
        let operationParameters = ["amount": String(normalizedAmount)]
        let output = makeCachedPreviewOutput(prefix: "saturate", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Color Saturate", kind: .colorSaturate, outputURL: output) {
            if let cached = self.cachedResultIfAvailable(output: output, command: "color saturate") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.colorSaturate, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result = try await self.processingService.colorSaturate(
                input: input,
                output: output,
                amount: normalizedAmount
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.colorSaturate, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func deconvolvePreview(iterations: Int, radius: Int, sigma: Double) async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        let normalizedIterations = min(max(iterations, 1), 32)
        let normalizedRadius = min(max(radius, 1), 8)
        let normalizedSigma = Self.normalizedFinite(sigma, in: 0.5...3.5, fallback: 1.2)
        let operationParameters = [
            "iterations": String(normalizedIterations),
            "radius": String(normalizedRadius),
            "sigma": String(normalizedSigma),
        ]
        let output = makeCachedPreviewOutput(prefix: "deconvolve", input: input, parameters: operationParameters)
        await runProcessingJob(title: "Deconvolve", kind: .deconvolve, outputURL: output) {
            if let cached = self.cachedResultIfAvailable(output: output, command: "deconvolve") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.recordOperation(.deconvolve, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return cached
            }
            let result = try await self.processingService.deconvolve(
                input: input,
                output: output,
                iterations: normalizedIterations,
                radius: normalizedRadius,
                sigma: normalizedSigma
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.recordOperation(.deconvolve, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return result
        }
    }

    public func runDrizzle(scale: Int, pixfrac: Double = 1.0, alignment: AlignmentMethod) async {
        guard rejectMissingAsset(in: lightAssets) == false else {
            return
        }
        let normalizedScale = min(max(scale, 1), 4)
        let normalizedPixfrac = Self.normalizedFinite(pixfrac, in: 0.1...1, fallback: 1)
        let plannedLights = lightAssets
        let plannedCacheInput = URL(fileURLWithPath: plannedLights.map(\.originalURL.path).joined(separator: "|"))
        let plannedOutput = makeCachedPreviewOutput(
            prefix: "drizzle",
            input: plannedCacheInput,
            parameters: [
                "scale": String(normalizedScale),
                "pixfrac": String(normalizedPixfrac),
                "alignment": alignment.rawValue,
                "frames": String(plannedLights.count),
                "lightIDs": plannedLights.map(\.id.uuidString).joined(separator: ","),
                "lights": inputSignature(plannedLights.map(\.originalURL)),
            ],
            extension: "tiff"
        )
        await runJob(title: "Drizzle", kind: .drizzle, outputURL: plannedOutput) {
            let lights = self.lightAssets
            guard lights.isEmpty == false else {
                throw WorkflowError.noLights
            }

            let operationParameters = [
                "scale": String(normalizedScale),
                "pixfrac": String(normalizedPixfrac),
                "alignment": alignment.rawValue,
                "frames": String(lights.count),
                "lightIDs": lights.map(\.id.uuidString).joined(separator: ","),
                "lights": self.inputSignature(lights.map(\.originalURL)),
            ]
            let cacheInput = URL(fileURLWithPath: lights.map(\.originalURL.path).joined(separator: "|"))
            let output = self.makeCachedPreviewOutput(
                prefix: "drizzle",
                input: cacheInput,
                parameters: operationParameters,
                extension: "tiff"
            )
            if let cached = self.cachedResultIfAvailable(output: output, command: "drizzle") {
                try await self.commitPreviewAfterHistogramRefresh(output)
                self.appendDerivedArtifactAndLayer(
                    kind: .editedImage,
                    name: "Drizzle \(normalizedScale)x",
                    operationKind: .drizzle,
                    sourceArtifactIDs: self.sourceArtifactIDs(for: lights),
                    outputURLs: [output],
                    parameters: operationParameters,
                    metrics: [
                        "frames": String(lights.count),
                        "scale": String(normalizedScale),
                        "pixfrac": String(normalizedPixfrac),
                        "alignment": alignment.rawValue,
                    ],
                    layerKind: .baseImage
                )
                self.recordOperation(.drizzle, parameters: operationParameters, outputURL: output)
                return self.message(for: cached)
            }

            let result = try await self.processingService.drizzle(
                inputs: lights.map(\.originalURL),
                output: output,
                scale: normalizedScale,
                pixfrac: normalizedPixfrac,
                alignment: alignment
            )
            try Task.checkCancellation()
            try await self.commitPreviewAfterHistogramRefresh(output)
            self.appendDerivedArtifactAndLayer(
                kind: .editedImage,
                name: "Drizzle \(normalizedScale)x",
                operationKind: .drizzle,
                sourceArtifactIDs: self.sourceArtifactIDs(for: lights),
                outputURLs: [output],
                parameters: operationParameters,
                metrics: [
                    "frames": String(lights.count),
                    "scale": String(normalizedScale),
                    "pixfrac": String(normalizedPixfrac),
                    "alignment": alignment.rawValue,
                ],
                layerKind: .baseImage
            )
            self.recordOperation(.drizzle, parameters: operationParameters, outputURL: output)
            return self.message(for: result)
        }
    }

    public func exportCurrentImage(to output: URL) async {
        guard editGraphNeedsReplay == false else {
            errorMessage = localized(.editGraphNeedsReplay)
            return
        }
        guard hasAmbiguousEditGraphProvenance == false else {
            errorMessage = localized(.editGraphAmbiguousProvenance)
            return
        }
        if let missingAsset = exportBlockingMissingAsset {
            errorMessage = "\(localized(.missingAssetExportError)): \(missingAsset.displayName)"
            return
        }
        if let missingWorkspaceSource = exportBlockingWorkspaceSourceName {
            errorMessage = "\(localized(.missingWorkspaceSourceError)): \(missingWorkspaceSource)"
            return
        }
        guard exportCanvasInputsAvailable else {
            errorMessage = localized(.missingWorkspaceSourceError)
            return
        }

        let taskRequestedExportOptions = exportOptions
        let taskExportOptions = Self.effectiveExportOptions(taskRequestedExportOptions, for: output)
        let taskRawOptions = parameters.rawProcessingOptions

        await runProcessingJob(title: "Export Image", kind: .export, outputURL: output) {
            defer {
                self.clearReplayTemporaryDirectories()
            }
            let input: URL?
            if self.canReplayFullResolutionForExport {
                input = try await self.replayEditGraphForExport()
            } else {
                input = try await self.exportInputURL()
            }
            guard let input else {
                throw LayerStackPreviewError.noVisibleLayers
            }
            let result = try await self.processingService.convert(
                input: input,
                output: output,
                options: taskExportOptions,
                rawOptions: taskRawOptions
            )
            try Task.checkCancellation()
            do {
                try self.writeExportSidecar(
                    input: input,
                    output: output,
                    options: taskExportOptions,
                    requestedOptions: taskRequestedExportOptions,
                    rawOptions: taskRawOptions
                )
            } catch {
                self.errorMessage = "\(self.localized(.exportSidecarFailed)) \(error.localizedDescription)"
            }
            return result
        }
    }

    public func exportProductManifest(to output: URL) async {
        await runLocalJob(title: "Export Product Manifest", kind: .export) {
            let markdown = self.productManifestMarkdown()
            let directory = output.deletingLastPathComponent()
            try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
            try markdown.write(to: output, atomically: true, encoding: .utf8)
            return "Exported product manifest: \(output.path)"
        }
    }

    public func previewLayerStack() async {
        guard canPreviewVisibleLayerStack else {
            errorMessage = localized(.missingWorkspaceSourceError)
            return
        }
        await runLocalJob(title: "Preview Layer Stack", kind: .preview) {
            let visibleLayers = self.visiblePreviewLayers
            guard visibleLayers.isEmpty == false else {
                throw LayerStackPreviewError.noVisibleLayers
            }

            let output = self.makePreviewOutput(prefix: "layer-stack", extension: "png")
            try self.renderLayerStackPreview(layers: visibleLayers, output: output)
            try await self.commitPreviewAfterHistogramRefresh(output, canvasMode: .layerComposite)
            return "Layer stack preview: \(output.path)"
        }
    }

    @discardableResult
    private func refreshVisibleLayerStackPreview(
        trackHistory: Bool = false,
        allowWhileProcessing: Bool = false
    ) -> Bool {
        guard allowWhileProcessing || isProcessing == false else {
            return false
        }
        let visibleLayers = visiblePreviewLayers
        if let unavailableLayer = visibleLayers.first(where: {
            isLayerInputAvailable($0) == false || isLayerMaskInputAvailable($0) == false
        }) {
            errorMessage = "\(localized(.missingWorkspaceSourceError)): \(unavailableLayer.name)"
            return false
        }
        guard let firstURL = visibleLayers.first?.inputURL
            ?? emptyLayerStackSourceLayer?.inputURL
        else {
            previewURL = nil
            previewOperationID = nil
            editGraphNeedsReplay = false
            canvasMode = .sourcePreview
            histogram = nil
            curveReferenceHistogram = nil
            artifactTrailReport = nil
            cloudRegionReport = nil
            detectedStarCount = nil
            replaceMarkedPreview(nil)
            commandOutput = "\(localized(.previewLayerStack)): 0"
            errorMessage = nil
            return true
        }

        let output = makeCachedPreviewOutput(
            prefix: visibleLayers.isEmpty ? "layer-stack-empty" : "layer-stack-live",
            input: firstURL,
            parameters: [
                "layers": visibleLayers.isEmpty ? "empty" : layerStackSignature(visibleLayers),
            ]
        )

        do {
            if visibleLayers.isEmpty {
                try renderEmptyLayerStack(sourceURL: firstURL, output: output, format: .RGBA8)
            } else {
                try renderLayerStackPreview(layers: visibleLayers, output: output)
            }
            setPreview(output, trackHistory: trackHistory, canvasMode: .layerComposite)
            commandOutput = "\(localized(.previewLayerStack)): \(visibleLayers.count)"
            errorMessage = nil
            return true
        } catch {
            errorMessage = error.localizedDescription
            return false
        }
    }

    @discardableResult
    private func performLayerCompositeMutation(_ mutation: () -> Void) -> Bool {
        let projectSnapshot = project
        let activeLayerSnapshot = activeLayerID
        let previewHistorySnapshot = previewHistory
        let canStepBackSnapshot = canStepBack
        let errorMessageSnapshot = errorMessage

        mutation()
        guard refreshVisibleLayerStackPreview() else {
            if processingService.requiresExistingInputFiles == false {
                canvasMode = .layerComposite
                errorMessage = errorMessageSnapshot
                triggerAutosave()
                return true
            }
            project = projectSnapshot
            activeLayerID = activeLayerSnapshot
            previewHistory = previewHistorySnapshot
            canStepBack = canStepBackSnapshot
            return false
        }
        triggerAutosave()
        return true
    }

    private func reconcileLayerCompositeAfterProjectMutation(layerStackChanged: Bool) {
        guard layerStackChanged, canvasMode == .layerComposite else {
            errorMessage = nil
            return
        }

        errorMessage = nil
        guard refreshVisibleLayerStackPreview() == false else {
            return
        }

        previewURL = nil
        previewOperationID = nil
        editGraphNeedsReplay = false
        artifactTrailReport = nil
        cloudRegionReport = nil
        detectedStarCount = nil
        replaceMarkedPreview(nil)
        histogram = nil
        curveReferenceHistogram = nil
    }

    public func refreshHistogram() async {
        guard let input = validatedCurrentInputURL() else {
            return
        }

        await runJob(title: "Histogram", kind: .histogram) {
            let result = try await self.processingService.histogram(input: input, bins: 64)
            try Task.checkCancellation()
            let snapshot = try self.parseHistogram(result, expectedBins: 64)
            self.histogram = snapshot
            self.curveReferenceHistogram = snapshot
            return self.message(for: result)
        }
    }

    public func applyCurvePreview(
        inputOverride: URL? = nil,
        referenceHistogramOverride: HistogramSnapshot? = nil
    ) async {
        guard let input = inputOverride ?? validatedCurrentInputURL() else {
            return
        }

        let operationParameters = curveOperationParameters(
            points: parameters.curveEditorPoints,
            channel: parameters.curveChannel
        )
        let curvePoints = parameters.curveEditorPoints
        let curvePointPairs = parameters.curvePointPairs
        let curveChannel = parameters.curveChannel
        let rawOptions = parameters.rawProcessingOptions
        let inputWidth = resolvedPixelWidthForCurveProcessing(at: input)
        let previewWidth = interactiveCurvePreviewWidth(for: parameters.previewWidth)
        let commitsProxy = processingService is CLIProcessingService &&
            inputWidth.map { $0 > previewWidth } == true
        var cacheParameters = operationParameters.merging(
            rawDecodeParameters(from: rawOptions),
            uniquingKeysWith: { _, new in new }
        )
        if commitsProxy {
            cacheParameters["previewWidth"] = String(previewWidth)
            cacheParameters["previewOnly"] = "true"
        }
        let output = makeCachedPreviewOutput(prefix: "curves", input: input, parameters: cacheParameters)

        await runProcessingJob(title: "Apply Curve", kind: .curves, outputURL: output) {
            let startedAt = Date()
            var executionPath = "processing-service"
            let referenceHistogram: HistogramSnapshot?
            if let referenceHistogramOverride {
                referenceHistogram = referenceHistogramOverride
            } else if let existingHistogram = self.curveReferenceHistogram {
                referenceHistogram = existingHistogram
            } else {
                referenceHistogram = try await self.bestEffortHistogram(for: input)
            }
            if let cached = self.cachedResultIfAvailable(output: output, command: "curves") {
                let annotated = self.annotatedCurveApplyResult(cached, executionSource: "cache")
                self.logCurveTelemetry(
                    event: "apply-curve-complete",
                    startedAt: startedAt,
                    source: "cache",
                    width: self.imagePixelWidth(at: input),
                    points: curvePoints.count
                )
                self.commitPreviewWithDeferredHistogramRefresh(
                    output,
                    resetCurveReferenceHistogram: false
                )
                self.curveReferenceHistogram = referenceHistogram
                self.recordOperation(.curves, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
                return annotated
            }
            let result: ProcessingCommandResult
            if let bridged = try await self.runEngineCurveApplyIfAvailable(
                input: input,
                output: output,
                points: curvePointPairs,
                channel: curveChannel,
                rawOptions: rawOptions,
                widthOverride: commitsProxy ? previewWidth : nil,
                retainDecodedSource: commitsProxy
            ) {
                executionPath = commitsProxy ? "engine-bridge-proxy" : "engine-bridge"
                result = bridged
            } else if commitsProxy {
                executionPath = "processing-service-proxy"
                result = try await self.processingService.makeCurvePreview(
                    input: input,
                    output: output,
                    width: previewWidth,
                    rawOptions: rawOptions,
                    points: curvePointPairs,
                    channel: curveChannel
                )
            } else {
                executionPath = "processing-service"
                result = try await self.processingService.curves(
                    input: input,
                    output: output,
                    points: curvePointPairs,
                    channel: curveChannel
                )
            }
            try Task.checkCancellation()
            self.logCurveTelemetry(
                event: "apply-curve-complete",
                startedAt: startedAt,
                source: executionPath,
                width: self.imagePixelWidth(at: input),
                points: curvePoints.count
            )
            self.commitPreviewWithDeferredHistogramRefresh(
                output,
                resetCurveReferenceHistogram: false
            )
            self.curveReferenceHistogram = referenceHistogram
            self.recordOperation(.curves, parameters: operationParameters, outputURL: output, promoteToLayer: true, sourceURL: input)
            return self.annotatedCurveApplyResult(result, executionSource: executionPath)
        }
    }

    public func register(reference: PhotonStackAsset, moving: PhotonStackAsset, alignment: AlignmentMethod) async {
        guard reference.id != moving.id,
              project.assets.contains(where: { $0.id == reference.id }),
              project.assets.contains(where: { $0.id == moving.id })
        else {
            return
        }
        guard rejectMissingAsset(in: [reference, moving]) == false else {
            return
        }
        let mode = alignment == .none ? AlignmentMethod.translation : alignment
        let operationParameters = [
            "reference": reference.id.uuidString,
            "moving": moving.id.uuidString,
            "alignment": mode.rawValue,
        ]
        let scientificOutput = makeCachedPreviewOutput(
            prefix: "registered",
            input: moving.originalURL,
            parameters: operationParameters,
            extension: "fits"
        )
        let previewOutput = makeCachedPreviewOutput(
            prefix: "registered-preview",
            input: scientificOutput,
            parameters: operationParameters,
            extension: "png"
        )

        await runProcessingJob(title: "Register Frames", kind: .register, outputURL: previewOutput) {
            let result = try await self.processingService.register(
                reference: reference.originalURL,
                moving: moving.originalURL,
                output: scientificOutput,
                alignment: mode
            )
            try Task.checkCancellation()
            let report = try self.parseRegistrationReport(
                result,
                reference: reference,
                moving: moving,
                mode: mode,
                output: scientificOutput
            )
            let previewResult = try await self.processingService.makePreview(
                input: scientificOutput,
                output: previewOutput,
                width: self.parameters.previewWidth,
                rawOptions: self.parameters.rawProcessingOptions
            )
            try Task.checkCancellation()
            self.registrationReport = report
            try await self.commitPreviewAfterHistogramRefresh(previewOutput)
            let recordedParameters = [
                "reference": reference.displayName,
                "referenceID": reference.id.uuidString,
                "moving": moving.displayName,
                "movingID": moving.id.uuidString,
                "alignment": mode.rawValue,
                "matches": String(report.matches),
                "previewPath": previewOutput.path,
            ]
            self.appendDerivedArtifactAndLayer(
                kind: .registeredSequence,
                name: "Registered \(moving.displayName)",
                operationKind: .register,
                sourceArtifactIDs: self.sourceArtifactIDs(for: [reference, moving]),
                outputDirectory: scientificOutput.deletingLastPathComponent(),
                outputURLs: [scientificOutput],
                parameters: recordedParameters,
                metrics: [
                    "matches": String(report.matches),
                    "dx": String(report.dx),
                    "dy": String(report.dy),
                    "scale": String(report.scale),
                    "rotationDegrees": String(report.rotationDegrees),
                ],
                layerKind: .baseImage
            )
            self.recordOperation(.register, parameters: recordedParameters, outputURL: previewOutput)
            return ProcessingCommandResult(
                command: result.command,
                exitCode: result.exitCode,
                standardOutput: [result.standardOutput, previewResult.standardOutput]
                    .filter { $0.isEmpty == false }
                    .joined(separator: "\n"),
                standardError: [result.standardError, previewResult.standardError]
                    .filter { $0.isEmpty == false }
                    .joined(separator: "\n"),
                durationMilliseconds: result.durationMilliseconds
            )
        }
    }

    public func registerBatch(reference: PhotonStackAsset, alignment: AlignmentMethod) async {
        let mode = alignment == .none ? AlignmentMethod.distortion : alignment
        syncBatchRegistrationSelection(selectNewCandidates: false)
        let movingFrames = selectedBatchRegistrationMovingFrames(reference: reference)
        guard movingFrames.isEmpty == false else {
            errorMessage = localized(.registrationNoFramesSelected)
            return
        }
        guard rejectMissingAsset(in: [reference] + movingFrames) == false else {
            return
        }

        let outputDirectory = previewDirectory.appendingPathComponent("registered-batch-\(UUID().uuidString)", isDirectory: true)
        let sourceArtifactIDs = sourceArtifactIDs(for: [reference] + movingFrames)
        let operationParameters = [
            "reference": reference.displayName,
            "referenceID": reference.id.uuidString,
            "alignment": mode.rawValue,
            "frames": String(movingFrames.count + 1),
            "movingFrames": String(movingFrames.count),
            "selectedMovingFrameIDs": movingFrames.map(\.id.uuidString).joined(separator: ","),
            "batch": "true",
        ]

        await runProcessingJob(title: localized(.registerBatchFrames), kind: .register, outputURL: outputDirectory) {
            let result = try await self.processingService.registerBatch(
                reference: reference.originalURL,
                inputs: movingFrames.map(\.originalURL),
                outputDirectory: outputDirectory,
                alignment: mode,
                outputFormat: "fits"
            )
            try Task.checkCancellation()
            let report = try self.validatedBatchReport(
                result,
                expectedCommand: "register-batch",
                expectedInputs: [reference.originalURL] + movingFrames.map(\.originalURL),
                outputDirectory: outputDirectory,
                expectedMode: mode,
                expectedOutputFormat: "fits"
            )
            var artifact = self.registeredSequenceArtifact(
                from: report.object,
                items: report.items,
                reference: reference,
                movingFrames: movingFrames,
                outputDirectory: outputDirectory,
                mode: mode,
                sourceArtifactIDs: sourceArtifactIDs
            )
            let outputItems = report.items.compactMap { item -> (path: String, isReference: Bool)? in
                guard let path = item["output"] as? String else {
                    return nil
                }
                return (path, item["reference"] as? Bool ?? false)
            }
            if let previewOutput = outputItems.first(where: { $0.isReference == false }) ?? outputItems.first {
                let firstOutput = previewOutput.path
                let scientificOutput = URL(fileURLWithPath: firstOutput)
                let displayPreview = self.makeCachedPreviewOutput(
                    prefix: "registered-batch-preview",
                    input: scientificOutput,
                    parameters: operationParameters,
                    extension: "png"
                )
                _ = try await self.processingService.makePreview(
                    input: scientificOutput,
                    output: displayPreview,
                    width: self.parameters.previewWidth,
                    rawOptions: self.parameters.rawProcessingOptions
                )
                let histogram = try await self.bestEffortHistogram(for: displayPreview)
                try Task.checkCancellation()
                artifact.parameters["previewPath"] = displayPreview.path
                artifact.parameters["outputFormat"] = "fits"
                self.project.appendArtifact(artifact)
                self.project.appendLayer(
                    ProcessingLayer(
                        name: artifact.name,
                        kind: .baseImage,
                        sourceArtifactID: artifact.id,
                        inputURL: displayPreview,
                        parameters: ["source": "registeredSequence"]
                    )
                )
                self.setPreview(displayPreview)
                self.histogram = histogram
                var recordedParameters = operationParameters
                recordedParameters["artifactID"] = artifact.id.uuidString
                recordedParameters["outputFormat"] = "fits"
                recordedParameters["previewPath"] = displayPreview.path
                self.recordOperation(.register, parameters: recordedParameters, outputURL: displayPreview)
            } else {
                throw ProcessingServiceError.invalidReport(command: "register-batch")
            }
            return result
        }
    }

    public func runStackWorkflow() async {
        guard rejectMissingAsset(in: lightAssets + darkAssets + biasAssets + flatAssets) == false else {
            return
        }
        if let warning = calibrationConfigurationWarning {
            errorMessage = warning
            return
        }
        await runJob(title: "Calibrate and Stack", kind: .stack, outputURL: nil) {
            let lights = self.lightAssets
            guard lights.isEmpty == false else {
                throw WorkflowError.noLights
            }
            let registeredSequence = self.latestArtifact(kind: .registeredSequence, minimumFrameCount: lights.count)
                .flatMap { artifact -> ProcessingArtifact? in
                    let outputs = Array(artifact.outputURLs.prefix(lights.count))
                    guard outputs.count == lights.count,
                          outputs.allSatisfy(Self.isUsableCachedOutput),
                          self.registeredArtifactMatches(artifact, lights: lights)
                    else {
                        return nil
                    }
                    return artifact
                }

            var log: [String] = []
            let runDirectory = self.previewDirectory.appendingPathComponent("workflow-\(UUID().uuidString)", isDirectory: true)
            let mastersDirectory = runDirectory.appendingPathComponent("masters", isDirectory: true)
            try FileManager.default.createDirectory(at: mastersDirectory, withIntermediateDirectories: true)
            log.append("Workflow directory: \(runDirectory.path)")
            log.append("Lights: \(lights.count), darks: \(self.darkAssets.count), bias: \(self.biasAssets.count), flats: \(self.flatAssets.count)")

            let masterSteps = [self.darkAssets, self.biasAssets, self.flatAssets].filter { $0.isEmpty == false }.count
            let restoreMeteorSteps = self.parameters.workflowRestoreMeteors ? lights.count : 0
            let alignmentPasses = 1
            let prealignmentSteps = self.parameters.workflowAlignment == .none ? 0 : max(lights.count - 1, 0) * alignmentPasses
            let totalSteps = max(
                masterSteps + lights.count + prealignmentSteps + restoreMeteorSteps + 17,
                1
            )
            var completedSteps = 0
            let progressScope = ProcessingProgressContext.progressScope
            let stepSpan = 1.0 / Double(totalSteps)
            progressScope?.update(lowerBound: 0, upperBound: stepSpan)
            defer {
                progressScope?.update(lowerBound: 0, upperBound: 1)
            }
            @MainActor func advanceProgress(_ message: String) {
                completedSteps += 1
                let fraction = min(Double(completedSteps) / Double(totalSteps), 1)
                let nextStep = min(Double(completedSteps + 1) / Double(totalSteps), 1)
                progressScope?.update(lowerBound: fraction, upperBound: nextStep)
                self.updateProgress(fraction, message: message)
            }

            let masterMethod = self.parameters.workflowMasterMethod == .average ? StackMethod.average : StackMethod.median
            var calibrationRawOptions = self.parameters.rawProcessingOptions
            calibrationRawOptions.linearOutput = true
            let masterDark = try await self.createMasterIfNeeded(
                title: "master dark",
                assets: self.darkAssets,
                output: mastersDirectory.appendingPathComponent("master-dark.fits"),
                method: masterMethod,
                rawOptions: calibrationRawOptions,
                log: &log
            )
            if masterDark != nil {
                advanceProgress("Master dark")
            }
            let masterBias = try await self.createMasterIfNeeded(
                title: "master bias",
                assets: self.biasAssets,
                output: mastersDirectory.appendingPathComponent("master-bias.fits"),
                method: masterMethod,
                rawOptions: calibrationRawOptions,
                log: &log
            )
            if masterBias != nil {
                advanceProgress("Master bias")
            }
            let masterFlat = try await self.createMasterIfNeeded(
                title: "master flat",
                assets: self.flatAssets,
                output: mastersDirectory.appendingPathComponent("master-flat.fits"),
                method: masterMethod,
                rawOptions: calibrationRawOptions,
                log: &log
            )
            if masterFlat != nil {
                advanceProgress("Master flat")
            }

            let hasCalibrationFrames = masterDark != nil || masterBias != nil || masterFlat != nil
            var stackInputs: [URL] = []
            var usedRegisteredSequence: ProcessingArtifact?
            if let registeredSequence,
               hasCalibrationFrames == false,
               registeredSequence.outputURLs.count >= lights.count {
                usedRegisteredSequence = registeredSequence
                stackInputs = Array(registeredSequence.outputURLs.prefix(lights.count))
                log.append("Using registered sequence artifact:")
                log.append("  \(registeredSequence.name)")
                log.append("  frames: \(stackInputs.count)")
                log.append("  output: \(registeredSequence.outputDirectory?.path ?? registeredSequence.previewURL?.deletingLastPathComponent().path ?? "")")
                advanceProgress("Loaded registered sequence")
            } else if hasCalibrationFrames {
                let calibratedDirectory = runDirectory.appendingPathComponent("calibrated", isDirectory: true)
                try FileManager.default.createDirectory(at: calibratedDirectory, withIntermediateDirectories: true)

                for light in lights {
                    try Task.checkCancellation()
                    let output = calibratedDirectory
                        .appendingPathComponent(light.originalURL.deletingPathExtension().lastPathComponent)
                        .appendingPathExtension("calibrated.fits")
                    let calibrated = try await self.processingService.calibrate(
                        light: light.originalURL,
                        output: output,
                        dark: masterDark,
                        bias: masterBias,
                        flat: masterFlat,
                        darkBiasState: self.parameters.workflowDarkBiasState,
                        flatBiasState: self.parameters.workflowFlatBiasState,
                        rawOptions: calibrationRawOptions
                    )
                    try Task.checkCancellation()
                    log.append("Calibrated \(light.displayName):")
                    log.append(self.message(for: calibrated))
                    stackInputs.append(output)
                    advanceProgress("Calibrated \(light.displayName)")
                }
            } else {
                log.append("No calibration frames assigned; decoding RAW lights with current RAW settings before stacking.")
                let rawDecodeDirectory = runDirectory.appendingPathComponent("decoded-raw", isDirectory: true)
                var createdRawDecodeDirectory = false
                let rawDecodeExportOptions = ExportOptions(
                    bitDepth: .sixteen,
                    colorSpace: self.parameters.rawProcessingOptions.linearOutput ? .linearSRGB : .srgb,
                    fitsValueMode: .scientific
                )
                for light in lights {
                    try Task.checkCancellation()
                    if light.kind == .raw {
                        if createdRawDecodeDirectory == false {
                            try FileManager.default.createDirectory(at: rawDecodeDirectory, withIntermediateDirectories: true)
                            createdRawDecodeDirectory = true
                        }
                        let decodedOutput = rawDecodeDirectory
                            .appendingPathComponent(light.originalURL.deletingPathExtension().lastPathComponent)
                            .appendingPathExtension("decoded.fits")
                        let decoded = try await self.processingService.convert(
                            input: light.originalURL,
                            output: decodedOutput,
                            options: rawDecodeExportOptions,
                            rawOptions: self.parameters.rawProcessingOptions
                        )
                        try Task.checkCancellation()
                        log.append("Decoded RAW \(light.displayName):")
                        log.append(self.message(for: decoded))
                        stackInputs.append(decodedOutput)
                        advanceProgress("Decoded \(light.displayName)")
                    } else {
                        stackInputs.append(light.originalURL)
                        advanceProgress("Queued \(light.displayName)")
                    }
                }
            }

            let requestedAlignment = usedRegisteredSequence == nil ? self.parameters.workflowAlignment : .none
            if requestedAlignment != .none && stackInputs.count > 1 {
                let alignedDirectory = runDirectory.appendingPathComponent("aligned", isDirectory: true)
                try FileManager.default.createDirectory(at: alignedDirectory, withIntermediateDirectories: true)

                let referenceIndex = stackInputs.count / 2
                let referenceURL = stackInputs[referenceIndex]
                var alignedInputs = stackInputs
                log.append("Pre-aligning lights before stacking:")
                log.append("Reference frame: \(referenceURL.lastPathComponent), alignment: \(requestedAlignment.rawValue)")
                if requestedAlignment == .distortion {
                    log.append("Distortion alignment uses projective baseline matching followed by residual refinement passes.")
                }

                for index in stackInputs.indices {
                    try Task.checkCancellation()
                    let input = stackInputs[index]
                    let baseName = input.deletingPathExtension().lastPathComponent
                    if index == referenceIndex {
                        log.append("Kept reference aligned frame \(input.lastPathComponent):")
                        log.append("  output: \(input.path)")
                        continue
                    }

                    let output = alignedDirectory
                        .appendingPathComponent("\(String(format: "%02d", index + 1))-\(baseName)")
                        .appendingPathExtension("aligned.fits")

                    var passInput = input
                    for pass in 1...alignmentPasses {
                        try Task.checkCancellation()
                        let passOutput: URL
                        if pass == alignmentPasses {
                            passOutput = output
                        } else {
                            passOutput = alignedDirectory
                                .appendingPathComponent("\(String(format: "%02d", index + 1))-\(baseName).pass\(pass)")
                                .appendingPathExtension("aligned.fits")
                        }

                        let registerResult = try await self.processingService.register(
                            reference: referenceURL,
                            moving: passInput,
                            output: passOutput,
                            alignment: requestedAlignment
                        )
                        try Task.checkCancellation()
                        log.append("Aligned \(input.lastPathComponent):")
                        log.append(self.message(for: registerResult))
                        let metrics = try self.parseRegistrationMetrics(
                            registerResult,
                            mode: requestedAlignment,
                            output: passOutput
                        )
                        log.append(
                            "Alignment check: pass=\(pass), matches=\(metrics.matches), fallback=\(metrics.usedFallback)"
                        )
                        passInput = passOutput
                        advanceProgress("Aligned \(input.lastPathComponent) pass \(pass)")
                    }
                    alignedInputs[index] = output
                }

                stackInputs = alignedInputs
            }

            let stackOutput = runDirectory.appendingPathComponent("stack.fits")
            let stackResult = try await self.processingService.stack(
                inputs: stackInputs,
                output: stackOutput,
                method: self.parameters.workflowStackMethod,
                alignment: .none
            )
            try Task.checkCancellation()
            log.append("Stacked calibrated lights:")
            log.append(self.message(for: stackResult))
            advanceProgress("Stacked calibrated lights")

            var finalStackOutput = stackOutput
            if self.parameters.workflowRestoreMeteors {
                for (index, source) in stackInputs.enumerated() {
                    try Task.checkCancellation()
                    let output = runDirectory.appendingPathComponent("stack-meteors-\(index + 1).fits")
                    let restoreResult = try await self.processingService.restoreMeteors(
                        base: finalStackOutput,
                        source: source,
                        output: output
                    )
                    try Task.checkCancellation()
                    log.append("Restored meteors from \(lights[index].displayName):")
                    log.append(self.message(for: restoreResult))
                    finalStackOutput = output
                    advanceProgress("Restored meteors \(index + 1)/\(stackInputs.count)")
                }
            }

            let backgroundCorrectedStack = runDirectory.appendingPathComponent("stack-background.fits")
            let backgroundResult = try await self.processingService.background(
                input: finalStackOutput,
                output: backgroundCorrectedStack,
                model: "grid",
                mode: "subtract",
                strength: 0.55,
                preserveBrightness: true,
                protectBrightTargets: true
            )
            try Task.checkCancellation()
            log.append("Removed stacked-frame background gradient:")
            log.append(self.message(for: backgroundResult))
            finalStackOutput = backgroundCorrectedStack
            advanceProgress("Removed background gradient")

            let stretchedPreview = runDirectory.appendingPathComponent("stack-stretched.png")
            let stretchResult = try await self.processingService.autoStretch(
                input: finalStackOutput,
                output: stretchedPreview,
                targetBackground: self.parameters.stretchTargetBackground
            )
            try Task.checkCancellation()
            log.append("Auto-stretched stack preview:")
            log.append(self.message(for: stretchResult))
            advanceProgress("Auto-stretched stack preview")

            let neutralizedPreview = runDirectory.appendingPathComponent("stack-neutralized.png")
            let neutralizeResult = try await self.processingService.colorNeutralize(
                input: stretchedPreview,
                output: neutralizedPreview,
                strength: 0.35
            )
            try Task.checkCancellation()
            log.append("Neutralized background color:")
            log.append(self.message(for: neutralizeResult))
            advanceProgress("Neutralized color")

            let saturatedPreview = runDirectory.appendingPathComponent("stack-saturated.png")
            let saturateResult = try await self.processingService.colorSaturate(
                input: neutralizedPreview,
                output: saturatedPreview,
                amount: 0.18
            )
            try Task.checkCancellation()
            log.append("Balanced saturation:")
            log.append(self.message(for: saturateResult))
            advanceProgress("Balanced saturation")

            let denoisedPreview = runDirectory.appendingPathComponent("stack-denoised.png")
            let denoiseResult = try await self.processingService.denoise(
                input: saturatedPreview,
                output: denoisedPreview,
                amount: self.parameters.denoiseAmount,
                chromaAmount: self.parameters.denoiseChromaAmount,
                radius: self.parameters.denoiseRadius
            )
            try Task.checkCancellation()
            log.append("Reduced luminance/chroma noise:")
            log.append(self.message(for: denoiseResult))
            advanceProgress("Denoised preview")

            let contrastPreview = runDirectory.appendingPathComponent("stack-local-contrast.png")
            let contrastResult = try await self.processingService.localContrast(
                input: denoisedPreview,
                output: contrastPreview,
                amount: self.parameters.localContrastAmount,
                radius: self.parameters.localContrastRadius
            )
            try Task.checkCancellation()
            log.append("Enhanced local Milky Way contrast:")
            log.append(self.message(for: contrastResult))
            advanceProgress("Enhanced local contrast")

            let redBalancedPreview = runDirectory.appendingPathComponent("stack-red-balanced.png")
            let redCurveResult = try await self.processingService.curves(
                input: contrastPreview,
                output: redBalancedPreview,
                points: [(0.0, 0.0), (0.45, 0.47), (1.0, 1.0)],
                channel: .red
            )
            try Task.checkCancellation()
            log.append("Applied red channel color balance:")
            log.append(self.message(for: redCurveResult))
            advanceProgress("Balanced red channel")

            let greenBalancedPreview = runDirectory.appendingPathComponent("stack-green-balanced.png")
            let greenCurveResult = try await self.processingService.curves(
                input: redBalancedPreview,
                output: greenBalancedPreview,
                points: [(0.0, 0.0), (0.42, 0.37), (0.70, 0.66), (1.0, 1.0)],
                channel: .green
            )
            try Task.checkCancellation()
            log.append("Applied green channel color balance:")
            log.append(self.message(for: greenCurveResult))
            advanceProgress("Balanced green channel")

            let blueBalancedPreview = runDirectory.appendingPathComponent("stack-blue-balanced.png")
            let blueCurveResult = try await self.processingService.curves(
                input: greenBalancedPreview,
                output: blueBalancedPreview,
                points: [(0.0, 0.0), (0.45, 0.455), (1.0, 1.0)],
                channel: .blue
            )
            try Task.checkCancellation()
            log.append("Applied blue channel color balance:")
            log.append(self.message(for: blueCurveResult))
            advanceProgress("Balanced blue channel")

            let luminanceBalancedPreview = runDirectory.appendingPathComponent("stack-luminance-balanced.png")
            let luminanceCurveResult = try await self.processingService.curves(
                input: blueBalancedPreview,
                output: luminanceBalancedPreview,
                points: [(0.0, 0.0), (0.25, 0.18), (0.55, 0.55), (1.0, 1.0)],
                channel: .luminance
            )
            try Task.checkCancellation()
            log.append("Applied luminance contrast balance:")
            log.append(self.message(for: luminanceCurveResult))
            advanceProgress("Balanced luminance")

            let greenSuppressedPreview = runDirectory.appendingPathComponent("stack-green-suppressed.png")
            let greenSuppressResult = try await self.processingService.removeGreenCast(
                input: luminanceBalancedPreview,
                output: greenSuppressedPreview,
                amount: 0.70,
                backgroundLimit: 0.34
            )
            try Task.checkCancellation()
            log.append("Suppressed low-background green cast:")
            log.append(self.message(for: greenSuppressResult))
            advanceProgress("Suppressed green cast")

            let starReducedPreview = runDirectory.appendingPathComponent("stack-star-reduced.png")
            let starReductionResult = try await self.processingService.reduceStars(
                input: greenSuppressedPreview,
                output: starReducedPreview,
                amount: self.parameters.starReductionAmount,
                profileAware: self.parameters.profileAwareStarReduction,
                edgeAware: self.parameters.edgeAwareStarReduction
            )
            try Task.checkCancellation()
            log.append("Reduced star dominance:")
            log.append(self.message(for: starReductionResult))
            advanceProgress("Reduced star dominance")

            let comaReducedPreview = runDirectory.appendingPathComponent("stack-coma-rounded.png")
            let comaReductionResult = try await self.processingService.reduceComa(
                input: starReducedPreview,
                output: comaReducedPreview,
                amount: self.parameters.comaReductionAmount,
                radius: self.parameters.comaReductionRadius,
                eccentricity: self.parameters.comaReductionEccentricity,
                edgeAware: self.parameters.edgeAwareComaReduction
            )
            try Task.checkCancellation()
            log.append("Rounded short star trails:")
            log.append(self.message(for: comaReductionResult))
            advanceProgress("Rounded star trails")

            let finalPreview = runDirectory.appendingPathComponent("stack-final.png")
            let polishStarReductionAmount = min(0.55, self.parameters.starReductionAmount + 0.15)
            let polishStarReductionResult = try await self.processingService.reduceStars(
                input: comaReducedPreview,
                output: finalPreview,
                amount: polishStarReductionAmount,
                profileAware: self.parameters.profileAwareStarReduction,
                edgeAware: self.parameters.edgeAwareStarReduction
            )
            try Task.checkCancellation()
            log.append("Polished shortened star profiles:")
            log.append(self.message(for: polishStarReductionResult))
            advanceProgress("Polished star profiles")

            let framedPreview = runDirectory.appendingPathComponent("stack-final-framed.png")
            let cropResult = try await self.processingService.runCLI(arguments: [
                "crop",
                "--input", finalPreview.path,
                "--output", framedPreview.path,
                "--aspect", "16:9",
                "--margin", "0.20",
            ])
            try Task.checkCancellation()
            log.append("Applied composition crop:")
            log.append(self.message(for: cropResult))
            advanceProgress("Applied composition crop")

            try await self.commitPreviewAfterHistogramRefresh(framedPreview)
            advanceProgress("Updated histogram")
            var sourceArtifactIDs = usedRegisteredSequence.map { [$0.id] } ?? []
            let sourceAssets = lights + self.darkAssets + self.biasAssets + self.flatAssets
            for sourceID in self.sourceArtifactIDs(for: sourceAssets)
                where sourceArtifactIDs.contains(sourceID) == false {
                sourceArtifactIDs.append(sourceID)
            }
            let stackArtifact = ProcessingArtifact(
                kind: .stackMaster,
                name: "Stack Master",
                operationKind: .stack,
                sourceArtifactIDs: sourceArtifactIDs,
                outputDirectory: runDirectory,
                outputURLs: [stackOutput],
                parameters: [
                    "stackMethod": self.parameters.workflowStackMethod.rawValue,
                    "alignment": requestedAlignment.rawValue,
                    "inputs": String(stackInputs.count),
                    "calibrationLightIDs": lights.map(\.id.uuidString).joined(separator: ","),
                    "calibrationDarkIDs": self.darkAssets.map(\.id.uuidString).joined(separator: ","),
                    "calibrationBiasIDs": self.biasAssets.map(\.id.uuidString).joined(separator: ","),
                    "calibrationFlatIDs": self.flatAssets.map(\.id.uuidString).joined(separator: ","),
                    "darkBiasState": self.parameters.workflowDarkBiasState.rawValue,
                    "flatBiasState": self.parameters.workflowFlatBiasState.rawValue,
                ],
                metrics: ["frames": String(stackInputs.count)]
            )
            self.project.appendArtifact(stackArtifact)
            let finalArtifact = ProcessingArtifact(
                kind: .editedImage,
                name: "Edited Stack Result",
                operationKind: .stack,
                sourceArtifactIDs: [stackArtifact.id],
                outputDirectory: runDirectory,
                outputURLs: [framedPreview],
                parameters: [
                    "background": "grid subtract 0.55",
                    "stretch": String(self.parameters.stretchTargetBackground),
                    "starReduction": String(self.parameters.starReductionAmount),
                    "comaReduction": String(self.parameters.comaReductionAmount),
                ],
                metrics: ["preview": framedPreview.lastPathComponent]
            )
            self.project.appendArtifact(finalArtifact)
            self.project.appendLayer(
                ProcessingLayer(
                    name: "Edited Stack",
                    kind: .baseImage,
                    sourceArtifactID: finalArtifact.id,
                    inputURL: framedPreview,
                    parameters: ["source": "stackWorkflow"]
                )
            )
            var stackOperationParameters = [
                "masterMethod": masterMethod.rawValue,
                "stackMethod": self.parameters.workflowStackMethod.rawValue,
                "alignment": requestedAlignment.rawValue,
                "lights": String(lights.count),
                "inputs": String(stackInputs.count),
                "artifactID": stackArtifact.id.uuidString,
                "sourceArtifact": usedRegisteredSequence?.name ?? "lights",
                "calibrationApplied": String(hasCalibrationFrames),
                "calibrationLightIDs": lights.map(\.id.uuidString).joined(separator: ","),
                "calibrationDarkIDs": self.darkAssets.map(\.id.uuidString).joined(separator: ","),
                "calibrationBiasIDs": self.biasAssets.map(\.id.uuidString).joined(separator: ","),
                "calibrationFlatIDs": self.flatAssets.map(\.id.uuidString).joined(separator: ","),
                "darkBiasState": self.parameters.workflowDarkBiasState.rawValue,
                "flatBiasState": self.parameters.workflowFlatBiasState.rawValue,
            ]
            stackOperationParameters.merge(self.rawDecodeParameters()) { _, recorded in recorded }
            if let usedRegisteredSequence {
                stackOperationParameters["sourceArtifactID"] = usedRegisteredSequence.id.uuidString
            }
            for (index, input) in stackInputs.enumerated() {
                stackOperationParameters["inputPath.\(index)"] = input.path
            }
            if hasCalibrationFrames {
                var calibrationParameters = [
                    "masterMethod": masterMethod.rawValue,
                    "calibrationLightIDs": lights.map(\.id.uuidString).joined(separator: ","),
                    "calibrationDarkIDs": self.darkAssets.map(\.id.uuidString).joined(separator: ","),
                    "calibrationBiasIDs": self.biasAssets.map(\.id.uuidString).joined(separator: ","),
                    "calibrationFlatIDs": self.flatAssets.map(\.id.uuidString).joined(separator: ","),
                    "darkBiasState": self.parameters.workflowDarkBiasState.rawValue,
                    "flatBiasState": self.parameters.workflowFlatBiasState.rawValue,
                ]
                calibrationParameters.merge(self.rawDecodeParameters(from: calibrationRawOptions)) { _, recorded in recorded }
                self.recordOperation(
                    .calibrate,
                    parameters: calibrationParameters,
                    outputURL: nil
                )
            }
            self.recordOperation(.stack, parameters: stackOperationParameters, outputURL: stackOutput)
            if self.parameters.workflowRestoreMeteors {
                var meteorParameters = [
                    "mode": "restoreSequence",
                    "sources": String(stackInputs.count),
                    "sourceLightIDs": lights.map(\.id.uuidString).joined(separator: ","),
                ]
                for (index, source) in stackInputs.enumerated() {
                    meteorParameters["sourcePath.\(index)"] = source.path
                }
                self.recordOperation(.meteorRestore, parameters: meteorParameters, outputURL: finalStackOutput)
            }
            self.recordOperation(.background, parameters: ["model": "grid", "mode": "subtract", "strength": "0.55"], outputURL: backgroundCorrectedStack)
            self.recordOperation(.stretch, parameters: ["targetBackground": String(self.parameters.stretchTargetBackground)], outputURL: stretchedPreview)
            self.recordOperation(.colorNeutralize, parameters: ["strength": "0.35"], outputURL: neutralizedPreview)
            self.recordOperation(.colorSaturate, parameters: ["amount": "0.18"], outputURL: saturatedPreview)
            self.recordOperation(
                .denoise,
                parameters: [
                    "amount": String(self.parameters.denoiseAmount),
                    "chromaAmount": String(self.parameters.denoiseChromaAmount),
                    "radius": String(self.parameters.denoiseRadius),
                ],
                outputURL: denoisedPreview
            )
            self.recordOperation(
                .localContrast,
                parameters: [
                    "amount": String(self.parameters.localContrastAmount),
                    "radius": String(self.parameters.localContrastRadius),
                ],
                outputURL: contrastPreview
            )
            self.recordOperation(.curves, parameters: ["channel": "red", "points": "0:0,0.45:0.47,1:1"], outputURL: redBalancedPreview)
            self.recordOperation(.curves, parameters: ["channel": "green", "points": "0:0,0.42:0.37,0.70:0.66,1:1"], outputURL: greenBalancedPreview)
            self.recordOperation(.curves, parameters: ["channel": "blue", "points": "0:0,0.45:0.455,1:1"], outputURL: blueBalancedPreview)
            self.recordOperation(.curves, parameters: ["channel": "luminance", "points": "0:0,0.25:0.18,0.55:0.55,1:1"], outputURL: luminanceBalancedPreview)
            self.recordOperation(.colorNeutralize, parameters: ["mode": "removeGreen", "amount": "0.70", "backgroundLimit": "0.34"], outputURL: greenSuppressedPreview)
            self.recordOperation(
                .starReduce,
                parameters: [
                    "amount": String(self.parameters.starReductionAmount),
                    "profileAware": String(self.parameters.profileAwareStarReduction),
                    "edgeAware": String(self.parameters.edgeAwareStarReduction),
                ],
                outputURL: starReducedPreview
            )
            self.recordOperation(
                .comaReduce,
                parameters: [
                    "amount": String(self.parameters.comaReductionAmount),
                    "radius": String(self.parameters.comaReductionRadius),
                    "eccentricity": String(self.parameters.comaReductionEccentricity),
                    "edgeAware": String(self.parameters.edgeAwareComaReduction),
                ],
                outputURL: comaReducedPreview
            )
            self.recordOperation(
                .starReduce,
                parameters: [
                    "amount": String(min(0.55, self.parameters.starReductionAmount + 0.15)),
                    "profileAware": String(self.parameters.profileAwareStarReduction),
                    "edgeAware": String(self.parameters.edgeAwareStarReduction),
                    "mode": "postComaPolish",
                ],
                outputURL: finalPreview
            )
            self.recordOperation(
                .crop,
                parameters: ["aspect": "16:9", "margin": "0.20"],
                outputURL: framedPreview
            )

            return log.joined(separator: "\n\n")
        }
    }

    public func runMosaic() async {
        guard rejectMissingAsset(in: mosaicAssets) == false else {
            return
        }
        await runJob(title: "Mosaic", kind: .mosaic) {
            let panels = self.mosaicAssets
            guard panels.isEmpty == false else {
                throw MosaicWorkflowError.noPanels
            }

            let operationParameters = [
                "panels": String(panels.count),
                "panelIDs": panels.map(\.id.uuidString).joined(separator: ","),
                "panelSignature": self.inputSignature(panels.map(\.originalURL)),
                "overlapPixels": String(self.parameters.mosaicOverlapPixels),
                "projection": self.parameters.mosaicProjection.rawValue,
                "layout": self.parameters.mosaicLayout.rawValue,
                "alignment": self.parameters.mosaicAlignment.rawValue,
                "blend": self.parameters.mosaicBlendMode.rawValue,
                "exposureMatching": String(self.parameters.mosaicExposureMatching),
                "columns": String(self.parameters.mosaicColumns),
            ]

            let runDirectory = self.previewDirectory.appendingPathComponent("mosaic-\(UUID().uuidString)", isDirectory: true)
            try FileManager.default.createDirectory(at: runDirectory, withIntermediateDirectories: true)
            let mosaicOutput = runDirectory.appendingPathComponent("mosaic.tiff")
            let previewOutput = runDirectory.appendingPathComponent("mosaic-preview.png")
            let progressScope = ProcessingProgressContext.progressScope
            progressScope?.update(lowerBound: 0, upperBound: 1.0 / 3.0)
            defer {
                progressScope?.update(lowerBound: 0, upperBound: 1)
            }

            let mosaicResult = try await self.processingService.mosaic(
                inputs: panels.map(\.originalURL),
                output: mosaicOutput,
                overlapPixels: self.parameters.mosaicOverlapPixels,
                projection: self.parameters.mosaicProjection,
                layout: self.parameters.mosaicLayout,
                alignment: self.parameters.mosaicAlignment,
                blendMode: self.parameters.mosaicBlendMode,
                exposureMatching: self.parameters.mosaicExposureMatching,
                columns: self.parameters.mosaicColumns,
                previewWidth: nil
            )
            try Task.checkCancellation()
            let qualityReport = try self.parseMosaicQualityReport(
                mosaicResult,
                expectedPanelCount: panels.count
            )
            self.updateProgress(1.0 / 3.0, message: "Mosaic assembled")
            progressScope?.update(lowerBound: 1.0 / 3.0, upperBound: 2.0 / 3.0)
            let previewResult = try await self.processingService.makePreview(
                input: mosaicOutput,
                output: previewOutput,
                width: self.parameters.previewWidth,
                rawOptions: self.parameters.rawProcessingOptions
            )
            try Task.checkCancellation()
            self.updateProgress(2.0 / 3.0, message: "Mosaic preview")

            progressScope?.update(lowerBound: 2.0 / 3.0, upperBound: 1)
            try await self.commitPreviewAfterHistogramRefresh(previewOutput)
            self.mosaicQualityReport = qualityReport
            self.mosaicQualityReportSourceURL = previewOutput.standardizedFileURL
            self.updateProgress(1.0, message: "Mosaic histogram")
            var artifactMetrics = ["panels": String(panels.count)]
            artifactMetrics["matches"] = String(qualityReport.matches)
            artifactMetrics["fallbackPanels"] = String(qualityReport.fallbackPanels)
            artifactMetrics["autoAligned"] = String(qualityReport.autoAligned)
            artifactMetrics["exposureMatched"] = String(qualityReport.exposureMatched)
            artifactMetrics["blend"] = qualityReport.blend
            self.appendDerivedArtifactAndLayer(
                kind: .editedImage,
                name: "Mosaic",
                operationKind: .mosaic,
                sourceArtifactIDs: self.sourceArtifactIDs(for: panels),
                outputDirectory: runDirectory,
                outputURLs: [mosaicOutput],
                parameters: operationParameters,
                metrics: artifactMetrics,
                layerKind: .baseImage
            )
            self.recordOperation(
                .mosaic,
                parameters: operationParameters,
                outputURL: mosaicOutput,
                previewProxyURL: previewOutput
            )

            return [self.message(for: mosaicResult), self.message(for: previewResult)].joined(separator: "\n\n")
        }
    }

    public func runMosaicPreview() async {
        guard rejectMissingAsset(in: mosaicAssets) == false else {
            return
        }
        let plannedOutput = makeCachedMosaicPreviewOutput(for: mosaicAssets)
        await runJob(title: "Mosaic Preview", kind: .mosaic, outputURL: plannedOutput) {
            let panels = self.mosaicAssets
            guard panels.isEmpty == false else {
                throw MosaicWorkflowError.noPanels
            }
            let progressScope = ProcessingProgressContext.progressScope
            defer {
                progressScope?.update(lowerBound: 0, upperBound: 1)
            }

            let previewOutput = plannedOutput
            if self.mosaicQualityReport != nil,
               self.mosaicQualityReportSourceURL == previewOutput.standardizedFileURL,
               let cached = self.cachedResultIfAvailable(output: previewOutput, command: "mosaic preview") {
                progressScope?.update(lowerBound: 0, upperBound: 1)
                try await self.commitPreviewAfterHistogramRefresh(previewOutput)
                return self.message(for: cached)
            }

            progressScope?.update(lowerBound: 0, upperBound: 0.85)
            let mosaicResult = try await self.processingService.mosaic(
                inputs: panels.map(\.originalURL),
                output: previewOutput,
                overlapPixels: self.parameters.mosaicOverlapPixels,
                projection: self.parameters.mosaicProjection,
                layout: self.parameters.mosaicLayout,
                alignment: self.parameters.mosaicAlignment,
                blendMode: self.parameters.mosaicBlendMode,
                exposureMatching: self.parameters.mosaicExposureMatching,
                columns: self.parameters.mosaicColumns,
                previewWidth: self.parameters.mosaicPreviewWidth
            )
            try Task.checkCancellation()
            let qualityReport = try self.parseMosaicQualityReport(
                mosaicResult,
                expectedPanelCount: panels.count
            )
            self.updateProgress(0.85, message: "Mosaic preview assembled")
            progressScope?.update(lowerBound: 0.85, upperBound: 1)
            try await self.commitPreviewAfterHistogramRefresh(previewOutput)
            self.mosaicQualityReport = qualityReport
            self.mosaicQualityReportSourceURL = previewOutput.standardizedFileURL
            self.updateProgress(1.0, message: "Mosaic preview histogram")

            return self.message(for: mosaicResult)
        }
    }

    public func runConsoleCommand(_ text: String) async {
        do {
            let arguments = try parseConsoleCommand(text)
            guard arguments.isEmpty == false else {
                errorMessage = localized(.consoleInputEmpty)
                return
            }

            await runProcessingJob(title: "Console", kind: .inspect) {
                try await self.processingService.runCLI(arguments: arguments)
            }
        } catch ConsoleCommandError.empty {
            errorMessage = localized(.consoleInputEmpty)
        } catch ConsoleCommandError.unclosedQuote {
            errorMessage = localized(.consoleInputUnclosedQuote)
        } catch {
            errorMessage = error.localizedDescription
        }
    }

    public func resetPreviewToOriginal() {
        guard canModifyWorkspaceProducts,
              let selectedAsset
        else {
            return
        }
        guard rejectMissingAsset(in: [selectedAsset]) == false else {
            return
        }
        let originalURL = selectedAsset.originalURL
        appendPreviewHistoryState()
        previewURL = originalURL
        previewOperationID = nil
        editGraphNeedsReplay = false
        for index in project.layers.indices {
            project.layers[index].isVisible = false
        }
        if let sourceIndex = project.layers.lastIndex(where: {
            $0.kind == .baseImage &&
                $0.inputURL.map { Self.urlsReferenceSameFile($0, originalURL) } == true &&
                $0.maskArtifactID == nil &&
                $0.blendMode == .normal &&
                $0.opacity >= 0.999
        }) {
            project.layers[sourceIndex].isVisible = true
            activeLayerID = project.layers[sourceIndex].id
        } else {
            activeLayerID = nil
        }
        canvasMode = activeLayerID == nil ? .sourcePreview : .layerComposite
        project.updatedAt = Date()
        artifactTrailReport = nil
        cloudRegionReport = nil
        detectedStarCount = nil
        replaceMarkedPreview(nil)
        histogram = nil
        curveReferenceHistogram = nil
        canStepBack = previewHistory.isEmpty == false
        errorMessage = nil
        commandOutput = ""
        triggerAutosave()
    }

    public func previewArtifact(_ artifact: ProcessingArtifact) {
        guard canModifyWorkspaceProducts,
              project.artifacts.contains(where: { $0.id == artifact.id })
        else {
            return
        }
        guard let url = artifact.previewURL else {
            return
        }
        guard rejectUnavailableWorkspaceURL(url, name: artifact.name) == false else {
            return
        }
        activeLayerID = nil
        setPreview(url, canvasMode: .sourcePreview)
        triggerAutosave()
    }

    public func previewLayer(_ layer: ProcessingLayer) {
        selectLayer(layer)
    }

    public func selectLayer(_ layer: ProcessingLayer) {
        guard canModifyWorkspaceProducts,
              project.layers.contains(where: { $0.id == layer.id })
        else {
            return
        }
        guard let url = layer.inputURL else {
            return
        }
        guard rejectUnavailableWorkspaceURL(url, name: layer.name) == false else {
            return
        }
        if layer.kind == .mask {
            activeLayerID = layer.id
            setPreview(url, trackHistory: false, canvasMode: .sourcePreview)
            commandOutput = "\(localized(.activeLayerLabel)): \(layer.name)"
            triggerAutosave()
        } else {
            activeLayerID = layer.id
            canvasMode = .layerComposite
            _ = refreshVisibleLayerStackPreview()
            commandOutput = "\(localized(.activeLayerLabel)): \(layer.name)"
            triggerAutosave()
        }
    }

    public func createLayer(from asset: PhotonStackAsset) {
        guard canModifyWorkspaceProducts,
              project.assets.contains(where: { $0.id == asset.id }),
              rejectMissingAsset(in: [asset]) == false
        else {
            return
        }
        let layer = ProcessingLayer(
            name: asset.displayName,
            kind: .baseImage,
            inputURL: asset.originalURL,
            parameters: [
                "source": "asset",
                "assetID": asset.id.uuidString,
            ]
        )
        _ = performLayerCompositeMutation {
            appendPreviewHistoryState()
            project.appendLayer(layer)
            activeLayerID = layer.id
        }
    }

    public func createLayer(from artifact: ProcessingArtifact) {
        guard canModifyWorkspaceProducts,
              project.artifacts.contains(where: { $0.id == artifact.id }),
              let previewURL = artifact.previewURL
        else {
            return
        }
        guard rejectUnavailableWorkspaceURL(previewURL, name: artifact.name) == false else {
            return
        }
        guard canCreateLayer(from: artifact) else {
            return
        }
        let layer = ProcessingLayer(
            name: artifact.name,
            kind: layerKind(for: artifact.kind),
            sourceArtifactID: artifact.id,
            inputURL: previewURL,
            parameters: [
                "source": "artifact",
                "artifactKind": artifact.kind.rawValue,
            ]
        )
        if layer.kind == .mask {
            appendPreviewHistoryState()
            project.appendLayer(layer)
            activeLayerID = layer.id
            setPreview(previewURL, trackHistory: false, canvasMode: .sourcePreview)
            triggerAutosave()
        } else {
            _ = performLayerCompositeMutation {
                appendPreviewHistoryState()
                project.appendLayer(layer)
                activeLayerID = layer.id
            }
        }
    }

    public func deleteProcessingLayer(_ layerID: ProcessingLayer.ID) {
        guard canModifyWorkspaceProducts,
              let index = project.layers.firstIndex(where: { $0.id == layerID })
        else {
            return
        }

        _ = performLayerCompositeMutation {
            appendPreviewHistoryState()
            let removedLayer = project.layers[index]
            let survivingParentID = removedLayer.parameters["sourceLayerID"]
                .flatMap(UUID.init(uuidString:))
                .flatMap { parentID in
                    project.layers.contains(where: { $0.id == parentID && $0.id != layerID }) ? parentID : nil
                }
            project.layers.remove(at: index)

            for childIndex in project.layers.indices
            where project.layers[childIndex].parameters["sourceLayerID"] == layerID.uuidString {
                if let survivingParentID, survivingParentID != project.layers[childIndex].id {
                    project.layers[childIndex].parameters["sourceLayerID"] = survivingParentID.uuidString
                } else {
                    project.layers[childIndex].parameters.removeValue(forKey: "sourceLayerID")
                }
            }

            if activeLayerID == layerID {
                activeLayerID = preferredActiveLayerID(near: index)
            }
            project.updatedAt = Date()
        }
    }

    private func preferredActiveLayerID(near removedIndex: Int) -> ProcessingLayer.ID? {
        func nearestID(in candidates: [(offset: Int, element: ProcessingLayer)]) -> ProcessingLayer.ID? {
            candidates.min { lhs, rhs in
                let lhsDistance = abs(lhs.offset - removedIndex)
                let rhsDistance = abs(rhs.offset - removedIndex)
                if lhsDistance == rhsDistance {
                    return lhs.offset > rhs.offset
                }
                return lhsDistance < rhsDistance
            }?.element.id
        }

        let indexedLayers = Array(project.layers.enumerated())
        return nearestID(in: indexedLayers.filter {
            $0.element.kind != .mask && isLayerInputAvailable($0.element)
        }) ?? nearestID(in: indexedLayers.filter { isLayerInputAvailable($0.element) })
    }

    public func layerIsVisible(_ layerID: ProcessingLayer.ID) -> Bool {
        project.layers.first { $0.id == layerID }?.isVisible ?? false
    }

    public func canShowLayer(_ layerID: ProcessingLayer.ID) -> Bool {
        guard let layer = project.layers.first(where: { $0.id == layerID }),
              layer.kind != .mask,
              isLayerInputAvailable(layer),
              isLayerMaskInputAvailable(layer)
        else {
            return false
        }
        return editOperation(for: layer)?.isEnabled != false
    }

    public func layerOpacity(_ layerID: ProcessingLayer.ID) -> Double {
        project.layers.first { $0.id == layerID }?.opacity ?? 1.0
    }

    public func layerBlendMode(_ layerID: ProcessingLayer.ID) -> ProcessingLayerBlendMode {
        project.layers.first { $0.id == layerID }?.blendMode ?? .normal
    }

    public func layerMaskArtifact(_ layerID: ProcessingLayer.ID) -> ProcessingArtifact? {
        guard let artifactID = project.layers.first(where: { $0.id == layerID })?.maskArtifactID else {
            return nil
        }
        return project.artifacts.first { $0.id == artifactID && $0.kind == .mask }
    }

    public func layerMaskIsInverted(_ layerID: ProcessingLayer.ID) -> Bool {
        project.layers.first(where: { $0.id == layerID })?.parameters["maskInverted"] == "true"
    }

    public func layerMaskDensity(_ layerID: ProcessingLayer.ID) -> Double {
        let value = project.layers.first(where: { $0.id == layerID })?.parameters["maskDensity"]
            .flatMap(Double.init) ?? 1
        return Self.normalizedFinite(value, in: 0...1, fallback: 1)
    }

    public func layerMaskPixelSize(_ layerID: ProcessingLayer.ID) -> CGSize? {
        guard let url = layerMaskArtifact(layerID)?.previewURL,
              let size = imagePixelSize(at: url)
        else {
            return nil
        }
        return CGSize(width: size.width, height: size.height)
    }

    public func layerMaskFeatherRadius(_ layerID: ProcessingLayer.ID) -> Double {
        guard let layer = project.layers.first(where: { $0.id == layerID }),
              let size = layerMaskPixelSize(layerID)
        else {
            return 0
        }
        let fraction = Self.normalizedFinite(
            layer.parameters["maskFeatherFraction"].flatMap(Double.init) ?? 0,
            in: 0...0.1,
            fallback: 0
        )
        return fraction * min(size.width, size.height)
    }

    public func setLayerMaskInverted(_ layerID: ProcessingLayer.ID, inverted: Bool) {
        guard canModifyWorkspaceProducts,
              let layer = project.layers.first(where: { $0.id == layerID }),
              isLayerMaskInputAvailable(layer),
              layerMaskIsInverted(layerID) != inverted
        else {
            return
        }
        setLayerMaskParameter(
            layerID,
            key: "maskInverted",
            value: String(inverted),
            trackHistory: true
        )
    }

    public func beginLayerMaskAdjustment(_ layerID: ProcessingLayer.ID) {
        guard canModifyWorkspaceProducts,
              project.layers.contains(where: {
            $0.id == layerID && $0.kind != .mask && $0.maskArtifactID != nil && isLayerMaskInputAvailable($0)
        }) else {
            return
        }
        guard pendingLayerMaskAdjustment?.layerID != layerID else {
            return
        }
        pendingLayerOpacityAdjustment = nil
        pendingLayerMaskAdjustment = LayerAdjustmentSession(
            layerID: layerID,
            historyState: currentPreviewHistoryState()
        )
    }

    public func endLayerMaskAdjustment(_ layerID: ProcessingLayer.ID) {
        guard pendingLayerMaskAdjustment?.layerID == layerID else {
            return
        }
        pendingLayerMaskAdjustment = nil
    }

    public func setLayerMaskDensity(_ layerID: ProcessingLayer.ID, density: Double) {
        guard canModifyWorkspaceProducts else {
            return
        }
        let adjustment = pendingLayerMaskAdjustment.flatMap { $0.layerID == layerID ? $0 : nil }
        if setLayerMaskParameter(
            layerID,
            key: "maskDensity",
            value: String(Self.normalizedFinite(
                density,
                in: 0...1,
                fallback: layerMaskDensity(layerID)
            )),
            historyState: adjustment?.historyState
        ), adjustment != nil {
            pendingLayerMaskAdjustment = nil
        }
    }

    public func setLayerMaskFeatherRadius(_ layerID: ProcessingLayer.ID, radius: Double) {
        guard canModifyWorkspaceProducts,
              radius.isFinite,
              let size = layerMaskPixelSize(layerID)
        else {
            return
        }
        let minimumDimension = max(1, min(size.width, size.height))
        let fraction = min(max(radius / minimumDimension, 0), 0.1)
        let adjustment = pendingLayerMaskAdjustment.flatMap { $0.layerID == layerID ? $0 : nil }
        if setLayerMaskParameter(
            layerID,
            key: "maskFeatherFraction",
            value: String(fraction),
            historyState: adjustment?.historyState
        ), adjustment != nil {
            pendingLayerMaskAdjustment = nil
        }
    }

    @discardableResult
    public func applyMaskBrushEdits(_ strokes: [MaskBrushStroke], to layerID: ProcessingLayer.ID) -> Bool {
        let visibleStrokes = strokes.filter { $0.points.isEmpty == false && $0.opacity > 0 }
        guard canModifyWorkspaceProducts,
              visibleStrokes.isEmpty == false,
              let layerIndex = project.layers.firstIndex(where: { $0.id == layerID }),
              project.layers[layerIndex].kind != .mask,
              let sourceArtifact = layerMaskArtifact(layerID),
              isArtifactPreviewAvailable(sourceArtifact),
              let sourceURL = sourceArtifact.previewURL
        else {
            return false
        }

        do {
            try FileManager.default.createDirectory(at: previewDirectory, withIntermediateDirectories: true)
            let output = makePreviewOutput(prefix: "mask-brush", extension: "png")
            try renderMaskBrushStrokes(visibleStrokes, input: sourceURL, output: output)

            let allStrokes = try decodeMaskBrushStrokes(sourceArtifact.parameters) + visibleStrokes
            var parameters = sourceArtifact.parameters
            parameters["source"] = "manualBrush"
            parameters["sourceMaskID"] = sourceArtifact.id.uuidString
            parameters["brushStrokes"] = try encodeMaskBrushStrokes(allStrokes)
            let artifact = ProcessingArtifact(
                kind: .mask,
                name: "\(sourceArtifact.name) · Brush",
                operationKind: sourceArtifact.operationKind,
                sourceArtifactIDs: [sourceArtifact.id],
                outputURLs: [output],
                parameters: parameters,
                metrics: sourceArtifact.metrics
            )

            let projectSnapshot = project
            let activeLayerSnapshot = activeLayerID
            let previewHistoryCount = previewHistory.count
            let canStepBackSnapshot = canStepBack
            appendPreviewHistoryState()
            project.appendArtifact(artifact)
            project.layers[layerIndex].maskArtifactID = artifact.id
            project.updatedAt = Date()
            guard refreshVisibleLayerStackPreview() else {
                project = projectSnapshot
                activeLayerID = activeLayerSnapshot
                if previewHistory.count > previewHistoryCount {
                    previewHistory.removeLast(previewHistory.count - previewHistoryCount)
                }
                canStepBack = canStepBackSnapshot
                try? FileManager.default.removeItem(at: output)
                return false
            }
            commandOutput = "Mask brush: \(visibleStrokes.count) stroke(s)"
            errorMessage = nil
            triggerAutosave()
            return true
        } catch {
            errorMessage = error.localizedDescription
            return false
        }
    }

    public func setLayerMask(_ layerID: ProcessingLayer.ID, artifactID: ProcessingArtifact.ID?) {
        guard canModifyWorkspaceProducts else {
            return
        }
        applyLayerMask(layerID, artifactID: artifactID)
    }

    private func applyLayerMask(_ layerID: ProcessingLayer.ID, artifactID: ProcessingArtifact.ID?) {
        guard let index = project.layers.firstIndex(where: { $0.id == layerID }),
              project.layers[index].kind != .mask
        else {
            return
        }
        if let artifactID {
            guard let artifact = project.artifacts.first(where: {
                $0.id == artifactID && $0.kind == .mask
            }), isArtifactPreviewAvailable(artifact) else {
                return
            }
        }
        guard project.layers[index].maskArtifactID != artifactID else {
            return
        }

        _ = performLayerCompositeMutation {
            appendPreviewHistoryState()
            project.layers[index].maskArtifactID = artifactID
            project.layers[index].parameters.removeValue(forKey: "maskInverted")
            project.layers[index].parameters.removeValue(forKey: "maskDensity")
            project.layers[index].parameters.removeValue(forKey: "maskFeatherFraction")
            project.updatedAt = Date()
        }
    }

    @discardableResult
    private func setLayerMaskParameter(
        _ layerID: ProcessingLayer.ID,
        key: String,
        value: String,
        trackHistory: Bool = false,
        historyState: PreviewHistoryState? = nil
    ) -> Bool {
        guard let index = project.layers.firstIndex(where: { $0.id == layerID }),
              project.layers[index].kind != .mask,
              project.layers[index].maskArtifactID != nil,
              isLayerMaskInputAvailable(project.layers[index]),
              project.layers[index].parameters[key] != value
        else {
            return false
        }

        return performLayerCompositeMutation {
            if let historyState {
                appendPreviewHistoryState(historyState)
            } else if trackHistory {
                appendPreviewHistoryState()
            }
            project.layers[index].parameters[key] = value
            project.updatedAt = Date()
        }
    }

    private func ensureImageLayer(for input: URL) -> ProcessingLayer.ID {
        let inputIdentity = Self.workspaceFileIdentityPath(for: input)
        let matchingLayers = project.layers.filter {
            $0.kind != .mask && $0.inputURL.map {
                Self.workspaceFileIdentityPath(for: $0) == inputIdentity
            } == true
        }
        if matchingLayers.count == 1, let existingLayer = matchingLayers.first {
            activeLayerID = existingLayer.id
            return existingLayer.id
        }

        let sourceAsset = project.assets.first {
            Self.workspaceFileIdentityPath(for: $0.originalURL) == inputIdentity
        }
        let layer = ProcessingLayer(
            name: sourceAsset?.displayName ?? input.lastPathComponent,
            kind: .baseImage,
            inputURL: input,
            parameters: [
                "source": "automatic",
                "assetID": sourceAsset?.id.uuidString ?? "",
            ]
        )
        project.appendLayer(layer)
        activeLayerID = layer.id
        return layer.id
    }

    public func setLayerVisibility(_ layerID: ProcessingLayer.ID, isVisible: Bool) {
        guard canModifyWorkspaceProducts,
              let index = project.layers.firstIndex(where: { $0.id == layerID }),
              project.layers[index].kind != .mask
        else {
            return
        }

        if isVisible,
           let operation = editOperation(for: project.layers[index]),
           operation.isEnabled == false {
            errorMessage = localized(.disabledOperationLayer)
            return
        }

        if isVisible,
           (isLayerInputAvailable(project.layers[index]) == false ||
               isLayerMaskInputAvailable(project.layers[index]) == false) {
            errorMessage = "\(localized(.missingWorkspaceSourceError)): \(project.layers[index].name)"
            return
        }

        guard project.layers[index].isVisible != isVisible else {
            return
        }

        _ = performLayerCompositeMutation {
            appendPreviewHistoryState()
            project.layers[index].isVisible = isVisible
            project.updatedAt = Date()
        }
    }

    public func beginLayerOpacityAdjustment(_ layerID: ProcessingLayer.ID) {
        guard canModifyWorkspaceProducts,
              project.layers.contains(where: {
            $0.id == layerID &&
                $0.kind != .mask &&
                isLayerInputAvailable($0) &&
                isLayerMaskInputAvailable($0)
        }) else {
            return
        }
        guard pendingLayerOpacityAdjustment?.layerID != layerID else {
            return
        }
        pendingLayerMaskAdjustment = nil
        pendingLayerOpacityAdjustment = LayerAdjustmentSession(
            layerID: layerID,
            historyState: currentPreviewHistoryState()
        )
    }

    public func endLayerOpacityAdjustment(_ layerID: ProcessingLayer.ID) {
        guard pendingLayerOpacityAdjustment?.layerID == layerID else {
            return
        }
        pendingLayerOpacityAdjustment = nil
    }

    public func setLayerOpacity(_ layerID: ProcessingLayer.ID, opacity: Double) {
        guard canModifyWorkspaceProducts,
              let index = project.layers.firstIndex(where: { $0.id == layerID }),
              project.layers[index].kind != .mask,
              isLayerInputAvailable(project.layers[index]),
              isLayerMaskInputAvailable(project.layers[index])
        else {
            return
        }

        let clampedOpacity = Self.normalizedFinite(
            opacity,
            in: 0...1,
            fallback: project.layers[index].opacity
        )
        guard project.layers[index].opacity != clampedOpacity else {
            return
        }

        let adjustment = pendingLayerOpacityAdjustment.flatMap { $0.layerID == layerID ? $0 : nil }
        let succeeded = performLayerCompositeMutation {
            if let adjustment {
                appendPreviewHistoryState(adjustment.historyState)
            }
            project.layers[index].opacity = clampedOpacity
            project.updatedAt = Date()
        }
        if succeeded, adjustment != nil {
            pendingLayerOpacityAdjustment = nil
        }
    }

    public func setLayerBlendMode(_ layerID: ProcessingLayer.ID, blendMode: ProcessingLayerBlendMode) {
        guard canModifyWorkspaceProducts,
              let index = project.layers.firstIndex(where: { $0.id == layerID }),
              project.layers[index].kind != .mask,
              isLayerInputAvailable(project.layers[index]),
              isLayerMaskInputAvailable(project.layers[index])
        else {
            return
        }

        guard project.layers[index].blendMode != blendMode else {
            return
        }

        _ = performLayerCompositeMutation {
            appendPreviewHistoryState()
            project.layers[index].blendMode = blendMode
            project.updatedAt = Date()
        }
    }

    public func canMoveProcessingLayer(_ layerID: ProcessingLayer.ID, direction: EditOperationMoveDirection) -> Bool {
        canModifyWorkspaceProducts && processingLayerMoveTargetIndex(layerID, direction: direction) != nil
    }

    public func moveProcessingLayer(_ layerID: ProcessingLayer.ID, direction: EditOperationMoveDirection) {
        guard canModifyWorkspaceProducts,
              let index = project.layers.firstIndex(where: { $0.id == layerID }),
              let targetIndex = processingLayerMoveTargetIndex(layerID, direction: direction)
        else {
            return
        }

        _ = performLayerCompositeMutation {
            appendPreviewHistoryState()
            project.layers.swapAt(index, targetIndex)
            project.updatedAt = Date()
        }
    }

    private func processingLayerMoveTargetIndex(
        _ layerID: ProcessingLayer.ID,
        direction: EditOperationMoveDirection
    ) -> Int? {
        guard let index = project.layers.firstIndex(where: { $0.id == layerID }),
              project.layers[index].kind != .mask
        else {
            return nil
        }

        switch direction {
        case .up:
            return project.layers.indices.dropFirst(index + 1).first(where: {
                project.layers[$0].kind != .mask
            })
        case .down:
            return project.layers.indices.prefix(index).reversed().first(where: {
                project.layers[$0].kind != .mask
            })
        }
    }

    private func layerKind(for artifactKind: ProcessingArtifactKind) -> ProcessingLayerKind {
        switch artifactKind {
        case .mask:
            return .mask
        case .meteorLayer:
            return .meteors
        case .starLayer:
            return .stars
        case .editedImage:
            return .adjustment
        case .rawSequence, .decodedSequence, .calibratedSequence, .registeredSequence, .cleanedTimelapseSequence, .stackMaster:
            return .baseImage
        }
    }

    private func editOperationBranch(containing operationID: EditOperation.ID) -> EditOperationBranch? {
        let operations = project.editGraph.operations
        guard let startIndex = operations.firstIndex(where: { $0.id == operationID }) else {
            return nil
        }

        var component: Set<Int> = [startIndex]
        var pending = [startIndex]
        while let index = pending.popLast() {
            let operation = operations[index]
            for candidateIndex in operations.indices where component.contains(candidateIndex) == false {
                let candidate = operations[candidateIndex]
                let operationInput = operation.parameters["input"]
                let operationOutput = operation.parameters["output"]
                let candidateInput = candidate.parameters["input"]
                let candidateOutput = candidate.parameters["output"]
                let isConnected = Self.storedPathsReferenceSameFile(operationInput, candidateOutput) ||
                    Self.storedPathsReferenceSameFile(operationOutput, candidateInput)
                if isConnected {
                    component.insert(candidateIndex)
                    pending.append(candidateIndex)
                }
            }
        }

        let roots = component.filter { index in
            guard let inputPath = operations[index].parameters["input"] else {
                return true
            }
            return component.contains { candidateIndex in
                Self.storedPath(
                    operations[candidateIndex].parameters["output"],
                    references: URL(fileURLWithPath: inputPath)
                )
            } == false
        }
        guard roots.count == 1, let rootIndex = roots.first else {
            return nil
        }

        var orderedIndices: [Int] = []
        var visited: Set<Int> = []
        var cursor: Int? = rootIndex
        while let index = cursor {
            guard visited.insert(index).inserted else {
                return nil
            }
            orderedIndices.append(index)
            guard let outputPath = operations[index].parameters["output"] else {
                cursor = nil
                continue
            }
            let successors = component.filter { candidateIndex in
                Self.storedPath(
                    operations[candidateIndex].parameters["input"],
                    references: URL(fileURLWithPath: outputPath)
                )
            }
            guard successors.count <= 1 else {
                return nil
            }
            cursor = successors.first
        }

        guard visited == component else {
            return nil
        }
        return EditOperationBranch(
            operationIDs: orderedIndices.map { operations[$0].id },
            rootInputPath: operations[rootIndex].parameters["input"]
        )
    }

    private func rewiredEditOperations(
        operationIDs: [EditOperation.ID],
        rootInputPath: String?
    ) -> [EditOperation]? {
        var result: [EditOperation] = []
        var inputPath = rootInputPath
        for (index, operationID) in operationIDs.enumerated() {
            guard var operation = project.editGraph.operations.first(where: { $0.id == operationID }) else {
                return nil
            }
            if let inputPath {
                operation.parameters["input"] = inputPath
            } else {
                operation.parameters.removeValue(forKey: "input")
            }
            result.append(operation)
            inputPath = operation.parameters["output"]
            if index < operationIDs.index(before: operationIDs.endIndex), inputPath == nil {
                return nil
            }
        }
        return result
    }

    public func stepBackPreview() {
        guard canModifyWorkspaceProducts else {
            return
        }
        _ = restorePreviousPreviewState(command: localized(.restoredPreview))
    }

    public func toggleEditOperation(_ operationID: EditOperation.ID) {
        guard canModifyWorkspaceProducts,
              let index = project.editGraph.operations.firstIndex(where: { $0.id == operationID })
        else {
            return
        }

        let affectedBranch = editOperationBranch(containing: operationID)?.operationIDs ?? [operationID]
        let affectsCurrentWorkspace = editGraphBranchAffectsCurrentWorkspace(affectedBranch)
        recordEditGraphUndoSnapshot()
        project.editGraph.operations[index].isEnabled.toggle()
        synchronizeOperationLayerVisibility(
            operationID: operationID,
            isEnabled: project.editGraph.operations[index].isEnabled
        )
        if affectsCurrentWorkspace {
            if previewOperationID.map(affectedBranch.contains) != true {
                previewOperationID = affectedBranch.last
            }
            editGraphNeedsReplay = true
        }
        project.updatedAt = Date()
        triggerAutosave()
    }

    public func canMoveEditOperation(_ operationID: EditOperation.ID, direction: EditOperationMoveDirection) -> Bool {
        guard canModifyWorkspaceProducts,
              let branch = editOperationBranch(containing: operationID),
              let index = branch.operationIDs.firstIndex(of: operationID)
        else {
            return false
        }

        switch direction {
        case .up:
            return index > branch.operationIDs.startIndex
        case .down:
            return index < branch.operationIDs.index(before: branch.operationIDs.endIndex)
        }
    }

    public func moveEditOperation(_ operationID: EditOperation.ID, direction: EditOperationMoveDirection) {
        guard canModifyWorkspaceProducts,
              let branch = editOperationBranch(containing: operationID),
              let index = branch.operationIDs.firstIndex(of: operationID)
        else {
            return
        }

        let targetIndex: Int
        switch direction {
        case .up:
            targetIndex = index - 1
        case .down:
            targetIndex = index + 1
        }

        guard branch.operationIDs.indices.contains(targetIndex) else {
            return
        }

        var reorderedIDs = branch.operationIDs
        reorderedIDs.swapAt(index, targetIndex)
        guard let reordered = rewiredEditOperations(
            operationIDs: reorderedIDs,
            rootInputPath: branch.rootInputPath
        ) else {
            return
        }

        let branchPositions = branch.operationIDs.compactMap { operationID in
            project.editGraph.operations.firstIndex(where: { $0.id == operationID })
        }.sorted()
        guard branchPositions.count == reordered.count else {
            return
        }
        let affectsCurrentWorkspace = editGraphBranchAffectsCurrentWorkspace(branch.operationIDs)
        recordEditGraphUndoSnapshot()
        for (position, operation) in zip(branchPositions, reordered) {
            project.editGraph.operations[position] = operation
        }
        reorderOperationLinkedLayers(
            originalOperationIDs: branch.operationIDs,
            reorderedOperationIDs: reorderedIDs
        )
        if affectsCurrentWorkspace {
            previewOperationID = reordered.last?.id
            editGraphNeedsReplay = true
        }
        project.updatedAt = Date()
        triggerAutosave()
    }

    public func deleteEditOperation(_ operationID: EditOperation.ID) {
        guard canModifyWorkspaceProducts,
              let branch = editOperationBranch(containing: operationID)
        else {
            return
        }

        let remainingIDs = branch.operationIDs.filter { $0 != operationID }
        let affectsCurrentWorkspace = editGraphBranchAffectsCurrentWorkspace(branch.operationIDs)
        let rewired: [EditOperation]
        if remainingIDs.isEmpty {
            rewired = []
        } else {
            guard let remainingOperations = rewiredEditOperations(
                operationIDs: remainingIDs,
                rootInputPath: branch.rootInputPath
            ) else {
                return
            }
            rewired = remainingOperations
        }

        recordEditGraphUndoSnapshot()
        for operation in rewired {
            guard let index = project.editGraph.operations.firstIndex(where: { $0.id == operation.id }) else {
                continue
            }
            project.editGraph.operations[index] = operation
        }
        project.editGraph.operations.removeAll { $0.id == operationID }
        removeLayersLinked(toOperationID: operationID)
        if let currentPreviewOperationID = previewOperationID,
           branch.operationIDs.contains(currentPreviewOperationID) {
            previewOperationID = rewired.last?.id
            if rewired.isEmpty {
                previewURL = branch.rootInputPath.map(URL.init(fileURLWithPath:)) ?? selectedAsset?.originalURL
                editGraphNeedsReplay = false
                canvasMode = .sourcePreview
                histogram = nil
                curveReferenceHistogram = nil
            } else {
                editGraphNeedsReplay = true
            }
        } else if affectsCurrentWorkspace {
            previewOperationID = rewired.last?.id
            if rewired.isEmpty {
                previewURL = branch.rootInputPath.map(URL.init(fileURLWithPath:)) ?? selectedAsset?.originalURL
                editGraphNeedsReplay = false
                canvasMode = .sourcePreview
                histogram = nil
                curveReferenceHistogram = nil
            } else {
                editGraphNeedsReplay = true
            }
        }
        project.updatedAt = Date()
        triggerAutosave()
    }

    private func removeLayersLinked(toOperationID operationID: EditOperation.ID) {
        let removedLayerIDs = Set(project.layers.compactMap { layer -> ProcessingLayer.ID? in
            layer.parameters["operationID"] == operationID.uuidString ? layer.id : nil
        })
        guard removedLayerIDs.isEmpty == false else {
            return
        }

        let originalLayers = project.layers
        let parentByLayerID = Dictionary(uniqueKeysWithValues: originalLayers.map { layer in
            (
                layer.id,
                layer.parameters["sourceLayerID"].flatMap(UUID.init(uuidString:))
            )
        })
        let firstRemovedIndex = originalLayers.indices.first(where: {
            removedLayerIDs.contains(originalLayers[$0].id)
        }) ?? originalLayers.endIndex
        project.layers.removeAll { removedLayerIDs.contains($0.id) }

        for index in project.layers.indices {
            guard var sourceLayerID = project.layers[index].parameters["sourceLayerID"]
                .flatMap(UUID.init(uuidString:)),
                removedLayerIDs.contains(sourceLayerID)
            else {
                continue
            }

            var visited: Set<ProcessingLayer.ID> = []
            while removedLayerIDs.contains(sourceLayerID), visited.insert(sourceLayerID).inserted {
                guard let parent = parentByLayerID[sourceLayerID] ?? nil else {
                    break
                }
                sourceLayerID = parent
            }
            if removedLayerIDs.contains(sourceLayerID) == false,
               sourceLayerID != project.layers[index].id,
               project.layers.contains(where: { $0.id == sourceLayerID }) {
                project.layers[index].parameters["sourceLayerID"] = sourceLayerID.uuidString
            } else {
                project.layers[index].parameters.removeValue(forKey: "sourceLayerID")
            }
        }

        if activeLayerID.map(removedLayerIDs.contains) == true {
            activeLayerID = preferredActiveLayerID(near: firstRemovedIndex)
        }
    }

    private func editGraphBranchAffectsCurrentWorkspace(
        _ operationIDs: [EditOperation.ID]
    ) -> Bool {
        let operationIDs = Set(operationIDs)
        if previewOperationID.map(operationIDs.contains) == true {
            return true
        }
        guard canvasMode == .layerComposite else {
            return false
        }
        return project.layers.contains { layer in
            layer.parameters["operationID"]
                .flatMap(UUID.init(uuidString:))
                .map(operationIDs.contains) == true
        }
    }

    @discardableResult
    private func synchronizeOperationLayerVisibilityWithEditGraph() -> Bool {
        var changed = false
        for operation in project.editGraph.operations {
            changed = synchronizeOperationLayerVisibility(
                operationID: operation.id,
                isEnabled: operation.isEnabled
            ) || changed
        }
        return changed
    }

    @discardableResult
    private func synchronizeOperationLayerVisibility(
        operationID: EditOperation.ID,
        isEnabled: Bool
    ) -> Bool {
        let storedVisibilityKey = "visibilityBeforeOperationDisable"
        var changed = false
        for index in project.layers.indices where
            project.layers[index].parameters["operationID"] == operationID.uuidString {
            if isEnabled {
                if let storedVisibility = project.layers[index].parameters.removeValue(
                    forKey: storedVisibilityKey
                ) {
                    let restoredVisibility = storedVisibility == "true"
                    changed = true
                    project.layers[index].isVisible = restoredVisibility
                }
            } else {
                if project.layers[index].parameters[storedVisibilityKey] == nil {
                    project.layers[index].parameters[storedVisibilityKey] = String(
                        project.layers[index].isVisible
                    )
                    changed = true
                }
                if project.layers[index].isVisible {
                    changed = true
                }
                project.layers[index].isVisible = false
            }
        }
        return changed
    }

    private func reorderOperationLinkedLayers(
        originalOperationIDs: [EditOperation.ID],
        reorderedOperationIDs: [EditOperation.ID]
    ) {
        let operationIDs = Set(originalOperationIDs)
        let linkedIndices = project.layers.indices.filter { index in
            project.layers[index].parameters["operationID"]
                .flatMap(UUID.init(uuidString:))
                .map(operationIDs.contains) == true
        }
        guard linkedIndices.count > 1 else {
            return
        }

        let originalLinkedLayers = linkedIndices.map { project.layers[$0] }
        let linkedLayerIDs = Set(originalLinkedLayers.map(\.id))
        var groups: [EditOperation.ID: [ProcessingLayer]] = [:]
        for layer in originalLinkedLayers {
            guard let operationID = layer.parameters["operationID"].flatMap(UUID.init(uuidString:)) else {
                continue
            }
            groups[operationID, default: []].append(layer)
        }
        var reorderedLayers = reorderedOperationIDs.flatMap { groups[$0] ?? [] }
        guard reorderedLayers.count == linkedIndices.count else {
            return
        }

        var rootSourceLayerID = originalLinkedLayers.first?.parameters["sourceLayerID"]
            .flatMap(UUID.init(uuidString:))
        var visited: Set<ProcessingLayer.ID> = []
        while let sourceLayerID = rootSourceLayerID,
              linkedLayerIDs.contains(sourceLayerID),
              visited.insert(sourceLayerID).inserted {
            rootSourceLayerID = originalLinkedLayers.first(where: { $0.id == sourceLayerID })?
                .parameters["sourceLayerID"]
                .flatMap(UUID.init(uuidString:))
        }
        if let sourceLayerID = rootSourceLayerID,
           project.layers.contains(where: { $0.id == sourceLayerID }) == false {
            rootSourceLayerID = nil
        }

        for index in reorderedLayers.indices {
            let sourceLayerID = index == reorderedLayers.startIndex
                ? rootSourceLayerID
                : reorderedLayers[reorderedLayers.index(before: index)].id
            if let sourceLayerID, sourceLayerID != reorderedLayers[index].id {
                reorderedLayers[index].parameters["sourceLayerID"] = sourceLayerID.uuidString
            } else {
                reorderedLayers[index].parameters.removeValue(forKey: "sourceLayerID")
            }
        }
        for (index, layer) in zip(linkedIndices, reorderedLayers) {
            project.layers[index] = layer
        }
    }

    func editableEditOperationParameters(
        for operation: EditOperation
    ) -> [EditableEditOperationParameter] {
        let rules = Self.editOperationParameterRules(for: operation)
        return operation.parameters.compactMap { key, value in
            guard let rule = rules[key] else {
                return nil
            }
            return EditableEditOperationParameter(key: key, value: value, editor: rule.editor)
        }
        .sorted { $0.key < $1.key }
    }

    @discardableResult
    public func updateEditOperationParameter(
        _ operationID: EditOperation.ID,
        key: String,
        value: String
    ) -> Bool {
        guard canModifyWorkspaceProducts,
              let index = project.editGraph.operations.firstIndex(where: { $0.id == operationID })
        else {
            return false
        }

        let operation = project.editGraph.operations[index]
        guard let rule = Self.editOperationParameterRules(for: operation)[key],
              let normalizedValue = rule.normalizedValue(value)
        else {
            showEditOperationParameterValidationError(key: key)
            return false
        }

        var candidateParameters = operation.parameters
        candidateParameters[key] = normalizedValue
        guard Self.editOperationParametersAreValid(candidateParameters, for: operation.kind) else {
            showEditOperationParameterValidationError(key: key)
            return false
        }

        if errorMessage == editParameterValidationErrorMessage {
            errorMessage = nil
        }
        editParameterValidationErrorMessage = nil

        guard operation.parameters[key] != normalizedValue else {
            return true
        }

        let affectedBranch = editOperationBranch(containing: operationID)?.operationIDs ?? [operationID]
        let affectsCurrentWorkspace = editGraphBranchAffectsCurrentWorkspace(affectedBranch)
        recordEditGraphUndoSnapshot()
        project.editGraph.operations[index].parameters[key] = normalizedValue
        if affectsCurrentWorkspace {
            if previewOperationID.map(affectedBranch.contains) != true {
                previewOperationID = affectedBranch.last
            }
            editGraphNeedsReplay = true
        }
        project.updatedAt = Date()
        triggerAutosave()
        return true
    }

    private func showEditOperationParameterValidationError(key: String) {
        let message = "\(localized(.invalidEditOperationParameter)): \(key)"
        editParameterValidationErrorMessage = message
        errorMessage = message
    }

    private static func editOperationParameterRules(
        for operation: EditOperation
    ) -> [String: EditOperationParameterRule] {
        let alignment = EditOperationParameterRule.options(AlignmentMethod.allCases.map(\.rawValue))
        let stackMethod = EditOperationParameterRule.options(StackMethod.allCases.map(\.rawValue))
        let masterMethod = EditOperationParameterRule.options([
            StackMethod.average.rawValue,
            StackMethod.median.rawValue,
        ])
        let calibrationBiasState = EditOperationParameterRule.options(CalibrationBiasState.allCases.map(\.rawValue))
        let rawRules: [String: EditOperationParameterRule] = [
            "rawWhiteBalance": .options(RawWhiteBalanceMode.allCases.map(\.rawValue)),
            "rawExposureBias": .decimal(-2...2),
            "rawBlackLevel": .options(RawBlackLevelMode.allCases.map(\.rawValue)),
            "rawDemosaic": .options(RawDemosaicQuality.allCases.map(\.rawValue)),
            "rawLinear": .toggle,
        ]

        switch operation.kind {
        case .rawDecode:
            return rawRules.merging(["width": .integer(320...4096)]) { current, _ in current }
        case .preview:
            return ["width": .integer(320...4096)]
        case .crop:
            return [
                "aspect": .aspectRatio,
                "margin": .decimal(0...0.45),
            ]
        case .calibrate:
            return [
                "masterMethod": masterMethod,
                "darkBiasState": calibrationBiasState,
                "flatBiasState": calibrationBiasState,
            ]
        case .register:
            return ["alignment": alignment]
        case .stack:
            return rawRules.merging([
                "masterMethod": masterMethod,
                "stackMethod": stackMethod,
                "method": stackMethod,
                "alignment": alignment,
            ]) { current, _ in current }
        case .drizzle:
            return [
                "scale": .integer(1...4),
                "pixfrac": .decimal(0.1...1),
                "alignment": alignment,
            ]
        case .cloudRemove:
            return ["strength": .decimal(0...1)]
        case .background:
            return [
                "model": .options(["global", "grid"]),
                "mode": .options(["subtract", "divide"]),
                "strength": .decimal(0...1),
                "preserveBrightness": .toggle,
                "protectBrightTargets": .toggle,
            ]
        case .normalize:
            return [
                "targetBackground": .decimal(0.02...0.6),
                "targetScale": .decimal(0.5...2),
            ]
        case .stretch:
            if operation.parameters["mode"] == "astro-v1" {
                return ["mode": .options(["astro-v1"]), "stellarBalance": .toggle,
                        "toneCurve": .options(["rational", "asinh"]), "toneScale": .decimal(0...1_000_000_000), "whitePoint": .decimal(0...1_000_000_000),
                        "brightness": .decimal(0.3...4), "background": .decimal(0.01...0.12), "starExposure": .decimal(0.1...1), "starPeakThreshold": .decimal(0...1),
                        "saturation": .decimal(0...1.5), "redGain": .decimal(0.6...1.4), "blueGain": .decimal(0.6...1.4)]
            }
            return ["targetBackground": .decimal(0.05...0.65)]
        case .curves:
            return [
                "black": .decimal(0...1),
                "midInput": .decimal(0...1),
                "midOutput": .decimal(0...1),
                "white": .decimal(0...1),
                "channel": .options(CurveChannel.allCases.map(\.rawValue)),
                "points": .curvePoints,
            ]
        case .localContrast:
            if operation.parameters["mode"] == "continuum-v1" {
                return ["amount": .decimal(0...2), "fineRadius": .integer(1...63), "radius": .integer(2...64),
                        "starChroma": .decimal(0...1), "points": .curvePoints]
            }
            return [
                "amount": .decimal(0...1),
                "radius": .integer(1...64),
            ]
        case .denoise:
            if operation.parameters["mode"] == "display-grid-v1" {
                return ["amount": .decimal(0...1)]
            }
            return [
                "amount": .decimal(0...1),
                "chromaAmount": .decimal(0...1),
                "radius": .integer(1...8),
            ]
        case .sharpen:
            return [
                "amount": .decimal(0...1),
                "radius": .integer(1...8),
            ]
        case .deconvolve:
            return [
                "iterations": .integer(1...32),
                "radius": .integer(1...8),
                "sigma": .decimal(0.5...3.5),
            ]
        case .colorNeutralize:
            if operation.parameters["mode"] == "removeGreen" {
                if operation.parameters["greenMethod"] != nil {
                    return ["amount": .decimal(0...1), "backgroundLimit": .decimal(0...1),
                            "greenMethod": .options(GreenCastSettings.Method.allCases.map(\.rawValue)),
                            "greenThreshold": .decimal(0...1), "preserveLightness": .toggle]
                }
                return [
                    "amount": .decimal(0...1),
                    "backgroundLimit": .decimal(0...1),
                ]
            }
            return ["strength": .decimal(0...1)]
        case .colorSaturate:
            return ["amount": .decimal(-0.5...1.5)]
        case .starDetect:
            return [
                "sigmaThreshold": .decimal(0.1...20),
                "minPeak": .decimal(0...1),
                "maxStars": .integer(1...100_000),
            ]
        case .starMask:
            return [
                "radius": .integer(1...32),
                "largeRadius": .integer(1...64),
                "layered": .toggle,
                "sigmaThreshold": .decimal(0.1...20),
                "minPeak": .decimal(0...1),
                "maxStars": .integer(1...100_000),
            ]
        case .starReduce:
            return [
                "amount": .decimal(0...1),
                "profileAware": .toggle,
                "edgeAware": .toggle,
            ]
        case .comaReduce:
            return [
                "amount": .decimal(0...1),
                "radius": .integer(2...16),
                "eccentricity": .decimal(0.1...0.9),
                "edgeAware": .toggle,
            ]
        case .artifactRemove:
            if operation.parameters["mode"] == "cleanSequence" {
                return [
                    "outputFormat": .options(["png", "tiff"]),
                    "minWeight": .decimal(0...1),
                    "recurrenceThreshold": .integer(1...64),
                ]
            }
            return [
                "removeAirplanes": .toggle,
                "removeDrones": .toggle,
                "removeSatellites": .toggle,
                "removeMeteors": .toggle,
            ]
        case .mosaic:
            return [
                "overlapPixels": .integer(0...4096),
                "projection": .options(MosaicProjection.allCases.map(\.rawValue)),
                "layout": .options(MosaicLayoutMode.allCases.map(\.rawValue)),
                "alignment": .options(MosaicAlignmentMode.allCases.map(\.rawValue)),
                "blend": .options(MosaicBlendMode.allCases.map(\.rawValue)),
                "exposureMatching": .toggle,
                "columns": .integer(1...12),
            ]
        case .inspect, .meteorRestore, .histogram, .artifactDetect, .cloudDetect, .export:
            return [:]
        }
    }

    private static func editOperationParametersAreValid(
        _ parameters: [String: String],
        for kind: ProcessingOperationKind
    ) -> Bool {
        switch kind {
        case .denoise:
            return parameters["mode"] == nil || DisplayGridSettings(operationParameters: parameters) != nil
        case .localContrast:
            if parameters["mode"] != nil {
                return ContinuumToneSettings(operationParameters: parameters) != nil
            }
            return true
        case .starMask:
            guard let radiusValue = parameters["radius"],
                  let largeRadiusValue = parameters["largeRadius"],
                  let radius = Int(radiusValue),
                  let largeRadius = Int(largeRadiusValue)
            else {
                return true
            }
            return largeRadius >= radius
        default:
            return true
        }
    }

    private static func invalidEditOperationParameterKey(in operation: EditOperation) -> String? {
        let rules = editOperationParameterRules(for: operation).merging(
            structuralEditOperationParameterRules(for: operation)
        ) { visible, _ in visible }
        for key in rules.keys.sorted() {
            guard let value = operation.parameters[key],
                  rules[key]?.normalizedValue(value) == nil
            else {
                continue
            }
            return key
        }
        guard editOperationParametersAreValid(operation.parameters, for: operation.kind) else {
            return operation.kind == .starMask ? "largeRadius" : "parameters"
        }
        return nil
    }

    private static func structuralEditOperationParameterRules(
        for operation: EditOperation
    ) -> [String: EditOperationParameterRule] {
        switch operation.kind {
        case .register:
            return ["batch": .toggle]
        case .denoise:
            return operation.parameters["mode"] == nil ? [:] : ["mode": .options(["display-grid-v1"])]
        case .localContrast:
            return operation.parameters["mode"] == nil ? [:] : ["mode": .options(["continuum-v1"])]
        case .stack:
            return [
                "calibrationApplied": .toggle,
                "inputs": .integer(1...100_000),
            ]
        case .drizzle:
            return ["alignTranslation": .toggle]
        case .colorNeutralize:
            return operation.parameters["mode"] == nil
                ? [:]
                : ["mode": .options(["removeGreen"])]
        case .artifactRemove:
            return operation.parameters["mode"] == nil
                ? [:]
                : ["mode": .options(["cleanSequence"])]
        case .meteorRestore:
            return [
                "mode": .options(["extract", "restore", "restoreSequence"]),
                "sources": .integer(1...100_000),
            ]
        default:
            return [:]
        }
    }

    public func undoEditGraphChange() {
        guard canModifyWorkspaceProducts,
              let previous = editGraphUndoStack.popLast()
        else {
            return
        }

        editGraphRedoStack.append(currentEditGraphHistoryState())
        restoreEditGraphHistoryState(previous)
        editGraphNeedsReplay = true
        project.updatedAt = Date()
        updateEditGraphHistoryAvailability()
        triggerAutosave()
    }

    public func redoEditGraphChange() {
        guard canModifyWorkspaceProducts,
              let next = editGraphRedoStack.popLast()
        else {
            return
        }

        editGraphUndoStack.append(currentEditGraphHistoryState())
        restoreEditGraphHistoryState(next)
        editGraphNeedsReplay = true
        project.updatedAt = Date()
        updateEditGraphHistoryAvailability()
        triggerAutosave()
    }

    public func parameterValue(_ operationID: EditOperation.ID, key: String) -> String {
        guard let operation = project.editGraph.operations.first(where: { $0.id == operationID }) else {
            return ""
        }
        return operation.parameters[key] ?? ""
    }

    private func launch(_ operation: @escaping @MainActor () async -> Void) {
        guard activeTask == nil, batchTask == nil, isBatchRunning == false, isProcessing == false else {
            return
        }

        foregroundGeneration += 1
        let generation = foregroundGeneration
        rawPreviewGeneration += 1
        foregroundTaskRequested = true
        rawPreviewTask?.cancel()
        rawPreviewTask = nil
        isRawPreviewUpdating = false

        let task = Task { @MainActor in
            await operation()
            guard self.foregroundGeneration == generation else {
                return
            }
            if self.isProcessing == false {
                self.activeTask = nil
                self.canCancel = false
            }
            self.foregroundTaskRequested = false
            self.importPendingAssetsIfPossible()
        }
        activeTask = task
    }

    private func setPreview(
        _ url: URL,
        trackHistory: Bool = true,
        resetCurveReferenceHistogram: Bool = true,
        resetDetectedStarCount: Bool = true,
        operationID: EditOperation.ID? = nil,
        canvasMode: WorkspaceCanvasMode = .sourcePreview
    ) {
        if trackHistory {
            appendPreviewHistoryState()
        }
        cancelHistogramRefresh()
        previewURL = url
        histogram = nil
        artifactTrailReport = nil
        cloudRegionReport = nil
        self.canvasMode = canvasMode
        previewOperationID = operationID ?? Self.uniqueEditOperationID(
            for: url,
            in: project.editGraph.operations
        )
        replaceMarkedPreview(nil)
        if resetDetectedStarCount {
            detectedStarCount = nil
        }
        if resetCurveReferenceHistogram {
            curveReferenceHistogram = nil
        }
    }

    private func cancelHistogramRefresh() {
        histogramRefreshGeneration += 1
        histogramRefreshTask?.cancel()
        histogramRefreshTask = nil
    }

    private func cancelCurvePreviewTask() {
        curvePreviewGeneration += 1
        pendingCurvePreviewPoints = nil
        curvePreviewLoopID = nil
        curvePreviewTask?.cancel()
        curvePreviewTask = nil
    }

    private func commitPreviewAfterHistogramRefresh(
        _ url: URL,
        bins: Int = 64,
        trackHistory: Bool = true,
        resetCurveReferenceHistogram: Bool = true,
        resetDetectedStarCount: Bool = true,
        operationID: EditOperation.ID? = nil,
        canvasMode: WorkspaceCanvasMode = .sourcePreview
    ) async throws {
        let snapshot = try await bestEffortHistogram(for: url, bins: bins)
        try Task.checkCancellation()
        setPreview(
            url,
            trackHistory: trackHistory,
            resetCurveReferenceHistogram: resetCurveReferenceHistogram,
            resetDetectedStarCount: resetDetectedStarCount,
            operationID: operationID,
            canvasMode: canvasMode
        )
        histogram = snapshot
    }

    private func commitPreviewWithDeferredHistogramRefresh(
        _ url: URL,
        bins: Int = 64,
        trackHistory: Bool = true,
        resetCurveReferenceHistogram: Bool = true,
        resetDetectedStarCount: Bool = true,
        operationID: EditOperation.ID? = nil,
        canvasMode: WorkspaceCanvasMode = .sourcePreview,
        delay: Duration = .milliseconds(300)
    ) {
        setPreview(
            url,
            trackHistory: trackHistory,
            resetCurveReferenceHistogram: resetCurveReferenceHistogram,
            resetDetectedStarCount: resetDetectedStarCount,
            operationID: operationID,
            canvasMode: canvasMode
        )

        let generation = histogramRefreshGeneration
        let previewIdentity = Self.workspaceFileIdentityPath(for: url)
        let startedAt = Date()
        logCurveTelemetry(
            event: "histogram-refresh-scheduled",
            startedAt: startedAt,
            source: "deferred",
            width: imagePixelWidth(at: url),
            points: parameters.curveEditorPoints.count,
            note: "delayMs=\(Int(delay.components.seconds * 1000) + Int(delay.components.attoseconds / 1_000_000_000_000_000)) bins=\(bins)"
        )
        histogramRefreshTask = Task { @MainActor in
            defer {
                if generation == self.histogramRefreshGeneration {
                    self.histogramRefreshTask = nil
                }
            }

            do {
                try await Task.sleep(for: delay)
                try Task.checkCancellation()
                let snapshot = try await self.bestEffortHistogram(for: url, bins: bins)
                try Task.checkCancellation()
                guard generation == self.histogramRefreshGeneration,
                      self.previewURL.map({ Self.workspaceFileIdentityPath(for: $0) }) == previewIdentity
                else {
                    return
                }
                self.histogram = snapshot
                self.logCurveTelemetry(
                    event: "histogram-refresh-complete",
                    startedAt: startedAt,
                    source: snapshot == nil ? "best-effort-miss" : "best-effort-hit",
                    width: self.imagePixelWidth(at: url),
                    points: self.parameters.curveEditorPoints.count,
                    note: "bins=\(bins)"
                )
            } catch is CancellationError {
                self.logCurveTelemetry(
                    event: "histogram-refresh-cancelled",
                    startedAt: startedAt,
                    source: "cancelled",
                    width: self.imagePixelWidth(at: url),
                    points: self.parameters.curveEditorPoints.count,
                    note: "bins=\(bins)"
                )
                // A newer preview superseded this histogram refresh.
            } catch {
                guard generation == self.histogramRefreshGeneration else {
                    return
                }
                self.logCurveTelemetry(
                    event: "histogram-refresh-failed",
                    startedAt: startedAt,
                    source: "failed",
                    width: self.imagePixelWidth(at: url),
                    points: self.parameters.curveEditorPoints.count,
                    note: error.localizedDescription
                )
            }
        }
    }

    private func bestEffortHistogram(for input: URL, bins: Int = 64) async throws -> HistogramSnapshot? {
        do {
            let result = try await processingService.histogram(input: input, bins: bins)
            try Task.checkCancellation()
            return try parseHistogram(result, expectedBins: bins)
        } catch is CancellationError {
            throw CancellationError()
        } catch {
            return nil
        }
    }

    private func clearPreviewHistory() {
        previewHistory.removeAll()
        pendingLayerOpacityAdjustment = nil
        pendingLayerMaskAdjustment = nil
        canStepBack = false
    }

    private func appendPreviewHistoryState() {
        appendPreviewHistoryState(currentPreviewHistoryState())
    }

    private func currentPreviewHistoryState() -> PreviewHistoryState {
        PreviewHistoryState(
            previewURL: previewURL ?? selectedAsset?.originalURL,
            previewOperationID: previewOperationID,
            editGraphNeedsReplay: editGraphNeedsReplay,
            canvasMode: canvasMode,
            activeLayerID: activeLayerID,
            layers: project.layers,
            editGraph: project.editGraph
        )
    }

    private func appendPreviewHistoryState(_ state: PreviewHistoryState) {
        previewHistory.append(state)
        if previewHistory.count > 50 {
            previewHistory.removeFirst(previewHistory.count - 50)
        }
        canStepBack = true
    }

    @discardableResult
    private func restorePreviousPreviewState(command: String) -> Bool {
        guard let previous = previewHistory.popLast() else {
            return false
        }

        pendingLayerOpacityAdjustment = nil
        pendingLayerMaskAdjustment = nil
        project.layers = previous.layers
        project.editGraph = previous.editGraph
        canvasMode = previous.canvasMode
        let restoredPreviewURL = previous.previewURL.flatMap { inputFileExists($0) ? $0 : nil }
        previewOperationID = previous.previewOperationID.flatMap { operationID in
            project.editGraph.operations.contains(where: { $0.id == operationID }) ? operationID : nil
        }
        activeLayerID = previous.activeLayerID.flatMap { layerID in
            project.layers.first(where: {
                $0.id == layerID && isLayerInputAvailable($0)
            })?.id
        }
        previewURL = restoredPreviewURL
            ?? activeLayer?.inputURL.flatMap { inputFileExists($0) ? $0 : nil }
            ?? (selectedAsset?.originalURL).flatMap { inputFileExists($0) ? $0 : nil }
            ?? project.layers.last(where: isLayerInputAvailable)?.inputURL
        let historyPreviewIsMissing = previous.previewURL != nil && restoredPreviewURL == nil
        editGraphNeedsReplay = previous.editGraphNeedsReplay ||
            (historyPreviewIsMissing && previewOperationID != nil)
        project.updatedAt = Date()
        artifactTrailReport = nil
        cloudRegionReport = nil
        detectedStarCount = nil
        replaceMarkedPreview(nil)
        histogram = nil
        curveReferenceHistogram = nil
        canStepBack = previewHistory.isEmpty == false
        clearEditGraphHistory()
        errorMessage = historyPreviewIsMissing ? localized(.missingWorkspaceSourceError) : nil
        commandOutput = command
        triggerAutosave()
        return true
    }

    private func clearEditGraphHistory() {
        editGraphUndoStack.removeAll()
        editGraphRedoStack.removeAll()
        updateEditGraphHistoryAvailability()
    }

    private func recordEditGraphUndoSnapshot() {
        let snapshot = currentEditGraphHistoryState()
        if editGraphUndoStack.last != snapshot {
            editGraphUndoStack.append(snapshot)
        }
        if editGraphUndoStack.count > 50 {
            editGraphUndoStack.removeFirst(editGraphUndoStack.count - 50)
        }
        editGraphRedoStack.removeAll()
        updateEditGraphHistoryAvailability()
    }

    private func currentEditGraphHistoryState() -> EditGraphHistoryState {
        EditGraphHistoryState(
            editGraph: project.editGraph,
            previewOperationID: previewOperationID,
            layers: project.layers,
            activeLayerID: activeLayerID
        )
    }

    private func restoreEditGraphHistoryState(_ state: EditGraphHistoryState) {
        pendingLayerOpacityAdjustment = nil
        pendingLayerMaskAdjustment = nil
        project.editGraph = state.editGraph
        project.layers = state.layers
        previewOperationID = state.previewOperationID.flatMap { operationID in
            state.editGraph.operations.contains(where: { $0.id == operationID }) ? operationID : nil
        }
        activeLayerID = state.activeLayerID.flatMap { layerID in
            state.layers.contains(where: { $0.id == layerID }) ? layerID : nil
        }
    }

    private func updateEditGraphHistoryAvailability() {
        canUndoEditGraph = editGraphUndoStack.isEmpty == false
        canRedoEditGraph = editGraphRedoStack.isEmpty == false
    }

    private func recordOperation(
        _ kind: ProcessingOperationKind,
        parameters: [String: String] = [:],
        outputURL: URL?,
        promoteToLayer: Bool = false,
        sourceURL: URL? = nil,
        previewProxyURL: URL? = nil
    ) {
        var operationParameters = parameters
        if let outputURL {
            operationParameters["output"] = outputURL.path
        }
        if let previewProxyURL {
            operationParameters["previewOutput"] = previewProxyURL.path
        }
        if let sourceURL {
            operationParameters["input"] = sourceURL.path
        }
        let operation = EditOperation(kind: kind, parameters: operationParameters)
        recordEditGraphUndoSnapshot()
        if let placeholderIndex = project.editGraph.operations.firstIndex(where: {
            isTemplatePlaceholder($0, for: kind)
        }) {
            project.editGraph.operations.remove(at: placeholderIndex)
        }
        project.appendOperation(operation)
        if previewURL.map({ preview in
            outputURL.map { Self.urlsReferenceSameFile($0, preview) } == true ||
                previewProxyURL.map { Self.urlsReferenceSameFile($0, preview) } == true
        }) == true {
            previewOperationID = operation.id
            editGraphNeedsReplay = false
        }
        if promoteToLayer {
            promoteOperationOutputToLayer(
                kind,
                operationID: operation.id,
                sourceURL: sourceURL,
                outputURL: outputURL
            )
        }
        triggerAutosave()
    }

    private func isTemplatePlaceholder(
        _ operation: EditOperation,
        for kind: ProcessingOperationKind
    ) -> Bool {
        guard operation.kind == kind,
              operation.parameters["output"] == nil
        else {
            return false
        }

        switch kind {
        case .calibrate, .stack, .stretch, .preview, .mosaic, .sharpen:
            return true
        default:
            return false
        }
    }

    private func promoteOperationOutputToLayer(
        _ kind: ProcessingOperationKind,
        operationID: EditOperation.ID,
        sourceURL: URL?,
        outputURL: URL?
    ) {
        guard let outputURL else {
            return
        }

        let sourceLayer: ProcessingLayer
        if let activeLayer,
           activeLayer.kind != .mask,
           let activeLayerInput = activeLayer.inputURL,
           let sourceURL,
           Self.urlsReferenceSameFile(activeLayerInput, sourceURL) {
            sourceLayer = activeLayer
        } else if let sourceURL {
            let sourceIdentity = Self.workspaceFileIdentityPath(for: sourceURL)
            let pristineSourceLayers = project.layers.filter {
                $0.kind != .mask &&
                    $0.inputURL.map {
                        Self.workspaceFileIdentityPath(for: $0) == sourceIdentity
                    } == true &&
                    $0.maskArtifactID == nil &&
                    $0.blendMode == .normal &&
                    $0.opacity >= 0.999
            }
            if pristineSourceLayers.count == 1, let pristineSourceLayer = pristineSourceLayers.first {
                sourceLayer = pristineSourceLayer
            } else {
                let sourceAsset = project.assets.first {
                    Self.workspaceFileIdentityPath(for: $0.originalURL) == sourceIdentity
                } ?? Self.uniqueEditOperationID(
                    for: sourceURL,
                    in: project.editGraph.operations
                )
                .flatMap { operationID in
                    project.editGraph.operations.first(where: { $0.id == operationID })?
                        .parameters["assetID"]
                        .flatMap(UUID.init(uuidString:))
                }
                .flatMap { assetID in
                    project.assets.first { $0.id == assetID }
                }
                let matchingArtifacts = project.artifacts.filter {
                    $0.previewURL.map {
                        Self.workspaceFileIdentityPath(for: $0) == sourceIdentity
                    } == true
                }
                let sourceArtifact = matchingArtifacts.count == 1 ? matchingArtifacts.first : nil
                sourceLayer = ProcessingLayer(
                    name: sourceAsset?.displayName ?? sourceArtifact?.name ?? sourceURL.lastPathComponent,
                    kind: .baseImage,
                    sourceArtifactID: sourceArtifact?.id,
                    inputURL: sourceURL,
                    parameters: [
                        "source": "automatic",
                        "assetID": sourceAsset?.id.uuidString ?? "",
                    ]
                )
                project.appendLayer(sourceLayer)
            }
        } else {
            return
        }

        let layer = ProcessingLayer(
            name: "\(sourceLayer.name) · \(kind.rawValue)",
            kind: outputLayerKind(for: kind),
            sourceArtifactID: sourceLayer.sourceArtifactID,
            inputURL: outputURL,
            maskArtifactID: sourceLayer.maskArtifactID,
            blendMode: .normal,
            opacity: 1.0,
            parameters: [
                "source": "operation",
                "sourceLayerID": sourceLayer.id.uuidString,
                "operation": kind.rawValue,
                "operationID": operationID.uuidString,
            ]
        )
        project.appendLayer(layer)
        activeLayerID = layer.id
        canvasMode = .layerComposite
        if layer.maskArtifactID != nil {
            refreshVisibleLayerStackPreview(allowWhileProcessing: true)
        }
    }

    private func outputLayerKind(for operationKind: ProcessingOperationKind) -> ProcessingLayerKind {
        switch operationKind {
        case .starReduce:
            return .stars
        case .meteorRestore:
            return .meteors
        case .cloudRemove, .artifactRemove:
            return .adjustment
        case .background:
            return .background
        case .stretch, .curves, .localContrast, .denoise, .sharpen, .deconvolve, .colorNeutralize, .colorSaturate, .normalize, .comaReduce, .drizzle:
            return .adjustment
        default:
            return .adjustment
        }
    }

    private func recordRawSequenceArtifact(for assets: [PhotonStackAsset]) {
        let supportedAssets = assets.filter { [.raw, .fits, .tiff, .jpeg, .heif, .png].contains($0.kind) }
        guard supportedAssets.isEmpty == false else {
            return
        }

        let roles = Set(supportedAssets.map(\.role))
        let recordedRole = roles.count == 1 ? roles.first?.rawValue ?? "mixed" : "mixed"

        let frames = supportedAssets.map { asset in
            ProcessingArtifactFrame(
                sourceURL: asset.originalURL,
                outputURL: asset.originalURL,
                metrics: [
                    "assetID": asset.id.uuidString,
                    "kind": asset.kind.rawValue,
                    "role": asset.role.rawValue,
                ]
            )
        }
        project.appendArtifact(
            ProcessingArtifact(
                kind: .rawSequence,
                name: supportedAssets.count == 1 ? supportedAssets[0].displayName : artifactKindName(.rawSequence),
                outputURLs: supportedAssets.map(\.originalURL),
                frames: frames,
                parameters: ["role": recordedRole, "source": "import"],
                metrics: ["frames": String(supportedAssets.count)]
            )
        )
    }

    private func sourceArtifactIDs(for assets: [PhotonStackAsset]) -> [UUID] {
        let assetIDs = Set(assets.map { $0.id.uuidString })
        let paths = Set(assets.map {
            PhotonStackProject.assetIdentityPath(for: $0.originalURL)
        })
        guard assetIDs.isEmpty == false else {
            return []
        }

        return project.artifacts.compactMap { artifact in
            guard artifact.kind == .rawSequence else {
                return nil
            }
            let matchesFrame = artifact.frames.contains { frame in
                if let assetID = frame.metrics["assetID"], assetIDs.contains(assetID) {
                    return true
                }
                return [frame.sourceURL, frame.outputURL].compactMap { $0 }.contains { url in
                    paths.contains(PhotonStackProject.assetIdentityPath(for: url))
                }
            }
            let matchesOutput = artifact.outputURLs.contains { url in
                paths.contains(PhotonStackProject.assetIdentityPath(for: url))
            }
            return matchesFrame || matchesOutput ? artifact.id : nil
        }
    }

    private func synchronizeSourceArtifactRole(for assetID: UUID, role: CalibrationFrameRole) {
        let assetIDText = assetID.uuidString
        for artifactIndex in project.artifacts.indices
            where project.artifacts[artifactIndex].kind == .rawSequence {
            var changed = false
            for frameIndex in project.artifacts[artifactIndex].frames.indices
                where project.artifacts[artifactIndex].frames[frameIndex].metrics["assetID"] == assetIDText {
                project.artifacts[artifactIndex].frames[frameIndex].metrics["role"] = role.rawValue
                changed = true
            }
            guard changed else {
                continue
            }
            let artifact = project.artifacts[artifactIndex]
            let frameRoles = Set(artifact.frames.compactMap { $0.metrics["role"] })
            project.artifacts[artifactIndex].parameters["role"] =
                frameRoles.count == 1 && frameRoles.count == artifact.frames.count
                ? frameRoles.first
                : "mixed"
            project.artifacts[artifactIndex].updatedAt = Date()
        }
    }

    private func latestArtifact(kind: ProcessingArtifactKind, minimumFrameCount: Int = 1) -> ProcessingArtifact? {
        project.artifacts
            .filter { $0.kind == kind && $0.frameCount >= minimumFrameCount }
            .sorted { $0.createdAt < $1.createdAt }
            .last
    }

    private func appendProcessingArtifact(_ artifact: ProcessingArtifact) {
        project.appendArtifact(artifact)
        triggerAutosave()
    }

    private func appendProcessingLayer(_ layer: ProcessingLayer) {
        project.appendLayer(layer)
        triggerAutosave()
    }

    @discardableResult
    private func appendDerivedArtifactAndLayer(
        kind: ProcessingArtifactKind,
        name: String,
        operationKind: ProcessingOperationKind,
        sourceArtifactIDs: [UUID] = [],
        outputDirectory: URL? = nil,
        outputURLs: [URL],
        parameters: [String: String] = [:],
        metrics: [String: String] = [:],
        layerKind: ProcessingLayerKind,
        blendMode: ProcessingLayerBlendMode = .normal,
        isVisible: Bool = true,
        makeActive: Bool = true
    ) -> ProcessingArtifact {
        let artifact = ProcessingArtifact(
            kind: kind,
            name: name,
            operationKind: operationKind,
            sourceArtifactIDs: sourceArtifactIDs,
            outputDirectory: outputDirectory,
            outputURLs: outputURLs,
            parameters: parameters,
            metrics: metrics
        )
        project.appendArtifact(artifact)

        if let previewURL = artifact.previewURL {
            let layer = ProcessingLayer(
                name: name,
                kind: layerKind,
                sourceArtifactID: artifact.id,
                inputURL: previewURL,
                blendMode: blendMode,
                isVisible: isVisible,
                parameters: [
                    "source": "artifact",
                    "artifactKind": kind.rawValue,
                    "operation": operationKind.rawValue,
                ]
            )
            project.appendLayer(layer)
            if makeActive {
                activeLayerID = layer.id
                canvasMode = .layerComposite
            }
        }

        return artifact
    }

    private func upsertRawDecodeOperation(
        for asset: PhotonStackAsset,
        options: RawProcessingOptions,
        outputURL: URL?
    ) {
        guard asset.kind == .raw else {
            return
        }

        var operationParameters = rawDecodeParameters(from: options)
        operationParameters["width"] = String(min(max(parameters.previewWidth, 320), 1200))
        operationParameters["assetID"] = asset.id.uuidString
        if let outputURL {
            operationParameters["output"] = outputURL.path
        }

        var updatedOperations = project.editGraph.operations
        let rawParameterNames = [
            "rawWhiteBalance",
            "rawExposureBias",
            "rawBlackLevel",
            "rawDemosaic",
            "rawLinear",
            "width",
        ]
        let rawIndices = updatedOperations.indices.filter {
            updatedOperations[$0].kind == .rawDecode
        }
        let settingsChanged = rawIndices.contains { index in
            rawParameterNames.contains { name in
                updatedOperations[index].parameters[name] != operationParameters[name]
            }
        }
        if settingsChanged {
            for index in rawIndices {
                for name in rawParameterNames {
                    updatedOperations[index].parameters[name] = operationParameters[name]
                }
                updatedOperations[index].parameters.removeValue(forKey: "output")
                updatedOperations[index].isEnabled = true
            }
        }

        let existingIndex = updatedOperations.lastIndex(where: { operation in
            guard operation.kind == .rawDecode else {
                return false
            }
            if operation.parameters["assetID"] == asset.id.uuidString {
                return true
            }
            return operation.parameters["assetID"] == nil && project.assets.count == 1
        })
        if let index = existingIndex {
            for name in rawParameterNames {
                updatedOperations[index].parameters[name] = operationParameters[name]
            }
            updatedOperations[index].parameters["assetID"] = asset.id.uuidString
            if let outputURL {
                updatedOperations[index].parameters["output"] = outputURL.path
            }
            updatedOperations[index].isEnabled = true
        } else {
            updatedOperations.insert(
                EditOperation(kind: .rawDecode, parameters: operationParameters),
                at: 0
            )
        }
        guard updatedOperations != project.editGraph.operations else {
            return
        }

        recordEditGraphUndoSnapshot()
        project.editGraph.operations = updatedOperations
        project.updatedAt = Date()
        triggerAutosave()
    }

    private func rawDecodeOperation(for asset: PhotonStackAsset) -> EditOperation? {
        if let scopedOperation = project.editGraph.operations.last(where: {
            $0.kind == .rawDecode && $0.parameters["assetID"] == asset.id.uuidString
        }) {
            return scopedOperation
        }
        return project.editGraph.operations.last(where: { $0.kind == .rawDecode })
    }

    private func persistedRawOptions(for asset: PhotonStackAsset) -> RawProcessingOptions? {
        rawDecodeOperation(for: asset).map(rawOptionsParameter(in:))
    }

    private func restorePersistedRawOptions(for asset: PhotonStackAsset?) {
        guard let asset,
              asset.kind == .raw,
              let options = persistedRawOptions(for: asset)
        else {
            return
        }
        parameters.rawProcessingOptions = options
    }

    private func replayEditGraph() async {
        await runJob(title: "Replay Edit Graph", kind: .preview) {
            guard self.hasAmbiguousEditGraphProvenance == false else {
                throw EditGraphReplayError(message: self.localized(.editGraphAmbiguousProvenance))
            }
            try FileManager.default.createDirectory(at: self.previewDirectory, withIntermediateDirectories: true)

            guard let replayPlan = self.previewEditGraphReplayPlan()
                ?? self.fallbackPreviewEditGraphReplayPlan()
            else {
                if self.project.editGraph.operations.contains(where: \.isEnabled) == false {
                    return self.localized(.noEditOperations)
                }
                throw EditGraphReplayError(message: self.localized(.editGraphAmbiguousProvenance))
            }
            let enabledOperations = replayPlan.operations
            var input = replayPlan.input

            var log: [String] = []
            var appliedCount = 0
            var replayedOperationOutputs: [EditOperation.ID: URL] = [:]
            var retainedReplayOutputs: Set<URL> = []
            self.detectedStarCount = nil
            self.clearReplayTemporaryDirectories()
            self.replayStackSourceInputs = nil
            self.replayRegisteredSequence = nil
            self.replayNeedsMeteorSequenceSources = enabledOperations.contains(where: self.isMeteorSequenceRestore)
            let total = max(enabledOperations.count + 1, 1)
            let progressScope = ProcessingProgressContext.progressScope
            defer {
                progressScope?.update(lowerBound: 0, upperBound: 1)
                self.replayStackSourceInputs = nil
                self.replayRegisteredSequence = nil
                self.replayNeedsMeteorSequenceSources = false
                self.clearReplayTemporaryDirectories(retaining: retainedReplayOutputs)
            }

            for (index, operation) in enabledOperations.enumerated() {
                try Task.checkCancellation()
                let lowerBound = Double(index) / Double(total)
                let upperBound = Double(index + 1) / Double(total)
                progressScope?.update(lowerBound: lowerBound, upperBound: upperBound)
                self.updateProgress(lowerBound, message: operation.kind.rawValue)
                let output = try await self.applyReplayOperation(operation, input: input, mode: .preview)
                try Task.checkCancellation()
                input = output
                replayedOperationOutputs[operation.id] = output
                appliedCount += 1
                log.append("Applied \(operation.kind.rawValue): \(output.lastPathComponent)")
            }

            let histogramLowerBound = Double(enabledOperations.count) / Double(total)
            progressScope?.update(lowerBound: histogramLowerBound, upperBound: 1)
            self.updateProgress(histogramLowerBound, message: "Replay histogram")
            if appliedCount > 0 || replayPlan.anchorOperationID != nil {
                try await self.commitPreviewAfterHistogramRefresh(
                    input,
                    resetDetectedStarCount: false,
                    operationID: replayPlan.anchorOperationID ?? enabledOperations.last?.id
                )
                retainedReplayOutputs = self.updateReplayedOperationLayerOutputs(replayedOperationOutputs)
                retainedReplayOutputs.insert(input.standardizedFileURL)
                self.editGraphNeedsReplay = false
                self.triggerAutosave()
            }
            self.updateProgress(1, message: "Replay complete")

            return log.isEmpty ? self.localized(.noEditOperations) : log.joined(separator: "\n")
        }
    }

    private func previewEditGraphReplayPlan() -> EditGraphReplayPlan? {
        if let previewOperationID,
           let plan = editGraphReplayPlan(
               finalOperationID: previewOperationID,
               finalOperationIsEligible: { _ in true }
           ) {
            return plan
        }

        var targets: [URL] = []
        if let previewURL {
            targets.append(previewURL)
        }
        if let savedPreview = project.workspaceState?.previewURL,
           targets.contains(where: { Self.urlsReferenceSameFile($0, savedPreview) }) == false {
            targets.append(savedPreview)
        }

        for target in targets {
            if let plan = editGraphReplayPlan(
                targetURL: target,
                finalOperationIsEligible: { _ in true }
            ) {
                return plan
            }
        }
        return nil
    }

    private func fallbackPreviewEditGraphReplayPlan() -> EditGraphReplayPlan? {
        guard let selectedAsset else {
            return nil
        }
        let enabledOperations = project.editGraph.operations.filter(\.isEnabled)
        guard enabledOperations.isEmpty == false else {
            return nil
        }

        func identityPath(_ path: String) -> String {
            Self.workspaceFileIdentityPath(for: URL(fileURLWithPath: path))
        }

        var expectedInputPath: String? = Self.workspaceFileIdentityPath(for: selectedAsset.originalURL)
        for operation in enabledOperations {
            if operation.kind == .rawDecode,
               let rawAssetID = operation.parameters["assetID"].flatMap(UUID.init(uuidString:)),
               let recordedAsset = project.assets.first(where: { $0.id == rawAssetID }),
               recordedAsset.id != selectedAsset.id {
                    return nil
            }
            if let recordedInput = operation.parameters["input"],
               let expectedInputPath,
               identityPath(recordedInput) != expectedInputPath {
                return nil
            }
            if replayOperationAdvancesMainImage(operation) {
                expectedInputPath = operation.parameters["output"].map(identityPath)
            }
        }

        return EditGraphReplayPlan(
            input: selectedAsset.originalURL,
            operations: enabledOperations,
            anchorOperationID: enabledOperations.last?.id
        )
    }

    private func replayOperationAdvancesMainImage(_ operation: EditOperation) -> Bool {
        switch operation.kind {
        case .calibrate, .starDetect, .starMask, .inspect, .histogram,
             .artifactDetect, .cloudDetect, .export:
            return false
        case .artifactRemove:
            return operation.parameters["mode"] != "cleanSequence"
        case .meteorRestore:
            return operation.parameters["mode"] != "extract"
        default:
            return true
        }
    }

    private func applyReplayOperation(
        _ operation: EditOperation,
        input: URL,
        mode: EditGraphReplayMode
    ) async throws -> URL {
        if let invalidParameter = Self.invalidEditOperationParameterKey(in: operation) {
            throw EditGraphReplayError(
                message: "\(localized(.invalidEditOperationParameter)): \(operation.kind.rawValue).\(invalidParameter)"
            )
        }

        switch operation.kind {
        case .rawDecode:
            let recordedAsset = try replayRawAsset(for: operation)
            guard Self.urlsReferenceSameFile(input, recordedAsset.originalURL) else {
                throw EditGraphReplayError(
                    message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
                )
            }
            if mode == .fullResolutionExport {
                let operationParameters = operation.parameters.merging(
                    ["mode": "fullResolutionExport"],
                    uniquingKeysWith: { _, new in new }
                )
                let output = makeCachedPreviewOutput(
                    prefix: "export-replay-raw",
                    input: input,
                    parameters: operationParameters,
                    extension: rawOptionsParameter(in: operation).linearOutput ? "fits" : "tiff"
                )
                _ = try await runValidatedCachedStep(output: output, command: "raw full-resolution") {
                    let rawOptions = rawOptionsParameter(in: operation)
                    return try await processingService.convert(
                        input: input,
                        output: output,
                        options: ExportOptions(
                            bitDepth: .sixteen,
                            colorSpace: rawOptions.linearOutput ? .linearSRGB : .srgb,
                            fitsValueMode: .scientific,
                            jpegQuality: 1.0
                        ),
                        rawOptions: rawOptions
                    )
                }
                return output
            }
            let output = makeCachedPreviewOutput(
                prefix: "replay-raw",
                input: input,
                parameters: operation.parameters
            )
            _ = try await runValidatedCachedStep(output: output, command: "raw preview") {
                try await processingService.makePreview(
                    input: input,
                    output: output,
                    width: intParameter("width", in: operation, defaultValue: min(max(parameters.previewWidth, 320), 1200)),
                    rawOptions: rawOptionsParameter(in: operation)
                )
            }
            return output
        case .background:
            let model = operation.parameters["model"] ?? "grid"
            let backgroundMode = operation.parameters["mode"] ?? "subtract"
            let preserveBrightness = boolParameter(
                "preserveBrightness",
                in: operation,
                defaultValue: false
            )
            let protectBrightTargets = boolParameter(
                "protectBrightTargets",
                in: operation,
                defaultValue: false
            )
            let operationParameters = [
                "model": model,
                "mode": backgroundMode,
                "strength": operation.parameters["strength"] ?? "0.35",
                "preserveBrightness": String(preserveBrightness),
                "protectBrightTargets": String(protectBrightTargets),
                "replayMode": mode.cacheKey,
            ]
            let strength = doubleParameter("strength", in: operation, defaultValue: 0.35)
            let output = makeCachedPreviewOutput(
                prefix: "replay-background",
                input: input,
                parameters: operationParameters,
                extension: mode.outputExtension(preservingScientificValuesFrom: input)
            )
            _ = try await runValidatedCachedStep(output: output, command: "background") {
                try await processingService.background(
                    input: input,
                    output: output,
                    model: model,
                    mode: backgroundMode,
                    strength: strength,
                    preserveBrightness: preserveBrightness,
                    protectBrightTargets: protectBrightTargets
                )
            }
            return output
        case .normalize:
            let targetBackground = doubleParameter("targetBackground", in: operation, defaultValue: 0.25)
            let targetScale = doubleParameter("targetScale", in: operation, defaultValue: 1.0)
            let operationParameters = [
                "targetBackground": String(targetBackground),
                "targetScale": String(targetScale),
                "replayMode": mode.cacheKey,
            ]
            let output = makeCachedPreviewOutput(
                prefix: "replay-normalize",
                input: input,
                parameters: operationParameters,
                extension: mode.outputExtension(preservingScientificValuesFrom: input)
            )
            _ = try await runValidatedCachedStep(output: output, command: "normalize") {
                try await processingService.normalize(
                    input: input,
                    output: output,
                    targetBackground: targetBackground,
                    targetScale: targetScale
                )
            }
            return output
        case .stack:
            if operation.parameters["mode"] == "deepSkyRecipe", let recipe = operation.parameters["recipe"] {
                let directory = trackReplayTemporaryDirectory(previewDirectory.appendingPathComponent("replay-deep-sky-\(UUID().uuidString)"))
                let recipeURL = previewDirectory.appendingPathComponent("recipe-\(UUID().uuidString).txt")
                try recipe.write(to: recipeURL, atomically: true, encoding: .utf8)
                defer { try? FileManager.default.removeItem(at: recipeURL) }
                let report = try await processingService.deepSky(recipe: recipeURL, outputDirectory: directory, analyzeOnly: false)
                return URL(fileURLWithPath: report.developed)
            }
            return try await replayStackMacro(operation, mode: mode)
        case .drizzle:
            return try await replayDrizzleMacro(operation, mode: mode)
        case .mosaic:
            return try await replayMosaicMacro(operation, mode: mode)
        case .preview:
            if mode == .fullResolutionExport || operation.parameters["source"] == "layerStack" {
                return input
            }
            let output = makePreviewOutput(prefix: "replay-preview", extension: "png")
            let width = intParameter("width", in: operation, defaultValue: parameters.previewWidth)
            _ = try await processingService.makePreview(
                input: input,
                output: output,
                width: width,
                rawOptions: rawOptionsParameter(in: operation)
            )
            return output
        case .crop:
            guard let aspect = operation.parameters["aspect"] else {
                throw EditGraphReplayError(
                    message: "\(localized(.invalidEditOperationParameter)): \(operation.kind.rawValue).aspect"
                )
            }
            let output = makeReplayOutput(
                prefix: "replay-crop",
                mode: mode,
                preservingScientificValuesFrom: input
            )
            var arguments = [
                "crop",
                "--input", input.path,
                "--output", output.path,
                "--aspect", aspect,
            ]
            if let margin = operation.parameters["margin"] {
                arguments.append(contentsOf: ["--margin", margin])
            }
            _ = try await processingService.runCLI(arguments: arguments)
            try Task.checkCancellation()
            return output
        case .calibrate:
            // Calibration is a sequence operation. The following stack node
            // rebuilds its calibrated inputs when recorded cache files are gone.
            return input
        case .register:
            if boolParameter("batch", in: operation, defaultValue: false) {
                let reference = try replayAssetParameter(
                    "referenceID",
                    legacyNameParameter: "reference",
                    in: operation
                )
                let movingFrames = try replayAssetsParameter(
                    "selectedMovingFrameIDs",
                    in: operation,
                    fallback: project.assets.filter { $0.id != reference.id }
                )
                guard movingFrames.isEmpty == false else {
                    throw EditGraphReplayError(
                        message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
                    )
                }
                guard movingFrames.contains(where: { $0.id == reference.id }) == false else {
                    throw EditGraphReplayError(
                        message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
                    )
                }
                let outputDirectory = trackReplayTemporaryDirectory(previewDirectory.appendingPathComponent(
                    "replay-registered-batch-\(UUID().uuidString)",
                    isDirectory: true
                ))
                let alignment = alignmentMethodParameter("alignment", in: operation, defaultValue: .distortion)
                let outputFormat = mode == .fullResolutionExport ? "fits" : "tiff"
                let result = try await processingService.registerBatch(
                    reference: reference.originalURL,
                    inputs: movingFrames.map(\.originalURL),
                    outputDirectory: outputDirectory,
                    alignment: alignment == .none ? .distortion : alignment,
                    outputFormat: outputFormat
                )
                try Task.checkCancellation()
                let outputs = try registeredSequenceOutputs(
                    from: result,
                    expectedInputs: [reference.originalURL] + movingFrames.map(\.originalURL),
                    outputDirectory: outputDirectory,
                    mode: alignment == .none ? .distortion : alignment,
                    outputFormat: outputFormat
                )
                replayRegisteredSequence = ReplayRegisteredSequence(
                    artifactID: operation.parameters["artifactID"].flatMap(UUID.init(uuidString:)),
                    outputs: outputs
                )
                return outputs.dropFirst().first ?? outputs[0]
            }
            let reference = try replayAssetParameter(
                "referenceID",
                legacyNameParameter: "reference",
                in: operation
            )
            let moving = try replayAssetParameter(
                "movingID",
                legacyNameParameter: "moving",
                in: operation
            )
            guard reference.id != moving.id else {
                throw EditGraphReplayError(
                    message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
                )
            }
            let output = makePreviewOutput(
                prefix: "replay-register",
                extension: mode == .fullResolutionExport ? "fits" : "tiff"
            )
            let alignment = alignmentMethodParameter("alignment", in: operation, defaultValue: .translation)
            _ = try await processingService.register(
                reference: reference.originalURL,
                moving: moving.originalURL,
                output: output,
                alignment: alignment == .none ? .translation : alignment
            )
            return output
        case .stretch:
            let output = operation.parameters["mode"] == "astro-v1"
                ? makePreviewOutput(prefix: "replay-develop", extension: "tiff")
                : makeReplayOutput(prefix: "replay-stretch", mode: mode)
            if operation.parameters["mode"] == "astro-v1" {
                _ = try await processingService.develop(input: input, output: output,
                    settings: AstroDevelopSettings(operationParameters: operation.parameters))
                return output
            }
            let targetBackground = doubleParameter("targetBackground", in: operation, defaultValue: parameters.stretchTargetBackground)
            _ = try await processingService.autoStretch(input: input, output: output, targetBackground: targetBackground)
            return output
        case .curves:
            let output = makeReplayOutput(prefix: "replay-curves", mode: mode)
            var curveParameters = parameters
            curveParameters.curveBlackPoint = doubleParameter("black", in: operation, defaultValue: parameters.curveBlackPoint)
            curveParameters.curveMidInput = doubleParameter("midInput", in: operation, defaultValue: parameters.curveMidInput)
            curveParameters.curveMidOutput = doubleParameter("midOutput", in: operation, defaultValue: parameters.curveMidOutput)
            curveParameters.curveWhitePoint = doubleParameter("white", in: operation, defaultValue: parameters.curveWhitePoint)
            curveParameters.curveChannel = CurveChannel(rawValue: operation.parameters["channel"] ?? "") ?? .rgb
            if let points = curvePointsParameter(in: operation) {
                curveParameters.curvePoints = points
            }
            if let bridged = try await runEngineCurveApplyIfAvailable(
                input: input,
                output: output,
                points: curveParameters.curvePointPairs,
                channel: curveParameters.curveChannel
            ) {
                _ = bridged
            } else {
                _ = try await processingService.curves(
                    input: input,
                    output: output,
                    points: curveParameters.curvePointPairs,
                    channel: curveParameters.curveChannel
                )
            }
            return output
        case .localContrast:
            if operation.parameters["mode"] == "continuum-v1" {
                guard let settings = ContinuumToneSettings(operationParameters: operation.parameters) else {
                    throw ContinuumToneError.invalidSettings
                }
                let output = makePreviewOutput(prefix: "replay-continuum-tone", extension: "tiff")
                _ = try await processingService.continuumTone(input: input, output: output, settings: settings)
                return output
            }
            let output = makeReplayOutput(prefix: "replay-contrast", mode: mode)
            let amount = doubleParameter("amount", in: operation, defaultValue: parameters.localContrastAmount)
            let radius = intParameter("radius", in: operation, defaultValue: parameters.localContrastRadius)
            _ = try await processingService.localContrast(input: input, output: output, amount: amount, radius: radius)
            return output
        case .denoise:
            if operation.parameters["mode"] != nil {
                guard let settings = DisplayGridSettings(operationParameters: operation.parameters) else {
                    throw DisplayGridError.invalidSettings
                }
                let output = makePreviewOutput(prefix: "replay-display-grid", extension: "tiff")
                _ = try await processingService.suppressDisplayGrid(input: input, output: output, settings: settings)
                return output
            }
            let output = makeReplayOutput(prefix: "replay-denoise", mode: mode)
            let amount = doubleParameter("amount", in: operation, defaultValue: parameters.denoiseAmount)
            let chromaAmount = doubleParameter("chromaAmount", in: operation, defaultValue: parameters.denoiseChromaAmount)
            let radius = intParameter("radius", in: operation, defaultValue: parameters.denoiseRadius)
            _ = try await processingService.denoise(input: input, output: output, amount: amount, chromaAmount: chromaAmount, radius: radius)
            return output
        case .sharpen:
            let output = makeReplayOutput(prefix: "replay-sharpen", mode: mode)
            let amount = doubleParameter("amount", in: operation, defaultValue: parameters.sharpenAmount)
            let radius = intParameter("radius", in: operation, defaultValue: parameters.sharpenRadius)
            _ = try await processingService.sharpen(input: input, output: output, amount: amount, radius: radius)
            return output
        case .deconvolve:
            let output = makeReplayOutput(prefix: "replay-deconvolve", mode: mode)
            let iterations = intParameter("iterations", in: operation, defaultValue: 8)
            let radius = intParameter("radius", in: operation, defaultValue: 2)
            let sigma = doubleParameter("sigma", in: operation, defaultValue: 1.2)
            _ = try await processingService.deconvolve(
                input: input,
                output: output,
                iterations: iterations,
                radius: radius,
                sigma: sigma
            )
            return output
        case .colorNeutralize:
            if operation.parameters["mode"] == "removeGreen" {
                if operation.parameters["greenMethod"] != nil {
                    // Display styling stays TIFF during full-resolution export replay too.
                    let output = makePreviewOutput(prefix: "replay-green-cast", extension: "tiff")
                    _ = try await processingService.removeGreenCast(input: input, output: output,
                        settings: GreenCastSettings(operationParameters: operation.parameters))
                    return output
                }
                let output = makeReplayOutput(prefix: "replay-remove-green", mode: mode)
                _ = try await processingService.removeGreenCast(
                    input: input,
                    output: output,
                    amount: doubleParameter("amount", in: operation, defaultValue: 0.65),
                    backgroundLimit: doubleParameter("backgroundLimit", in: operation, defaultValue: 0.32)
                )
                return output
            }
            let output = makeReplayOutput(prefix: "replay-neutralize", mode: mode)
            let strength = doubleParameter("strength", in: operation, defaultValue: 1.0)
            _ = try await processingService.colorNeutralize(input: input, output: output, strength: strength)
            return output
        case .colorSaturate:
            let output = makeReplayOutput(prefix: "replay-saturate", mode: mode)
            let amount = doubleParameter("amount", in: operation, defaultValue: 0.2)
            _ = try await processingService.colorSaturate(input: input, output: output, amount: amount)
            return output
        case .starDetect:
            if mode == .fullResolutionExport {
                return input
            }
            let result = try await processingService.detectStars(
                input: input,
                sigmaThreshold: doubleParameter("sigmaThreshold", in: operation, defaultValue: 3.0),
                minPeak: doubleParameter("minPeak", in: operation, defaultValue: 0.05),
                maxStars: intParameter("maxStars", in: operation, defaultValue: 50_000)
            )
            try Task.checkCancellation()
            detectedStarCount = try parseStarCount(result, expectedCommand: "stars detect")
            return input
        case .starMask:
            if mode == .fullResolutionExport {
                return input
            }
            let output = makePreviewOutput(prefix: "replay-star-mask", extension: "png")
            let result = try await processingService.createStarMask(
                input: input,
                output: output,
                radius: intParameter("radius", in: operation, defaultValue: 2),
                largeRadius: intParameter("largeRadius", in: operation, defaultValue: 6),
                layered: boolParameter("layered", in: operation, defaultValue: true),
                sigmaThreshold: doubleParameter("sigmaThreshold", in: operation, defaultValue: 3.0),
                minPeak: doubleParameter("minPeak", in: operation, defaultValue: 0.05),
                maxStars: intParameter("maxStars", in: operation, defaultValue: 50_000)
            )
            try Task.checkCancellation()
            detectedStarCount = try parseStarCount(result, expectedCommand: "stars mask")
            return input
        case .starReduce:
            let output = makeReplayOutput(prefix: "replay-stars", mode: mode)
            let amount = doubleParameter("amount", in: operation, defaultValue: parameters.starReductionAmount)
            let profileAware = boolParameter("profileAware", in: operation, defaultValue: parameters.profileAwareStarReduction)
            let edgeAware = boolParameter("edgeAware", in: operation, defaultValue: parameters.edgeAwareStarReduction)
            _ = try await processingService.reduceStars(
                input: input,
                output: output,
                amount: amount,
                profileAware: profileAware,
                edgeAware: edgeAware
            )
            return output
        case .comaReduce:
            let output = makeReplayOutput(prefix: "replay-coma", mode: mode)
            let amount = doubleParameter("amount", in: operation, defaultValue: parameters.comaReductionAmount)
            let radius = intParameter("radius", in: operation, defaultValue: parameters.comaReductionRadius)
            let eccentricity = doubleParameter("eccentricity", in: operation, defaultValue: parameters.comaReductionEccentricity)
            let edgeAware = boolParameter("edgeAware", in: operation, defaultValue: parameters.edgeAwareComaReduction)
            _ = try await processingService.reduceComa(
                input: input,
                output: output,
                amount: amount,
                radius: radius,
                eccentricity: eccentricity,
                edgeAware: edgeAware
            )
            return output
        case .artifactRemove:
            if operation.parameters["mode"] == "cleanSequence" {
                if mode == .fullResolutionExport {
                    return input
                }
                let sequenceInputs: [URL]
                if operation.parameters["sourceFrameIDs"] != nil {
                    sequenceInputs = try replayAssetsParameter(
                        "sourceFrameIDs",
                        in: operation,
                        fallback: []
                    ).map(\.originalURL)
                } else if let rawFrameCount = operation.parameters["frames"] {
                    guard let recordedFrameCount = Int(rawFrameCount), recordedFrameCount > 0 else {
                        throw EditGraphReplayError(
                            message: "\(localized(.invalidEditOperationParameter)): \(operation.kind.rawValue).frames"
                        )
                    }
                    let recordedInputs = (0..<recordedFrameCount).compactMap { index -> URL? in
                        guard let path = operation.parameters["inputPath.\(index)"]?
                            .trimmingCharacters(in: .whitespacesAndNewlines),
                              path.isEmpty == false
                        else {
                            return nil
                        }
                        return URL(fileURLWithPath: path)
                    }
                    guard recordedInputs.count == recordedFrameCount else {
                        throw EditGraphReplayError(
                            message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
                        )
                    }
                    sequenceInputs = recordedInputs
                } else {
                    sequenceInputs = (lightAssets.isEmpty ? project.assets : lightAssets).map(\.originalURL)
                }
                guard sequenceInputs.isEmpty == false else {
                    throw EditGraphReplayError(
                        message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
                    )
                }
                let outputDirectory = trackReplayTemporaryDirectory(previewDirectory.appendingPathComponent(
                    "replay-clean-sequence-\(UUID().uuidString)",
                    isDirectory: true
                ))
                let result = try await processingService.cleanArtifactSequence(
                    inputs: sequenceInputs,
                    outputDirectory: outputDirectory,
                    outputFormat: operation.parameters["outputFormat"] ?? "png",
                    minWeight: doubleParameter("minWeight", in: operation, defaultValue: 0.30),
                    recurrenceThreshold: intParameter("recurrenceThreshold", in: operation, defaultValue: 2)
                )
                try Task.checkCancellation()
                _ = try validatedBatchReport(
                    result,
                    expectedCommand: "artifacts clean-sequence",
                    expectedInputs: sequenceInputs,
                    outputDirectory: outputDirectory,
                    expectedOutputFormat: operation.parameters["outputFormat"] ?? "png"
                )
                return input
            }
            let output = makeReplayOutput(
                prefix: "replay-artifacts",
                mode: mode,
                preservingScientificValuesFrom: input
            )
            let removeMeteors = boolParameter("removeMeteors", in: operation, defaultValue: false)
            let selectedIndices: [Int]
            if let encodedSelection = operation.parameters["selectedTrailsV1"] {
                let detection = try await processingService.detectArtifacts(input: input)
                try Task.checkCancellation()
                var report = try parseArtifactTrailReport(detection)
                report.sourceURL = input.standardizedFileURL
                if report.imageWidth == nil || report.imageHeight == nil,
                   let size = imagePixelSize(at: input) {
                    report.imageWidth = Int(size.width)
                    report.imageHeight = Int(size.height)
                }
                selectedIndices = try matchedArtifactTrailIndices(
                    encodedSelection,
                    in: report,
                    operationKind: operation.kind
                )
            } else {
                selectedIndices = try intListParameter("selectedIndices", in: operation)
            }
            _ = try await processingService.removeArtifacts(
                input: input,
                output: output,
                selectedIndices: selectedIndices.isEmpty ? nil : selectedIndices,
                removeAirplanes: boolParameter("removeAirplanes", in: operation, defaultValue: true),
                removeDrones: boolParameter("removeDrones", in: operation, defaultValue: true),
                removeSatellites: boolParameter("removeSatellites", in: operation, defaultValue: true),
                removeMeteors: removeMeteors
            )
            return output
        case .cloudRemove:
            let output = makeReplayOutput(prefix: "replay-clouds", mode: mode)
            let selectedIndices: [Int]
            if let encodedSelection = operation.parameters["selectedCloudRegionsV1"] {
                let detection = try await processingService.detectClouds(input: input)
                try Task.checkCancellation()
                var report = try parseCloudRegionReport(detection)
                report.sourceURL = input.standardizedFileURL
                if report.imageWidth == nil || report.imageHeight == nil,
                   let size = imagePixelSize(at: input) {
                    report.imageWidth = Int(size.width)
                    report.imageHeight = Int(size.height)
                }
                selectedIndices = try matchedCloudRegionIndices(
                    encodedSelection,
                    in: report,
                    operationKind: operation.kind
                )
            } else {
                selectedIndices = try intListParameter("selectedIndices", in: operation)
            }
            _ = try await processingService.removeClouds(
                input: input,
                output: output,
                selectedIndices: selectedIndices.isEmpty ? nil : selectedIndices,
                strength: doubleParameter("strength", in: operation, defaultValue: parameters.cloudRemovalStrength)
            )
            return output
        case .meteorRestore:
            let meteorMode = operation.parameters["mode"] ?? "restore"
            if meteorMode == "extract" {
                if mode == .fullResolutionExport {
                    return input
                }
                let output = makePreviewOutput(prefix: "replay-meteor-layer", extension: "png")
                _ = try await processingService.extractMeteors(input: input, output: output)
                return input
            }
            if meteorMode == "restoreSequence" {
                let sourceCount = intParameter("sources", in: operation, defaultValue: 0)
                guard sourceCount > 0 else {
                    throw EditGraphReplayError(
                        message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
                    )
                }
                if operation.parameters["sourceLightIDs"] != nil {
                    let recordedLights = try replayAssetsParameter(
                        "sourceLightIDs",
                        in: operation,
                        fallback: []
                    )
                    guard recordedLights.count == sourceCount else {
                        throw EditGraphReplayError(
                            message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
                        )
                    }
                }
                let recordedSources = (0..<sourceCount).compactMap { index -> URL? in
                    guard let path = operation.parameters["sourcePath.\(index)"]?.trimmingCharacters(in: .whitespacesAndNewlines),
                          path.isEmpty == false
                    else {
                        return nil
                    }
                    return URL(fileURLWithPath: path)
                }
                let meteorSources: [URL]
                if let replayStackSourceInputs, replayStackSourceInputs.count == sourceCount {
                    meteorSources = replayStackSourceInputs
                } else {
                    guard recordedSources.count == sourceCount else {
                        throw EditGraphReplayError(
                            message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
                        )
                    }
                    meteorSources = recordedSources
                }
                replayStackSourceInputs = nil
                var restoredOutput = input
                for (index, source) in meteorSources.enumerated() {
                    let output = makePreviewOutput(
                        prefix: "replay-restore-meteors-\(index + 1)",
                        extension: mode == .fullResolutionExport && AssetKind.detect(from: restoredOutput) == .fits
                            ? "fits"
                            : "tiff"
                    )
                    _ = try await processingService.restoreMeteors(
                        base: restoredOutput,
                        source: source,
                        output: output
                    )
                    try Task.checkCancellation()
                    restoredOutput = output
                }
                return restoredOutput
            }
            let source: URL
            if operation.parameters["sourceAssetID"] != nil {
                let sourceAssets = try replayAssetsParameter(
                    "sourceAssetID",
                    in: operation,
                    fallback: []
                )
                guard sourceAssets.count == 1 else {
                    throw EditGraphReplayError(
                        message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
                    )
                }
                source = sourceAssets[0].originalURL
            } else if let sourceName = operation.parameters["source"],
                      project.assets.filter({ $0.displayName == sourceName }).count == 1,
                      let legacyAsset = project.assets.first(where: { $0.displayName == sourceName }) {
                source = legacyAsset.originalURL
            } else if let sourcePath = operation.parameters["sourcePath"]?.trimmingCharacters(in: .whitespacesAndNewlines),
                      sourcePath.isEmpty == false {
                source = URL(fileURLWithPath: sourcePath)
            } else {
                throw EditGraphReplayError(
                    message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
                )
            }
            let output = makePreviewOutput(
                prefix: "replay-restore-meteors",
                extension: mode == .fullResolutionExport && AssetKind.detect(from: input) == .fits
                    ? "fits"
                    : "tiff"
            )
            _ = try await processingService.restoreMeteors(
                base: input,
                source: source,
                output: output
            )
            return output
        case .inspect, .histogram, .artifactDetect, .cloudDetect, .export:
            throw EditGraphReplayError(
                message: "\(localized(.editGraphUnsupportedOperation)): \(operation.kind.rawValue)"
            )
        }
    }

    private func isMeteorSequenceRestore(_ operation: EditOperation) -> Bool {
        operation.kind == .meteorRestore && operation.parameters["mode"] == "restoreSequence"
    }

    private func replayStackMacro(
        _ operation: EditOperation,
        mode: EditGraphReplayMode
    ) async throws -> URL {
        let artifactOutput = operation.parameters["artifactID"]
            .flatMap(UUID.init(uuidString:))
            .flatMap { artifactID in
                project.artifacts.first(where: {
                    $0.id == artifactID && $0.kind == .stackMaster
                })?.previewURL
            }
            .flatMap { Self.isUsableCachedOutput($0) ? $0 : nil }
        let reusableArtifactOutput = artifactOutput.flatMap { output -> URL? in
            switch mode {
            case .preview:
                return AssetKind.detect(from: output) == .fits ? nil : output
            case .fullResolutionExport:
                return AssetKind.detect(from: output) == .fits ? output : nil
            }
        }
        if let reusableArtifactOutput, replayNeedsMeteorSequenceSources == false {
            return reusableArtifactOutput
        }
        if let artifactOutput,
           mode == .preview,
           AssetKind.detect(from: artifactOutput) == .fits,
           replayNeedsMeteorSequenceSources == false {
            let output = makeCachedPreviewOutput(
                prefix: "replay-stack-display",
                input: artifactOutput,
                parameters: ["replayMode": mode.cacheKey]
            )
            _ = try await runValidatedCachedStep(output: output, command: "stack preview") {
                try await processingService.makePreview(
                    input: artifactOutput,
                    output: output,
                    width: parameters.previewWidth,
                    rawOptions: parameters.rawProcessingOptions
                )
            }
            return output
        }

        let method = stackMethodParameter("stackMethod", in: operation, defaultValue: stackMethodParameter("method", in: operation, defaultValue: parameters.workflowStackMethod))
        let alignment = alignmentMethodParameter("alignment", in: operation, defaultValue: parameters.workflowAlignment)
        let recordedInputCount = intParameter("inputs", in: operation, defaultValue: 0)
        let recordedInputs = (0..<recordedInputCount).compactMap { index -> URL? in
            guard let path = operation.parameters["inputPath.\(index)"]?
                .trimmingCharacters(in: .whitespacesAndNewlines),
                  path.isEmpty == false
            else {
                return nil
            }
            return URL(fileURLWithPath: path)
        }
        let recordedArtifactName = operation.parameters["sourceArtifact"]?
            .trimmingCharacters(in: .whitespacesAndNewlines)
        let hasAuthoritativeRecordedSources = operation.parameters["sourceArtifactID"] != nil ||
            (recordedArtifactName?.isEmpty == false && recordedArtifactName != "lights") ||
            operation.parameters["calibrationLightIDs"] != nil
        if operation.parameters["inputs"] != nil,
           recordedInputs.count != recordedInputCount,
           hasAuthoritativeRecordedSources == false {
            throw EditGraphReplayError(
                message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
            )
        }
        let registeredArtifact = try registeredSourceArtifact(
            for: operation,
            recordedInputs: recordedInputs
        )
        if let registeredArtifact,
           operation.parameters["calibrationLightIDs"] != nil {
            let recordedLights = try replayAssetsParameter(
                "calibrationLightIDs",
                in: operation,
                fallback: []
            )
            guard registeredArtifactMatches(registeredArtifact, lights: recordedLights) else {
                throw missingRecordedArtifactError(operation.kind)
            }
        }
        let sourceInputs: [URL]
        if let registeredArtifact {
            let expectedCount = recordedInputCount > 0
                ? recordedInputCount
                : max(registeredArtifact.frameCount, registeredArtifact.outputURLs.count)
            guard expectedCount > 0 else {
                throw missingRecordedArtifactError(operation.kind)
            }
            let recordedArtifactID = operation.parameters["sourceArtifactID"].flatMap(UUID.init(uuidString:))
            let contextMatches = replayRegisteredSequence.map { sequence in
                recordedArtifactID == nil || sequence.artifactID == nil || sequence.artifactID == recordedArtifactID
            } ?? false
            if let replayRegisteredSequence,
               contextMatches,
               replayRegisteredSequence.outputs.count >= expectedCount {
                sourceInputs = Array(replayRegisteredSequence.outputs.prefix(expectedCount))
                self.replayRegisteredSequence = nil
            } else if recordedInputs.count == expectedCount,
                      recordedInputs.allSatisfy(Self.isUsableCachedOutput) {
                sourceInputs = recordedInputs
            } else {
                let artifactOutputs = Array(registeredArtifact.outputURLs.prefix(expectedCount))
                if artifactOutputs.count == expectedCount,
                   artifactOutputs.allSatisfy(Self.isUsableCachedOutput) {
                    sourceInputs = artifactOutputs
                } else {
                    let rebuiltOutputs = try await rebuildRegisteredSequenceInputs(
                        artifact: registeredArtifact
                    )
                    guard rebuiltOutputs.count >= expectedCount else {
                        throw missingRecordedArtifactError(operation.kind)
                    }
                    sourceInputs = Array(rebuiltOutputs.prefix(expectedCount))
                }
            }
        } else if operation.parameters["calibrationLightIDs"] == nil,
                  recordedInputCount > 0,
                  recordedInputs.count == recordedInputCount,
                  recordedInputs.allSatisfy(Self.isUsableCachedOutput) {
            sourceInputs = recordedInputs
        } else {
            let calibrationOperation = calibrationOperation(before: operation)
            var calibrationParameters = operation.parameters
            if let calibrationOperation {
                calibrationParameters.merge(calibrationOperation.parameters) { _, recorded in recorded }
            }
            let replayLights = try replayAssetsParameter(
                "calibrationLightIDs",
                in: calibrationParameters,
                operationKind: operation.kind,
                fallback: lightAssets
            )
            let shouldCalibrate = calibrationOperation?.isEnabled
                ?? boolParameter("calibrationApplied", in: operation, defaultValue: false)
            if shouldCalibrate {
                sourceInputs = try await rebuildCalibratedStackInputs(
                    lights: replayLights,
                    stackOperation: operation,
                    calibrationParameters: calibrationParameters
                )
            } else {
                sourceInputs = try await rebuildDecodedStackInputs(
                    lights: replayLights,
                    stackOperation: operation
                )
            }
        }
        guard sourceInputs.isEmpty == false else {
            throw EditGraphReplayError(
                message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
            )
        }
        replayStackSourceInputs = sourceInputs
        if let reusableArtifactOutput {
            return reusableArtifactOutput
        }

        let cacheInput = sourceInputs.map(\.path).joined(separator: "|")
        let cacheURL = URL(fileURLWithPath: cacheInput)
        let output = makeCachedPreviewOutput(
            prefix: "replay-stack",
            input: cacheURL,
            parameters: [
                "alignment": alignment.rawValue,
                "lights": inputSignature(sourceInputs),
                "method": method.rawValue,
            ],
            extension: mode == .fullResolutionExport ? "fits" : "tiff"
        )
        _ = try await runValidatedCachedStep(output: output, command: "stack") {
            try await processingService.stack(
                inputs: sourceInputs,
                output: output,
                method: method,
                alignment: alignment
            )
        }
        return output
    }

    private func registeredSourceArtifact(
        for operation: EditOperation,
        recordedInputs: [URL]
    ) throws -> ProcessingArtifact? {
        if let rawArtifactID = operation.parameters["sourceArtifactID"] {
            guard let artifactID = UUID(uuidString: rawArtifactID),
                  let artifact = project.artifacts.first(where: {
                      $0.id == artifactID && $0.kind == .registeredSequence
                  })
            else {
                throw missingRecordedArtifactError(operation.kind)
            }
            return artifact
        }
        guard let artifactName = operation.parameters["sourceArtifact"],
              artifactName != "lights"
        else {
            return nil
        }
        let candidates = project.artifacts.filter {
            $0.kind == .registeredSequence && $0.name == artifactName
        }
        if recordedInputs.isEmpty == false {
            let exactMatches = candidates.filter { artifact in
                let candidateOutputs = Array(artifact.outputURLs.prefix(recordedInputs.count))
                return candidateOutputs.count == recordedInputs.count &&
                    zip(candidateOutputs, recordedInputs).allSatisfy { pair in
                        Self.urlsReferenceSameFile(pair.0, pair.1)
                    }
            }
            if exactMatches.count == 1 {
                return exactMatches[0]
            }
        }
        guard candidates.count == 1 else {
            throw missingRecordedArtifactError(operation.kind)
        }
        return candidates[0]
    }

    private func registeredArtifactMatches(
        _ artifact: ProcessingArtifact,
        lights: [PhotonStackAsset]
    ) -> Bool {
        let lightIDs = Set(lights.map(\.id))
        if let rawSourceIDs = artifact.parameters["sourceFrameIDs"] {
            let tokens = rawSourceIDs.split(separator: ",", omittingEmptySubsequences: false)
                .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
            let sourceIDs = tokens.compactMap(UUID.init(uuidString:))
            return sourceIDs.count == lights.count &&
                sourceIDs.count == tokens.count &&
                Set(sourceIDs) == lightIDs
        }
        let sourceURLs = artifact.frames.compactMap(\.sourceURL).map {
            Self.workspaceFileIdentityPath(for: $0)
        }
        return sourceURLs.count == lights.count &&
            Set(sourceURLs) == Set(lights.map {
                Self.workspaceFileIdentityPath(for: $0.originalURL)
            })
    }

    private func rebuildRegisteredSequenceInputs(
        artifact: ProcessingArtifact
    ) async throws -> [URL] {
        let sourceAssets: [PhotonStackAsset]
        if artifact.parameters["sourceFrameIDs"] != nil {
            sourceAssets = try replayAssetsParameter(
                "sourceFrameIDs",
                in: artifact.parameters,
                operationKind: .register,
                fallback: []
            )
        } else {
            sourceAssets = try legacyRegisteredSequenceAssets(artifact)
        }
        guard sourceAssets.count >= 2 else {
            throw missingRecordedArtifactError(.register)
        }
        let referenceID = artifact.parameters["referenceID"].flatMap(UUID.init(uuidString:))
        let referenceIndex = referenceID.flatMap { id in sourceAssets.firstIndex(where: { $0.id == id }) }
            ?? artifact.frames.firstIndex(where: \.isReference)
            ?? 0
        guard sourceAssets.indices.contains(referenceIndex) else {
            throw missingRecordedArtifactError(.register)
        }
        let reference = sourceAssets[referenceIndex]
        let movingFrames = sourceAssets.enumerated().compactMap { index, asset in
            index == referenceIndex ? nil : asset
        }
        guard movingFrames.isEmpty == false else {
            throw missingRecordedArtifactError(.register)
        }

        let outputDirectory = trackReplayTemporaryDirectory(previewDirectory.appendingPathComponent(
            "replay-registered-artifact-\(artifact.id.uuidString)-\(UUID().uuidString)",
            isDirectory: true
        ))
        let alignment = artifact.parameters["alignment"]
            .flatMap(AlignmentMethod.init(rawValue:))
            ?? .distortion
        let result = try await processingService.registerBatch(
            reference: reference.originalURL,
            inputs: movingFrames.map(\.originalURL),
            outputDirectory: outputDirectory,
            alignment: alignment == .none ? .distortion : alignment,
            outputFormat: "fits"
        )
        try Task.checkCancellation()
        return try registeredSequenceOutputs(
            from: result,
            expectedInputs: [reference.originalURL] + movingFrames.map(\.originalURL),
            outputDirectory: outputDirectory,
            mode: alignment == .none ? .distortion : alignment,
            outputFormat: "fits"
        )
    }

    private func legacyRegisteredSequenceAssets(
        _ artifact: ProcessingArtifact
    ) throws -> [PhotonStackAsset] {
        guard artifact.frames.isEmpty == false else {
            throw missingRecordedArtifactError(.register)
        }
        var assets: [PhotonStackAsset] = []
        assets.reserveCapacity(artifact.frames.count)
        for frame in artifact.frames {
            guard let sourceURL = frame.sourceURL else {
                throw missingRecordedArtifactError(.register)
            }
            let matches = project.assets.filter {
                Self.urlsReferenceSameFile($0.originalURL, sourceURL)
            }
            guard matches.count == 1 else {
                throw missingRecordedArtifactError(.register)
            }
            assets.append(matches[0])
        }
        return assets
    }

    private func registeredSequenceOutputs(
        from result: ProcessingCommandResult,
        expectedInputs: [URL],
        outputDirectory: URL,
        mode: AlignmentMethod,
        outputFormat: String
    ) throws -> [URL] {
        guard expectedInputs.isEmpty == false else {
            throw ProcessingServiceError.outputMissing(path: outputDirectory.path)
        }
        let report = try validatedBatchReport(
            result,
            expectedCommand: "register-batch",
            expectedInputs: expectedInputs,
            outputDirectory: outputDirectory,
            expectedMode: mode,
            expectedOutputFormat: outputFormat
        )
        let outputs = report.items.compactMap { item -> URL? in
            (item["output"] as? String).map(URL.init(fileURLWithPath:))
        }
        guard outputs.count == expectedInputs.count else {
            throw ProcessingServiceError.outputMissing(path: outputDirectory.path)
        }
        return outputs
    }

    private func validatedBatchReport(
        _ result: ProcessingCommandResult,
        expectedCommand: String,
        expectedInputs: [URL],
        outputDirectory: URL,
        expectedMode: AlignmentMethod? = nil,
        expectedOutputFormat: String? = nil
    ) throws -> (object: [String: Any], items: [[String: Any]]) {
        do {
            let expectedItemCount = expectedInputs.count
            guard expectedItemCount > 0,
                  let object = try parseJSONObject(from: result.standardOutput),
                  object["type"] as? String == "complete",
                  object["command"] as? String == expectedCommand,
                  jsonInteger(object["frames"]) == expectedItemCount,
                  jsonInteger(object["failedFrames"]) == 0,
                  let reportedOutputDirectory = object["outputDirectory"] as? String,
                  reportedOutputDirectory.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false,
                  URL(fileURLWithPath: reportedOutputDirectory).standardizedFileURL == outputDirectory.standardizedFileURL,
                  let items = object["items"] as? [[String: Any]],
                  items.count == expectedItemCount,
                  items.enumerated().allSatisfy({ index, item in
                      guard jsonBoolean(item["ok"]) == true,
                            let input = item["input"] as? String,
                            input.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false,
                            URL(fileURLWithPath: input).standardizedFileURL == expectedInputs[index].standardizedFileURL,
                            let output = item["output"] as? String,
                            output.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false,
                            Self.batchOutputIsUsable(
                                URL(fileURLWithPath: output),
                                inside: outputDirectory
                            )
                      else {
                          return false
                      }
                      return true
                  })
            else {
                throw ProcessingServiceError.invalidReport(command: expectedCommand)
            }
            let outputURLs = items.compactMap { item in
                (item["output"] as? String).map { URL(fileURLWithPath: $0).standardizedFileURL }
            }
            guard Set(outputURLs.map(\.path)).count == expectedItemCount else {
                throw ProcessingServiceError.invalidReport(command: expectedCommand)
            }
            if let expectedMode {
                guard object["mode"] as? String == expectedMode.rawValue,
                      jsonInteger(object["alignedFrames"]) == expectedItemCount,
                      items.enumerated().allSatisfy({ index, item in
                          jsonBoolean(item["reference"]) == (index == 0)
                      })
                else {
                    throw ProcessingServiceError.invalidReport(command: expectedCommand)
                }
            }
            if let expectedOutputFormat {
                guard object["outputFormat"] as? String == expectedOutputFormat,
                      outputURLs.allSatisfy({ outputURL in
                          Self.outputExtension(
                              outputURL.pathExtension,
                              matchesFormat: expectedOutputFormat
                          )
                      })
                else {
                    throw ProcessingServiceError.invalidReport(command: expectedCommand)
                }
                if expectedCommand == "artifacts clean-sequence",
                   jsonInteger(object["cleanedFrames"]) != expectedItemCount {
                    throw ProcessingServiceError.invalidReport(command: expectedCommand)
                }
            }
            return (object, items)
        } catch let error as ProcessingServiceError {
            throw error
        } catch {
            throw ProcessingServiceError.invalidReport(command: expectedCommand)
        }
    }

    private static func batchOutputIsUsable(_ output: URL, inside outputDirectory: URL) -> Bool {
        let resolvedDirectory = outputDirectory.standardizedFileURL.resolvingSymlinksInPath()
        let resolvedOutput = output.standardizedFileURL.resolvingSymlinksInPath()
        let directoryPrefix = resolvedDirectory.path.hasSuffix("/")
            ? resolvedDirectory.path
            : resolvedDirectory.path + "/"
        guard resolvedOutput.path.hasPrefix(directoryPrefix) else {
            return false
        }
        return isUsableCachedOutput(resolvedOutput)
    }

    private static func outputExtension(_ pathExtension: String, matchesFormat format: String) -> Bool {
        let normalizedExtension = pathExtension.lowercased()
        switch format.lowercased() {
        case "png":
            return normalizedExtension == "png"
        case "tif", "tiff":
            return normalizedExtension == "tif" || normalizedExtension == "tiff"
        case "fit", "fits", "fts":
            return normalizedExtension == "fit" || normalizedExtension == "fits" || normalizedExtension == "fts"
        default:
            return false
        }
    }

    private func missingRecordedArtifactError(
        _ operationKind: ProcessingOperationKind
    ) -> EditGraphReplayError {
        EditGraphReplayError(
            message: "\(localized(.editGraphMissingRecordedArtifact)): \(operationKind.rawValue)"
        )
    }

    private func rebuildDecodedStackInputs(
        lights: [PhotonStackAsset],
        stackOperation: EditOperation
    ) async throws -> [URL] {
        let rawOptions = rawOptionsParameter(in: stackOperation)
        let decodeExportOptions = ExportOptions(
            bitDepth: .sixteen,
            colorSpace: rawOptions.linearOutput ? .linearSRGB : .srgb,
            fitsValueMode: .scientific,
            jpegQuality: 1.0
        )
        var inputs: [URL] = []
        inputs.reserveCapacity(lights.count)
        for light in lights {
            try Task.checkCancellation()
            guard light.kind == .raw else {
                inputs.append(light.originalURL)
                continue
            }
            var decodeParameters = rawDecodeParameters(from: rawOptions)
            decodeParameters["assetID"] = light.id.uuidString
            decodeParameters["replayMode"] = "stackInput"
            let output = makeCachedPreviewOutput(
                prefix: "replay-stack-raw",
                input: light.originalURL,
                parameters: decodeParameters,
                extension: "fits"
            )
            _ = try await runValidatedCachedStep(output: output, command: "stack raw decode") {
                try await processingService.convert(
                    input: light.originalURL,
                    output: output,
                    options: decodeExportOptions,
                    rawOptions: rawOptions
                )
            }
            inputs.append(output)
        }
        return inputs
    }

    private func calibrationOperation(before stackOperation: EditOperation) -> EditOperation? {
        guard let stackIndex = project.editGraph.operations.firstIndex(where: { $0.id == stackOperation.id }) else {
            return nil
        }
        return project.editGraph.operations[..<stackIndex]
            .reversed()
            .first { $0.kind == .calibrate }
    }

    private func replayAssetsParameter(
        _ name: String,
        in operation: EditOperation,
        fallback: [PhotonStackAsset]
    ) throws -> [PhotonStackAsset] {
        try replayAssetsParameter(
            name,
            in: operation.parameters,
            operationKind: operation.kind,
            fallback: fallback
        )
    }

    private func replayRawAsset(for operation: EditOperation) throws -> PhotonStackAsset {
        if let rawAssetID = operation.parameters["assetID"] {
            guard let assetID = UUID(uuidString: rawAssetID),
                  let asset = project.assets.first(where: { $0.id == assetID && $0.kind == .raw })
            else {
                throw EditGraphReplayError(
                    message: "\(localized(.editGraphMissingRecordedAssets)): \(operation.kind.rawValue)"
                )
            }
            return asset
        }

        let rawAssets = project.assets.filter { $0.kind == .raw }
        guard rawAssets.count == 1 else {
            throw EditGraphReplayError(
                message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
            )
        }
        return rawAssets[0]
    }

    private func replayAssetParameter(
        _ name: String,
        legacyNameParameter: String,
        in operation: EditOperation
    ) throws -> PhotonStackAsset {
        let legacyName = operation.parameters[legacyNameParameter]
        let legacyMatches = legacyName.map { name in
            project.assets.filter { $0.displayName == name }
        } ?? []
        let resolved = try replayAssetsParameter(
            name,
            in: operation,
            fallback: legacyMatches.count == 1 ? legacyMatches : []
        )
        guard resolved.count == 1 else {
            throw EditGraphReplayError(
                message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
            )
        }
        return resolved[0]
    }

    private func replayAssetsParameter(
        _ name: String,
        in parameters: [String: String],
        operationKind: ProcessingOperationKind,
        fallback: [PhotonStackAsset],
        allowsEmpty: Bool = false
    ) throws -> [PhotonStackAsset] {
        guard let value = parameters[name] else {
            return fallback
        }
        let trimmedValue = value.trimmingCharacters(in: .whitespacesAndNewlines)
        if trimmedValue.isEmpty, allowsEmpty {
            return []
        }
        let tokens = trimmedValue.split(separator: ",", omittingEmptySubsequences: false)
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        let ids = tokens.compactMap(UUID.init(uuidString:))
        guard ids.isEmpty == false,
              ids.count == tokens.count,
              tokens.allSatisfy({ $0.isEmpty == false })
        else {
            throw EditGraphReplayError(
                message: "\(localized(.editGraphInvalidRecordedAssets)): \(operationKind.rawValue)"
            )
        }
        var assetsByID: [PhotonStackAsset.ID: PhotonStackAsset] = [:]
        for asset in project.assets {
            guard assetsByID[asset.id] == nil else {
                throw EditGraphReplayError(
                    message: "\(localized(.editGraphInvalidRecordedAssets)): \(operationKind.rawValue)"
                )
            }
            assetsByID[asset.id] = asset
        }
        let resolved = ids.compactMap { assetsByID[$0] }
        guard resolved.count == ids.count else {
            throw EditGraphReplayError(
                message: "\(localized(.editGraphMissingRecordedAssets)): \(operationKind.rawValue)"
            )
        }
        return resolved
    }

    private func rebuildCalibratedStackInputs(
        lights: [PhotonStackAsset],
        stackOperation: EditOperation,
        calibrationParameters: [String: String]
    ) async throws -> [URL] {
        let darks = try replayAssetsParameter(
            "calibrationDarkIDs",
            in: calibrationParameters,
            operationKind: stackOperation.kind,
            fallback: darkAssets,
            allowsEmpty: true
        )
        let biases = try replayAssetsParameter(
            "calibrationBiasIDs",
            in: calibrationParameters,
            operationKind: stackOperation.kind,
            fallback: biasAssets,
            allowsEmpty: true
        )
        let flats = try replayAssetsParameter(
            "calibrationFlatIDs",
            in: calibrationParameters,
            operationKind: stackOperation.kind,
            fallback: flatAssets,
            allowsEmpty: true
        )
        guard darks.isEmpty == false || biases.isEmpty == false || flats.isEmpty == false else {
            return lights.map(\.originalURL)
        }

        let masterMethod = calibrationParameters["masterMethod"]
            .flatMap(StackMethod.init(rawValue:))
            ?? stackMethodParameter("masterMethod", in: stackOperation, defaultValue: .median)
        let darkBiasState = calibrationParameters["darkBiasState"]
            .flatMap(CalibrationBiasState.init(rawValue:))
            ?? .included
        let flatBiasState = calibrationParameters["flatBiasState"]
            .flatMap(CalibrationBiasState.init(rawValue:))
            ?? .included
        var calibrationRawOptions = rawOptionsParameter(in: stackOperation)
        if calibrationParameters["rawWhiteBalance"] != nil {
            calibrationRawOptions = rawOptionsParameter(in: calibrationParameters)
        }
        calibrationRawOptions.linearOutput = true
        let outputDirectory = trackReplayTemporaryDirectory(previewDirectory.appendingPathComponent(
            "replay-calibration-\(stackOperation.id.uuidString)",
            isDirectory: true
        ))
        try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)

        var log: [String] = []
        let masterDark = try await createMasterIfNeeded(
            title: "master dark",
            assets: darks,
            output: outputDirectory.appendingPathComponent("master-dark.fits"),
            method: masterMethod,
            rawOptions: calibrationRawOptions,
            log: &log
        )
        let masterBias = try await createMasterIfNeeded(
            title: "master bias",
            assets: biases,
            output: outputDirectory.appendingPathComponent("master-bias.fits"),
            method: masterMethod,
            rawOptions: calibrationRawOptions,
            log: &log
        )
        let masterFlat = try await createMasterIfNeeded(
            title: "master flat",
            assets: flats,
            output: outputDirectory.appendingPathComponent("master-flat.fits"),
            method: masterMethod,
            rawOptions: calibrationRawOptions,
            log: &log
        )

        var calibratedInputs: [URL] = []
        calibratedInputs.reserveCapacity(lights.count)
        for (index, light) in lights.enumerated() {
            try Task.checkCancellation()
            let baseName = light.originalURL.deletingPathExtension().lastPathComponent
            let output = outputDirectory.appendingPathComponent(
                "\(index + 1)-\(baseName).calibrated.fits"
            )
            _ = try await processingService.calibrate(
                light: light.originalURL,
                output: output,
                dark: masterDark,
                bias: masterBias,
                flat: masterFlat,
                darkBiasState: darkBiasState,
                flatBiasState: flatBiasState,
                rawOptions: calibrationRawOptions
            )
            calibratedInputs.append(output)
        }
        return calibratedInputs
    }

    private func replayDrizzleMacro(
        _ operation: EditOperation,
        mode: EditGraphReplayMode
    ) async throws -> URL {
        let lights = try replayAssetsParameter("lightIDs", in: operation, fallback: lightAssets)
        guard lights.isEmpty == false else {
            throw EditGraphReplayError(
                message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
            )
        }

        let scale = intParameter("scale", in: operation, defaultValue: 2)
        let pixfrac = doubleParameter("pixfrac", in: operation, defaultValue: 1.0)
        let alignment: AlignmentMethod
        if let rawAlignment = operation.parameters["alignment"],
           let storedAlignment = AlignmentMethod(rawValue: rawAlignment) {
            alignment = storedAlignment
        } else {
            alignment = boolParameter("alignTranslation", in: operation, defaultValue: true) ? .translation : .none
        }
        let cacheInput = lights.map(\.originalURL.path).joined(separator: "|")
        let cacheURL = URL(fileURLWithPath: cacheInput)
        let output = makeCachedPreviewOutput(
            prefix: "replay-drizzle",
            input: cacheURL,
            parameters: [
                "scale": String(scale),
                "pixfrac": String(pixfrac),
                "alignment": alignment.rawValue,
                "lights": inputSignature(lights.map(\.originalURL)),
                "replayMode": mode.cacheKey,
            ],
            extension: mode.scientificOutputExtension
        )
        _ = try await runValidatedCachedStep(output: output, command: "drizzle") {
            try await processingService.drizzle(
                inputs: lights.map(\.originalURL),
                output: output,
                scale: scale,
                pixfrac: pixfrac,
                alignment: alignment
            )
        }
        return output
    }

    private func replayMosaicMacro(
        _ operation: EditOperation,
        mode: EditGraphReplayMode
    ) async throws -> URL {
        let panels = try replayAssetsParameter("panelIDs", in: operation, fallback: mosaicAssets)
        guard panels.isEmpty == false else {
            throw EditGraphReplayError(
                message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
            )
        }

        let overlapPixels = intParameter("overlapPixels", in: operation, defaultValue: parameters.mosaicOverlapPixels)
        let projection = mosaicProjectionParameter("projection", in: operation, defaultValue: parameters.mosaicProjection)
        let layout = mosaicLayoutParameter("layout", in: operation, defaultValue: parameters.mosaicLayout)
        let alignment = mosaicAlignmentParameter("alignment", in: operation, defaultValue: parameters.mosaicAlignment)
        let blendMode = mosaicBlendModeParameter("blend", in: operation, defaultValue: parameters.mosaicBlendMode)
        let exposureMatching = boolParameter(
            "exposureMatching",
            in: operation,
            defaultValue: parameters.mosaicExposureMatching
        )
        let columns = intParameter("columns", in: operation, defaultValue: parameters.mosaicColumns)
        let cacheInput = panels.map(\.originalURL.path).joined(separator: "|")
        let cacheURL = URL(fileURLWithPath: cacheInput)
        let output = makeCachedPreviewOutput(
            prefix: "replay-mosaic",
            input: cacheURL,
            parameters: [
                "overlapPixels": String(overlapPixels),
                "projection": projection.rawValue,
                "layout": layout.rawValue,
                "alignment": alignment.rawValue,
                "blend": blendMode.rawValue,
                "exposureMatching": String(exposureMatching),
                "columns": String(columns),
                "panels": inputSignature(panels.map(\.originalURL)),
                "width": mode == .preview ? String(parameters.previewWidth) : "full",
                "replayMode": mode.cacheKey,
            ],
            extension: mode.scientificOutputExtension
        )
        _ = try await runValidatedCachedStep(output: output, command: "mosaic") {
            let mosaicOutput: URL
            if mode == .fullResolutionExport {
                mosaicOutput = output
            } else {
                let runDirectory = trackReplayTemporaryDirectory(
                    previewDirectory.appendingPathComponent("replay-mosaic-\(UUID().uuidString)", isDirectory: true)
                )
                try FileManager.default.createDirectory(at: runDirectory, withIntermediateDirectories: true)
                mosaicOutput = runDirectory.appendingPathComponent("mosaic.tiff")
            }
            let result = try await processingService.mosaic(
                inputs: panels.map(\.originalURL),
                output: mosaicOutput,
                overlapPixels: overlapPixels,
                projection: projection,
                layout: layout,
                alignment: alignment,
                blendMode: blendMode,
                exposureMatching: exposureMatching,
                columns: columns,
                previewWidth: nil
            )
            try Task.checkCancellation()
            if mode != .fullResolutionExport {
                _ = try await processingService.makePreview(
                    input: mosaicOutput,
                    output: output,
                    width: parameters.previewWidth,
                    rawOptions: parameters.rawProcessingOptions
                )
            }
            return result
        }
        return output
    }

    private func makeReplayOutput(
        prefix: String,
        mode: EditGraphReplayMode,
        preservingScientificValuesFrom input: URL? = nil
    ) -> URL {
        let outputExtension = input.map(mode.outputExtension(preservingScientificValuesFrom:))
            ?? mode.outputExtension
        return makePreviewOutput(prefix: prefix, extension: outputExtension)
    }

    private static func normalizedFinite(
        _ value: Double,
        in range: ClosedRange<Double>,
        fallback: Double
    ) -> Double {
        guard value.isFinite else {
            return min(max(fallback, range.lowerBound), range.upperBound)
        }
        return min(max(value, range.lowerBound), range.upperBound)
    }

    private static func normalizedProcessingParameters(
        _ value: ProcessingParameters
    ) -> ProcessingParameters {
        let defaults = ProcessingParameters()
        var normalized = value

        normalized.previewWidth = min(max(value.previewWidth, 320), 4096)
        normalized.stretchTargetBackground = normalizedFinite(
            value.stretchTargetBackground,
            in: 0.05...0.65,
            fallback: defaults.stretchTargetBackground
        )
        normalized.starReductionAmount = normalizedFinite(
            value.starReductionAmount,
            in: 0...1,
            fallback: defaults.starReductionAmount
        )
        normalized.comaReductionAmount = normalizedFinite(
            value.comaReductionAmount,
            in: 0...1,
            fallback: defaults.comaReductionAmount
        )
        normalized.comaReductionRadius = min(max(value.comaReductionRadius, 2), 16)
        normalized.comaReductionEccentricity = normalizedFinite(
            value.comaReductionEccentricity,
            in: 0.1...0.9,
            fallback: defaults.comaReductionEccentricity
        )
        normalized.localContrastAmount = normalizedFinite(
            value.localContrastAmount,
            in: 0...1,
            fallback: defaults.localContrastAmount
        )
        normalized.localContrastRadius = min(max(value.localContrastRadius, 1), 64)
        normalized.denoiseAmount = normalizedFinite(
            value.denoiseAmount,
            in: 0...1,
            fallback: defaults.denoiseAmount
        )
        normalized.denoiseChromaAmount = normalizedFinite(
            value.denoiseChromaAmount,
            in: 0...1,
            fallback: defaults.denoiseChromaAmount
        )
        normalized.denoiseRadius = min(max(value.denoiseRadius, 1), 8)
        normalized.sharpenAmount = normalizedFinite(
            value.sharpenAmount,
            in: 0...1,
            fallback: defaults.sharpenAmount
        )
        normalized.sharpenRadius = min(max(value.sharpenRadius, 1), 8)
        normalized.cloudRemovalStrength = normalizedFinite(
            value.cloudRemovalStrength,
            in: 0...1,
            fallback: defaults.cloudRemovalStrength
        )
        normalized.mosaicOverlapPixels = min(max(value.mosaicOverlapPixels, 0), 4096)
        normalized.mosaicColumns = min(max(value.mosaicColumns, 1), 12)
        normalized.mosaicPreviewWidth = min(max(value.mosaicPreviewWidth, 320), 4096)
        normalized.rawProcessingOptions.exposureBias = normalizedFinite(
            value.rawProcessingOptions.exposureBias,
            in: -2...2,
            fallback: defaults.rawProcessingOptions.exposureBias
        )
        normalized.rawProcessingOptions.manualWhiteBalanceTemperature = normalizedFinite(
            value.rawProcessingOptions.manualWhiteBalanceTemperature,
            in: 2000...50000,
            fallback: defaults.rawProcessingOptions.manualWhiteBalanceTemperature
        )
        normalized.rawProcessingOptions.manualWhiteBalanceTint = normalizedFinite(
            value.rawProcessingOptions.manualWhiteBalanceTint,
            in: -150...150,
            fallback: defaults.rawProcessingOptions.manualWhiteBalanceTint
        )
        normalized.rawProcessingOptions.manualBlackLevel = normalizedFinite(
            value.rawProcessingOptions.manualBlackLevel,
            in: 0...1,
            fallback: defaults.rawProcessingOptions.manualBlackLevel
        )
        if normalized.rawProcessingOptions.demosaicQuality == .balanced {
            normalized.rawProcessingOptions.demosaicQuality = .high
        }

        var seenCurvePointIDs = Set<CurveEditorPoint.ID>()
        let finiteCurvePoints = value.curvePoints.compactMap { point -> CurveEditorPoint? in
            guard point.input.isFinite, point.output.isFinite else {
                return nil
            }
            let id = seenCurvePointIDs.insert(point.id).inserted ? point.id : UUID()
            return CurveEditorPoint(id: id, input: point.input, output: point.output)
        }
        normalized.replaceCurvePoints(finiteCurvePoints)
        return normalized
    }

    private func doubleParameter(_ name: String, in operation: EditOperation, defaultValue: Double) -> Double {
        let text = operation.parameters[name]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        guard let value = Double(text), value.isFinite else {
            return defaultValue
        }
        return value
    }

    private func doubleParameter(
        _ name: String,
        parameters: [String: String],
        defaultValue: Double
    ) -> Double {
        let text = parameters[name]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        guard let value = Double(text), value.isFinite else {
            return defaultValue
        }
        return value
    }

    private func intParameter(_ name: String, in operation: EditOperation, defaultValue: Int) -> Int {
        let text = operation.parameters[name]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return Int(text) ?? defaultValue
    }

    private func intParameter(
        _ name: String,
        parameters: [String: String],
        defaultValue: Int
    ) -> Int {
        let text = parameters[name]?.trimmingCharacters(in: .whitespacesAndNewlines) ?? ""
        return Int(text) ?? defaultValue
    }

    private func intListParameter(_ name: String, in operation: EditOperation) throws -> [Int] {
        guard let value = operation.parameters[name] else {
            return []
        }
        let trimmed = value.trimmingCharacters(in: .whitespacesAndNewlines)
        guard trimmed.isEmpty == false else {
            return []
        }
        let tokens = trimmed.split(separator: ",", omittingEmptySubsequences: false)
            .map { $0.trimmingCharacters(in: .whitespacesAndNewlines) }
        let values = tokens.compactMap(Int.init)
        guard values.count == tokens.count,
              values.allSatisfy({ $0 >= 0 }),
              Set(values).count == values.count
        else {
            throw EditGraphReplayError(
                message: "\(localized(.invalidEditOperationParameter)): \(operation.kind.rawValue).\(name)"
            )
        }
        return values
    }

    private func curvePointsParameter(in operation: EditOperation) -> [CurveEditorPoint]? {
        guard let encoded = operation.parameters["points"], encoded.isEmpty == false else {
            return nil
        }
        let tokens = encoded.split(separator: ",", omittingEmptySubsequences: false)
        let points = tokens.compactMap { token -> CurveEditorPoint? in
            let parts = token.split(separator: ":", maxSplits: 1)
            guard parts.count == 2,
                  let input = Double(parts[0]),
                  input.isFinite,
                  let output = Double(parts[1]),
                  output.isFinite else {
                return nil
            }
            return CurveEditorPoint(input: input, output: output)
        }
        return points.count == tokens.count && points.count >= 2 ? points : nil
    }

    private func encodedCurvePoints(_ points: [CurveEditorPoint]) -> String {
        points
            .map { "\($0.input):\($0.output)" }
            .joined(separator: ",")
    }

    private func boolParameter(_ name: String, in operation: EditOperation, defaultValue: Bool) -> Bool {
        guard let value = operation.parameters[name]?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() else {
            return defaultValue
        }
        switch value {
        case "true", "1", "yes", "on":
            return true
        case "false", "0", "no", "off":
            return false
        default:
            return defaultValue
        }
    }

    private func boolParameter(
        _ name: String,
        parameters: [String: String],
        defaultValue: Bool
    ) -> Bool {
        guard let value = parameters[name]?.trimmingCharacters(in: .whitespacesAndNewlines).lowercased() else {
            return defaultValue
        }
        switch value {
        case "true", "1", "yes", "on":
            return true
        case "false", "0", "no", "off":
            return false
        default:
            return defaultValue
        }
    }

    private func rawOptionsParameter(in operation: EditOperation) -> RawProcessingOptions {
        rawOptionsParameter(in: operation.parameters)
    }

    private func rawOptionsParameter(in values: [String: String]) -> RawProcessingOptions {
        var options = RawProcessingOptions(
            whiteBalanceMode: RawWhiteBalanceMode(rawValue: values["rawWhiteBalance"] ?? "") ?? parameters.rawProcessingOptions.whiteBalanceMode,
            manualWhiteBalanceTemperature: doubleParameter(
                "rawTemperature",
                parameters: values,
                defaultValue: parameters.rawProcessingOptions.manualWhiteBalanceTemperature
            ),
            manualWhiteBalanceTint: doubleParameter(
                "rawTint",
                parameters: values,
                defaultValue: parameters.rawProcessingOptions.manualWhiteBalanceTint
            ),
            exposureBias: doubleParameter("rawExposureBias", parameters: values, defaultValue: parameters.rawProcessingOptions.exposureBias),
            blackLevelMode: RawBlackLevelMode(rawValue: values["rawBlackLevel"] ?? "") ?? parameters.rawProcessingOptions.blackLevelMode,
            manualBlackLevel: doubleParameter(
                "rawBlackValue",
                parameters: values,
                defaultValue: parameters.rawProcessingOptions.manualBlackLevel
            ),
            demosaicQuality: RawDemosaicQuality(rawValue: values["rawDemosaic"] ?? "") ?? parameters.rawProcessingOptions.demosaicQuality,
            linearOutput: boolParameter("rawLinear", parameters: values, defaultValue: parameters.rawProcessingOptions.linearOutput)
        )
        options.manualWhiteBalanceTemperature = Self.normalizedFinite(
            options.manualWhiteBalanceTemperature,
            in: 2000...50000,
            fallback: parameters.rawProcessingOptions.manualWhiteBalanceTemperature
        )
        options.manualWhiteBalanceTint = Self.normalizedFinite(
            options.manualWhiteBalanceTint,
            in: -150...150,
            fallback: parameters.rawProcessingOptions.manualWhiteBalanceTint
        )
        options.exposureBias = Self.normalizedFinite(
            options.exposureBias,
            in: -2...2,
            fallback: parameters.rawProcessingOptions.exposureBias
        )
        options.manualBlackLevel = Self.normalizedFinite(
            options.manualBlackLevel,
            in: 0...1,
            fallback: parameters.rawProcessingOptions.manualBlackLevel
        )
        return options
    }

    private func mosaicProjectionParameter(
        _ name: String,
        in operation: EditOperation,
        defaultValue: MosaicProjection
    ) -> MosaicProjection {
        guard let value = operation.parameters[name],
              let projection = MosaicProjection(rawValue: value)
        else {
            return defaultValue
        }
        return projection
    }

    private func mosaicLayoutParameter(
        _ name: String,
        in operation: EditOperation,
        defaultValue: MosaicLayoutMode
    ) -> MosaicLayoutMode {
        guard let value = operation.parameters[name],
              let layout = MosaicLayoutMode(rawValue: value)
        else {
            return defaultValue
        }
        return layout
    }

    private func mosaicAlignmentParameter(
        _ name: String,
        in operation: EditOperation,
        defaultValue: MosaicAlignmentMode
    ) -> MosaicAlignmentMode {
        guard let value = operation.parameters[name],
              let alignment = MosaicAlignmentMode(rawValue: value)
        else {
            return defaultValue
        }
        return alignment
    }

    private func mosaicBlendModeParameter(
        _ name: String,
        in operation: EditOperation,
        defaultValue: MosaicBlendMode
    ) -> MosaicBlendMode {
        guard let value = operation.parameters[name],
              let blendMode = MosaicBlendMode(rawValue: value)
        else {
            return defaultValue
        }
        return blendMode
    }

    private func inputSignature(_ urls: [URL]) -> String {
        urls.map { url in
            let modifiedAt = ((try? FileManager.default.attributesOfItem(atPath: url.path)[.modificationDate]) as? Date)?.timeIntervalSince1970 ?? 0
            return "\(url.path)@\(modifiedAt)"
        }
        .joined(separator: ",")
    }

    private func stackMethodParameter(_ name: String, in operation: EditOperation, defaultValue: StackMethod) -> StackMethod {
        guard let value = operation.parameters[name],
              let method = StackMethod(rawValue: value)
        else {
            return defaultValue
        }
        return method
    }

    private func alignmentMethodParameter(_ name: String, in operation: EditOperation, defaultValue: AlignmentMethod) -> AlignmentMethod {
        guard let value = operation.parameters[name],
              let method = AlignmentMethod(rawValue: value)
        else {
            return defaultValue
        }
        return method
    }

    private func markRecentProject(directory: URL) {
        var settings = AppSettings(
            recentProjects: recentProjects,
            autosaveEnabled: autosaveEnabled,
            selectedTemplate: selectedTemplate,
            language: language,
            customCurvePresets: customCurvePresets,
            exportOptions: exportOptions
        )
        settings.markRecentProject(name: project.name, directory: directory)
        recentProjects = settings.recentProjects
        persistUserSettings()
    }

    private func nextCustomCurvePresetName() -> String {
        uniqueCustomCurvePresetName(preferred: "Curve \(customCurvePresets.count + 1)")
    }

    private func uniqueCustomCurvePresetName(preferred: String, excluding presetID: CustomCurvePreset.ID? = nil) -> String {
        let trimmed = preferred.trimmingCharacters(in: .whitespacesAndNewlines)
        let baseName = trimmed.isEmpty ? "Curve" : trimmed
        let existingNames = Set(customCurvePresets.compactMap { preset in
            preset.id == presetID ? nil : preset.name
        })
        return uniqueCustomCurvePresetName(preferred: baseName, existingNames: existingNames)
    }

    private func uniqueCustomCurvePresetName(preferred: String, existingNames: Set<String>) -> String {
        let trimmed = preferred.trimmingCharacters(in: .whitespacesAndNewlines)
        let baseName = trimmed.isEmpty ? "Curve" : trimmed
        guard existingNames.contains(baseName) else {
            return baseName
        }

        var index = 2
        var candidate = "\(baseName) \(index)"
        while existingNames.contains(candidate) {
            index += 1
            candidate = "\(baseName) \(index)"
        }
        return candidate
    }

    private func writeExportSidecar(
        input: URL,
        output: URL,
        options: ExportOptions,
        requestedOptions: ExportOptions,
        rawOptions: RawProcessingOptions
    ) throws {
        let operationDictionaries = project.editGraph.operations.map { operation -> [String: Any] in
            [
                "id": operation.id.uuidString,
                "kind": operation.kind.rawValue,
                "parameters": operation.parameters,
                "isEnabled": operation.isEnabled,
            ]
        }
        let object: [String: Any] = [
            "type": "photonstack-export",
            "source": input.path,
            "output": output.path,
            "exportedAt": ISO8601DateFormatter().string(from: Date()),
            "projectID": project.id.uuidString,
            "projectName": project.name,
            "exportOptions": [
                "bitDepth": options.bitDepth.rawValue,
                "requestedBitDepth": requestedOptions.bitDepth.rawValue,
                "colorSpace": options.colorSpace.rawValue,
                "fitsValueMode": options.fitsValueMode.rawValue,
                "nonFiniteHandling": Self.exportNonFiniteHandling(options: options, output: output),
                "dynamicRangeHandling": Self.exportDynamicRangeHandling(options: options, output: output),
                "jpegQuality": options.jpegQuality,
                "alphaHandling": Self.exportAlphaHandling(for: output),
            ],
            "rawOptions": [
                "whiteBalanceMode": rawOptions.whiteBalanceMode.rawValue,
                "exposureBias": rawOptions.exposureBias,
                "blackLevelMode": rawOptions.blackLevelMode.rawValue,
                "demosaicQuality": rawOptions.demosaicQuality.rawValue,
                "linearOutput": rawOptions.linearOutput,
            ],
            "operations": operationDictionaries,
        ]
        let data = try JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted, .sortedKeys])
        try data.write(to: exportSidecarURL(for: output), options: .atomic)
    }

    private func exportSidecarURL(for output: URL) -> URL {
        output.deletingLastPathComponent().appendingPathComponent("\(output.lastPathComponent).photonstack.json")
    }

    private static func effectiveExportOptions(_ options: ExportOptions, for output: URL) -> ExportOptions {
        let imageExtension = output.pathExtension.lowercased()
        let bitDepth: ExportBitDepth
        switch imageExtension {
        case "jpg", "jpeg", "heic", "heif", "hif":
            bitDepth = .eight
        case "tif", "tiff":
            bitDepth = options.bitDepth == .automatic ? .sixteen : options.bitDepth
        case "png":
            bitDepth = options.bitDepth == .automatic ? .eight : options.bitDepth
        default:
            bitDepth = options.bitDepth
        }
        return ExportOptions(
            bitDepth: bitDepth,
            colorSpace: options.colorSpace,
            fitsValueMode: options.fitsValueMode,
            jpegQuality: options.jpegQuality
        )
    }

    private static func exportAlphaHandling(for output: URL) -> String {
        switch output.pathExtension.lowercased() {
        case "jpg", "jpeg":
            return "flatten-black"
        default:
            return "preserve"
        }
    }

    private static func exportNonFiniteHandling(options: ExportOptions, output: URL) -> String {
        if options.fitsValueMode == .display {
            return "display-zero"
        }
        switch output.pathExtension.lowercased() {
        case "fits", "fit", "fts":
            return "preserve"
        default:
            return "mask"
        }
    }

    private static func exportDynamicRangeHandling(options: ExportOptions, output: URL) -> String {
        switch output.pathExtension.lowercased() {
        case "fits", "fit", "fts":
            return options.fitsValueMode == .scientific
                ? "float32-preserved"
                : "display-normalized-float32"
        default:
            return options.fitsValueMode == .scientific
                ? "integer-clamped-quantized"
                : "display-normalized-quantized"
        }
    }

    private func defaultProductManifestURL() -> URL {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd-HH-mm-ss"
        let fileName = "PhotonStack-Product-Manifest-\(formatter.string(from: Date())).md"
        return FileManager.default.homeDirectoryForCurrentUser
            .appendingPathComponent("Desktop", isDirectory: true)
            .appendingPathComponent(fileName)
    }

    private func layerStackSignature(_ layers: [ProcessingLayer]) -> String {
        layers
            .map { layer in
                [
                    layer.id.uuidString,
                    layer.name,
                    layer.inputURL?.path ?? "",
                    String(layer.isVisible),
                    String(format: "%.4f", layer.opacity),
                    layer.blendMode.rawValue,
                    layer.maskArtifactID?.uuidString ?? "",
                    layer.maskArtifactID.flatMap { maskID in
                        project.artifacts.first(where: { $0.id == maskID })?.previewURL?.path
                    } ?? "",
                    layer.parameters["maskInverted"] ?? "false",
                    layer.parameters["maskDensity"] ?? "1",
                    layer.parameters["maskFeatherFraction"] ?? "0",
                ].joined(separator: ":")
            }
            .joined(separator: "|")
    }

    private var canReplayFullResolutionForExport: Bool {
        guard fullResolutionExportReplayPlan() != nil
        else {
            return false
        }

        if canvasMode == .sourcePreview {
            return true
        }

        return project.layers.allSatisfy { layer in
            guard layer.kind != .mask else {
                return true
            }
            guard layer.isVisible,
                  layer.maskArtifactID == nil,
                  layer.blendMode == .normal,
                  layer.opacity >= 0.999
            else {
                return false
            }

            if layer.kind == .stars || layer.kind == .meteors {
                return layer.parameters["source"] == "operation"
            }
            return true
        }
    }

    private func isFullResolutionExportOperation(_ operation: EditOperation) -> Bool {
        switch operation.kind {
        case .rawDecode, .background, .normalize, .stack, .drizzle, .mosaic, .crop, .register,
             .stretch, .curves, .localContrast, .denoise, .sharpen, .deconvolve,
             .colorNeutralize, .colorSaturate, .starReduce, .comaReduce, .cloudRemove:
            return true
        case .artifactRemove:
            return operation.parameters["mode"] != "cleanSequence"
        case .meteorRestore:
            return operation.parameters["mode"] != "extract"
        case .inspect, .preview, .calibrate, .histogram, .starDetect, .starMask,
             .artifactDetect, .cloudDetect, .export:
            return false
        }
    }

    private func replayEditGraphForExport() async throws -> URL? {
        guard let replayPlan = fullResolutionExportReplayPlan() else {
            return nil
        }

        try FileManager.default.createDirectory(at: previewDirectory, withIntermediateDirectories: true)
        clearReplayTemporaryDirectories()
        replayStackSourceInputs = nil
        replayRegisteredSequence = nil
        replayNeedsMeteorSequenceSources = replayPlan.operations.contains(where: isMeteorSequenceRestore)
        defer {
            replayStackSourceInputs = nil
            replayRegisteredSequence = nil
            replayNeedsMeteorSequenceSources = false
        }
        var input = replayPlan.input
        for operation in replayPlan.operations {
            try Task.checkCancellation()
            input = try await applyReplayOperation(
                operation,
                input: input,
                mode: .fullResolutionExport
            )
        }
        return input
    }

    private func trackReplayTemporaryDirectory(_ directory: URL) -> URL {
        replayTemporaryDirectories.insert(directory.standardizedFileURL)
        return directory
    }

    private func clearReplayTemporaryDirectories(retaining outputs: Set<URL> = []) {
        let retainedOutputs = Set(outputs.map(\.standardizedFileURL))
        for directory in replayTemporaryDirectories {
            let directoryPrefix = directory.path.hasSuffix("/") ? directory.path : directory.path + "/"
            if retainedOutputs.contains(where: { $0.path.hasPrefix(directoryPrefix) }) {
                continue
            }
            try? FileManager.default.removeItem(at: directory)
        }
        replayTemporaryDirectories.removeAll()
    }

    private func fullResolutionExportReplayPlan() -> EditGraphReplayPlan? {
        if let previewOperationID,
           let plan = editGraphReplayPlan(
               finalOperationID: previewOperationID,
               finalOperationIsEligible: isFullResolutionExportOperation
           ) {
            return plan
        }
        guard let previewURL else {
            return nil
        }
        return editGraphReplayPlan(
            targetURL: previewURL,
            finalOperationIsEligible: isFullResolutionExportOperation
        )
    }

    private func updateReplayedOperationLayerOutputs(
        _ outputs: [EditOperation.ID: URL]
    ) -> Set<URL> {
        var retainedOutputs: Set<URL> = []
        var changed = false
        for index in project.layers.indices {
            guard let operationID = project.layers[index].parameters["operationID"]
                .flatMap(UUID.init(uuidString:)),
                let output = outputs[operationID]
            else {
                continue
            }
            let standardizedOutput = output.standardizedFileURL
            retainedOutputs.insert(standardizedOutput)
            if project.layers[index].inputURL?.standardizedFileURL != standardizedOutput {
                project.layers[index].inputURL = standardizedOutput
                changed = true
            }
        }
        if changed {
            project.updatedAt = Date()
        }
        return retainedOutputs
    }

    private func editGraphReplayPlan(
        targetURL: URL? = nil,
        finalOperationID: EditOperation.ID? = nil,
        finalOperationIsEligible: (EditOperation) -> Bool
    ) -> EditGraphReplayPlan? {
        guard targetURL != nil || finalOperationID != nil else {
            return nil
        }
        let operations = project.editGraph.operations
        let matchingIndices = operations.indices.filter { index in
            let operation = operations[index]
            let matchesTarget: Bool
            if let finalOperationID {
                matchesTarget = operation.id == finalOperationID
            } else if let targetURL {
                matchesTarget = Self.storedPath(
                    operation.parameters["output"],
                    references: targetURL
                ) || Self.storedPath(
                    operation.parameters["previewOutput"],
                    references: targetURL
                )
            } else {
                matchesTarget = false
            }
            return matchesTarget &&
                finalOperationIsEligible(operation) &&
                (operation.isEnabled || finalOperationID != nil)
        }
        guard matchingIndices.count == 1, let finalIndex = matchingIndices.first else {
            return nil
        }

        var lineage: [EditOperation] = []
        var cursor = finalIndex
        var visited: Set<Int> = []
        var input: URL?

        while true {
            guard visited.insert(cursor).inserted else {
                return nil
            }
            let operation = operations[cursor]
            if operation.isEnabled {
                lineage.append(operation)
            }

            if operation.kind == .rawDecode,
               let assetID = operation.parameters["assetID"].flatMap(UUID.init(uuidString:)),
               let asset = project.assets.first(where: { $0.id == assetID }) {
                input = asset.originalURL
                break
            }

            if let inputPath = operation.parameters["input"] {
                let predecessors = operations.indices[..<cursor].filter { index in
                    Self.storedPath(
                        operations[index].parameters["output"],
                        references: URL(fileURLWithPath: inputPath)
                    )
                }
                guard predecessors.count <= 1 else {
                    return nil
                }
                if let predecessor = predecessors.first {
                    cursor = predecessor
                    continue
                }
                input = URL(fileURLWithPath: inputPath)
                break
            }

            guard cursor > operations.startIndex else {
                input = selectedAsset?.originalURL
                break
            }
            cursor = operations.index(before: cursor)
        }

        guard let input = input ?? selectedAsset?.originalURL else {
            return nil
        }
        return EditGraphReplayPlan(
            input: input,
            operations: Array(lineage.reversed()),
            anchorOperationID: operations[finalIndex].id
        )
    }

    private func renderLayerStackPreview(layers: [ProcessingLayer], output: URL) throws {
        try renderLayerStack(layers: layers, output: output, format: .RGBA8)
    }

    private func exportInputURL() async throws -> URL? {
        if canvasMode == .sourcePreview {
            return currentInputURL
        }

        let visibleLayers = visiblePreviewLayers
        guard visibleLayers.isEmpty == false else {
            if let sourceLayer = emptyLayerStackSourceLayer {
                let replayContext = FullResolutionLayerReplayContext()
                let sourceURL = try await fullResolutionInput(for: sourceLayer, context: replayContext)
                let compositingURL = try await fullResolutionCompositingInput(
                    sourceURL,
                    context: replayContext
                )
                let output = makePreviewOutput(prefix: "export-layer-stack-empty", extension: "png")
                try renderEmptyLayerStack(sourceURL: compositingURL, output: output, format: .RGBA16)
                return output
            }
            return selectedAsset?.originalURL ?? currentInputURL
        }

        let replayContext = FullResolutionLayerReplayContext()
        let canReturnScientificLayerDirectly = visibleLayers.count == 1 &&
            visibleLayers[0].maskArtifactID == nil &&
            visibleLayers[0].blendMode == .normal &&
            visibleLayers[0].opacity >= 0.999
        var layerInputURLs: [ProcessingLayer.ID: URL] = [:]
        var maskInputURLs: [ProcessingLayer.ID: URL] = [:]
        for layer in visibleLayers {
            try Task.checkCancellation()
            let resolvedInput = try await fullResolutionInput(for: layer, context: replayContext)
            let layerInput: URL
            if canReturnScientificLayerDirectly {
                layerInput = resolvedInput
            } else {
                layerInput = try await fullResolutionCompositingInput(
                    resolvedInput,
                    context: replayContext
                )
            }
            layerInputURLs[layer.id] = layerInput
            if let maskArtifactID = layer.maskArtifactID {
                let maskInput = try await fullResolutionMaskInput(
                    for: maskArtifactID,
                    layer: layer,
                    layerInput: layerInput,
                    context: replayContext
                )
                maskInputURLs[layer.id] = maskInput
            }
        }

        if canReturnScientificLayerDirectly,
           let layer = visibleLayers.first {
            guard let layerInput = layerInputURLs[layer.id] else {
                throw LayerStackPreviewError.cannotReadLayer(layer.name)
            }
            return layerInput
        }

        let output = makePreviewOutput(prefix: "export-layer-stack", extension: "png")
        try renderLayerStack(
            layers: visibleLayers,
            output: output,
            format: .RGBA16,
            layerInputURLs: layerInputURLs,
            maskInputURLs: maskInputURLs
        )
        return output
    }

    private func fullResolutionInput(
        for layer: ProcessingLayer,
        context: FullResolutionLayerReplayContext
    ) async throws -> URL {
        guard isLayerReplaySourceAvailable(layer) else {
            throw EditGraphReplayError(
                message: "\(localized(.editGraphInvalidRecordedAssets)): \(layer.name)"
            )
        }
        guard let fallbackInput = layer.inputURL ?? selectedAsset?.originalURL else {
            throw LayerStackPreviewError.noVisibleLayers
        }
        if let cached = context.layerOutputs[layer.id] {
            return cached
        }
        guard context.resolvingLayerIDs.insert(layer.id).inserted else {
            throw EditGraphReplayError(
                message: "\(localized(.editGraphInvalidRecordedAssets)): \(layer.name)"
            )
        }
        defer {
            context.resolvingLayerIDs.remove(layer.id)
        }

        let resolved: URL
        if let operation = editOperation(for: layer) {
            resolved = try await fullResolutionOutput(
                for: operation,
                sourceLayerID: layer.parameters["sourceLayerID"].flatMap(UUID.init(uuidString:)),
                context: context
            )
        } else if let assetID = layer.parameters["assetID"].flatMap(UUID.init(uuidString:)),
                  let asset = project.assets.first(where: { $0.id == assetID }) {
            resolved = try await fullResolutionInput(for: asset, context: context)
        } else {
            resolved = fallbackInput
        }

        context.layerOutputs[layer.id] = resolved
        return resolved
    }

    private func editOperation(for layer: ProcessingLayer) -> EditOperation? {
        if let rawOperationID = layer.parameters["operationID"] {
            guard let operationID = UUID(uuidString: rawOperationID) else {
                return nil
            }
            return project.editGraph.operations.first(where: { $0.id == operationID })
        }
        guard let inputURL = layer.inputURL else {
            return nil
        }
        let matchingOperations = project.editGraph.operations.filter {
            Self.storedPath($0.parameters["output"], references: inputURL)
        }
        return matchingOperations.count == 1 ? matchingOperations[0] : nil
    }

    private func fullResolutionOutput(
        for operation: EditOperation,
        sourceLayerID: ProcessingLayer.ID?,
        context: FullResolutionLayerReplayContext
    ) async throws -> URL {
        if let cached = context.operationOutputs[operation.id] {
            return cached
        }
        guard context.resolvingOperationIDs.insert(operation.id).inserted else {
            throw EditGraphReplayError(
                message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
            )
        }
        defer {
            context.resolvingOperationIDs.remove(operation.id)
        }

        let previousMeteorSequenceRequirement = replayNeedsMeteorSequenceSources
        if isMeteorSequenceRestore(operation) {
            replayNeedsMeteorSequenceSources = true
        }
        defer {
            replayNeedsMeteorSequenceSources = previousMeteorSequenceRequirement
            if isMeteorSequenceRestore(operation) {
                replayStackSourceInputs = nil
            }
        }

        let input: URL?
        if operation.kind == .rawDecode {
            input = try replayRawAsset(for: operation).originalURL
        } else if let sourceLayerID,
                  let sourceLayer = project.layers.first(where: { $0.id == sourceLayerID }) {
            input = try await fullResolutionInput(for: sourceLayer, context: context)
        } else if let inputPath = operation.parameters["input"] {
            input = try await fullResolutionInput(
                matching: URL(fileURLWithPath: inputPath),
                context: context
            )
        } else {
            input = selectedAsset?.originalURL
        }

        guard let input else {
            throw EditGraphReplayError(
                message: "\(localized(.editGraphInvalidRecordedAssets)): \(operation.kind.rawValue)"
            )
        }
        guard operation.isEnabled else {
            context.operationOutputs[operation.id] = input
            return input
        }
        let output = try await applyReplayOperation(
            operation,
            input: input,
            mode: .fullResolutionExport
        )
        context.operationOutputs[operation.id] = output
        return output
    }

    private func fullResolutionInput(
        matching sourceURL: URL,
        context: FullResolutionLayerReplayContext
    ) async throws -> URL {
        let sourceIdentity = Self.workspaceFileIdentityPath(for: sourceURL)
        if let asset = project.assets.first(where: {
            Self.workspaceFileIdentityPath(for: $0.originalURL) == sourceIdentity
        }) {
            return try await fullResolutionInput(for: asset, context: context)
        }
        let sourceLayers = project.layers.filter {
            $0.kind != .mask && $0.inputURL.map {
                Self.workspaceFileIdentityPath(for: $0) == sourceIdentity
            } == true
        }
        guard sourceLayers.count <= 1 else {
            throw EditGraphReplayError(message: localized(.editGraphAmbiguousProvenance))
        }
        if let sourceLayer = sourceLayers.first {
            return try await fullResolutionInput(for: sourceLayer, context: context)
        }
        let sourceOperations = project.editGraph.operations.filter {
            $0.parameters["output"].map {
                Self.workspaceFileIdentityPath(for: URL(fileURLWithPath: $0)) == sourceIdentity
            } ?? false
        }
        guard sourceOperations.count <= 1 else {
            throw EditGraphReplayError(message: localized(.editGraphAmbiguousProvenance))
        }
        if let operation = sourceOperations.first {
            return try await fullResolutionOutput(
                for: operation,
                sourceLayerID: nil,
                context: context
            )
        }
        return sourceURL.standardizedFileURL
    }

    private func fullResolutionInput(
        for asset: PhotonStackAsset,
        context: FullResolutionLayerReplayContext
    ) async throws -> URL {
        guard asset.kind == .raw else {
            return asset.originalURL
        }
        if let cached = context.assetOutputs[asset.id] {
            return cached
        }

        let rawOptions = persistedRawOptions(for: asset) ?? parameters.rawProcessingOptions
        let scientificOutput = rawOptions.linearOutput
        var cacheParameters = rawDecodeParameters(from: rawOptions)
        cacheParameters["assetID"] = asset.id.uuidString
        cacheParameters["mode"] = "fullResolutionLayerExport"
        let output = makeCachedPreviewOutput(
            prefix: "export-layer-raw",
            input: asset.originalURL,
            parameters: cacheParameters,
            extension: scientificOutput ? "fits" : "tiff"
        )
        _ = try await runValidatedCachedStep(output: output, command: "full-resolution layer RAW") {
            try await processingService.convert(
                input: asset.originalURL,
                output: output,
                options: ExportOptions(
                    bitDepth: .sixteen,
                    colorSpace: rawOptions.linearOutput ? .linearSRGB : .srgb,
                    fitsValueMode: scientificOutput ? .scientific : .display,
                    jpegQuality: 1.0
                ),
                rawOptions: rawOptions
            )
        }
        context.assetOutputs[asset.id] = output
        return output
    }

    private func fullResolutionCompositingInput(
        _ input: URL,
        context: FullResolutionLayerReplayContext
    ) async throws -> URL {
        guard AssetKind.detect(from: input) == .fits else {
            return input
        }
        let cacheKey = Self.workspaceFileIdentityPath(for: input)
        if let cached = context.compositingOutputs[cacheKey] {
            return cached
        }
        let output = makeCachedPreviewOutput(
            prefix: "export-layer-composite-input",
            input: input,
            parameters: ["mode": "fullResolutionDisplayComposite"],
            extension: "tiff"
        )
        _ = try await runValidatedCachedStep(output: output, command: "full-resolution compositing input") {
            try await processingService.convert(
                input: input,
                output: output,
                options: ExportOptions(
                    bitDepth: .sixteen,
                    colorSpace: .linearSRGB,
                    fitsValueMode: .display,
                    jpegQuality: 1.0
                ),
                rawOptions: parameters.rawProcessingOptions
            )
        }
        context.compositingOutputs[cacheKey] = output
        return output
    }

    private func fullResolutionMaskInput(
        for artifactID: ProcessingArtifact.ID,
        layer: ProcessingLayer,
        layerInput: URL,
        context: FullResolutionLayerReplayContext
    ) async throws -> URL {
        let cacheKey = "\(artifactID.uuidString):\(layer.id.uuidString)"
        if let cached = context.maskOutputs[cacheKey] {
            return cached
        }
        guard let artifact = project.artifacts.first(where: { $0.id == artifactID }),
              let previewURL = artifact.previewURL,
              inputFileExists(previewURL)
        else {
            throw LayerStackPreviewError.cannotReadMask(layer.name)
        }

        let matchingOperations = project.editGraph.operations.filter { operation in
            operation.kind == .starMask && Self.storedPath(
                operation.parameters["output"],
                references: previewURL
            )
        }
        let operation = matchingOperations.count == 1 ? matchingOperations[0] : nil
        guard artifact.operationKind == .starMask || operation != nil else {
            context.maskOutputs[cacheKey] = previewURL
            return previewURL
        }

        let maskParameters = artifact.parameters.merging(
            operation?.parameters ?? [:],
            uniquingKeysWith: { current, _ in current }
        )
        let radiusScale = try fullResolutionMaskRadiusScale(
            previewMask: previewURL,
            fullResolutionInput: layerInput,
            layerName: layer.name
        )
        let previewRadius = intParameter("radius", parameters: maskParameters, defaultValue: 2)
        let previewLargeRadius = intParameter("largeRadius", parameters: maskParameters, defaultValue: 4)
        let fullResolutionRadius = scaledMaskRadius(
            previewRadius,
            scale: radiusScale,
            lowerBound: 1,
            upperBound: 32
        )
        let fullResolutionLargeRadius = scaledMaskRadius(
            previewLargeRadius,
            scale: radiusScale,
            lowerBound: fullResolutionRadius,
            upperBound: 64
        )
        let brushStrokes = try decodeMaskBrushStrokes(maskParameters)
        var generatorParameters = maskParameters
        generatorParameters.removeValue(forKey: "brushStrokes")
        generatorParameters.removeValue(forKey: "sourceMaskID")
        generatorParameters.removeValue(forKey: "source")
        generatorParameters["layerID"] = layer.id.uuidString
        generatorParameters["mode"] = "fullResolutionExportBase"
        generatorParameters["fullResolutionRadius"] = String(fullResolutionRadius)
        generatorParameters["fullResolutionLargeRadius"] = String(fullResolutionLargeRadius)
        let baseOutput = makeCachedPreviewOutput(
            prefix: "export-layer-mask-base",
            input: layerInput,
            parameters: generatorParameters,
            extension: "tiff"
        )
        _ = try await runValidatedCachedStep(output: baseOutput, command: "full-resolution star mask") {
            try await processingService.createStarMask(
                input: layerInput,
                output: baseOutput,
                radius: fullResolutionRadius,
                largeRadius: fullResolutionLargeRadius,
                layered: boolParameter("layered", parameters: maskParameters, defaultValue: true),
                sigmaThreshold: doubleParameter("sigmaThreshold", parameters: maskParameters, defaultValue: 3.0),
                minPeak: doubleParameter("minPeak", parameters: maskParameters, defaultValue: 0.02),
                maxStars: intParameter("maxStars", parameters: maskParameters, defaultValue: 50_000)
            )
        }

        let output: URL
        if brushStrokes.isEmpty {
            output = baseOutput
        } else {
            var brushParameters = [
                "artifactID": artifactID.uuidString,
                "layerID": layer.id.uuidString,
                "mode": "fullResolutionExportBrush",
                "brushStrokes": try encodeMaskBrushStrokes(brushStrokes),
            ]
            brushParameters["sourceMaskID"] = artifact.parameters["sourceMaskID"] ?? ""
            output = makeCachedPreviewOutput(
                prefix: "export-layer-mask-brush",
                input: baseOutput,
                parameters: brushParameters,
                extension: "png"
            )
            if cachedResultIfAvailable(output: output, command: "full-resolution mask brush") == nil {
                try renderMaskBrushStrokes(brushStrokes, input: baseOutput, output: output)
                try Task.checkCancellation()
                recordValidatedCacheOutput(output)
            }
        }
        context.maskOutputs[cacheKey] = output
        return output
    }

    private func fullResolutionMaskRadiusScale(
        previewMask: URL,
        fullResolutionInput: URL,
        layerName: String
    ) throws -> Double {
        guard let previewSize = imagePixelSize(at: previewMask),
              let fullResolutionSize = imagePixelSize(at: fullResolutionInput),
              previewSize.width > 0,
              previewSize.height > 0
        else {
            throw LayerStackPreviewError.cannotReadMask(layerName)
        }
        let scale = max(
            1,
            max(
                fullResolutionSize.width / previewSize.width,
                fullResolutionSize.height / previewSize.height
            )
        )
        guard scale.isFinite else {
            throw LayerStackPreviewError.cannotReadMask(layerName)
        }
        return scale
    }

    private func scaledMaskRadius(
        _ previewRadius: Int,
        scale: Double,
        lowerBound: Int,
        upperBound: Int
    ) -> Int {
        let clampedPreviewRadius = min(max(previewRadius, 1), upperBound)
        let scaled = Double(clampedPreviewRadius) * scale
        guard scaled.isFinite else {
            return upperBound
        }
        let bounded = min(
            max(scaled.rounded(), Double(lowerBound)),
            Double(upperBound)
        )
        return Int(bounded)
    }

    private func imagePixelSize(at url: URL) -> (width: Double, height: Double)? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil),
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = (properties[kCGImagePropertyPixelWidth] as? NSNumber)?.doubleValue,
              let height = (properties[kCGImagePropertyPixelHeight] as? NSNumber)?.doubleValue,
              width.isFinite,
              height.isFinite,
              width > 0,
              height > 0
        else {
            return nil
        }
        let orientation = (properties[kCGImagePropertyOrientation] as? NSNumber)?.intValue ?? 1
        return (5...8).contains(orientation) ? (height, width) : (width, height)
    }

    // Temporal cloud confirmation only makes sense for compatible light frames.
    // Excluding calibration frames avoids a dimension mismatch.  When a project
    // has many lights, sample the whole sequence rather than only its nearest
    // neighbors: a weak bank can become obvious only later in the session.
    private func temporalCloudReferenceURLs(for input: URL) -> [URL] {
        guard let source = project.assets.first(where: {
            Self.urlsReferenceSameFile($0.originalURL, input)
        }) else {
            return []
        }
        let sourceWidth = source.metadata?.width
        let sourceHeight = source.metadata?.height
        let candidateRoles: Set<CalibrationFrameRole> = [.light, .reference]
        let candidates = project.assets
            .filter { asset in
                guard asset.id != source.id,
                      asset.originalURL.standardizedFileURL != input.standardizedFileURL,
                      candidateRoles.contains(asset.role),
                      asset.kind == source.kind
                else {
                    return false
                }
                guard let sourceWidth, let sourceHeight,
                      let width = asset.metadata?.width,
                      let height = asset.metadata?.height
                else {
                    return true
                }
                return width == sourceWidth && height == sourceHeight
            }
            .sorted { lhs, rhs in
                let lhsCapture = lhs.metadata?.captureDate ?? ""
                let rhsCapture = rhs.metadata?.captureDate ?? ""
                if lhsCapture != rhsCapture {
                    return lhsCapture < rhsCapture
                }
                if lhs.importedAt != rhs.importedAt {
                    return lhs.importedAt < rhs.importedAt
                }
                return lhs.originalURL.path < rhs.originalURL.path
            }
        let maximumReferences = 4
        guard candidates.count > maximumReferences else {
            return candidates.map(\.originalURL)
        }
        let samples = maximumReferences
        return (0..<samples).map { sample in
            let index = sample * (candidates.count - 1) / (samples - 1)
            return candidates[index].originalURL
        }
    }

    private func metadataPixelSize(at url: URL) -> (width: Double, height: Double)? {
        guard let asset = project.assets.first(where: {
            Self.urlsReferenceSameFile($0.originalURL, url)
        }),
        let width = asset.metadata?.width,
        let height = asset.metadata?.height,
        width > 0,
        height > 0
        else {
            return nil
        }
        return (Double(width), Double(height))
    }

    func resolvedPixelWidthForCurveProcessing(at url: URL) -> Int? {
        let pixelSize = metadataPixelSize(at: url) ?? imagePixelSize(at: url)
        guard let pixelSize else {
            return nil
        }
        return max(Int(pixelSize.width.rounded()), 1)
    }

    private func imagePixelWidth(at url: URL) -> Int? {
        resolvedPixelWidthForCurveProcessing(at: url)
    }

    private func runEngineCurveApplyIfAvailable(
        input: URL,
        output: URL,
        points: [(Double, Double)],
        channel: CurveChannel,
        rawOptions: RawProcessingOptions = RawProcessingOptions(),
        widthOverride: Int? = nil,
        retainDecodedSource: Bool = false
    ) async throws -> ProcessingCommandResult? {
        guard let width = widthOverride ?? resolvedPixelWidthForCurveProcessing(at: input) else {
            return nil
        }
        return try await EngineCurvePreview.runIfAvailableAsync(
            using: processingService,
            input: input,
            output: output,
            width: width,
            rawOptions: rawOptions,
            points: points,
            channel: channel,
            retainDecodedSource: retainDecodedSource
        )
    }

    private func annotatedCurveApplyResult(
        _ result: ProcessingCommandResult,
        executionSource: String
    ) -> ProcessingCommandResult {
        let note = "[curve apply source: \(executionSource)]"
        let annotatedJSON = annotatedCurveApplyJSON(result.standardOutput, executionSource: executionSource)
        let output: String
        if annotatedJSON.isEmpty {
            output = note
        } else {
            output = "\(annotatedJSON)\n\(note)"
        }
        return ProcessingCommandResult(
            command: result.command,
            exitCode: result.exitCode,
            standardOutput: output,
            standardError: result.standardError,
            durationMilliseconds: result.durationMilliseconds
        )
    }

    private func annotatedCurveApplyJSON(_ output: String, executionSource: String) -> String {
        guard output.isEmpty == false,
              let data = output.data(using: .utf8),
              var object = try? JSONSerialization.jsonObject(with: data) as? [String: Any]
        else {
            return output
        }
        object["executionSource"] = executionSource
        guard let annotatedData = try? JSONSerialization.data(withJSONObject: object, options: [.prettyPrinted]),
              let annotatedOutput = String(data: annotatedData, encoding: .utf8)
        else {
            return output
        }
        return annotatedOutput
    }

    private func logCurveTelemetry(
        event: String,
        startedAt: Date,
        source: String,
        width: Int?,
        points: Int,
        note: String? = nil
    ) {
        let durationMilliseconds = Date().timeIntervalSince(startedAt) * 1000.0
        let widthText = width.map(String.init) ?? "unknown"
        let noteText = note ?? "-"
        curveTelemetryLogger.info(
            "\(event, privacy: .public) durationMs=\(durationMilliseconds, format: .fixed(precision: 1)) source=\(source, privacy: .public) width=\(widthText, privacy: .public) points=\(points) note=\(noteText, privacy: .public)"
        )
    }

    private func renderLayerStack(
        layers: [ProcessingLayer],
        output: URL,
        format: CIFormat,
        layerInputURLs: [ProcessingLayer.ID: URL] = [:],
        maskInputURLs: [ProcessingLayer.ID: URL] = [:]
    ) throws {
        let compositableLayers = layers.filter { $0.kind != .mask }
        guard let firstLayer = compositableLayers.first,
              let firstURL = layerInputURLs[firstLayer.id] ?? firstLayer.inputURL
        else {
            throw LayerStackPreviewError.noVisibleLayers
        }
        guard var composite = CIImage(contentsOf: firstURL, options: [.applyOrientationProperty: true]) else {
            throw LayerStackPreviewError.cannotReadLayer(firstLayer.name)
        }
        let extent = composite.extent
        composite = try preparedLayerImage(
            composite,
            layer: firstLayer,
            extent: extent,
            maskInputURLs: maskInputURLs
        )

        for layer in compositableLayers.dropFirst() {
            guard let inputURL = layerInputURLs[layer.id] ?? layer.inputURL else {
                throw LayerStackPreviewError.cannotReadLayer(layer.name)
            }
            guard var foreground = CIImage(contentsOf: inputURL, options: [.applyOrientationProperty: true]) else {
                throw LayerStackPreviewError.cannotReadLayer(layer.name)
            }

            foreground = try preparedLayerImage(
                foreground,
                layer: layer,
                extent: extent,
                maskInputURLs: maskInputURLs
            )

            composite = blend(foreground: foreground, over: composite, mode: layer.blendMode)
                .cropped(to: extent)
        }

        let context = CIContext(options: [.workingColorSpace: NSNull()])
        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
            throw LayerStackPreviewError.cannotRender
        }
        try AtomicLocalImageWriter.write(
            to: output,
            validate: { Self.isUsableCachedOutput($0) }
        ) { temporaryOutput in
            try context.writePNGRepresentation(
                of: composite,
                to: temporaryOutput,
                format: format,
                colorSpace: colorSpace
            )
        }
    }

    private func renderEmptyLayerStack(sourceURL: URL, output: URL, format: CIFormat) throws {
        guard let source = CIImage(contentsOf: sourceURL, options: [.applyOrientationProperty: true]) else {
            throw LayerStackPreviewError.cannotReadLayer(sourceURL.lastPathComponent)
        }
        let transparent = CIImage(color: .clear).cropped(to: source.extent)
        let context = CIContext(options: [.workingColorSpace: NSNull()])
        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
            throw LayerStackPreviewError.cannotRender
        }
        try AtomicLocalImageWriter.write(
            to: output,
            validate: { Self.isUsableCachedOutput($0) }
        ) { temporaryOutput in
            try context.writePNGRepresentation(
                of: transparent,
                to: temporaryOutput,
                format: format,
                colorSpace: colorSpace
            )
        }
    }

    private func preparedLayerImage(
        _ inputImage: CIImage,
        layer: ProcessingLayer,
        extent: CGRect,
        maskInputURLs: [ProcessingLayer.ID: URL]
    ) throws -> CIImage {
        let layerExtent = inputImage.extent
        var prepared = inputImage.cropped(to: extent)
        if let maskArtifactID = layer.maskArtifactID {
            guard let maskArtifact = project.artifacts.first(where: { $0.id == maskArtifactID }),
                  let maskURL = maskInputURLs[layer.id] ?? maskArtifact.previewURL,
                  let maskImage = CIImage(contentsOf: maskURL, options: [.applyOrientationProperty: true])
            else {
                throw LayerStackPreviewError.cannotReadMask(layer.name)
            }
            let opaqueBlack = CIImage(color: CIColor(red: 0, green: 0, blue: 0, alpha: 1))
                .cropped(to: extent)
            var fittedMask = image(maskImage, fittedTo: layerExtent)
                .composited(over: opaqueBlack)
                .cropped(to: extent)
            if layer.parameters["maskInverted"] == "true" {
                fittedMask = fittedMask.applyingFilter("CIColorInvert")
            }
            let featherFraction = Self.normalizedFinite(
                layer.parameters["maskFeatherFraction"].flatMap(Double.init) ?? 0,
                in: 0...0.1,
                fallback: 0
            )
            if featherFraction > 0 {
                let radius = min(layerExtent.width, layerExtent.height) * featherFraction
                fittedMask = fittedMask
                    .clampedToExtent()
                    .applyingFilter("CIGaussianBlur", parameters: [kCIInputRadiusKey: radius])
                    .cropped(to: extent)
            }
            let density = Self.normalizedFinite(
                layer.parameters["maskDensity"].flatMap(Double.init) ?? 1,
                in: 0...1,
                fallback: 1
            )
            if density < 0.9999 {
                let densityValue = CGFloat(density)
                let bias = CGFloat(1 - density)
                fittedMask = fittedMask.applyingFilter(
                    "CIColorMatrix",
                    parameters: [
                        "inputRVector": CIVector(x: densityValue, y: 0, z: 0, w: 0),
                        "inputGVector": CIVector(x: 0, y: densityValue, z: 0, w: 0),
                        "inputBVector": CIVector(x: 0, y: 0, z: densityValue, w: 0),
                        "inputAVector": CIVector(x: 0, y: 0, z: 0, w: 1),
                        "inputBiasVector": CIVector(x: bias, y: bias, z: bias, w: 0),
                    ]
                )
            }
            let transparent = CIImage(color: .clear).cropped(to: extent)
            prepared = prepared.applyingFilter(
                "CIBlendWithMask",
                parameters: [
                    kCIInputBackgroundImageKey: transparent,
                    kCIInputMaskImageKey: fittedMask,
                ]
            )
            .cropped(to: extent)
        }
        return image(prepared, applyingOpacity: layer.opacity)
    }

    private func renderMaskBrushStrokes(
        _ strokes: [MaskBrushStroke],
        input: URL,
        output: URL
    ) throws {
        guard let source = CGImageSourceCreateWithURL(input as CFURL, nil),
              let image = CGImageSourceCreateImageAtIndex(source, 0, [
                  kCGImageSourceShouldCache: true,
                  kCGImageSourceShouldCacheImmediately: true,
              ] as CFDictionary),
              let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(
                  data: nil,
                  width: image.width,
                  height: image.height,
                  bitsPerComponent: 8,
                  bytesPerRow: 0,
                  space: colorSpace,
                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
              )
        else {
            throw MaskEditingError.cannotReadMask
        }

        let width = CGFloat(image.width)
        let height = CGFloat(image.height)
        let minimumDimension = min(width, height)
        let bounds = CGRect(x: 0, y: 0, width: width, height: height)
        context.interpolationQuality = .high
        context.setShouldAntialias(true)
        context.setFillColor(CGColor(gray: 0, alpha: 1))
        context.fill(bounds)
        context.draw(image, in: bounds)

        for stroke in strokes where stroke.points.isEmpty == false && stroke.opacity > 0 {
            let points = stroke.points.map { point in
                CGPoint(
                    x: CGFloat(point.x) * width,
                    y: (1 - CGFloat(point.y)) * height
                )
            }
            let diameter = max(1, CGFloat(stroke.diameterFraction) * minimumDimension)
            let color = CGColor(
                gray: stroke.mode == .reveal ? 1 : 0,
                alpha: CGFloat(stroke.opacity)
            )
            context.setBlendMode(.normal)
            context.setFillColor(color)
            context.setStrokeColor(color)
            context.setLineWidth(diameter)
            context.setLineCap(.round)
            context.setLineJoin(.round)

            if points.count == 1, let point = points.first {
                context.fillEllipse(in: CGRect(
                    x: point.x - diameter * 0.5,
                    y: point.y - diameter * 0.5,
                    width: diameter,
                    height: diameter
                ))
            } else {
                context.addLines(between: points)
                context.strokePath()
            }
        }

        guard let editedImage = context.makeImage() else {
            throw MaskEditingError.cannotRenderMask
        }

        try AtomicLocalImageWriter.write(
            to: output,
            validate: { Self.isUsableCachedOutput($0) }
        ) { temporaryOutput in
            guard let destination = CGImageDestinationCreateWithURL(
                temporaryOutput as CFURL,
                UTType.png.identifier as CFString,
                1,
                nil
            ) else {
                throw MaskEditingError.cannotRenderMask
            }
            CGImageDestinationAddImage(destination, editedImage, nil)
            guard CGImageDestinationFinalize(destination) else {
                throw MaskEditingError.cannotRenderMask
            }
        }
    }

    private func encodeMaskBrushStrokes(_ strokes: [MaskBrushStroke]) throws -> String {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        let data = try encoder.encode(strokes)
        guard let text = String(data: data, encoding: .utf8) else {
            throw MaskEditingError.cannotEncodeStrokes
        }
        return text
    }

    private func decodeMaskBrushStrokes(_ parameters: [String: String]) throws -> [MaskBrushStroke] {
        guard let text = parameters["brushStrokes"] else {
            return []
        }
        guard let data = text.data(using: .utf8) else {
            throw MaskEditingError.cannotDecodeStrokes
        }
        do {
            return try JSONDecoder().decode([MaskBrushStroke].self, from: data)
        } catch {
            throw MaskEditingError.cannotDecodeStrokes
        }
    }

    private func image(_ inputImage: CIImage, fittedTo extent: CGRect) -> CIImage {
        let sourceExtent = inputImage.extent
        guard sourceExtent.width > 0, sourceExtent.height > 0 else {
            return inputImage.cropped(to: extent)
        }
        return inputImage
            .transformed(by: CGAffineTransform(translationX: -sourceExtent.minX, y: -sourceExtent.minY))
            .transformed(by: CGAffineTransform(
                scaleX: extent.width / sourceExtent.width,
                y: extent.height / sourceExtent.height
            ))
            .transformed(by: CGAffineTransform(translationX: extent.minX, y: extent.minY))
            .cropped(to: extent)
    }

    private func image(_ image: CIImage, applyingOpacity opacity: Double) -> CIImage {
        image.applyingFilter(
            "CIColorMatrix",
            parameters: [
                "inputAVector": CIVector(x: 0, y: 0, z: 0, w: min(max(opacity, 0.0), 1.0)),
            ]
        )
    }

    private func blend(foreground: CIImage, over background: CIImage, mode: ProcessingLayerBlendMode) -> CIImage {
        let filterName: String
        switch mode {
        case .normal:
            filterName = "CISourceOverCompositing"
        case .screen:
            filterName = "CIScreenBlendMode"
        case .lighten:
            filterName = "CILightenBlendMode"
        case .multiply:
            filterName = "CIMultiplyBlendMode"
        case .overlay:
            filterName = "CIOverlayBlendMode"
        }
        return foreground.applyingFilter(filterName, parameters: [kCIInputBackgroundImageKey: background])
    }

    private func productManifestMarkdown() -> String {
        var lines: [String] = []
        let timestamp = ISO8601DateFormatter().string(from: Date())
        lines.append("# PhotonStack Product Manifest")
        lines.append("")
        lines.append("- Generated: \(timestamp)")
        lines.append("- Project: \(project.name)")
        lines.append("- Assets: \(project.assets.count)")
        lines.append("- Artifacts: \(project.artifacts.count)")
        lines.append("- Layers: \(project.layers.count)")
        if let previewURL {
            lines.append("- Current preview: \(previewURL.path)")
        }
        lines.append("")

        lines.append("## Assets")
        lines.append("")
        lines.append("| Name | Role | Kind | Source |")
        lines.append("| --- | --- | --- | --- |")
        if project.assets.isEmpty {
            lines.append("| None | | | |")
        } else {
            for asset in project.assets {
                lines.append("| \(manifestCell(asset.displayName)) | \(manifestCell(language.roleName(asset.role))) | \(manifestCell(asset.kind.rawValue)) | \(manifestCell(asset.originalURL.path)) |")
            }
        }
        lines.append("")

        lines.append("## Artifacts")
        lines.append("")
        lines.append("| Kind | Name | Frames | Operation | Output | Metrics |")
        lines.append("| --- | --- | ---: | --- | --- | --- |")
        if project.artifacts.isEmpty {
            lines.append("| None | | 0 | | | |")
        } else {
            for artifact in project.artifacts {
                let metrics = artifact.metrics
                    .sorted { $0.key < $1.key }
                    .map { "\($0.key)=\($0.value)" }
                    .joined(separator: ", ")
                let output = artifact.outputDirectory?.path ?? artifact.previewURL?.path ?? ""
                lines.append("| \(manifestCell(artifactKindName(artifact.kind))) | \(manifestCell(artifact.name)) | \(artifact.frameCount) | \(manifestCell(artifact.operationKind?.rawValue ?? "")) | \(manifestCell(output)) | \(manifestCell(metrics)) |")
            }
        }
        lines.append("")

        lines.append("## Layers")
        lines.append("")
        lines.append("| Order | Name | Kind | Visible | Opacity | Blend | Source |")
        lines.append("| ---: | --- | --- | --- | ---: | --- | --- |")
        if project.layers.isEmpty {
            lines.append("| 0 | None | | | | | |")
        } else {
            for (offset, layer) in project.layers.reversed().enumerated() {
                let sourceName = layer.sourceArtifactID.flatMap { sourceID in
                    project.artifacts.first { $0.id == sourceID }?.name
                } ?? layer.inputURL?.lastPathComponent ?? ""
                lines.append("| \(offset + 1) | \(manifestCell(layer.name)) | \(manifestCell(layerKindName(layer.kind))) | \(layer.isVisible) | \(String(format: "%.2f", layer.opacity)) | \(manifestCell(layerBlendModeName(layer.blendMode))) | \(manifestCell(sourceName)) |")
            }
        }
        lines.append("")

        lines.append("## Recent Jobs")
        lines.append("")
        lines.append("| Title | Kind | Status | Output |")
        lines.append("| --- | --- | --- | --- |")
        if jobs.isEmpty {
            lines.append("| None | | | |")
        } else {
            for job in jobs.suffix(10).reversed() {
                lines.append("| \(manifestCell(job.title)) | \(manifestCell(job.kind.rawValue)) | \(manifestCell(job.status.rawValue)) | \(manifestCell(job.outputURL?.path ?? "")) |")
            }
        }

        if commandOutput.isEmpty == false {
            lines.append("")
            lines.append("## Last Command Output")
            lines.append("")
            lines.append("```text")
            lines.append(truncatedManifestText(commandOutput))
            lines.append("```")
        }

        return lines.joined(separator: "\n") + "\n"
    }

    private func manifestCell(_ value: String) -> String {
        value
            .replacingOccurrences(of: "|", with: "\\|")
            .replacingOccurrences(of: "\n", with: " ")
            .replacingOccurrences(of: "\r", with: " ")
    }

    private func truncatedManifestText(_ value: String) -> String {
        let maxLength = 12_000
        guard value.count > maxLength else {
            return value
        }
        return String(value.prefix(maxLength)) + "\n...[truncated]"
    }

    private func persistBatchQueue() {
        syncProjectBatchQueue()
        triggerAutosave()
    }

    private func syncProjectBatchQueue() {
        project.replaceBatchQueue(batchQueue)
    }

    private func syncProjectWorkspaceState() {
        if previewOperationID == nil, let previewURL {
            previewOperationID = Self.uniqueEditOperationID(
                for: previewURL,
                in: project.editGraph.operations
            )
        }
        project.workspaceState = ProjectWorkspaceState(
            selectedAssetID: selectedAssetID,
            activeLayerID: activeLayerID,
            previewURL: previewURL,
            previewOperationID: previewOperationID,
            editGraphNeedsReplay: editGraphNeedsReplay,
            canvasMode: canvasMode
        )
    }

    private var hasAmbiguousEditGraphProvenance: Bool {
        if let previewOperationID {
            return editGraphReplayPlan(
                finalOperationID: previewOperationID,
                finalOperationIsEligible: { _ in true }
            ) == nil
        }
        guard canvasMode == .sourcePreview else {
            return false
        }
        var targets: [URL] = []
        if let previewURL {
            targets.append(previewURL)
        }
        if let persistedPreviewURL = project.workspaceState?.previewURL,
           targets.contains(where: {
               Self.urlsReferenceSameFile($0, persistedPreviewURL)
           }) == false {
            targets.append(persistedPreviewURL)
        }
        return targets.contains { target in
            Self.editOperations(for: target, in: project.editGraph.operations).count > 1
        }
    }

    private static func uniqueEditOperationID(
        for outputURL: URL,
        in operations: [EditOperation]
    ) -> EditOperation.ID? {
        let matches = editOperations(for: outputURL, in: operations)
        return matches.count == 1 ? matches[0].id : nil
    }

    private static func editOperations(
        for outputURL: URL,
        in operations: [EditOperation]
    ) -> [EditOperation] {
        operations.filter { operation in
            storedPath(operation.parameters["output"], references: outputURL) ||
                storedPath(operation.parameters["previewOutput"], references: outputURL)
        }
    }

    private static func workspaceFileIdentityPath(for url: URL) -> String {
        PhotonStackProject.assetIdentityPath(for: url)
    }

    private static func urlsReferenceSameFile(_ lhs: URL, _ rhs: URL) -> Bool {
        workspaceFileIdentityPath(for: lhs) == workspaceFileIdentityPath(for: rhs)
    }

    private static func storedPath(_ path: String?, references url: URL) -> Bool {
        guard let path, path.hasPrefix("/") else {
            return false
        }
        return workspaceFileIdentityPath(for: URL(fileURLWithPath: path)) ==
            workspaceFileIdentityPath(for: url)
    }

    private static func storedPathsReferenceSameFile(_ lhs: String?, _ rhs: String?) -> Bool {
        guard let lhs, let rhs, lhs.hasPrefix("/"), rhs.hasPrefix("/") else {
            return false
        }
        return workspaceFileIdentityPath(for: URL(fileURLWithPath: lhs)) ==
            workspaceFileIdentityPath(for: URL(fileURLWithPath: rhs))
    }

    private func inferredLegacyCanvasMode(savedPreview: URL?) -> WorkspaceCanvasMode {
        guard let activeLayer else {
            return .sourcePreview
        }
        if activeLayer.kind == .mask {
            return .sourcePreview
        }
        if let savedPreview,
           selectedAsset.map({ Self.urlsReferenceSameFile(savedPreview, $0.originalURL) }) == true,
           activeLayer.inputURL.map({ Self.urlsReferenceSameFile(savedPreview, $0) }) != true {
            return .sourcePreview
        }
        return .layerComposite
    }

    private func triggerAutosave() {
        projectChangeGeneration += 1
        hasUnsavedChanges = true
        scheduleAutosaveIfNeeded()
    }

    private func scheduleAutosaveIfNeeded() {
        guard hasUnsavedChanges, autosaveEnabled, let currentProjectDirectory else {
            return
        }

        syncProjectWorkspaceState()

        let projectSnapshot = project
        let directorySnapshot = currentProjectDirectory
        let projectGeneration = projectChangeGeneration
        let previousTask = autosaveTask
        let repository = projectRepository
        autosaveGeneration += 1
        let generation = autosaveGeneration
        previousTask?.cancel()

        autosaveTask = Task { @MainActor [weak self] in
            await previousTask?.value
            defer {
                if let self, self.autosaveGeneration == generation {
                    self.autosaveTask = nil
                }
            }
            do {
                try Task.checkCancellation()
                try await repository.save(projectSnapshot, to: directorySnapshot)
                try Task.checkCancellation()
                guard let self,
                      self.autosaveGeneration == generation,
                      self.projectChangeGeneration == projectGeneration,
                      self.currentProjectDirectory == directorySnapshot
                else {
                    return
                }
                self.hasUnsavedChanges = false
                self.commandOutput = "\(self.localized(.autosavedProject)): \(directorySnapshot.path)"
            } catch is CancellationError {
                return
            } catch {
                guard let self,
                      self.autosaveGeneration == generation,
                      self.currentProjectDirectory == directorySnapshot
                else {
                    return
                }
                self.errorMessage = error.localizedDescription
            }
        }
    }

    private func waitForPendingAutosave() async {
        await autosaveTask?.value
    }

    private func makePreviewOutput(prefix: String, extension pathExtension: String) -> URL {
        previewDirectory
            .appendingPathComponent("\(prefix)-\(UUID().uuidString)")
            .appendingPathExtension(pathExtension)
    }

    private static func managedPreviewDirectory(for projectID: PhotonStackProject.ID) -> URL {
        FileManager.default.temporaryDirectory
            .appendingPathComponent("PhotonStackPreviews", isDirectory: true)
            .appendingPathComponent(projectID.uuidString, isDirectory: true)
    }

    private func makeCachedPreviewOutput(
        prefix: String,
        input: URL,
        parameters: [String: String],
        extension pathExtension: String = "png",
        includeEditGraph: Bool = true
    ) -> URL {
        let parameterText = parameters
            .map { "\($0.key)=\($0.value)" }
            .sorted()
            .joined(separator: "|")
        var keyParts = [
            "schema=\(previewCacheSchemaVersion)",
            "build=\(cacheBuildIdentity)",
            "project=\(project.id.uuidString)",
            "prefix=\(prefix)",
            "input=\(input.path)",
            "source=\(fileSignature(input))",
            "parameters=\(parameterText)",
        ]
        if includeEditGraph {
            keyParts.append("editGraph=\(editGraphCacheSignature(project.editGraph))")
        }
        let key = keyParts.joined(separator: "|")
        let digest = stableDigest(key)
        return previewDirectory
            .appendingPathComponent("cache-\(prefix)-\(digest)")
            .appendingPathExtension(pathExtension)
    }

    private func editGraphCacheSignature(_ editGraph: EditGraph) -> String {
        let text = editGraph.operations.map { operation in
            let parameters = operation.parameters
                .filter { $0.key != "output" }
                .map { "\($0.key)=\($0.value)" }
                .sorted()
                .joined(separator: ",")
            return "\(operation.id.uuidString):\(operation.kind.rawValue):\(operation.isEnabled):\(parameters)"
        }
        .joined(separator: ";")
        return stableDigest(text)
    }

    private func fileSignature(_ url: URL) -> String {
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: url.path) else {
            return "missing"
        }

        let modifiedAt = (attributes[.modificationDate] as? Date)?.timeIntervalSince1970 ?? 0
        let size = (attributes[.size] as? NSNumber)?.int64Value ?? 0
        return "\(modifiedAt):\(size)"
    }

    private func rawDecodeParameters() -> [String: String] {
        rawDecodeParameters(from: parameters.rawProcessingOptions)
    }

    private func rawDecodeParameters(from options: RawProcessingOptions) -> [String: String] {
        [
            "rawWhiteBalance": options.whiteBalanceMode.rawValue,
            "rawTemperature": String(options.manualWhiteBalanceTemperature),
            "rawTint": String(options.manualWhiteBalanceTint),
            "rawExposureBias": String(options.exposureBias),
            "rawBlackLevel": options.blackLevelMode.rawValue,
            "rawBlackValue": String(options.manualBlackLevel),
            "rawDemosaic": options.demosaicQuality.rawValue,
            "rawLinear": String(options.linearOutput),
        ]
    }

    private func curveOperationParameters(
        points: [CurveEditorPoint],
        channel: CurveChannel,
        previewWidth: Int? = nil
    ) -> [String: String] {
        let controlPoints = parameters.curveControlPoints
        var operationParameters = [
            "black": String(controlPoints[0].input),
            "midInput": String(controlPoints[1].input),
            "midOutput": String(controlPoints[1].output),
            "white": String(controlPoints[2].input),
            "channel": channel.rawValue,
            "points": encodedCurvePoints(points),
        ]
        let curvePreset = parameters.matchingCurvePreset
        if curvePreset != .custom {
            operationParameters["preset"] = curvePreset.rawValue
        } else if let customPreset = matchingCustomCurvePreset(for: parameters) {
            operationParameters["preset"] = "custom:\(customPreset.name)"
        }
        if let previewWidth {
            operationParameters["previewWidth"] = String(previewWidth)
        }
        return operationParameters
    }

    private func interactiveCurvePreviewWidth(for previewWidth: Int) -> Int {
        let clampedPreviewWidth = min(max(previewWidth, 320), 4096)
        return min(max(clampedPreviewWidth / 2, 512), 1600)
    }

    private func makeCachedMosaicPreviewOutput(for panels: [PhotonStackAsset]) -> URL {
        let cacheInput = URL(fileURLWithPath: panels.map(\.originalURL.path).joined(separator: "|"))
        return makeCachedPreviewOutput(
            prefix: "mosaic-preview",
            input: cacheInput,
            parameters: [
                "panels": inputSignature(panels.map(\.originalURL)),
                "overlapPixels": String(parameters.mosaicOverlapPixels),
                "projection": parameters.mosaicProjection.rawValue,
                "layout": parameters.mosaicLayout.rawValue,
                "alignment": parameters.mosaicAlignment.rawValue,
                "blend": parameters.mosaicBlendMode.rawValue,
                "exposureMatching": String(parameters.mosaicExposureMatching),
                "columns": String(parameters.mosaicColumns),
                "previewWidth": String(parameters.mosaicPreviewWidth),
            ]
        )
    }

    private func makeBatchOutputURL(for item: BatchQueueItem, parameters: [String: String]) throws -> URL {
        guard let outputDirectory = project.batchOutputDirectory else {
            return makeCachedPreviewOutput(
                prefix: "batch-\(item.kind.rawValue)",
                input: item.inputURL,
                parameters: parameters,
                extension: project.batchOutputFormat.fileExtension
            )
        }

        try FileManager.default.createDirectory(at: outputDirectory, withIntermediateDirectories: true)
        let parameterText = parameters
            .map { "\($0.key)=\($0.value)" }
            .sorted()
            .joined(separator: "|")
        let cacheKey = [
            "schema=\(previewCacheSchemaVersion)",
            "build=\(cacheBuildIdentity)",
            "input=\(item.inputURL.path)",
            "source=\(fileSignature(item.inputURL))",
            "operation=\(item.kind.rawValue)",
            "parameters=\(parameterText)",
        ].joined(separator: "|")
        let digest = String(stableDigest(cacheKey).prefix(8))
        let baseName = sanitizedFilename(item.inputURL.deletingPathExtension().lastPathComponent)
        let filename = "\(baseName)-\(item.kind.rawValue)-\(digest).\(project.batchOutputFormat.fileExtension)"
        return outputDirectory.appendingPathComponent(filename)
    }

    private func sanitizedFilename(_ value: String) -> String {
        let allowed = CharacterSet.alphanumerics.union(CharacterSet(charactersIn: "-_"))
        let scalars = value.unicodeScalars.map { scalar in
            allowed.contains(scalar) ? Character(scalar) : "-"
        }
        let sanitized = String(scalars).trimmingCharacters(in: CharacterSet(charactersIn: "-"))
        return sanitized.isEmpty ? "photonstack" : sanitized
    }

    private func cachedResultIfAvailable(output: URL, command: String) -> ProcessingCommandResult? {
        guard Self.isUsableCachedOutput(output), hasValidatedCacheReceipt(for: output) else {
            invalidateCacheReceipt(for: output)
            return nil
        }
        return ProcessingCommandResult(
            command: ["cache", command, output.path],
            exitCode: 0,
            standardOutput: "Using cached preview: \(output.path)",
            standardError: ""
        )
    }

    private func isManagedPreviewCacheOutput(_ output: URL) -> Bool {
        let standardizedOutput = output.standardizedFileURL
        let standardizedDirectory = previewDirectory.standardizedFileURL
        let imageExtensions: Set<String> = ["png", "jpg", "jpeg", "tif", "tiff", "heic", "heif", "hif"]
        return standardizedOutput.deletingLastPathComponent() == standardizedDirectory &&
            standardizedOutput.lastPathComponent.hasPrefix("cache-") &&
            imageExtensions.contains(standardizedOutput.pathExtension.lowercased())
    }

    private func previewCacheReceiptURL(for output: URL) -> URL {
        output.appendingPathExtension("photonstack-cache-receipt")
    }

    private func cacheOutputSignature(_ output: URL) -> String {
        guard let attributes = try? FileManager.default.attributesOfItem(atPath: output.path) else {
            return "missing"
        }
        let fileNumber = (attributes[.systemFileNumber] as? NSNumber)?.uint64Value ?? 0
        return "\(fileSignature(output)):\(fileNumber)"
    }

    private func validatedCacheReceipt(for output: URL) -> PreviewCacheReceipt? {
        guard isManagedPreviewCacheOutput(output),
              let data = try? Data(contentsOf: previewCacheReceiptURL(for: output)),
              let receipt = try? JSONDecoder().decode(PreviewCacheReceipt.self, from: data)
        else {
            return nil
        }
        guard receipt.schema == previewCacheReceiptSchemaVersion,
              receipt.buildIdentity == cacheBuildIdentity,
              receipt.outputPath == output.standardizedFileURL.path,
              receipt.outputSignature == cacheOutputSignature(output)
        else {
            return nil
        }
        return receipt
    }

    private func hasValidatedCacheReceipt(for output: URL) -> Bool {
        validatedCacheReceipt(for: output) != nil
    }

    private func cachedRawDecodeStatus(for output: URL) -> RawDecodeStatus? {
        validatedCacheReceipt(for: output)?.rawDecodeStatus
    }

    private func recordValidatedCacheOutput(_ output: URL, rawDecodeStatus: RawDecodeStatus? = nil) {
        guard isManagedPreviewCacheOutput(output), Self.isUsableCachedOutput(output) else {
            invalidateCacheReceipt(for: output)
            return
        }
        let receipt = PreviewCacheReceipt(
            schema: previewCacheReceiptSchemaVersion,
            buildIdentity: cacheBuildIdentity,
            outputPath: output.standardizedFileURL.path,
            outputSignature: cacheOutputSignature(output),
            rawDecodeStatus: rawDecodeStatus
        )
        do {
            let encoder = JSONEncoder()
            encoder.outputFormatting = [.sortedKeys]
            let data = try encoder.encode(receipt)
            try data.write(to: previewCacheReceiptURL(for: output), options: .atomic)
        } catch {
            invalidateCacheReceipt(for: output)
        }
    }

    private func invalidateCacheReceipt(for output: URL?) {
        guard let output, isManagedPreviewCacheOutput(output) else {
            return
        }
        try? FileManager.default.removeItem(at: previewCacheReceiptURL(for: output))
    }

    private func runValidatedCachedStep(
        output: URL,
        command: String,
        operation: () async throws -> ProcessingCommandResult
    ) async throws -> ProcessingCommandResult {
        if let cached = cachedResultIfAvailable(output: output, command: command) {
            return cached
        }
        let result = try await operation()
        try Task.checkCancellation()
        recordValidatedCacheOutput(output)
        return result
    }

    private func stableDigest(_ text: String) -> String {
        var hash: UInt64 = 14_695_981_039_346_656_037
        for byte in text.utf8 {
            hash ^= UInt64(byte)
            hash &*= 1_099_511_628_211
        }
        return String(hash, radix: 16)
    }

    private struct BatchItemOutcome: Sendable {
        var index: Int
        var output: URL?
        var parameters: [String: String]
        var result: ProcessingCommandResult?
        var errorMessage: String?
        var wasCancelled: Bool
        var progressJobID: UUID
    }

    private static let supportedBatchOperationKinds: [ProcessingOperationKind] = [
        .stretch,
        .denoise,
        .sharpen,
        .starReduce,
        .comaReduce,
    ]

    private func runBatchQueue(generation: Int) async {
        defer {
            if batchGeneration == generation {
                persistBatchQueue()
                isBatchRunning = false
                isBatchPaused = false
                batchTask = nil
                activeBatchItemIDs.removeAll()
                batchProgressJobItems.removeAll()
                batchItemProgress.removeAll()
                isProcessing = jobs.contains { $0.status == .running }
                canCancel = isProcessing
                if isProcessing == false {
                    progressFraction = nil
                    progressMessage = nil
                }
                importPendingAssetsIfPossible()
            }
        }

        while true {
            guard batchGeneration == generation else {
                return
            }
            guard batchQueue.contains(where: { $0.status == .queued }) else {
                return
            }
            while isBatchPaused {
                if batchGeneration != generation {
                    return
                }
                if Task.isCancelled {
                    markRunningBatchItemsCancelled()
                    return
                }
                try? await Task.sleep(for: .milliseconds(150))
            }

            guard batchGeneration == generation else {
                return
            }
            if Task.isCancelled {
                markRunningBatchItemsCancelled()
                return
            }

            let indices = nextQueuedBatchIndices(limit: project.batchMaxConcurrentTasks)
            guard indices.isEmpty == false else {
                return
            }

            for index in indices {
                batchQueue[index].status = .running
                batchQueue[index].recordAttempt()
            }
            persistBatchQueue()

            let workItems = indices.map { index -> (index: Int, item: BatchQueueItem, output: URL?, parameters: [String: String], setupError: String?) in
                let item = batchQueue[index]
                let operationParameters = batchParameters(for: item.kind)
                do {
                    return (index, item, try makeBatchOutputURL(for: item, parameters: operationParameters), operationParameters, nil)
                } catch {
                    return (index, item, nil, operationParameters, error.localizedDescription)
                }
            }

            for workItem in workItems where workItem.setupError != nil {
                guard batchGeneration == generation else {
                    return
                }
                batchQueue[workItem.index].status = .failed
                batchQueue[workItem.index].message = workItem.setupError ?? ""
                batchItemProgress[workItem.item.id] = 1
                persistBatchQueue()
                errorMessage = workItem.setupError
                updateBatchAggregateProgress(message: workItem.item.title)
            }

            let runnableItems = workItems.compactMap { workItem -> (index: Int, item: BatchQueueItem, output: URL, parameters: [String: String], progressJobID: UUID)? in
                guard let output = workItem.output, workItem.setupError == nil else {
                    return nil
                }
                return (workItem.index, workItem.item, output, workItem.parameters, UUID())
            }
            for workItem in runnableItems {
                batchProgressJobItems[workItem.progressJobID] = workItem.item.id
            }

            await withTaskGroup(of: BatchItemOutcome.self) { group in
                let service = processingService
                let previewDirectory = previewDirectory
                let parameters = parameters
                for workItem in runnableItems {
                    group.addTask {
                        do {
                            let result = try await ProcessingProgressContext.$jobID.withValue(workItem.progressJobID) {
                                try await Self.runQueuedItem(
                                    workItem.item,
                                    output: workItem.output,
                                    parameters: parameters,
                                    previewDirectory: previewDirectory,
                                    processingService: service
                                )
                            }
                            return BatchItemOutcome(
                                index: workItem.index,
                                output: workItem.output,
                                parameters: workItem.parameters,
                                result: result,
                                errorMessage: nil,
                                wasCancelled: false,
                                progressJobID: workItem.progressJobID
                            )
                        } catch is CancellationError {
                            return BatchItemOutcome(
                                index: workItem.index,
                                output: workItem.output,
                                parameters: workItem.parameters,
                                result: nil,
                                errorMessage: nil,
                                wasCancelled: true,
                                progressJobID: workItem.progressJobID
                            )
                        } catch {
                            return BatchItemOutcome(
                                index: workItem.index,
                                output: workItem.output,
                                parameters: workItem.parameters,
                                result: nil,
                                errorMessage: error.localizedDescription,
                                wasCancelled: false,
                                progressJobID: workItem.progressJobID
                            )
                        }
                    }
                }

                for await outcome in group {
                    guard batchGeneration == generation else {
                        continue
                    }
                    guard batchQueue.indices.contains(outcome.index) else {
                        continue
                    }
                    batchProgressJobItems.removeValue(forKey: outcome.progressJobID)
                    batchItemProgress[batchQueue[outcome.index].id] = 1

                    if outcome.wasCancelled || Task.isCancelled {
                        batchQueue[outcome.index].status = .cancelled
                        batchQueue[outcome.index].message = localized(.cancellingTask)
                        persistBatchQueue()
                        updateBatchAggregateProgress(message: batchQueue[outcome.index].title)
                        continue
                    }

                    if let result = outcome.result, let output = outcome.output {
                        batchQueue[outcome.index].status = .succeeded
                        batchQueue[outcome.index].outputURL = output
                        batchQueue[outcome.index].message = message(for: result)
                        persistBatchQueue()
                        setPreview(output)
                        recordOperation(
                            batchQueue[outcome.index].kind,
                            parameters: outcome.parameters,
                            outputURL: output,
                            sourceURL: batchQueue[outcome.index].inputURL
                        )
                        commandOutput = batchQueue[outcome.index].message
                        updateBatchAggregateProgress(message: batchQueue[outcome.index].title)
                    } else {
                        batchQueue[outcome.index].status = .failed
                        batchQueue[outcome.index].message = outcome.errorMessage ?? "Batch item failed"
                        persistBatchQueue()
                        errorMessage = batchQueue[outcome.index].message
                        updateBatchAggregateProgress(message: batchQueue[outcome.index].title)
                    }
                }
            }

            guard batchGeneration == generation else {
                return
            }
            if Task.isCancelled {
                markRunningBatchItemsCancelled()
                return
            }
        }
    }

    private func nextQueuedBatchIndices(limit: Int) -> [Int] {
        let clampedLimit = max(1, min(limit, 4))
        return Array(batchQueue.indices.filter { batchQueue[$0].status == .queued }.prefix(clampedLimit))
    }

    private func markRunningBatchItemsCancelled() {
        for index in batchQueue.indices where batchQueue[index].status == .running {
            batchQueue[index].status = .cancelled
            batchQueue[index].message = localized(.cancellingTask)
        }
        persistBatchQueue()
    }

    private static func runQueuedItem(
        _ item: BatchQueueItem,
        output: URL,
        parameters: ProcessingParameters,
        previewDirectory: URL,
        processingService: any ProcessingService
    ) async throws -> ProcessingCommandResult {
        try FileManager.default.createDirectory(at: previewDirectory, withIntermediateDirectories: true)
        try Task.checkCancellation()
        let result: ProcessingCommandResult
        switch item.kind {
        case .stretch:
            result = try await processingService.autoStretch(
                input: item.inputURL,
                output: output,
                targetBackground: parameters.stretchTargetBackground
            )
        case .denoise:
            result = try await processingService.denoise(
                input: item.inputURL,
                output: output,
                amount: parameters.denoiseAmount,
                chromaAmount: parameters.denoiseChromaAmount,
                radius: parameters.denoiseRadius
            )
        case .sharpen:
            result = try await processingService.sharpen(
                input: item.inputURL,
                output: output,
                amount: parameters.sharpenAmount,
                radius: parameters.sharpenRadius
            )
        case .starReduce:
            result = try await processingService.reduceStars(
                input: item.inputURL,
                output: output,
                amount: parameters.starReductionAmount,
                profileAware: parameters.profileAwareStarReduction,
                edgeAware: parameters.edgeAwareStarReduction
            )
        case .comaReduce:
            result = try await processingService.reduceComa(
                input: item.inputURL,
                output: output,
                amount: parameters.comaReductionAmount,
                radius: parameters.comaReductionRadius,
                eccentricity: parameters.comaReductionEccentricity,
                edgeAware: parameters.edgeAwareComaReduction
            )
        default:
            throw BatchQueueError.unsupportedOperation
        }
        try Task.checkCancellation()
        return result
    }

    private static func isUsableCachedOutput(_ output: URL) -> Bool {
        var isDirectory = ObjCBool(false)
        guard FileManager.default.fileExists(atPath: output.path, isDirectory: &isDirectory),
              isDirectory.boolValue == false,
              let resourceValues = try? output.resourceValues(forKeys: [.isRegularFileKey, .isSymbolicLinkKey]),
              resourceValues.isRegularFile == true,
              resourceValues.isSymbolicLink != true,
              let attributes = try? FileManager.default.attributesOfItem(atPath: output.path),
              ((attributes[.size] as? NSNumber)?.int64Value ?? 0) > 0
        else {
            return false
        }

        let pathExtension = output.pathExtension.lowercased()
        if ["fit", "fits", "fts"].contains(pathExtension) {
            return isReadableFITS(output)
        }
        let imageExtensions: Set<String> = ["png", "jpg", "jpeg", "tif", "tiff", "heic", "heif", "hif"]
        guard imageExtensions.contains(pathExtension) else {
            return true
        }
        guard let source = CGImageSourceCreateWithURL(output as CFURL, nil),
              CGImageSourceGetCount(source) > 0
        else {
            return false
        }
        let validationOptions = [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceThumbnailMaxPixelSize: 8,
            kCGImageSourceShouldCacheImmediately: true,
        ] as CFDictionary
        guard let thumbnail = CGImageSourceCreateThumbnailAtIndex(source, 0, validationOptions) else {
            return false
        }
        return thumbnail.width > 0 && thumbnail.height > 0
    }

    private static func isReadableFITS(_ url: URL) -> Bool {
        guard let handle = try? FileHandle(forReadingFrom: url) else {
            return false
        }
        defer { try? handle.close() }
        let header: Data
        let fileSize: UInt64
        do {
            guard let data = try handle.read(upToCount: 80) else {
                return false
            }
            header = data
            fileSize = try handle.seekToEnd()
        } catch {
            return false
        }
        guard header.count == 80, fileSize >= 2_880 else {
            return false
        }
        let keyword = String(decoding: header.prefix(10), as: UTF8.self)
        return keyword.hasPrefix("SIMPLE  =") || keyword.hasPrefix("XTENSION=")
    }

    private func batchParameters(for kind: ProcessingOperationKind) -> [String: String] {
        switch kind {
        case .stretch:
            return ["targetBackground": String(parameters.stretchTargetBackground)]
        case .denoise:
            return [
                "amount": String(parameters.denoiseAmount),
                "chromaAmount": String(parameters.denoiseChromaAmount),
                "radius": String(parameters.denoiseRadius),
            ]
        case .sharpen:
            return ["amount": String(parameters.sharpenAmount), "radius": String(parameters.sharpenRadius)]
        case .starReduce:
            return [
                "amount": String(parameters.starReductionAmount),
                "profileAware": String(parameters.profileAwareStarReduction),
                "edgeAware": String(parameters.edgeAwareStarReduction),
            ]
        case .comaReduce:
            return [
                "amount": String(parameters.comaReductionAmount),
                "radius": String(parameters.comaReductionRadius),
                "eccentricity": String(parameters.comaReductionEccentricity),
                "edgeAware": String(parameters.edgeAwareComaReduction),
            ]
        default:
            return [:]
        }
    }

    private func createMasterIfNeeded(
        title: String,
        assets: [PhotonStackAsset],
        output: URL,
        method: StackMethod,
        rawOptions: RawProcessingOptions,
        log: inout [String]
    ) async throws -> URL? {
        guard assets.isEmpty == false else {
            return nil
        }
        try Task.checkCancellation()
        let result = try await processingService.createMaster(
            output: output,
            method: method,
            inputs: assets.map(\.originalURL),
            rawOptions: rawOptions
        )
        log.append("Created \(title):")
        log.append(message(for: result))
        return output
    }

    private func assets(with role: CalibrationFrameRole) -> [PhotonStackAsset] {
        project.assets.filter { $0.role == role }
    }

    private static func batchRegistrationAssets(in project: PhotonStackProject) -> [PhotonStackAsset] {
        let supportedKinds: Set<AssetKind> = [.raw, .fits, .tiff, .jpeg, .heif, .png]
        let lights = project.assets.filter { $0.role == .light }
        let sourceAssets = lights.isEmpty ? project.assets : lights
        return sourceAssets.filter { supportedKinds.contains($0.kind) }
    }

    private static func batchRegistrationCandidateIDs(in project: PhotonStackProject) -> Set<PhotonStackAsset.ID> {
        Set(batchRegistrationAssets(in: project).map(\.id))
    }

    private func resetBatchRegistrationSelection(selectAll: Bool) {
        batchRegistrationCandidateIDs = Self.batchRegistrationCandidateIDs(in: project)
        batchRegistrationSelection = selectAll ? batchRegistrationCandidateIDs : []
    }

    private func syncBatchRegistrationSelection(selectNewCandidates: Bool) {
        let candidateIDs = Self.batchRegistrationCandidateIDs(in: project)
        let newCandidateIDs = candidateIDs.subtracting(batchRegistrationCandidateIDs)
        batchRegistrationCandidateIDs = candidateIDs
        batchRegistrationSelection.formIntersection(candidateIDs)
        if selectNewCandidates {
            batchRegistrationSelection.formUnion(newCandidateIDs)
        }
    }

    private func selectedBatchRegistrationMovingFrames(reference: PhotonStackAsset?) -> [PhotonStackAsset] {
        let selectedIDs = batchRegistrationSelection
        return batchRegistrationAssets.filter { asset in
            asset.id != reference?.id && selectedIDs.contains(asset.id)
        }
    }

    private func message(for result: ProcessingCommandResult) -> String {
        if result.standardOutput.isEmpty == false {
            return result.standardOutput
        }
        if result.standardError.isEmpty == false {
            return result.standardError
        }
        return result.command.joined(separator: " ")
    }

    private func parseConsoleCommand(_ text: String) throws -> [String] {
        var arguments: [String] = []
        var current = ""
        var quote: Character?
        var escaping = false

        for character in text {
            if escaping {
                current.append(character)
                escaping = false
                continue
            }

            if character == "\\" {
                escaping = true
                continue
            }

            if let activeQuote = quote {
                if character == activeQuote {
                    quote = nil
                } else {
                    current.append(character)
                }
                continue
            }

            if character == "\"" || character == "'" {
                quote = character
                continue
            }

            if character.isWhitespace {
                if current.isEmpty == false {
                    arguments.append(current)
                    current = ""
                }
                continue
            }

            current.append(character)
        }

        if escaping {
            current.append("\\")
        }
        guard quote == nil else {
            throw ConsoleCommandError.unclosedQuote
        }
        if current.isEmpty == false {
            arguments.append(current)
        }

        if let first = arguments.first,
           first == "photonstack" || first.hasSuffix("/photonstack") {
            arguments.removeFirst()
        }
        if arguments.isEmpty {
            throw ConsoleCommandError.empty
        }
        return arguments
    }

    private func parseHistogram(
        _ result: ProcessingCommandResult,
        expectedBins: Int
    ) throws -> HistogramSnapshot {
        guard let object = try parseJSONObject(from: result.standardOutput),
              object["type"] as? String == "complete",
              object["command"] as? String == "histogram",
              expectedBins > 0,
              let rawBins = object["bins"] as? [NSNumber],
              rawBins.count == expectedBins,
              rawBins.allSatisfy({
                  let value = $0.doubleValue
                  return CFGetTypeID($0) != CFBooleanGetTypeID()
                      && value.isFinite
                      && value >= 0
              }),
              let minimum = jsonFiniteDouble(object["minimum"]),
              let maximum = jsonFiniteDouble(object["maximum"]),
              let mean = jsonFiniteDouble(object["mean"])
        else {
            throw ProcessingServiceError.invalidReport(command: "histogram")
        }

        guard maximum >= minimum,
              mean >= minimum,
              mean <= maximum
        else {
            throw ProcessingServiceError.invalidReport(command: "histogram")
        }

        return HistogramSnapshot(
            bins: rawBins.map(\.doubleValue),
            minimum: minimum,
            maximum: maximum,
            mean: mean
        )
    }

    private func parseRawDecodeStatus(_ result: ProcessingCommandResult) throws -> RawDecodeStatus? {
        guard let object = try parseJSONObject(from: result.standardOutput) else {
            return nil
        }
        let decodeKeys = [
            "decodeBackend",
            "decodeFallback",
            "decodeFallbackCode",
            "decodeFallbackMessage",
        ]
        guard decodeKeys.contains(where: { object[$0] != nil }) else {
            return nil
        }
        guard object["type"] as? String == "complete",
              object["command"] as? String == "preview",
              let backendName = object["decodeBackend"] as? String,
              let backend = RawDecodeBackend(rawValue: backendName),
              let usedFallback = jsonBoolean(object["decodeFallback"]),
              let fallbackErrorCode = object["decodeFallbackCode"] as? String,
              let fallbackMessage = object["decodeFallbackMessage"] as? String,
              (usedFallback && backend == .imageIO && fallbackErrorCode.isEmpty == false &&
                  fallbackMessage.isEmpty == false) ||
                  (usedFallback == false && backend == .appleRaw && fallbackErrorCode.isEmpty &&
                      fallbackMessage.isEmpty)
        else {
            throw ProcessingServiceError.invalidReport(command: "preview")
        }
        return RawDecodeStatus(
            backend: backend,
            usedFallback: usedFallback,
            fallbackErrorCode: fallbackErrorCode,
            fallbackMessage: fallbackMessage
        )
    }

    private func parseInspectMetadata(_ result: ProcessingCommandResult) throws -> AssetMetadata {
        guard let object = try parseJSONObject(from: result.standardOutput),
              let width = jsonInteger(object["width"]),
              let height = jsonInteger(object["height"]),
              let channels = jsonInteger(object["channels"]),
              let bitsPerChannel = jsonInteger(object["bitsPerChannel"]),
              let format = object["format"] as? String,
              width > 0,
              height > 0,
              channels > 0,
              bitsPerChannel > 0,
              format.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false
        else {
            throw ProcessingServiceError.invalidReport(command: "inspect")
        }

        let exposureTimeSeconds = try positiveMetadataDouble("exposureTimeSeconds", in: object)
        let fNumber = try positiveMetadataDouble("fNumber", in: object)
        let focalLengthMM = try positiveMetadataDouble("focalLengthMM", in: object)
        let iso = try positiveMetadataInteger("iso", in: object)
        let orientation = try metadataInteger("orientation", in: object, range: 1...8)

        return AssetMetadata(
            width: width,
            height: height,
            channels: channels,
            bitsPerChannel: bitsPerChannel,
            formatDescription: format,
            cameraMake: metadataString("cameraMake", in: object),
            cameraModel: metadataString("cameraModel", in: object),
            lensModel: metadataString("lensModel", in: object),
            captureDate: metadataString("captureDate", in: object),
            colorModel: metadataString("colorModel", in: object),
            colorProfile: metadataString("colorProfile", in: object),
            rawDecoder: metadataString("rawDecoder", in: object),
            whiteBalance: metadataString("whiteBalance", in: object),
            exposureBias: metadataString("exposureBias", in: object),
            exposureTimeSeconds: exposureTimeSeconds,
            fNumber: fNumber,
            focalLengthMM: focalLengthMM,
            iso: iso,
            orientation: orientation
        )
    }

    private func metadataObject(in object: [String: Any]) -> [String: Any] {
        object["metadata"] as? [String: Any] ?? object
    }

    private func metadataString(_ key: String, in object: [String: Any]) -> String? {
        let metadata = metadataObject(in: object)
        let value = (metadata[key] as? String)?.trimmingCharacters(in: .whitespacesAndNewlines)
        return value?.isEmpty == false ? value : nil
    }

    private func positiveMetadataDouble(_ key: String, in object: [String: Any]) throws -> Double? {
        let metadata = metadataObject(in: object)
        guard let rawValue = metadata[key] else {
            return nil
        }
        guard let value = jsonFiniteDouble(rawValue), value > 0 else {
            throw ProcessingServiceError.invalidReport(command: "inspect")
        }
        return value
    }

    private func positiveMetadataInteger(_ key: String, in object: [String: Any]) throws -> Int? {
        try metadataInteger(key, in: object, range: 1...Int.max)
    }

    private func metadataInteger(
        _ key: String,
        in object: [String: Any],
        range: ClosedRange<Int>
    ) throws -> Int? {
        let metadata = metadataObject(in: object)
        guard let rawValue = metadata[key] else {
            return nil
        }
        guard let value = jsonInteger(rawValue), range.contains(value) else {
            throw ProcessingServiceError.invalidReport(command: "inspect")
        }
        return value
    }

    private func parseMosaicQualityReport(
        _ result: ProcessingCommandResult,
        expectedPanelCount: Int
    ) throws -> MosaicQualityReport {
        guard let object = try parseJSONObject(from: result.standardOutput),
              object["type"] as? String == "complete",
              object["command"] as? String == "mosaic",
              let autoAligned = jsonBoolean(object["autoAligned"]),
              let matches = jsonInteger(object["matches"]),
              let fallbackPanels = jsonInteger(object["fallbackPanels"]),
              let exposureMatched = jsonBoolean(object["exposureMatched"]),
              let blend = object["blend"] as? String,
              blend.isEmpty == false,
              let rawPlacements = object["placements"] as? [[String: Any]],
              rawPlacements.count == expectedPanelCount,
              matches >= 0,
              fallbackPanels >= 0,
              fallbackPanels <= expectedPanelCount
        else {
            throw ProcessingServiceError.invalidReport(command: "mosaic")
        }

        let placements = try rawPlacements.map { placement in
            guard let index = jsonInteger(placement["index"]),
                  let x = jsonFiniteDouble(placement["x"]),
                  let y = jsonFiniteDouble(placement["y"]),
                  let a = jsonFiniteDouble(placement["a"]),
                  let b = jsonFiniteDouble(placement["b"]),
                  let c = jsonFiniteDouble(placement["c"]),
                  let d = jsonFiniteDouble(placement["d"]),
                  let dx = jsonFiniteDouble(placement["dx"]),
                  let dy = jsonFiniteDouble(placement["dy"]),
                  let model = placement["model"] as? String,
                  model.isEmpty == false,
                  let placementMatches = jsonInteger(placement["matches"]),
                  let references = jsonInteger(placement["references"]),
                  let placementAutoAligned = jsonBoolean(placement["autoAligned"]),
                  let fallback = jsonBoolean(placement["fallback"]),
                  let reducedModel = jsonBoolean(placement["reducedModel"]),
                  let coarseAlignment = jsonBoolean(placement["coarseAlignment"]),
                  index >= 0,
                  index < expectedPanelCount,
                  placementMatches >= 0,
                  references >= 0
            else {
                throw ProcessingServiceError.invalidReport(command: "mosaic")
            }
            return MosaicPlacementReport(
                index: index,
                x: x,
                y: y,
                a: a,
                b: b,
                c: c,
                d: d,
                dx: dx,
                dy: dy,
                model: model,
                matches: placementMatches,
                references: references,
                autoAligned: placementAutoAligned,
                fallback: fallback,
                reducedModel: reducedModel,
                coarseAlignment: coarseAlignment
            )
        }
        guard Set(placements.map(\.index)).count == expectedPanelCount else {
            throw ProcessingServiceError.invalidReport(command: "mosaic")
        }

        return MosaicQualityReport(
            autoAligned: autoAligned,
            matches: matches,
            fallbackPanels: fallbackPanels,
            exposureMatched: exposureMatched,
            blend: blend,
            placements: placements
        )
    }

    private func parseArtifactTrailReport(_ result: ProcessingCommandResult) throws -> ArtifactTrailReport {
        guard let object = try parseJSONObject(from: result.standardOutput),
              object["type"] as? String == "complete",
              object["command"] as? String == "artifacts detect",
              let trailCount = jsonInteger(object["trails"]),
              let removedTrailCount = jsonInteger(object["removedTrails"]),
              let protectedMeteorCount = jsonInteger(object["protectedMeteors"]),
              let rawItems = object["items"] as? [[String: Any]],
              trailCount >= 0,
              removedTrailCount >= 0,
              protectedMeteorCount >= 0,
              trailCount == rawItems.count,
              removedTrailCount <= trailCount,
              protectedMeteorCount <= trailCount
        else {
            throw ProcessingServiceError.invalidReport(command: "artifacts detect")
        }

        let imageWidth: Int?
        if object["width"] != nil {
            guard let value = jsonInteger(object["width"]), value > 0 else {
                throw ProcessingServiceError.invalidReport(command: "artifacts detect")
            }
            imageWidth = value
        } else {
            imageWidth = nil
        }
        let imageHeight: Int?
        if object["height"] != nil {
            guard let value = jsonInteger(object["height"]), value > 0 else {
                throw ProcessingServiceError.invalidReport(command: "artifacts detect")
            }
            imageHeight = value
        } else {
            imageHeight = nil
        }
        guard (imageWidth == nil) == (imageHeight == nil) else {
            throw ProcessingServiceError.invalidReport(command: "artifacts detect")
        }

        let items = try rawItems.enumerated().map { index, item in
            guard let kind = item["kind"] as? String,
                  ["airplane", "drone", "satellite", "meteor"].contains(kind),
                  let confidence = jsonFiniteDouble(item["confidence"]),
                  let x1 = jsonFiniteDouble(item["x1"]),
                  let y1 = jsonFiniteDouble(item["y1"]),
                  let x2 = jsonFiniteDouble(item["x2"]),
                  let y2 = jsonFiniteDouble(item["y2"]),
                  let length = jsonFiniteDouble(item["length"]),
                  let width = jsonFiniteDouble(item["width"]),
                  let meanBrightness = jsonFiniteDouble(item["meanBrightness"]),
                  (0...1).contains(confidence),
                  length > 0,
                  width > 0,
                  meanBrightness >= 0
            else {
                throw ProcessingServiceError.invalidReport(command: "artifacts detect")
            }

            let weight = try optionalUnitMetric("weight", in: item)
                ?? artifactTrailWeightFallback(item)
            let peakPosition = try optionalUnitMetric("peakPosition", in: item) ?? 0.5
            let taperScore = try optionalUnitMetric("taperScore", in: item) ?? 0

            let rawPath: [[String: Any]]
            if let pathValue = item["path"] {
                guard let decodedPath = pathValue as? [[String: Any]],
                      (2...4096).contains(decodedPath.count)
                else {
                    throw ProcessingServiceError.invalidReport(command: "artifacts detect")
                }
                rawPath = decodedPath
            } else {
                rawPath = []
            }
            let path = try rawPath.map { point -> ArtifactTrailPathPoint in
                guard let x = jsonFiniteDouble(point["x"]),
                      let y = jsonFiniteDouble(point["y"])
                else {
                    throw ProcessingServiceError.invalidReport(command: "artifacts detect")
                }
                return ArtifactTrailPathPoint(x: x, y: y)
            }
            return ArtifactTrailItem(
                index: index,
                kind: kind,
                confidence: confidence,
                x1: x1,
                y1: y1,
                x2: x2,
                y2: y2,
                length: length,
                width: width,
                meanBrightness: meanBrightness,
                weight: weight,
                peakPosition: peakPosition,
                taperScore: taperScore,
                path: path
            )
        }

        return ArtifactTrailReport(
            trails: trailCount,
            removedTrails: removedTrailCount,
            protectedMeteors: protectedMeteorCount,
            items: items,
            imageWidth: imageWidth,
            imageHeight: imageHeight
        )
    }

    private func encodedArtifactTrailRemovalHints(_ report: ArtifactTrailReport) -> String? {
        let orderedItems = report.items.sorted { $0.index < $1.index }
        guard orderedItems.indices.allSatisfy({ orderedItems[$0].index == $0 }) else {
            return nil
        }
        let supportedKinds: Set<String> = ["airplane", "drone", "satellite", "meteor"]
        var encodedTrails: [String] = []
        encodedTrails.reserveCapacity(orderedItems.count)
        for item in orderedItems {
            let values = [
                item.confidence,
                item.x1,
                item.y1,
                item.x2,
                item.y2,
                item.length,
                item.width,
                item.meanBrightness,
                item.weight,
                item.peakPosition,
                item.taperScore,
            ]
            guard supportedKinds.contains(item.kind),
                  values.allSatisfy(\.isFinite),
                  item.confidence >= 0,
                  item.confidence <= 1,
                  item.length >= 0,
                  item.width > 0,
                  item.peakPosition >= 0,
                  item.peakPosition <= 1,
                  item.path.allSatisfy({ $0.x.isFinite && $0.y.isFinite })
            else {
                return nil
            }
            let encodedPath = item.path
                .map { "\($0.x):\($0.y)" }
                .joined(separator: ",")
            encodedTrails.append([
                item.kind,
                String(item.confidence),
                String(item.x1),
                String(item.y1),
                String(item.x2),
                String(item.y2),
                String(item.length),
                String(item.width),
                String(item.meanBrightness),
                String(item.weight),
                String(item.peakPosition),
                String(item.taperScore),
                encodedPath,
            ].joined(separator: "|"))
        }
        return encodedTrails.joined(separator: ";")
    }

    private func encodedArtifactTrailSelection(
        _ selectedIndices: [Int],
        report: ArtifactTrailReport
    ) -> String? {
        guard selectedIndices.isEmpty == false,
              selectedIndices.allSatisfy({ $0 >= 0 }),
              Set(selectedIndices).count == selectedIndices.count
        else {
            return nil
        }
        guard let width = report.imageWidth,
              let height = report.imageHeight,
              width > 0,
              height > 0
        else {
            return nil
        }
        let widthValue = Double(width)
        let heightValue = Double(height)
        let diagonal = hypot(widthValue, heightValue)
        let minimumDimension = Double(min(width, height))
        var itemsByIndex: [Int: ArtifactTrailItem] = [:]
        for item in report.items {
            guard itemsByIndex[item.index] == nil else {
                return nil
            }
            itemsByIndex[item.index] = item
        }
        let selections = selectedIndices.compactMap { index -> RecordedArtifactTrailSelection? in
            guard let item = itemsByIndex[index] else {
                return nil
            }
            return recordedArtifactTrailSelection(
                item,
                imageWidth: widthValue,
                imageHeight: heightValue,
                diagonal: diagonal,
                minimumDimension: minimumDimension
            )
        }
        guard selections.count == selectedIndices.count,
              selections.allSatisfy(\.isValid)
        else {
            return nil
        }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        guard let data = try? encoder.encode(selections) else {
            return nil
        }
        return String(data: data, encoding: .utf8)
    }

    private func matchedArtifactTrailIndices(
        _ encodedSelection: String,
        in report: ArtifactTrailReport,
        operationKind: ProcessingOperationKind
    ) throws -> [Int] {
        guard let data = encodedSelection.data(using: .utf8),
              let recorded = try? JSONDecoder().decode([RecordedArtifactTrailSelection].self, from: data),
              recorded.isEmpty == false,
              recorded.allSatisfy(\.isValid),
              let width = report.imageWidth,
              let height = report.imageHeight,
              width > 0,
              height > 0
        else {
            throw ProcessingServiceError.invalidSelection(command: operationKind.rawValue)
        }

        let widthValue = Double(width)
        let heightValue = Double(height)
        let diagonal = hypot(widthValue, heightValue)
        let minimumDimension = Double(min(width, height))
        let candidates = report.items.map { item in
            (
                index: item.index,
                selection: recordedArtifactTrailSelection(
                    item,
                    imageWidth: widthValue,
                    imageHeight: heightValue,
                    diagonal: diagonal,
                    minimumDimension: minimumDimension
                )
            )
        }
        guard candidates.count >= recorded.count,
              candidates.allSatisfy(\.selection.isValid)
        else {
            throw ProcessingServiceError.invalidSelection(command: operationKind.rawValue)
        }
        let costs = recorded.map { target in
            candidates.map { candidate in
                guard candidate.selection.kind == target.kind else {
                    return Double.infinity
                }
                let score = target.matchScore(to: candidate.selection)
                return score <= 0.035 ? score : Double.infinity
            }
        }
        let assignment = try unambiguousCandidateAssignment(
            costs,
            ambiguityGap: 0.0015,
            command: operationKind.rawValue
        )
        return assignment.map { candidates[$0].index }.sorted()
    }

    private func recordedArtifactTrailSelection(
        _ item: ArtifactTrailItem,
        imageWidth: Double,
        imageHeight: Double,
        diagonal: Double,
        minimumDimension: Double
    ) -> RecordedArtifactTrailSelection {
        RecordedArtifactTrailSelection(
            kind: item.kind,
            x1: item.x1 / imageWidth,
            y1: item.y1 / imageHeight,
            x2: item.x2 / imageWidth,
            y2: item.y2 / imageHeight,
            length: item.length / diagonal,
            width: item.width / minimumDimension,
            path: normalizedArtifactTrailPath(
                item,
                imageWidth: imageWidth,
                imageHeight: imageHeight
            )
        )
    }

    private func normalizedArtifactTrailPath(
        _ item: ArtifactTrailItem,
        imageWidth: Double,
        imageHeight: Double
    ) -> [RecordedArtifactTrailPoint]? {
        let sourcePoints = item.path.count >= 2
            ? item.path
            : [
                ArtifactTrailPathPoint(x: item.x1, y: item.y1),
                ArtifactTrailPathPoint(x: item.x2, y: item.y2),
            ]
        let points = sourcePoints.map {
            RecordedArtifactTrailPoint(
                x: $0.x / imageWidth,
                y: $0.y / imageHeight
            )
        }
        guard points.allSatisfy(\.isValid) else {
            return nil
        }
        return resampledArtifactTrailPath(points, sampleCount: RecordedArtifactTrailSelection.pathSampleCount)
    }

    private func resampledArtifactTrailPath(
        _ points: [RecordedArtifactTrailPoint],
        sampleCount: Int
    ) -> [RecordedArtifactTrailPoint]? {
        guard points.count >= 2, sampleCount >= 2 else {
            return nil
        }
        var cumulative = Array(repeating: 0.0, count: points.count)
        for index in 1..<points.count {
            cumulative[index] = cumulative[index - 1] + hypot(
                points[index].x - points[index - 1].x,
                points[index].y - points[index - 1].y
            )
        }
        guard let totalLength = cumulative.last,
              totalLength.isFinite,
              totalLength > 0
        else {
            return nil
        }

        var result: [RecordedArtifactTrailPoint] = []
        result.reserveCapacity(sampleCount)
        var segment = 1
        for sample in 0..<sampleCount {
            let targetDistance = totalLength * Double(sample) / Double(sampleCount - 1)
            while segment + 1 < cumulative.count, cumulative[segment] < targetDistance {
                segment += 1
            }
            let startIndex = max(segment - 1, 0)
            let segmentLength = cumulative[segment] - cumulative[startIndex]
            let fraction = segmentLength > 0
                ? (targetDistance - cumulative[startIndex]) / segmentLength
                : 0
            result.append(
                RecordedArtifactTrailPoint(
                    x: points[startIndex].x + (points[segment].x - points[startIndex].x) * fraction,
                    y: points[startIndex].y + (points[segment].y - points[startIndex].y) * fraction
                )
            )
        }
        return result
    }

    private func artifactTrailWeightFallback(_ item: [String: Any]) -> Double {
        let confidence = jsonFiniteDouble(item["confidence"]) ?? 0
        let length = jsonFiniteDouble(item["length"]) ?? 0
        let width = jsonFiniteDouble(item["width"]) ?? 0
        let brightness = jsonFiniteDouble(item["meanBrightness"]) ?? 0
        let lengthScore = min(max(length / 320.0, 0), 1)
        let widthScore = min(max((width - 1.0) / 7.0, 0), 1)
        let sizeScore = min(max(lengthScore * 0.82 + widthScore * 0.18, 0), 1)
        let brightnessScore = min(max(brightness / 0.18, 0), 1)
        return min(max(confidence * 0.50 + sizeScore * 0.25 + brightnessScore * 0.25, 0), 1)
    }

    private func optionalUnitMetric(_ key: String, in object: [String: Any]) throws -> Double? {
        guard object[key] != nil else {
            return nil
        }
        guard let value = jsonFiniteDouble(object[key]), (0...1).contains(value) else {
            throw ProcessingServiceError.invalidReport(command: "artifacts detect")
        }
        return value
    }

    private func parseCloudRegionReport(_ result: ProcessingCommandResult) throws -> CloudRegionReport {
        guard let object = try parseJSONObject(from: result.standardOutput),
              object["type"] as? String == "complete",
              let command = object["command"] as? String,
              (command == "clouds detect" || command == "clouds temporal-detect"),
              let cloudCount = jsonInteger(object["clouds"]),
              let removedCloudCount = jsonInteger(object["removedClouds"]),
              let rawItems = object["items"] as? [[String: Any]],
              cloudCount >= 0,
              removedCloudCount >= 0,
              cloudCount == rawItems.count,
              removedCloudCount <= cloudCount
        else {
            throw ProcessingServiceError.invalidReport(command: "clouds detect")
        }

        let imageWidth: Int?
        if object["width"] != nil {
            guard let value = jsonInteger(object["width"]), value > 0 else {
                throw ProcessingServiceError.invalidReport(command: "clouds detect")
            }
            imageWidth = value
        } else {
            imageWidth = nil
        }
        let imageHeight: Int?
        if object["height"] != nil {
            guard let value = jsonInteger(object["height"]), value > 0 else {
                throw ProcessingServiceError.invalidReport(command: "clouds detect")
            }
            imageHeight = value
        } else {
            imageHeight = nil
        }
        guard (imageWidth == nil) == (imageHeight == nil) else {
            throw ProcessingServiceError.invalidReport(command: "clouds detect")
        }
        let temporalFramesUsed = jsonInteger(object["temporalFramesUsed"]) ?? 0
        let temporallyConfirmedClouds = jsonInteger(object["temporallyConfirmedClouds"]) ?? 0
        guard temporalFramesUsed >= 0, temporallyConfirmedClouds >= 0, temporallyConfirmedClouds <= cloudCount else {
            throw ProcessingServiceError.invalidReport(command: "clouds detect")
        }

        let items = try rawItems.enumerated().map { index, item in
            guard let confidence = jsonFiniteDouble(item["confidence"]),
                  let x = jsonFiniteDouble(item["x"]),
                  let y = jsonFiniteDouble(item["y"]),
                  let width = jsonFiniteDouble(item["width"]),
                  let height = jsonFiniteDouble(item["height"]),
                  let coverage = jsonFiniteDouble(item["coverage"]),
                  let meanLuminance = jsonFiniteDouble(item["meanLuminance"]),
                  let backgroundLuminance = jsonFiniteDouble(item["backgroundLuminance"]),
                  let rawMask = item["mask"] as? [[String: Any]],
                  rawMask.count <= 10_000,
                  (0...1).contains(confidence),
                  (0...1).contains(coverage),
                  width > 0,
                  height > 0,
                  meanLuminance >= 0,
                  backgroundLuminance >= 0
            else {
                throw ProcessingServiceError.invalidReport(command: "clouds detect")
            }
            let temporalSupport = jsonInteger(item["temporalSupport"]) ?? 0
            guard temporalSupport >= 0, temporalSupport <= temporalFramesUsed else {
                throw ProcessingServiceError.invalidReport(command: "clouds detect")
            }
            let decodedMask = try rawMask.map { rect -> CloudMaskRectItem in
                guard let maskX = jsonFiniteDouble(rect["x"]),
                      let maskY = jsonFiniteDouble(rect["y"]),
                      let maskWidth = jsonFiniteDouble(rect["width"]),
                      let maskHeight = jsonFiniteDouble(rect["height"]),
                      maskWidth > 0,
                      maskHeight > 0
                else {
                    throw ProcessingServiceError.invalidReport(command: "clouds detect")
                }
                return CloudMaskRectItem(
                    x: maskX,
                    y: maskY,
                    width: maskWidth,
                    height: maskHeight
                )
            }
            return CloudRegionItem(
                index: index,
                confidence: confidence,
                x: x,
                y: y,
                width: width,
                height: height,
                coverage: coverage,
                meanLuminance: meanLuminance,
                backgroundLuminance: backgroundLuminance,
                temporalSupport: temporalSupport,
                maskRects: decodedMask.isEmpty
                    ? [CloudMaskRectItem(x: x, y: y, width: width, height: height)]
                    : decodedMask
            )
        }

        return CloudRegionReport(
            clouds: cloudCount,
            removedClouds: removedCloudCount,
            items: items,
            imageWidth: imageWidth,
            imageHeight: imageHeight,
            temporalFramesUsed: temporalFramesUsed,
            temporallyConfirmedClouds: temporallyConfirmedClouds
        )
    }

    private func encodedCloudRegionSelection(
        _ selectedIndices: [Int],
        report: CloudRegionReport
    ) -> String? {
        guard selectedIndices.isEmpty == false,
              selectedIndices.allSatisfy({ $0 >= 0 }),
              Set(selectedIndices).count == selectedIndices.count
        else {
            return nil
        }
        guard let width = report.imageWidth,
              let height = report.imageHeight,
              width > 0,
              height > 0
        else {
            return nil
        }
        let widthValue = Double(width)
        let heightValue = Double(height)
        var itemsByIndex: [Int: CloudRegionItem] = [:]
        for item in report.items {
            guard itemsByIndex[item.index] == nil else {
                return nil
            }
            itemsByIndex[item.index] = item
        }
        let selections = selectedIndices.compactMap { index -> RecordedCloudRegionSelection? in
            guard let item = itemsByIndex[index] else {
                return nil
            }
            return RecordedCloudRegionSelection(
                x: item.x / widthValue,
                y: item.y / heightValue,
                width: item.width / widthValue,
                height: item.height / heightValue,
                coverage: item.coverage
            )
        }
        guard selections.count == selectedIndices.count else {
            return nil
        }
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        guard let data = try? encoder.encode(selections) else {
            return nil
        }
        return String(data: data, encoding: .utf8)
    }

    private func matchedCloudRegionIndices(
        _ encodedSelection: String,
        in report: CloudRegionReport,
        operationKind: ProcessingOperationKind
    ) throws -> [Int] {
        guard let data = encodedSelection.data(using: .utf8),
              let recorded = try? JSONDecoder().decode([RecordedCloudRegionSelection].self, from: data),
              recorded.isEmpty == false,
              recorded.allSatisfy(\.isValid),
              let width = report.imageWidth,
              let height = report.imageHeight,
              width > 0,
              height > 0
        else {
            throw ProcessingServiceError.invalidSelection(command: operationKind.rawValue)
        }

        let widthValue = Double(width)
        let heightValue = Double(height)
        let candidates = report.items.map { item in
            (
                index: item.index,
                selection: RecordedCloudRegionSelection(
                    x: item.x / widthValue,
                    y: item.y / heightValue,
                    width: item.width / widthValue,
                    height: item.height / heightValue,
                    coverage: item.coverage
                )
            )
        }
        guard candidates.count >= recorded.count,
              candidates.allSatisfy(\.selection.isValid)
        else {
            throw ProcessingServiceError.invalidSelection(command: operationKind.rawValue)
        }
        let costs = recorded.map { target in
            candidates.map { candidate in
                let score = target.matchScore(to: candidate.selection)
                return score <= 0.06 ? score : Double.infinity
            }
        }
        let assignment = try unambiguousCandidateAssignment(
            costs,
            ambiguityGap: 0.002,
            command: operationKind.rawValue
        )
        return assignment.map { candidates[$0].index }.sorted()
    }

    private func unambiguousCandidateAssignment(
        _ costs: [[Double]],
        ambiguityGap: Double,
        command: String
    ) throws -> [Int] {
        guard let best = minimumCostCandidateAssignment(costs) else {
            throw ProcessingServiceError.invalidSelection(command: command)
        }

        var secondBestCost = Double.infinity
        for row in best.columns.indices {
            if let alternative = minimumCostCandidateAssignment(
                costs,
                excluding: (row: row, column: best.columns[row])
            ) {
                secondBestCost = min(secondBestCost, alternative.cost)
            }
        }
        guard secondBestCost - best.cost >= ambiguityGap else {
            throw ProcessingServiceError.invalidSelection(command: command)
        }
        return best.columns
    }

    private func minimumCostCandidateAssignment(
        _ costs: [[Double]],
        excluding excludedEdge: (row: Int, column: Int)? = nil
    ) -> (columns: [Int], cost: Double)? {
        guard costs.isEmpty == false,
              let columnCount = costs.first?.count,
              columnCount >= costs.count,
              columnCount > 0,
              costs.allSatisfy({ $0.count == columnCount })
        else {
            return nil
        }

        let rowCount = costs.count
        let impossibleCost = 1_000_000.0
        let normalizedCosts = costs.enumerated().map { row, values in
            values.enumerated().map { column, value in
                if excludedEdge?.row == row, excludedEdge?.column == column {
                    return impossibleCost
                }
                return value.isFinite && value >= 0 ? value : impossibleCost
            }
        }
        var rowPotential = Array(repeating: 0.0, count: rowCount + 1)
        var columnPotential = Array(repeating: 0.0, count: columnCount + 1)
        var matchedRow = Array(repeating: 0, count: columnCount + 1)
        var previousColumn = Array(repeating: 0, count: columnCount + 1)

        for row in 1...rowCount {
            matchedRow[0] = row
            var minimum = Array(repeating: impossibleCost, count: columnCount + 1)
            var used = Array(repeating: false, count: columnCount + 1)
            var column = 0

            repeat {
                used[column] = true
                let currentRow = matchedRow[column]
                var delta = impossibleCost
                var nextColumn = 0
                for candidateColumn in 1...columnCount where used[candidateColumn] == false {
                    let reducedCost = normalizedCosts[currentRow - 1][candidateColumn - 1]
                        - rowPotential[currentRow]
                        - columnPotential[candidateColumn]
                    if reducedCost < minimum[candidateColumn] {
                        minimum[candidateColumn] = reducedCost
                        previousColumn[candidateColumn] = column
                    }
                    if minimum[candidateColumn] < delta {
                        delta = minimum[candidateColumn]
                        nextColumn = candidateColumn
                    }
                }
                guard nextColumn != 0, delta < impossibleCost * 0.5 else {
                    return nil
                }
                for candidateColumn in 0...columnCount {
                    if used[candidateColumn] {
                        rowPotential[matchedRow[candidateColumn]] += delta
                        columnPotential[candidateColumn] -= delta
                    } else {
                        minimum[candidateColumn] -= delta
                    }
                }
                column = nextColumn
            } while matchedRow[column] != 0

            repeat {
                let predecessor = previousColumn[column]
                matchedRow[column] = matchedRow[predecessor]
                column = predecessor
            } while column != 0
        }

        var columns = Array(repeating: -1, count: rowCount)
        for column in 1...columnCount where matchedRow[column] > 0 {
            columns[matchedRow[column] - 1] = column - 1
        }
        guard columns.allSatisfy({ $0 >= 0 }) else {
            return nil
        }
        let selectedCosts = columns.enumerated().map { row, column in
            normalizedCosts[row][column]
        }
        guard selectedCosts.allSatisfy({ $0 < impossibleCost * 0.5 }) else {
            return nil
        }
        return (columns, selectedCosts.reduce(0, +))
    }

    private func parseStarCount(_ result: ProcessingCommandResult, expectedCommand: String) throws -> Int {
        guard let object = try parseJSONObject(from: result.standardOutput),
              object["type"] as? String == "complete",
              object["command"] as? String == expectedCommand
        else {
            throw ProcessingServiceError.invalidReport(command: expectedCommand)
        }
        if object["count"] != nil, let count = jsonInteger(object["count"]) {
            guard count >= 0 else {
                throw ProcessingServiceError.invalidReport(command: expectedCommand)
            }
            return count
        }
        if object["stars"] != nil, let count = jsonInteger(object["stars"]) {
            guard count >= 0 else {
                throw ProcessingServiceError.invalidReport(command: expectedCommand)
            }
            return count
        }
        if let stars = object["stars"] as? [[String: Any]] {
            return stars.count
        }
        throw ProcessingServiceError.invalidReport(command: expectedCommand)
    }

    private func parseStarMaskMetrics(_ result: ProcessingCommandResult) throws -> [String: String] {
        guard let object = try parseJSONObject(from: result.standardOutput),
              object["type"] as? String == "complete",
              object["command"] as? String == "stars mask",
              let stars = jsonInteger(object["stars"]),
              let largeStars = jsonInteger(object["largeStars"]),
              stars >= 0,
              largeStars >= 0,
              largeStars <= stars
        else {
            throw ProcessingServiceError.invalidReport(command: "stars mask")
        }
        return [
            "stars": metricString(stars),
            "largeStars": metricString(largeStars),
        ]
    }

    private func validatedStoredStarMaskMetrics(_ metrics: [String: String]) -> [String: String]? {
        guard let starsText = metrics["stars"]?.trimmingCharacters(in: .whitespacesAndNewlines),
              let largeStarsText = metrics["largeStars"]?.trimmingCharacters(in: .whitespacesAndNewlines),
              let stars = Int(starsText),
              let largeStars = Int(largeStarsText),
              stars >= 0,
              largeStars >= 0,
              largeStars <= stars
        else {
            return nil
        }
        return [
            "stars": metricString(stars),
            "largeStars": metricString(largeStars),
        ]
    }

    private func updateArtifactMarkedPreview(
        input: URL,
        report: ArtifactTrailReport,
        selectedIndices: Set<Int>
    ) {
        updateMarkedPreview {
            try createArtifactMarkedPreview(
                input: input,
                report: report,
                selectedIndices: selectedIndices
            )
        }
    }

    private func updateCloudMarkedPreview(
        input: URL,
        report: CloudRegionReport,
        selectedIndices: Set<Int>
    ) {
        updateMarkedPreview {
            try createCloudMarkedPreview(
                input: input,
                report: report,
                selectedIndices: selectedIndices
            )
        }
    }

    private func updateMarkedPreview(_ render: () throws -> URL?) {
        let renderFailureMessage = localized(.markedPreviewRenderFailed)
        do {
            replaceMarkedPreview(try render())
            if errorMessage == renderFailureMessage {
                errorMessage = nil
            }
        } catch {
            replaceMarkedPreview(nil)
            errorMessage = renderFailureMessage
        }
    }

    private func replaceMarkedPreview(_ url: URL?) {
        let previous = artifactMarkedPreviewURL
        artifactMarkedPreviewURL = url
        guard previous?.standardizedFileURL != url?.standardizedFileURL,
              let previous,
              previous.deletingLastPathComponent().standardizedFileURL == previewDirectory.standardizedFileURL,
              previous.lastPathComponent.hasPrefix("artifact-marked-") ||
                previous.lastPathComponent.hasPrefix("cloud-marked-")
        else {
            return
        }
        try? FileManager.default.removeItem(at: previous)
    }

    func createArtifactMarkedPreview(
        input: URL,
        report: ArtifactTrailReport,
        selectedIndices: Set<Int>
    ) throws -> URL? {
        let markerItems = report.topItems
        guard markerItems.isEmpty == false else {
            return nil
        }
        guard let source = CGImageSourceCreateWithURL(input as CFURL, nil),
              let image = markedPreviewImage(from: source),
              reportDimensionsMatch(width: image.width, height: image.height, reportWidth: report.imageWidth, reportHeight: report.imageHeight)
        else {
            throw MarkedPreviewRenderingError.cannotDecodeInput
        }

        try FileManager.default.createDirectory(at: previewDirectory, withIntermediateDirectories: true)
        let output = previewDirectory.appendingPathComponent(
            "artifact-marked-\(artifactMarkerRendererRevision)-\(UUID().uuidString).jpg"
        )
        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(
                  data: nil,
                  width: image.width,
                  height: image.height,
                  bitsPerComponent: 8,
                  bytesPerRow: 0,
                  space: colorSpace,
                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
              )
        else {
            throw MarkedPreviewRenderingError.cannotCreateContext
        }

        let width = CGFloat(image.width)
        let height = CGFloat(image.height)
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        let labelLayout = artifactMarkerLabelLayout(
            for: markerItems,
            selectedIndices: selectedIndices,
            imageSize: CGSize(width: width, height: height)
        )

        for item in markerItems {
            drawArtifactMarker(
                item,
                selected: selectedIndices.contains(item.index),
                imageHeight: height,
                labelPlacement: labelLayout.placements[item.index],
                in: context
            )
        }
        drawMarkerOverflowSummary(labelLayout, in: context)

        guard let markedImage = context.makeImage() else {
            throw MarkedPreviewRenderingError.cannotCreateDestination
        }

        try AtomicLocalImageWriter.write(
            to: output,
            validate: { Self.isUsableCachedOutput($0) }
        ) { temporaryOutput in
            guard let destination = CGImageDestinationCreateWithURL(
                temporaryOutput as CFURL,
                UTType.jpeg.identifier as CFString,
                1,
                nil
            ) else {
                throw MarkedPreviewRenderingError.cannotCreateDestination
            }
            CGImageDestinationAddImage(destination, markedImage, [
                kCGImageDestinationLossyCompressionQuality: 0.95,
            ] as CFDictionary)
            guard CGImageDestinationFinalize(destination) else {
                throw MarkedPreviewRenderingError.cannotFinalizeDestination
            }
        }
        return output
    }

    func createCloudMarkedPreview(
        input: URL,
        report: CloudRegionReport,
        selectedIndices: Set<Int>
    ) throws -> URL? {
        guard report.items.isEmpty == false else {
            return nil
        }
        guard let source = CGImageSourceCreateWithURL(input as CFURL, nil),
              let image = markedPreviewImage(from: source),
              reportDimensionsMatch(width: image.width, height: image.height, reportWidth: report.imageWidth, reportHeight: report.imageHeight)
        else {
            throw MarkedPreviewRenderingError.cannotDecodeInput
        }

        try FileManager.default.createDirectory(at: previewDirectory, withIntermediateDirectories: true)
        let output = previewDirectory.appendingPathComponent(
            "cloud-marked-\(cloudMarkerRendererRevision)-\(UUID().uuidString).jpg"
        )
        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let context = CGContext(
                  data: nil,
                  width: image.width,
                  height: image.height,
                  bitsPerComponent: 8,
                  bytesPerRow: 0,
                  space: colorSpace,
                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
              )
        else {
            throw MarkedPreviewRenderingError.cannotCreateContext
        }

        let width = CGFloat(image.width)
        let height = CGFloat(image.height)
        context.draw(image, in: CGRect(x: 0, y: 0, width: width, height: height))
        let markerItems = report.topItems
        let labelLayout = cloudMarkerLabelLayout(
            for: markerItems,
            selectedIndices: selectedIndices,
            imageSize: CGSize(width: width, height: height)
        )

        for item in markerItems {
            drawCloudMarker(
                item,
                selected: selectedIndices.contains(item.index),
                imageHeight: height,
                labelPlacement: labelLayout.placements[item.index],
                in: context
            )
        }
        drawMarkerOverflowSummary(labelLayout, in: context)

        guard let markedImage = context.makeImage() else {
            throw MarkedPreviewRenderingError.cannotCreateDestination
        }

        try AtomicLocalImageWriter.write(
            to: output,
            validate: { Self.isUsableCachedOutput($0) }
        ) { temporaryOutput in
            guard let destination = CGImageDestinationCreateWithURL(
                temporaryOutput as CFURL,
                UTType.jpeg.identifier as CFString,
                1,
                nil
            ) else {
                throw MarkedPreviewRenderingError.cannotCreateDestination
            }
            CGImageDestinationAddImage(destination, markedImage, [
                kCGImageDestinationLossyCompressionQuality: 0.95,
            ] as CFDictionary)
            guard CGImageDestinationFinalize(destination) else {
                throw MarkedPreviewRenderingError.cannotFinalizeDestination
            }
        }
        return output
    }

    private func markedPreviewImage(from source: CGImageSource) -> CGImage? {
        let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any]
        let orientation = (properties?[kCGImagePropertyOrientation] as? NSNumber)?.intValue ?? 1
        guard (1...8).contains(orientation) else {
            return CGImageSourceCreateImageAtIndex(source, 0, [
                kCGImageSourceShouldCache: true,
                kCGImageSourceShouldCacheImmediately: true,
            ] as CFDictionary)
        }
        guard orientation != 1 else {
            return CGImageSourceCreateImageAtIndex(source, 0, [
                kCGImageSourceShouldCache: true,
                kCGImageSourceShouldCacheImmediately: true,
            ] as CFDictionary)
        }
        guard let width = (properties?[kCGImagePropertyPixelWidth] as? NSNumber)?.int64Value,
              let height = (properties?[kCGImagePropertyPixelHeight] as? NSNumber)?.int64Value,
              width > 0,
              height > 0,
              max(width, height) <= Int64(Int.max)
        else {
            return nil
        }
        return CGImageSourceCreateThumbnailAtIndex(source, 0, [
            kCGImageSourceCreateThumbnailFromImageAlways: true,
            kCGImageSourceCreateThumbnailWithTransform: true,
            kCGImageSourceShouldCacheImmediately: true,
            kCGImageSourceThumbnailMaxPixelSize: Int(max(width, height)),
        ] as CFDictionary)
    }

    private func reportDimensionsMatch(
        width: Int,
        height: Int,
        reportWidth: Int?,
        reportHeight: Int?
    ) -> Bool {
        guard let reportWidth, let reportHeight else {
            return true
        }
        return width == reportWidth && height == reportHeight
    }

    func artifactMarkerLabelPlacements(
        for items: [ArtifactTrailItem],
        selectedIndices: Set<Int>,
        imageSize: CGSize
    ) -> [Int: ArtifactMarkerLabelPlacement] {
        artifactMarkerLabelLayout(
            for: items,
            selectedIndices: selectedIndices,
            imageSize: imageSize
        ).placements
    }

    func artifactMarkerLabelLayout(
        for items: [ArtifactTrailItem],
        selectedIndices: Set<Int>,
        imageSize: CGSize
    ) -> MarkerLabelLayout {
        guard imageSize.width.isFinite,
              imageSize.height.isFinite,
              imageSize.width > 0,
              imageSize.height > 0
        else {
            return MarkerLabelLayout(placements: [:], omittedCount: 0, overflowPlacement: nil)
        }
        let pathObstacles = items.flatMap { item -> [CGRect] in
            let points = artifactMarkerPoints(item, imageHeight: imageSize.height)
            guard points.count >= 2 else {
                return []
            }
            let clearance = max(7, CGFloat(item.width) * 0.5 + 5)
            return zip(points, points.dropFirst()).map { pair in
                CGRect(
                    x: min(pair.0.x, pair.1.x),
                    y: min(pair.0.y, pair.1.y),
                    width: abs(pair.1.x - pair.0.x),
                    height: abs(pair.1.y - pair.0.y)
                ).insetBy(dx: -clearance, dy: -clearance)
            }
        }
        let requests = items.compactMap { item -> MarkerLabelRequest? in
            guard selectedIndices.contains(item.index) else {
                return nil
            }
            let points = artifactMarkerPoints(item, imageHeight: imageSize.height)
            guard points.isEmpty == false else {
                return nil
            }
            return MarkerLabelRequest(
                index: item.index,
                anchor: points[points.count / 2],
                size: artifactLabelSize(item.displayLabel, selected: true)
            )
        }
        return markerLabelLayout(
            requests: requests,
            obstacles: pathObstacles,
            imageSize: imageSize
        )
    }

    func cloudMarkerLabelPlacements(
        for items: [CloudRegionItem],
        selectedIndices: Set<Int>,
        imageSize: CGSize
    ) -> [Int: ArtifactMarkerLabelPlacement] {
        cloudMarkerLabelLayout(
            for: items,
            selectedIndices: selectedIndices,
            imageSize: imageSize
        ).placements
    }

    func cloudMarkerLabelLayout(
        for items: [CloudRegionItem],
        selectedIndices: Set<Int>,
        imageSize: CGSize
    ) -> MarkerLabelLayout {
        guard imageSize.width.isFinite,
              imageSize.height.isFinite,
              imageSize.width > 0,
              imageSize.height > 0
        else {
            return MarkerLabelLayout(placements: [:], omittedCount: 0, overflowPlacement: nil)
        }
        let obstacles = items.map {
            cloudMarkerBounds($0, imageHeight: imageSize.height)
                .insetBy(dx: -4, dy: -4)
        }
        let selectedItems = items.filter { selectedIndices.contains($0.index) }
        let unselectedItems = items.filter { selectedIndices.contains($0.index) == false }
        let requests = (selectedItems + unselectedItems).map { item in
            let selected = selectedIndices.contains(item.index)
            let bounds = cloudMarkerBounds(item, imageHeight: imageSize.height)
            return MarkerLabelRequest(
                index: item.index,
                anchor: CGPoint(x: bounds.midX, y: bounds.minY + 4),
                size: artifactLabelSize(item.displayLabel, selected: selected)
            )
        }
        return markerLabelLayout(
            requests: requests,
            obstacles: obstacles,
            imageSize: imageSize
        )
    }

    private func markerLabelLayout(
        requests: [MarkerLabelRequest],
        obstacles: [CGRect],
        imageSize: CGSize
    ) -> MarkerLabelLayout {
        var placements = markerLabelPlacements(
            requests: requests,
            obstacles: obstacles,
            imageSize: imageSize
        )
        var omittedCount = requests.count - placements.count
        guard omittedCount > 0 else {
            return MarkerLabelLayout(
                placements: placements,
                omittedCount: 0,
                overflowPlacement: nil
            )
        }

        let overflowIndex = Int.min
        while true {
            let overflowRequest = MarkerLabelRequest(
                index: overflowIndex,
                anchor: CGPoint(x: imageSize.width, y: imageSize.height),
                size: artifactLabelSize("+\(omittedCount)", selected: true)
            )
            if var overflowPlacement = markerLabelPlacements(
                requests: [overflowRequest],
                obstacles: obstacles,
                imageSize: imageSize,
                initialOccupiedRects: placements.values.map(\.rect)
            )[overflowIndex] {
                overflowPlacement.drawsLeader = false
                return MarkerLabelLayout(
                    placements: placements,
                    omittedCount: omittedCount,
                    overflowPlacement: overflowPlacement
                )
            }

            // Preserve at least one real label so the summary never replaces the
            // highest-priority candidate on a physically tiny canvas.
            guard placements.count > 1,
                  let lowestPriorityRequest = requests.reversed().first(where: {
                      placements[$0.index] != nil
                  })
            else {
                return MarkerLabelLayout(
                    placements: placements,
                    omittedCount: omittedCount,
                    overflowPlacement: nil
                )
            }
            placements.removeValue(forKey: lowestPriorityRequest.index)
            omittedCount += 1
        }
    }

    private func markerLabelPlacements(
        requests: [MarkerLabelRequest],
        obstacles: [CGRect],
        imageSize: CGSize,
        initialOccupiedRects: [CGRect] = []
    ) -> [Int: ArtifactMarkerLabelPlacement] {
        guard imageSize.width.isFinite,
              imageSize.height.isFinite,
              imageSize.width > 0,
              imageSize.height > 0
        else {
            return [:]
        }

        let imageBounds = CGRect(origin: .zero, size: imageSize)
        let margin = min(6, min(imageSize.width, imageSize.height) * 0.05)
        let insetBounds = imageBounds.insetBy(dx: margin, dy: margin)

        var occupiedRects = initialOccupiedRects
        var placements: [Int: ArtifactMarkerLabelPlacement] = [:]
        for request in requests {
            let anchor = request.anchor
            let labelSize = request.size
            let safeBounds = insetBounds.width >= labelSize.width && insetBounds.height >= labelSize.height
                ? insetBounds
                : imageBounds
            let candidates = artifactLabelCandidateRects(
                anchor: anchor,
                labelSize: labelSize,
                bounds: safeBounds,
                occupiedRects: occupiedRects
            )

            var bestRect: CGRect?
            var bestScore = CGFloat.greatestFiniteMagnitude
            for (candidateIndex, rect) in candidates.enumerated() {
                let labelOverlap = occupiedRects.reduce(CGFloat.zero) {
                    $0 + rectangleIntersectionArea($1, rect)
                }
                guard labelOverlap == 0 else {
                    continue
                }
                let obstacleOverlap = obstacles.reduce(CGFloat.zero) {
                    $0 + rectangleIntersectionArea($1, rect)
                }
                let anchorPenalty: CGFloat = rect.insetBy(dx: -2, dy: -2).contains(anchor) ? 10_000 : 0
                if obstacleOverlap == 0, anchorPenalty == 0 {
                    bestRect = rect
                    break
                }
                let distance = hypot(rect.midX - anchor.x, rect.midY - anchor.y)
                let score = obstacleOverlap * 25 + anchorPenalty +
                    distance * 0.05 + CGFloat(candidateIndex) * 0.0001
                if score < bestScore {
                    bestScore = score
                    bestRect = rect
                }
            }

            guard let rect = bestRect else {
                continue
            }
            let leaderDestination = nearestPoint(on: rect, to: anchor)
            let drawsLeader = hypot(
                leaderDestination.x - anchor.x,
                leaderDestination.y - anchor.y
            ) > 18
            placements[request.index] = ArtifactMarkerLabelPlacement(
                rect: rect,
                anchor: anchor,
                drawsLeader: drawsLeader
            )
            occupiedRects.append(rect)
        }
        return placements
    }

    private func artifactMarkerPoints(
        _ item: ArtifactTrailItem,
        imageHeight: CGFloat
    ) -> [CGPoint] {
        if item.path.count >= 2 {
            return item.path.map {
                CGPoint(x: CGFloat($0.x), y: imageHeight - CGFloat($0.y))
            }
        }
        return [
            CGPoint(x: CGFloat(item.x1), y: imageHeight - CGFloat(item.y1)),
            CGPoint(x: CGFloat(item.x2), y: imageHeight - CGFloat(item.y2)),
        ]
    }

    private func artifactLabelCandidateRects(
        anchor: CGPoint,
        labelSize: CGSize,
        bounds: CGRect,
        occupiedRects: [CGRect]
    ) -> [CGRect] {
        guard bounds.width >= labelSize.width, bounds.height >= labelSize.height else {
            return []
        }

        var candidates: [CGRect] = []
        var seenOrigins = Set<String>()
        func appendCandidate(_ origin: CGPoint) {
            let maximumX = bounds.maxX - labelSize.width
            let maximumY = bounds.maxY - labelSize.height
            let clampedOrigin = CGPoint(
                x: min(max(origin.x, bounds.minX), maximumX),
                y: min(max(origin.y, bounds.minY), maximumY)
            )
            let key = "\(Int((clampedOrigin.x * 10).rounded())):\(Int((clampedOrigin.y * 10).rounded()))"
            guard seenOrigins.insert(key).inserted else {
                return
            }
            candidates.append(CGRect(origin: clampedOrigin, size: labelSize))
        }

        for gap in [CGFloat(8), 18, 34, 58, 86, 120] {
            let origins = [
                CGPoint(x: anchor.x + gap, y: anchor.y - labelSize.height - gap),
                CGPoint(x: anchor.x + gap, y: anchor.y + gap),
                CGPoint(x: anchor.x - labelSize.width - gap, y: anchor.y - labelSize.height - gap),
                CGPoint(x: anchor.x - labelSize.width - gap, y: anchor.y + gap),
                CGPoint(x: anchor.x - labelSize.width * 0.5, y: anchor.y + gap),
                CGPoint(x: anchor.x - labelSize.width * 0.5, y: anchor.y - labelSize.height - gap),
                CGPoint(x: anchor.x + gap, y: anchor.y - labelSize.height * 0.5),
                CGPoint(x: anchor.x - labelSize.width - gap, y: anchor.y - labelSize.height * 0.5),
            ]
            for origin in origins {
                appendCandidate(origin)
            }
        }

        let spacing: CGFloat = 4
        for occupied in occupiedRects {
            let alignedY = [
                occupied.minY,
                occupied.midY - labelSize.height * 0.5,
                occupied.maxY - labelSize.height,
            ]
            for y in alignedY {
                appendCandidate(CGPoint(x: occupied.maxX + spacing, y: y))
                appendCandidate(CGPoint(x: occupied.minX - labelSize.width - spacing, y: y))
            }
            let alignedX = [
                occupied.minX,
                occupied.midX - labelSize.width * 0.5,
                occupied.maxX - labelSize.width,
            ]
            for x in alignedX {
                appendCandidate(CGPoint(x: x, y: occupied.maxY + spacing))
                appendCandidate(CGPoint(x: x, y: occupied.minY - labelSize.height - spacing))
            }
        }

        let columnCount = max(
            1,
            min(20, Int(floor((bounds.width + spacing) / (labelSize.width + spacing))))
        )
        let rowCount = max(
            1,
            min(20, Int(floor((bounds.height + spacing) / (labelSize.height + spacing))))
        )
        let horizontalSpan = max(0, bounds.width - labelSize.width)
        let verticalSpan = max(0, bounds.height - labelSize.height)
        var gridOrigins: [CGPoint] = []
        gridOrigins.reserveCapacity(columnCount * rowCount)
        for row in 0..<rowCount {
            let y = rowCount == 1
                ? bounds.midY - labelSize.height * 0.5
                : bounds.minY + verticalSpan * CGFloat(row) / CGFloat(rowCount - 1)
            for column in 0..<columnCount {
                let x = columnCount == 1
                    ? bounds.midX - labelSize.width * 0.5
                    : bounds.minX + horizontalSpan * CGFloat(column) / CGFloat(columnCount - 1)
                gridOrigins.append(CGPoint(x: x, y: y))
            }
        }
        gridOrigins.sort { left, right in
            let leftDistance = hypot(
                left.x + labelSize.width * 0.5 - anchor.x,
                left.y + labelSize.height * 0.5 - anchor.y
            )
            let rightDistance = hypot(
                right.x + labelSize.width * 0.5 - anchor.x,
                right.y + labelSize.height * 0.5 - anchor.y
            )
            if leftDistance != rightDistance {
                return leftDistance < rightDistance
            }
            if left.y != right.y {
                return left.y < right.y
            }
            return left.x < right.x
        }
        for origin in gridOrigins.prefix(80) {
            appendCandidate(origin)
        }
        return candidates
    }

    private func rectangleIntersectionArea(_ lhs: CGRect, _ rhs: CGRect) -> CGFloat {
        let intersection = lhs.intersection(rhs)
        guard intersection.isNull == false, intersection.isEmpty == false else {
            return 0
        }
        return intersection.width * intersection.height
    }

    private func nearestPoint(on rect: CGRect, to point: CGPoint) -> CGPoint {
        CGPoint(
            x: min(max(point.x, rect.minX), rect.maxX),
            y: min(max(point.y, rect.minY), rect.maxY)
        )
    }

    private func cloudMarkerBounds(_ item: CloudRegionItem, imageHeight: CGFloat) -> CGRect {
        CGRect(
            x: CGFloat(item.x),
            y: imageHeight - CGFloat(item.y) - CGFloat(item.height),
            width: CGFloat(item.width),
            height: CGFloat(item.height)
        )
    }

    private func drawCloudMarker(
        _ item: CloudRegionItem,
        selected: Bool,
        imageHeight: CGFloat,
        labelPlacement: ArtifactMarkerLabelPlacement?,
        in context: CGContext
    ) {
        let color = CGColor(red: 0.25, green: 0.78, blue: 1.0, alpha: 1.0)
        let bounds = cloudMarkerBounds(item, imageHeight: imageHeight)
        context.saveGState()
        context.setFillColor(color.copy(alpha: selected ? 0.18 : 0.06) ?? color)
        for mask in item.maskRects {
            context.fill(CGRect(
                x: CGFloat(mask.x),
                y: imageHeight - CGFloat(mask.y) - CGFloat(mask.height),
                width: CGFloat(mask.width),
                height: CGFloat(mask.height)
            ))
        }
        context.setStrokeColor(color.copy(alpha: selected ? 0.96 : 0.28) ?? color)
        context.setLineWidth(selected ? 4.0 : 2.0)
        context.stroke(bounds)
        if let labelPlacement {
            if labelPlacement.drawsLeader {
                let destination = nearestPoint(on: labelPlacement.rect, to: labelPlacement.anchor)
                context.setStrokeColor(color.copy(alpha: selected ? 0.82 : 0.42) ?? color)
                context.setLineWidth(selected ? 1.5 : 1.0)
                context.move(to: labelPlacement.anchor)
                context.addLine(to: destination)
                context.strokePath()
            }
            drawArtifactLabel(
                item.displayLabel,
                color: color,
                selected: selected,
                rect: labelPlacement.rect,
                in: context
            )
        }
        context.restoreGState()
    }

    private func drawArtifactMarker(
        _ item: ArtifactTrailItem,
        selected: Bool,
        imageHeight: CGFloat,
        labelPlacement: ArtifactMarkerLabelPlacement?,
        in context: CGContext
    ) {
        let color = artifactMarkerColor(item.kind)
        let markerPoints = artifactMarkerPoints(item, imageHeight: imageHeight)
        guard let firstPoint = markerPoints.first, let lastPoint = markerPoints.last else {
            return
        }
        let lineWidth = max(2.0, CGFloat(item.width))
        let alpha: CGFloat = selected ? 0.96 : 0.18
        let haloAlpha: CGFloat = selected ? 0.34 : 0.08

        context.saveGState()
        context.setLineCap(.round)
        context.setLineJoin(.round)

        context.setStrokeColor(color.copy(alpha: haloAlpha) ?? color)
        context.setLineWidth(lineWidth + (selected ? 12 : 5))
        context.addLines(between: markerPoints)
        context.strokePath()

        context.setStrokeColor(color.copy(alpha: alpha) ?? color)
        context.setLineWidth(selected ? 3.5 : 1.5)
        context.addLines(between: markerPoints)
        context.strokePath()

        context.setFillColor(color.copy(alpha: selected ? 0.90 : 0.25) ?? color)
        context.fillEllipse(in: CGRect(x: firstPoint.x - 4, y: firstPoint.y - 4, width: 8, height: 8))
        context.setStrokeColor(color.copy(alpha: alpha) ?? color)
        context.setLineWidth(selected ? 2.0 : 1.0)
        context.strokeEllipse(in: CGRect(x: lastPoint.x - 5, y: lastPoint.y - 5, width: 10, height: 10))
        if selected, let labelPlacement {
            if labelPlacement.drawsLeader {
                let destination = nearestPoint(on: labelPlacement.rect, to: labelPlacement.anchor)
                context.setStrokeColor(color.copy(alpha: 0.82) ?? color)
                context.setLineWidth(1.5)
                context.move(to: labelPlacement.anchor)
                context.addLine(to: destination)
                context.strokePath()
            }
            drawArtifactLabel(
                item.displayLabel,
                color: color,
                selected: true,
                rect: labelPlacement.rect,
                in: context
            )
        }
        context.restoreGState()
    }

    private func drawArtifactLabel(
        _ text: String,
        color: CGColor,
        selected: Bool,
        x: CGFloat,
        y: CGFloat,
        in context: CGContext
    ) {
        let labelSize = artifactLabelSize(text, selected: selected)
        let rect = CGRect(
            x: x + 8,
            y: y - labelSize.height - 8,
            width: labelSize.width,
            height: labelSize.height
        )
        drawArtifactLabel(text, color: color, selected: selected, rect: rect, in: context)
    }

    private func drawArtifactLabel(
        _ text: String,
        color: CGColor,
        selected: Bool,
        rect: CGRect,
        in context: CGContext
    ) {
        guard let labelImage = makeArtifactLabelImage(text, color: color, selected: selected) else {
            return
        }

        context.saveGState()
        context.setBlendMode(.normal)
        context.draw(labelImage, in: rect)
        context.restoreGState()
    }

    private func drawMarkerOverflowSummary(_ layout: MarkerLabelLayout, in context: CGContext) {
        guard let text = layout.overflowText,
              let placement = layout.overflowPlacement
        else {
            return
        }
        drawArtifactLabel(
            text,
            color: CGColor(gray: 0.92, alpha: 1.0),
            selected: true,
            rect: placement.rect,
            in: context
        )
    }

    func makeArtifactLabelImage(_ text: String, color: CGColor, selected: Bool) -> CGImage? {
        let labelSize = artifactLabelSize(text, selected: selected)
        let labelWidth = Int(labelSize.width)
        let labelHeight = Int(labelSize.height)
        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB),
              let labelContext = CGContext(
                  data: nil,
                  width: labelWidth,
                  height: labelHeight,
                  bitsPerComponent: 8,
                  bytesPerRow: 0,
                  space: colorSpace,
                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
              )
        else {
            return nil
        }
        drawArtifactLabelContents(
            text,
            color: color,
            selected: selected,
            bounds: CGRect(origin: .zero, size: labelSize),
            in: labelContext
        )
        return labelContext.makeImage()
    }

    private func artifactLabelSize(_ text: String, selected: Bool) -> CGSize {
        let fontSize: CGFloat = selected ? 22 : 16
        let labelWidth = Int(ceil(max(42, CGFloat(text.count) * fontSize * 0.62 + 14)))
        let labelHeight = Int(ceil(fontSize + 10))
        return CGSize(width: labelWidth, height: labelHeight)
    }

    private func drawArtifactLabelContents(
        _ text: String,
        color: CGColor,
        selected: Bool,
        bounds: CGRect,
        in context: CGContext
    ) {
        let fontSize: CGFloat = selected ? 22 : 16
        let backgroundAlpha: CGFloat = selected ? 0.78 : 0.36
        let textAlpha: CGFloat = selected ? 1.0 : 0.55
        let lineWidth: CGFloat = selected ? 2.0 : 1.0
        let pathBounds = bounds.insetBy(dx: lineWidth * 0.5, dy: lineWidth * 0.5)
        let backgroundPath = CGPath(roundedRect: pathBounds, cornerWidth: 5, cornerHeight: 5, transform: nil)
        context.setFillColor(CGColor(gray: 0.0, alpha: backgroundAlpha))
        context.addPath(backgroundPath)
        context.fillPath()
        context.setStrokeColor(color.copy(alpha: selected ? 0.96 : 0.35) ?? color)
        context.setLineWidth(lineWidth)
        context.addPath(backgroundPath)
        context.strokePath()

        let textColor = selected ? CGColor(gray: 1.0, alpha: textAlpha) : (color.copy(alpha: textAlpha) ?? color)
        let font = CTFontCreateWithName("HelveticaNeue-Bold" as CFString, fontSize, nil)
        let characters = Array(text.utf16)
        var glyphs = Array(repeating: CGGlyph(), count: characters.count)
        var advances = Array(repeating: CGSize.zero, count: characters.count)
        guard CTFontGetGlyphsForCharacters(font, characters, &glyphs, characters.count) else {
            return
        }
        CTFontGetAdvancesForGlyphs(font, .horizontal, glyphs, &advances, glyphs.count)
        let textY = max(3, (bounds.height - fontSize) * 0.5 + 2)
        var cursorX = bounds.minX + 7
        context.setFillColor(textColor)
        for index in glyphs.indices {
            if let glyphPath = CTFontCreatePathForGlyph(font, glyphs[index], nil) {
                var transform = CGAffineTransform(
                    translationX: cursorX,
                    y: bounds.minY + textY
                )
                if let positionedPath = glyphPath.copy(using: &transform) {
                    context.addPath(positionedPath)
                }
            }
            cursorX += advances[index].width
        }
        context.fillPath()
    }

    private func artifactMarkerColor(_ kind: String) -> CGColor {
        switch kind {
        case "airplane":
            return CGColor(red: 0.20, green: 0.62, blue: 1.0, alpha: 1.0)
        case "drone":
            return CGColor(red: 1.0, green: 0.62, blue: 0.05, alpha: 1.0)
        case "satellite":
            return CGColor(red: 0.15, green: 0.82, blue: 0.92, alpha: 1.0)
        case "meteor":
            return CGColor(red: 0.20, green: 0.90, blue: 0.35, alpha: 1.0)
        default:
            return CGColor(red: 1.0, green: 1.0, blue: 1.0, alpha: 1.0)
        }
    }

    private func parseJSONObject(from output: String) throws -> [String: Any]? {
        var objects: [[String: Any]] = []
        var searchIndex = output.startIndex

        while searchIndex < output.endIndex,
              let startIndex = output[searchIndex...].firstIndex(of: "{") {
            var depth = 0
            var inString = false
            var isEscaping = false
            var endIndex: String.Index?

            for index in output.indices[startIndex...] {
                let character = output[index]
                if inString {
                    if isEscaping {
                        isEscaping = false
                    } else if character == "\\" {
                        isEscaping = true
                    } else if character == "\"" {
                        inString = false
                    }
                    continue
                }

                if character == "\"" {
                    inString = true
                } else if character == "{" {
                    depth += 1
                } else if character == "}" {
                    depth -= 1
                    if depth == 0 {
                        endIndex = index
                        break
                    }
                }
            }

            if let endIndex, depth == 0, inString == false {
                let jsonText = String(output[startIndex...endIndex])
                if let data = jsonText.data(using: .utf8),
                   let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
                    objects.append(object)
                }
                searchIndex = output.index(after: endIndex)
            } else {
                searchIndex = output.index(after: startIndex)
            }
        }

        var selectedObject: [String: Any]?
        var selectedRank = -1
        for object in objects {
            let isComplete = object["type"] as? String == "complete"
            let isLegacyInspect = object["width"] != nil && object["height"] != nil &&
                object["channels"] != nil && object["bitsPerChannel"] != nil && object["format"] != nil
            let rank = isComplete ? 2 : (isLegacyInspect ? 1 : 0)
            if rank >= selectedRank {
                selectedObject = object
                selectedRank = rank
            }
        }
        return selectedObject
    }

    private func registeredSequenceArtifact(
        from object: [String: Any],
        items: [[String: Any]],
        reference: PhotonStackAsset,
        movingFrames: [PhotonStackAsset],
        outputDirectory: URL,
        mode: AlignmentMethod,
        sourceArtifactIDs: [UUID]
    ) -> ProcessingArtifact {
        let frames = items.compactMap { item -> ProcessingArtifactFrame? in
            guard (item["ok"] as? Bool) == true else {
                return nil
            }
            let sourceURL = (item["input"] as? String).map { URL(fileURLWithPath: $0) }
            let outputURL = (item["output"] as? String).map { URL(fileURLWithPath: $0) }
            var metrics: [String: String] = [:]
            for key in ["matches", "detectedReferenceStars", "detectedMovingStars", "inlierRatio", "usedFallback", "message"] {
                if let value = item[key] {
                    metrics[key] = metricString(value)
                }
            }
            return ProcessingArtifactFrame(
                sourceURL: sourceURL,
                outputURL: outputURL,
                isReference: item["reference"] as? Bool ?? false,
                metrics: metrics
            )
        }
        let outputURLs = frames.compactMap(\.outputURL)
        return ProcessingArtifact(
            kind: .registeredSequence,
            name: "Registered \(reference.displayName)",
            operationKind: .register,
            sourceArtifactIDs: sourceArtifactIDs,
            outputDirectory: outputDirectory,
            outputURLs: outputURLs,
            frames: frames,
            parameters: [
                "reference": reference.displayName,
                "referenceID": reference.id.uuidString,
                "alignment": mode.rawValue,
                "movingFrames": movingFrames.map(\.displayName).joined(separator: ", "),
                "movingFrameIDs": movingFrames.map(\.id.uuidString).joined(separator: ","),
                "sourceFrameIDs": ([reference] + movingFrames).map(\.id.uuidString).joined(separator: ","),
            ],
            metrics: [
                "frames": metricString(object["frames"] ?? frames.count),
                "alignedFrames": metricString(object["alignedFrames"] ?? outputURLs.count),
                "failedFrames": metricString(object["failedFrames"] ?? 0),
            ]
        )
    }

    private func cleanedTimelapseSequenceArtifact(
        from object: [String: Any],
        items: [[String: Any]],
        sourceFrames: [PhotonStackAsset],
        outputDirectory: URL,
        sourceArtifactIDs: [UUID]
    ) -> ProcessingArtifact {
        let frames = items.compactMap { item -> ProcessingArtifactFrame? in
            guard (item["ok"] as? Bool) == true else {
                return nil
            }
            let sourceURL = (item["input"] as? String).map { URL(fileURLWithPath: $0) }
            let outputURL = (item["output"] as? String).map { URL(fileURLWithPath: $0) }
            var metrics: [String: String] = [:]
            for key in ["trails", "removedTrails", "protectedMeteors", "recurrentTrails", "selectedIndices", "message"] {
                if let value = item[key] {
                    metrics[key] = metricString(value)
                }
            }
            return ProcessingArtifactFrame(
                sourceURL: sourceURL,
                outputURL: outputURL,
                isReference: false,
                metrics: metrics
            )
        }
        let outputURLs = frames.compactMap(\.outputURL)
        return ProcessingArtifact(
            kind: .cleanedTimelapseSequence,
            name: localized(.cleanTimelapseTrails),
            operationKind: .artifactRemove,
            sourceArtifactIDs: sourceArtifactIDs,
            outputDirectory: outputDirectory,
            outputURLs: outputURLs,
            frames: frames,
            parameters: [
                "sourceFrames": sourceFrames.map(\.displayName).joined(separator: ", "),
                "sourceFrameIDs": sourceFrames.map(\.id.uuidString).joined(separator: ","),
                "outputFormat": object["outputFormat"] as? String ?? "",
            ],
            metrics: [
                "frames": metricString(object["frames"] ?? frames.count),
                "cleanedFrames": metricString(object["cleanedFrames"] ?? 0),
                "failedFrames": metricString(object["failedFrames"] ?? 0),
                "trails": metricString(object["trails"] ?? 0),
                "selectedTrails": metricString(object["selectedTrails"] ?? 0),
                "removedTrails": metricString(object["removedTrails"] ?? 0),
                "recurrentTrails": metricString(object["recurrentTrails"] ?? 0),
            ]
        )
    }

    private func metricString(_ value: Any) -> String {
        if let number = value as? NSNumber {
            if CFGetTypeID(number) == CFBooleanGetTypeID() {
                return number.boolValue ? "true" : "false"
            }
            return number.stringValue
        }
        if let bool = value as? Bool {
            return String(bool)
        }
        return String(describing: value)
    }

    private func parseRegistrationReport(
        _ result: ProcessingCommandResult,
        reference: PhotonStackAsset,
        moving: PhotonStackAsset,
        mode: AlignmentMethod,
        output: URL
    ) throws -> RegistrationReport {
        let metrics = try parseRegistrationMetrics(result, mode: mode, output: output)

        return RegistrationReport(
            referenceName: reference.displayName,
            movingName: moving.displayName,
            mode: metrics.mode,
            dx: metrics.dx,
            dy: metrics.dy,
            scale: metrics.scale,
            rotationDegrees: metrics.rotationRadians * 180.0 / .pi,
            matches: metrics.matches,
            outputURL: output
        )
    }

    private func parseRegistrationMetrics(
        _ result: ProcessingCommandResult,
        mode: AlignmentMethod,
        output: URL
    ) throws -> ParsedRegistrationMetrics {
        guard let object = try parseJSONObject(from: result.standardOutput),
              object["type"] as? String == "complete",
              object["command"] as? String == "register",
              let reportedMode = object["mode"] as? String,
              let parsedMode = AlignmentMethod(rawValue: reportedMode),
              let dx = jsonFiniteDouble(object["dx"]),
              let dy = jsonFiniteDouble(object["dy"]),
              let scale = jsonFiniteDouble(object["scale"]),
              let rotationRadians = jsonFiniteDouble(object["rotationRadians"]),
              let matches = jsonInteger(object["matches"]),
              let detectedReferenceStars = jsonInteger(object["detectedReferenceStars"]),
              let detectedMovingStars = jsonInteger(object["detectedMovingStars"]),
              let inlierRatio = jsonFiniteDouble(object["inlierRatio"]),
              let usedFallback = jsonBoolean(object["usedFallback"]),
              let reportedOutput = object["output"] as? String,
              parsedMode == mode,
              scale > 0,
              matches >= 0,
              detectedReferenceStars >= 0,
              detectedMovingStars >= 0,
              inlierRatio.isFinite,
              (0...1).contains(inlierRatio),
              reportedOutput.isEmpty == false,
              URL(fileURLWithPath: reportedOutput).standardizedFileURL == output.standardizedFileURL
        else {
            throw ProcessingServiceError.invalidReport(command: "register")
        }

        return ParsedRegistrationMetrics(
            mode: parsedMode,
            dx: dx,
            dy: dy,
            scale: scale,
            rotationRadians: rotationRadians,
            matches: matches,
            usedFallback: usedFallback
        )
    }

    private func jsonFiniteDouble(_ value: Any?) -> Double? {
        guard let number = value as? NSNumber,
              CFGetTypeID(number) != CFBooleanGetTypeID()
        else {
            return nil
        }
        let result = number.doubleValue
        return result.isFinite ? result : nil
    }

    private func jsonInteger(_ value: Any?) -> Int? {
        guard let number = jsonFiniteDouble(value),
              number.rounded() == number,
              number >= Double(Int.min),
              number < Double(Int.max)
        else {
            return nil
        }
        return Int(number)
    }

    private func jsonBoolean(_ value: Any?) -> Bool? {
        guard let number = value as? NSNumber,
              CFGetTypeID(number) == CFBooleanGetTypeID()
        else {
            return nil
        }
        return number.boolValue
    }

    private func runProcessingJob(
        title: String,
        kind: ProcessingOperationKind,
        outputURL: URL? = nil,
        operation: @escaping @MainActor () async throws -> ProcessingCommandResult
    ) async {
        await runJob(title: title, kind: kind, outputURL: outputURL) {
            try FileManager.default.createDirectory(at: self.previewDirectory, withIntermediateDirectories: true)
            let result = try await operation()
            if title == "Console", let executable = result.command.first {
                let output = result.standardOutput.isEmpty ? result.command.joined(separator: " ") : result.standardOutput
                return "CLI: \(executable)\n\(output)"
            }
            return result.standardOutput.isEmpty ? result.command.joined(separator: " ") : result.standardOutput
        }
    }

    private func runLocalJob(
        title: String,
        kind: ProcessingOperationKind,
        operation: @escaping @MainActor () async throws -> String
    ) async {
        await runJob(title: title, kind: kind, operation: operation)
    }

    private func runJob(
        title: String,
        kind: ProcessingOperationKind,
        outputURL: URL? = nil,
        operation: @escaping @MainActor () async throws -> String
    ) async {
        guard activeProgressJobID == nil, isBatchRunning == false else {
            return
        }

        let jobID = UUID()
        let startedAt = Date()
        let job = ProcessingJob(id: jobID, title: title, kind: kind, status: .running, outputURL: outputURL)
        jobs.append(job)
        activeProgressJobID = jobID
        isProcessing = true
        canCancel = true
        updateProgress(0, message: title)
        errorMessage = nil

        do {
            let progressScope = ProcessingProgressScope()
            let message = try await ProcessingProgressContext.$jobID.withValue(jobID) {
                try await ProcessingProgressContext.$progressScope.withValue(progressScope) {
                    try await operation()
                }
            }
            try Task.checkCancellation()
            guard activeProgressJobID == jobID else {
                throw CancellationError()
            }
            if let outputURL {
                recordValidatedCacheOutput(outputURL)
            }
            updateProgress(1, message: title)
            let duration = Date().timeIntervalSince(startedAt) * 1000.0
            let cacheHit = isCacheHitMessage(message)
            let displayMessage = appendPerformanceSummary(to: message, durationMilliseconds: duration, cacheHit: cacheHit)
            commandOutput = displayMessage
            finishJob(
                id: jobID,
                status: .succeeded,
                message: displayMessage,
                outputURL: outputURL,
                durationMilliseconds: duration,
                cacheHit: cacheHit
            )
        } catch is CancellationError {
            invalidateCacheReceipt(for: outputURL)
            let message = localized(.cancelledTask)
            if activeProgressJobID == jobID {
                commandOutput = message
            }
            finishJob(id: jobID, status: .cancelled, message: message, outputURL: outputURL, durationMilliseconds: Date().timeIntervalSince(startedAt) * 1000.0)
        } catch {
            invalidateCacheReceipt(for: outputURL)
            let message = error.localizedDescription
            if activeProgressJobID == jobID {
                errorMessage = message
            }
            finishJob(id: jobID, status: .failed, message: message, outputURL: outputURL, durationMilliseconds: Date().timeIntervalSince(startedAt) * 1000.0)
        }

        guard activeProgressJobID == jobID else {
            return
        }
        isProcessing = jobs.contains { $0.status == .running }
        canCancel = isProcessing
        if isProcessing == false {
            progressFraction = nil
            progressMessage = nil
        }
        activeProgressJobID = nil
        activeTask = nil
    }

    private func updateProgress(_ fraction: Double, message: String? = nil) {
        guard fraction.isFinite else {
            return
        }
        let incoming = min(max(fraction, 0), 1)
        if isProcessing,
           let progressFraction,
           incoming < progressFraction
        {
            if let message {
                progressMessage = message
            }
            return
        }
        progressFraction = incoming
        if let message {
            progressMessage = message
        }
    }

    private func handleProcessingProgress(_ event: ProcessingProgressEvent) {
        guard event.progress.isFinite else {
            return
        }
        if isBatchRunning,
           let jobID = event.jobID,
           let itemID = batchProgressJobItems[jobID],
           activeBatchItemIDs.contains(itemID)
        {
            let incomingProgress = min(max(event.progress, 0), 1)
            if incomingProgress < batchItemProgress[itemID, default: 0] {
                return
            }
            batchItemProgress[itemID] = incomingProgress
            updateBatchAggregateProgress(message: processingProgressDetail(for: event))
            return
        }

        guard isProcessing,
              isBatchRunning == false,
              let activeProgressJobID,
              event.jobID == activeProgressJobID
        else {
            return
        }

        let incomingProgress = min(max(event.progress, 0), 1)
        if incomingProgress < progressFraction ?? 0 {
            return
        }

        updateProgress(incomingProgress, message: processingProgressDetail(for: event))
    }

    private func processingProgressDetail(for event: ProcessingProgressEvent) -> String {
        if event.command == "deep-sky" {
            if event.stage == "stack-combine", let detail = DeepSkyProcessingPresentation.combinationDetail(
                step: event.step, total: event.total, overall: event.progress, chinese: language == .simplifiedChinese) {
                return detail
            }
            let labels = ["calibrate": "校准", "measure": "测量原片", "align": "星点对齐", "assess": "分析筛片",
                          "sensor-pattern-estimate": "估计传感器固定纹理", "sensor-pattern-apply": "校正原片并对齐",
                          "stack": "准备合成缓存", "stack-combine": "像素合成与剔除", "crop": "覆盖率裁边", "background": "背景校正",
                          "align-color-channels": "校正通道彩边", "multiscale-denoise": "多尺度降噪",
                          "background-denoise": "背景纹理降噪",
                          "denoise": "结构保护降噪", "develop": "色彩显影", "complete": "完成"]
            let stage = language == .simplifiedChinese ? (labels[event.stage] ?? event.stage) : event.stage
            let percent = Int((min(max(event.progress, 0), 1) * 100).rounded())
            if let step = event.step, let total = event.total, step > 0, total > 0 {
                return "\(stage) \(step)/\(total) · \(percent)%"
            }
            return "\(stage) · \(percent)%"
        }
        if let step = event.step, let total = event.total {
            return "\(event.command) \(event.stage) \(step)/\(total)"
        }
        return "\(event.command) \(event.stage)"
    }

    private func updateBatchAggregateProgress(message: String? = nil) {
        let total = activeBatchItemIDs.count
        guard total > 0 else {
            updateProgress(0, message: localized(.batchQueueSection))
            return
        }

        let progress = activeBatchItemIDs.reduce(0.0) { partial, itemID in
            partial + (batchItemProgress[itemID] ?? 0)
        } / Double(total)
        let completed = activeBatchItemIDs.reduce(0) { partial, itemID in
            partial + ((batchItemProgress[itemID] ?? 0) >= 1 ? 1 : 0)
        }
        let summary = "\(localized(.batchQueueSection)) \(completed)/\(total)"
        updateProgress(progress, message: message.map { "\(summary) · \($0)" } ?? summary)
    }

    private func isCacheHitMessage(_ message: String) -> Bool {
        message.localizedCaseInsensitiveContains("Using cached")
    }

    private func appendPerformanceSummary(to message: String, durationMilliseconds: Double, cacheHit: Bool) -> String {
        let seconds = durationMilliseconds / 1000.0
        let source = cacheHit ? "cache hit" : "executed"
        let summary = String(format: "[%@, %.2fs]", source, seconds)
        return message.isEmpty ? summary : "\(message)\n\(summary)"
    }

    private func finishJob(
        id: UUID,
        status: ProcessingJobStatus,
        message: String,
        outputURL: URL?,
        durationMilliseconds: Double? = nil,
        cacheHit: Bool = false
    ) {
        guard let index = jobs.firstIndex(where: { $0.id == id }) else {
            return
        }

        jobs[index].status = status
        jobs[index].finishedAt = Date()
        jobs[index].message = message
        jobs[index].outputURL = outputURL
        jobs[index].durationMilliseconds = durationMilliseconds
        jobs[index].cacheHit = cacheHit
    }
}

private struct RecordedArtifactTrailSelection: Codable, Equatable {
    static let pathSampleCount = 9

    var kind: String
    var x1: Double
    var y1: Double
    var x2: Double
    var y2: Double
    var length: Double
    var width: Double
    var path: [RecordedArtifactTrailPoint]? = nil

    var isValid: Bool {
        let coordinates = [x1, y1, x2, y2]
        let pathIsValid = path.map {
            $0.count == Self.pathSampleCount && $0.allSatisfy(\.isValid)
        } ?? true
        return kind.isEmpty == false &&
            coordinates.allSatisfy { $0.isFinite && (-0.5...1.5).contains($0) } &&
            length.isFinite && length > 0 && length <= 2 &&
            width.isFinite && width > 0 && width <= 1 &&
            pathIsValid
    }

    func matchScore(to candidate: RecordedArtifactTrailSelection) -> Double {
        let direct = (
            hypot(x1 - candidate.x1, y1 - candidate.y1) +
                hypot(x2 - candidate.x2, y2 - candidate.y2)
        ) * 0.5
        let reversed = (
            hypot(x1 - candidate.x2, y1 - candidate.y2) +
            hypot(x2 - candidate.x1, y2 - candidate.y1)
        ) * 0.5
        return min(direct, reversed) +
            abs(length - candidate.length) * 0.30 +
            abs(width - candidate.width) * 0.05 +
            pathDistance(to: candidate) * 0.35
    }

    private func pathDistance(to candidate: RecordedArtifactTrailSelection) -> Double {
        guard let path,
              let candidatePath = candidate.path,
              path.count == candidatePath.count,
              path.isEmpty == false
        else {
            return 0
        }
        let targetResiduals = pathResiduals(path)
        let directResiduals = pathResiduals(candidatePath)
        let reversedResiduals = pathResiduals(Array(candidatePath.reversed()))
        let direct = zip(targetResiduals, directResiduals).reduce(0.0) { partial, pair in
            partial + hypot(pair.0.x - pair.1.x, pair.0.y - pair.1.y)
        } / Double(path.count)
        let reversed = zip(targetResiduals, reversedResiduals).reduce(0.0) { partial, pair in
            partial + hypot(pair.0.x - pair.1.x, pair.0.y - pair.1.y)
        } / Double(path.count)
        return min(direct, reversed)
    }

    private func pathResiduals(
        _ points: [RecordedArtifactTrailPoint]
    ) -> [RecordedArtifactTrailPoint] {
        guard let first = points.first, let last = points.last, points.count >= 2 else {
            return points
        }
        return points.enumerated().map { index, point in
            let fraction = Double(index) / Double(points.count - 1)
            let baselineX = first.x + (last.x - first.x) * fraction
            let baselineY = first.y + (last.y - first.y) * fraction
            return RecordedArtifactTrailPoint(
                x: point.x - baselineX,
                y: point.y - baselineY
            )
        }
    }
}

private struct RecordedArtifactTrailPoint: Codable, Equatable {
    var x: Double
    var y: Double

    var isValid: Bool {
        x.isFinite && (-0.5...1.5).contains(x) &&
            y.isFinite && (-0.5...1.5).contains(y)
    }
}

private struct RecordedCloudRegionSelection: Codable, Equatable {
    var x: Double
    var y: Double
    var width: Double
    var height: Double
    var coverage: Double

    var isValid: Bool {
        x.isFinite && (-0.25...1.25).contains(x) &&
            y.isFinite && (-0.25...1.25).contains(y) &&
            width.isFinite && (0...1.5).contains(width) &&
            height.isFinite && (0...1.5).contains(height) &&
            coverage.isFinite && (0...1).contains(coverage)
    }

    func matchScore(to candidate: RecordedCloudRegionSelection) -> Double {
        let centerX = x + width * 0.5
        let centerY = y + height * 0.5
        let candidateCenterX = candidate.x + candidate.width * 0.5
        let candidateCenterY = candidate.y + candidate.height * 0.5
        let centerDistance = hypot(centerX - candidateCenterX, centerY - candidateCenterY)
        let sizeDistance = abs(width - candidate.width) + abs(height - candidate.height)
        return centerDistance + sizeDistance * 0.25 + abs(coverage - candidate.coverage) * 0.05
    }
}

enum AtomicLocalImageWriter {
    static func write(
        to destinationURL: URL,
        validate: (URL) throws -> Bool,
        body: (URL) throws -> Void
    ) throws {
        let fileManager = FileManager.default
        let directory = destinationURL.deletingLastPathComponent()
        try fileManager.createDirectory(at: directory, withIntermediateDirectories: true)

        let extensionSuffix = destinationURL.pathExtension.isEmpty
            ? ""
            : ".\(destinationURL.pathExtension)"
        let baseName = destinationURL.deletingPathExtension().lastPathComponent
        let temporaryURL = directory.appendingPathComponent(
            ".\(baseName).photonstack-render-\(UUID().uuidString)\(extensionSuffix)"
        )
        defer {
            try? fileManager.removeItem(at: temporaryURL)
        }

        try body(temporaryURL)
        guard try validate(temporaryURL) else {
            throw AtomicLocalImageWriterError.invalidOutput
        }

        if fileManager.fileExists(atPath: destinationURL.path) {
            _ = try fileManager.replaceItemAt(
                destinationURL,
                withItemAt: temporaryURL,
                backupItemName: nil,
                options: .usingNewMetadataOnly
            )
        } else {
            try fileManager.moveItem(at: temporaryURL, to: destinationURL)
        }
    }
}

enum AtomicLocalImageWriterError: LocalizedError {
    case invalidOutput

    var errorDescription: String? {
        "Rendered image output is incomplete or unreadable"
    }
}

private enum WorkflowError: LocalizedError {
    case noLights

    var errorDescription: String? {
        switch self {
        case .noLights:
            return AppLanguage.systemDefault.text(.workflowNoLights)
        }
    }
}

private enum MosaicWorkflowError: LocalizedError {
    case noPanels

    var errorDescription: String? {
        AppLanguage.systemDefault.text(.mosaicNoPanels)
    }
}

private struct EditGraphReplayError: LocalizedError {
    let message: String

    var errorDescription: String? {
        message
    }
}

private enum BatchQueueError: LocalizedError {
    case unsupportedOperation

    var errorDescription: String? {
        "Unsupported batch queue operation"
    }
}

private enum LayerStackPreviewError: LocalizedError {
    case noVisibleLayers
    case cannotReadLayer(String)
    case cannotReadMask(String)
    case cannotRender

    var errorDescription: String? {
        switch self {
        case .noVisibleLayers:
            return "No visible layers with preview images"
        case let .cannotReadLayer(name):
            return "Could not read layer image: \(name)"
        case let .cannotReadMask(name):
            return "Could not read mask for layer: \(name)"
        case .cannotRender:
            return "Could not render layer stack preview"
        }
    }
}

private enum MarkedPreviewRenderingError: Error {
    case cannotDecodeInput
    case cannotCreateContext
    case cannotCreateDestination
    case cannotFinalizeDestination
}

private enum MaskEditingError: LocalizedError {
    case cannotReadMask
    case cannotRenderMask
    case cannotEncodeStrokes
    case cannotDecodeStrokes

    var errorDescription: String? {
        switch self {
        case .cannotReadMask:
            return "Could not read the layer mask"
        case .cannotRenderMask:
            return "Could not render the edited layer mask"
        case .cannotEncodeStrokes:
            return "Could not save layer mask brush strokes"
        case .cannotDecodeStrokes:
            return "Could not read saved layer mask brush strokes"
        }
    }
}

private enum ConsoleCommandError: Error {
    case empty
    case unclosedQuote
}

private struct PreviewHistoryState {
    var previewURL: URL?
    var previewOperationID: EditOperation.ID?
    var editGraphNeedsReplay: Bool
    var canvasMode: WorkspaceCanvasMode
    var activeLayerID: ProcessingLayer.ID?
    var layers: [ProcessingLayer]
    var editGraph: EditGraph
}

private struct LayerAdjustmentSession {
    var layerID: ProcessingLayer.ID
    var historyState: PreviewHistoryState
}

private struct EditGraphHistoryState: Equatable {
    var editGraph: EditGraph
    var previewOperationID: EditOperation.ID?
    var layers: [ProcessingLayer]
    var activeLayerID: ProcessingLayer.ID?
}

private struct ReplayRegisteredSequence {
    let artifactID: UUID?
    let outputs: [URL]
}

private struct ExportDependencyTraversal {
    var assetIDs: Set<PhotonStackAsset.ID> = []
    var layerIDs: Set<ProcessingLayer.ID> = []
    var artifactIDs: Set<ProcessingArtifact.ID> = []
    var operationIDs: Set<EditOperation.ID> = []
}

private struct EditOperationBranch {
    var operationIDs: [EditOperation.ID]
    var rootInputPath: String?
}

private struct EditGraphReplayPlan {
    var input: URL
    var operations: [EditOperation]
    var anchorOperationID: EditOperation.ID?
}

private final class FullResolutionLayerReplayContext {
    var assetOutputs: [PhotonStackAsset.ID: URL] = [:]
    var layerOutputs: [ProcessingLayer.ID: URL] = [:]
    var operationOutputs: [EditOperation.ID: URL] = [:]
    var maskOutputs: [String: URL] = [:]
    var compositingOutputs: [String: URL] = [:]
    var resolvingLayerIDs: Set<ProcessingLayer.ID> = []
    var resolvingOperationIDs: Set<EditOperation.ID> = []
}

private enum EditGraphReplayMode {
    case preview
    case fullResolutionExport

    var cacheKey: String {
        switch self {
        case .preview:
            return "preview"
        case .fullResolutionExport:
            return "fullResolutionExport"
        }
    }

    var outputExtension: String {
        switch self {
        case .preview:
            return "png"
        case .fullResolutionExport:
            return "tiff"
        }
    }

    var scientificOutputExtension: String {
        switch self {
        case .preview:
            return "png"
        case .fullResolutionExport:
            return "fits"
        }
    }

    func outputExtension(preservingScientificValuesFrom input: URL) -> String {
        guard self == .fullResolutionExport,
              AssetKind.detect(from: input) == .fits
        else {
            return outputExtension
        }
        return scientificOutputExtension
    }
}

public enum EditOperationMoveDirection: Sendable {
    case up
    case down
}
