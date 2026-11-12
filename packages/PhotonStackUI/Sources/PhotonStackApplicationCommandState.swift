import PhotonStackAppCore
import SwiftUI

public struct PhotonStackApplicationCommandSnapshot: Equatable, Sendable {
    public var language: AppLanguage
    public var analysisWorkerCount: Int
    public var stackWorkerCount: Int
    public var computeBackend: DeepSkyComputeBackend
    public var performance: DeepSkyRunPerformance?
    public var gpuAccelerationEnabled: Bool { computeBackend != .cpu }
    public var isWorkspaceActive: Bool
    public var canRunBatchRegistration: Bool
    public var canPreviewLayerStack: Bool

    public init(
        language: AppLanguage,
        analysisWorkerCount: Int = 0,
        stackWorkerCount: Int = 0,
        gpuAccelerationEnabled: Bool? = nil,
        computeBackend: DeepSkyComputeBackend? = nil,
        performance: DeepSkyRunPerformance? = nil,
        isWorkspaceActive: Bool,
        canRunBatchRegistration: Bool,
        canPreviewLayerStack: Bool
    ) {
        self.language = language
        self.analysisWorkerCount = AppSettings.normalizedAnalysisWorkerCount(analysisWorkerCount)
        self.stackWorkerCount = AppSettings.normalizedStackWorkerCount(stackWorkerCount)
        self.computeBackend = computeBackend ?? DeepSkyComputeBackend(gpuAcceleration: gpuAccelerationEnabled)
        self.performance = performance
        self.isWorkspaceActive = isWorkspaceActive
        self.canRunBatchRegistration = canRunBatchRegistration
        self.canPreviewLayerStack = canPreviewLayerStack
    }

    public static func inactive(
        language: AppLanguage,
        analysisWorkerCount: Int = 0,
        stackWorkerCount: Int = 0,
        gpuAccelerationEnabled: Bool? = nil,
        computeBackend: DeepSkyComputeBackend? = nil
    ) -> Self {
        Self(
            language: language,
            analysisWorkerCount: analysisWorkerCount,
            stackWorkerCount: stackWorkerCount,
            gpuAccelerationEnabled: gpuAccelerationEnabled,
            computeBackend: computeBackend,
            isWorkspaceActive: false,
            canRunBatchRegistration: false,
            canPreviewLayerStack: false
        )
    }
}

@MainActor
public final class PhotonStackApplicationCommandState: ObservableObject {
    @Published public private(set) var snapshot: PhotonStackApplicationCommandSnapshot

    public init(language: AppLanguage = .systemDefault) {
        snapshot = .inactive(language: language)
    }

    public var processMenuTitle: String {
        snapshot.language.text(.processMenu)
    }

    public var batchRegistrationTitle: String {
        snapshot.language.text(.registerBatchFrames)
    }

    public var layerStackPreviewTitle: String {
        snapshot.language.text(.previewLayerStack)
    }

    public var productManifestExportTitle: String {
        snapshot.language.text(.exportProductManifest)
    }

    public func update(_ snapshot: PhotonStackApplicationCommandSnapshot) {
        guard self.snapshot != snapshot else {
            return
        }
        self.snapshot = snapshot
    }

    public func requestLanguage(_ language: AppLanguage) {
        guard snapshot.language != language else {
            return
        }
        snapshot.language = language
        NotificationCenter.default.post(
            name: .photonStackSetApplicationLanguage,
            object: language
        )
    }

    public func requestAnalysisWorkerCount(_ count: Int) {
        let count = AppSettings.normalizedAnalysisWorkerCount(count)
        guard snapshot.analysisWorkerCount != count else { return }
        snapshot.analysisWorkerCount = count
        NotificationCenter.default.post(name: .photonStackSetAnalysisWorkerCount, object: count)
    }

    public func requestStackWorkerCount(_ count: Int) {
        let count = AppSettings.normalizedStackWorkerCount(count)
        guard snapshot.stackWorkerCount != count else { return }
        snapshot.stackWorkerCount = count
        NotificationCenter.default.post(name: .photonStackSetStackWorkerCount, object: count)
    }

    public func requestGPUAccelerationEnabled(_ isEnabled: Bool) {
        requestComputeBackend(isEnabled ? .metal : .cpu)
    }

    public func requestComputeBackend(_ backend: DeepSkyComputeBackend) {
        guard snapshot.computeBackend != backend else { return }
        snapshot.computeBackend = backend
        NotificationCenter.default.post(name: .photonStackSetGPUAccelerationEnabled, object: backend)
    }
}

#if os(macOS)
public struct PhotonStackProcessCommands: Commands {
    @ObservedObject private var state: PhotonStackApplicationCommandState

    public init(state: PhotonStackApplicationCommandState) {
        self.state = state
    }

    public var body: some Commands {
        CommandMenu(state.processMenuTitle) {
            Button(state.snapshot.language == .simplifiedChinese ? "深空筛片与堆栈…" : "Deep Sky Assessment & Stack…") {
                NotificationCenter.default.post(name: .photonStackOpenDeepSky, object: nil)
            }
            .keyboardShortcut("d", modifiers: [.command, .shift])
            .disabled(!state.snapshot.isWorkspaceActive)
            Divider()

            Button(state.batchRegistrationTitle) {
                NotificationCenter.default.post(name: .photonStackRunBatchRegistration, object: nil)
            }
            .keyboardShortcut("r", modifiers: [.command, .shift])
            .disabled(state.snapshot.canRunBatchRegistration == false)

            Button(state.layerStackPreviewTitle) {
                NotificationCenter.default.post(name: .photonStackPreviewLayerStack, object: nil)
            }
            .keyboardShortcut("l", modifiers: [.command, .shift])
            .disabled(state.snapshot.canPreviewLayerStack == false)

            Button(state.productManifestExportTitle) {
                NotificationCenter.default.post(name: .photonStackExportProductManifest, object: nil)
            }
            .keyboardShortcut("m", modifiers: [.command, .shift])
            .disabled(state.snapshot.isWorkspaceActive == false)
        }
    }
}
#endif
