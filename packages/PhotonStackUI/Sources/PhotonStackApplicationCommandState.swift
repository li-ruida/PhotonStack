import PhotonStackAppCore
import SwiftUI

public struct PhotonStackApplicationCommandSnapshot: Equatable, Sendable {
    public var language: AppLanguage
    public var isWorkspaceActive: Bool
    public var canRunBatchRegistration: Bool
    public var canPreviewLayerStack: Bool

    public init(
        language: AppLanguage,
        isWorkspaceActive: Bool,
        canRunBatchRegistration: Bool,
        canPreviewLayerStack: Bool
    ) {
        self.language = language
        self.isWorkspaceActive = isWorkspaceActive
        self.canRunBatchRegistration = canRunBatchRegistration
        self.canPreviewLayerStack = canPreviewLayerStack
    }

    public static func inactive(language: AppLanguage) -> Self {
        Self(
            language: language,
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
}

#if os(macOS)
public struct PhotonStackProcessCommands: Commands {
    @ObservedObject private var state: PhotonStackApplicationCommandState

    public init(state: PhotonStackApplicationCommandState) {
        self.state = state
    }

    public var body: some Commands {
        CommandMenu(state.processMenuTitle) {
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
