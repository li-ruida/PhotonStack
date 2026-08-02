import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

private let photonStackUILogger = Logger(subsystem: "dev.photonstack.app", category: "ui")

@MainActor
private func presentRelinkPanel(for asset: PhotonStackAsset, model: PhotonStackWorkspaceModel) {
    #if os(macOS)
    guard let replacementURL = NativeFilePanels.relinkAssetURL(
        message: model.localized(.relinkAssetMessage)
    ) else {
        return
    }
    model.relinkAsset(asset, to: replacementURL)
    #endif
}

public extension Notification.Name {
    static let photonStackImportAssetURLs = Notification.Name("dev.photonstack.importAssetURLs")
    static let photonStackRunBatchRegistration = Notification.Name("dev.photonstack.runBatchRegistration")
    static let photonStackExportProductManifest = Notification.Name("dev.photonstack.exportProductManifest")
    static let photonStackPreviewLayerStack = Notification.Name("dev.photonstack.previewLayerStack")
    static let photonStackRequestApplicationTermination = Notification.Name("dev.photonstack.requestApplicationTermination")
    static let photonStackCloseWindowWithoutPrompt = Notification.Name("dev.photonstack.closeWindowWithoutPrompt")
}

@MainActor
public enum PhotonStackApplicationActivity {
    public private(set) static var hasActiveProcessing = false
    private static var pendingSettingsPersistence: [UUID: Task<Void, Never>] = [:]
    private static var pendingExternalFileImports: [URL] = []

    public static var hasPendingSettingsPersistence: Bool {
        pendingSettingsPersistence.isEmpty == false
    }

    public static func update(hasActiveProcessing: Bool) {
        self.hasActiveProcessing = hasActiveProcessing
    }

    public static func trackSettingsPersistence(_ task: Task<Void, Never>) {
        let id = UUID()
        pendingSettingsPersistence[id] = task
        Task { @MainActor in
            await task.value
            pendingSettingsPersistence.removeValue(forKey: id)
        }
    }

    public static func waitForPendingSettingsPersistence() async {
        while pendingSettingsPersistence.isEmpty == false {
            let tasks = Array(pendingSettingsPersistence.values)
            for task in tasks {
                await task.value
            }
        }
    }

    public static func enqueueExternalFileImports(_ urls: [URL]) {
        guard urls.isEmpty == false else {
            return
        }
        for url in urls {
            let normalized = url.standardizedFileURL
            if pendingExternalFileImports.contains(normalized) == false {
                pendingExternalFileImports.append(normalized)
            }
        }
    }

    public static func drainPendingExternalFileImports() -> [URL] {
        let urls = pendingExternalFileImports
        pendingExternalFileImports.removeAll()
        return urls
    }
}

private struct ApplicationStateSynchronizationModifier: ViewModifier {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let commandState: PhotonStackApplicationCommandState

    func body(content: Content) -> some View {
        content
            .onAppear {
                updateDocumentEditedState()
                PhotonStackApplicationActivity.update(hasActiveProcessing: model.hasActiveProcessing)
                commandState.update(model.applicationCommandSnapshot)
            }
            .onChange(of: model.hasUnsavedChanges) { _, _ in
                updateDocumentEditedState()
            }
            .onChange(of: model.hasActiveProcessing) { _, hasActiveProcessing in
                PhotonStackApplicationActivity.update(hasActiveProcessing: hasActiveProcessing)
            }
            .onChange(of: model.applicationCommandSnapshot) { _, snapshot in
                commandState.update(snapshot)
            }
            .onDisappear {
                PhotonStackApplicationActivity.update(hasActiveProcessing: false)
                let snapshot = PhotonStackApplicationCommandSnapshot.inactive(language: model.language)
                commandState.update(snapshot)
            }
    }

    private func updateDocumentEditedState() {
        #if os(macOS)
        for window in NSApp.windows where (window is NSPanel) == false {
            window.isDocumentEdited = model.hasUnsavedChanges
        }
        #endif
    }
}

private struct PreviewCacheCleanupConfirmationModifier: ViewModifier {
    @ObservedObject var model: PhotonStackWorkspaceModel

    func body(content: Content) -> some View {
        content.confirmationDialog(
            model.localized(.clearPreviewCacheConfirmationTitle),
            isPresented: Binding(
                get: { model.pendingPreviewCacheCleanupPlan != nil },
                set: { isPresented in
                    if isPresented == false {
                        model.cancelPreviewCacheCleanup()
                    }
                }
            ),
            titleVisibility: .visible,
            presenting: model.pendingPreviewCacheCleanupPlan
        ) { plan in
            Button(model.localized(.clearPreviewCache), role: .destructive) {
                model.confirmPreviewCacheCleanup(plan)
            }
            Button(model.localized(.cancelAction), role: .cancel) {
                model.cancelPreviewCacheCleanup()
            }
        } message: { plan in
            Text(confirmationMessage(for: plan))
        }
    }

    private func confirmationMessage(for plan: PreviewCacheCleanupPlan) -> String {
        let size = ByteCountFormatter.string(fromByteCount: plan.totalBytes, countStyle: .file)
        return String(
            format: model.localized(.clearPreviewCacheConfirmationMessage),
            plan.fileCount,
            size
        )
    }
}

private struct ProcessingInterruptionConfirmationModifier: ViewModifier {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let onComplete: (PendingProjectTransition) -> Void
    let onCancel: (PendingProcessingInterruption) -> Void

    func body(content: Content) -> some View {
        content.confirmationDialog(
            model.localized(.processingInterruptionTitle),
            isPresented: Binding(
                get: { model.pendingProcessingInterruption != nil },
                set: { isPresented in
                    guard isPresented == false,
                          let interruption = model.pendingProcessingInterruption
                    else {
                        return
                    }
                    model.cancelPendingProcessingInterruption()
                    onCancel(interruption)
                }
            ),
            titleVisibility: .visible,
            presenting: model.pendingProcessingInterruption
        ) { interruption in
            Button(actionTitle(for: interruption), role: .destructive) {
                if let transition = model.confirmPendingProcessingInterruption(interruption) {
                    onComplete(transition)
                }
            }
            Button(model.localized(.cancelAction), role: .cancel) {
                model.cancelPendingProcessingInterruption()
                onCancel(interruption)
            }
        } message: { interruption in
            Text(message(for: interruption))
        }
    }

    private func actionTitle(for interruption: PendingProcessingInterruption) -> String {
        model.localized(
            interruption == .terminateApplication
                ? .stopProcessingAndQuitAction
                : .stopProcessingAndCloseAction
        )
    }

    private func message(for interruption: PendingProcessingInterruption) -> String {
        model.localized(
            interruption == .terminateApplication
                ? .processingInterruptionQuitMessage
                : .processingInterruptionCloseMessage
        )
    }
}

public struct PhotonStackRootView: View {
    @StateObject private var model: PhotonStackWorkspaceModel
    private let applicationCommandState: PhotonStackApplicationCommandState
    @State private var isImporting = false
    @State private var isCreatingProject = false
    @State private var newProjectName = ""
    @State private var newProjectTemplate: ProjectTemplate = .deepSky
    @State private var pendingProductManifestExport = false
    @State private var pendingLayerStackPreview = false

    public init(
        processingService: any ProcessingService,
        applicationCommandState: PhotonStackApplicationCommandState = PhotonStackApplicationCommandState()
    ) {
        _model = StateObject(wrappedValue: PhotonStackWorkspaceModel(processingService: processingService))
        self.applicationCommandState = applicationCommandState
    }

    public var body: some View {
        NavigationSplitView {
            AssetSidebar(
                model: model,
                onImportFiles: { startImportFiles() },
                onImportFolder: { startImportFolder() }
            )
        } detail: {
            HStack(spacing: 0) {
                VStack(spacing: 0) {
                    PhotonStackPreview(model: model)

                    Divider()

                    ActionBar(model: model)
                }

                Divider()

                InspectorPanel(model: model)
                    .frame(width: 320)
            }
        }
        .toolbar {
            ToolbarItemGroup(placement: .automatic) {
                Button {
                    presentNewProjectSheet()
                } label: {
                    Label(model.localized(.newProject), systemImage: "doc.badge.plus")
                }
                .help(model.localized(.newProject))
                .keyboardShortcut("n", modifiers: [.command, .shift])
                .disabled(model.hasActiveProcessing)

                Menu {
                    Button {
                        startImportFiles()
                    } label: {
                        Label(model.localized(.importFilesAction), systemImage: "photo.on.rectangle.angled")
                    }
                    .keyboardShortcut("i", modifiers: [.command])

                    Button {
                        startImportFolder()
                    } label: {
                        Label(model.localized(.importFolderAction), systemImage: "folder.badge.plus")
                    }
                    .keyboardShortcut("i", modifiers: [.command, .shift])
                } label: {
                    Label(model.localized(.importAction), systemImage: "plus")
                }
                .help(model.localized(.importAction))
                .disabled(model.hasActiveProcessing)

                Button {
                    openProject()
                } label: {
                    Label(model.localized(.openAction), systemImage: "folder")
                }
                .help(model.localized(.openAction))
                .keyboardShortcut("o", modifiers: [.command])
                .disabled(model.hasActiveProcessing)

                Button {
                    saveProject()
                } label: {
                    Label(model.localized(.saveAction), systemImage: "square.and.arrow.down")
                }
                .help(model.localized(.saveAction))
                .keyboardShortcut("s", modifiers: [.command])
                .disabled(model.hasActiveProcessing)
            }
        }
        .fileImporter(
            isPresented: $isImporting,
            allowedContentTypes: [.item],
            allowsMultipleSelection: true
        ) { result in
            if case let .success(urls) = result {
                model.importAssets(from: urls)
            }
        }
        .sheet(isPresented: $isCreatingProject) {
            NewProjectSheet(
                model: model,
                name: $newProjectName,
                template: $newProjectTemplate,
                onCancel: {
                    isCreatingProject = false
                },
                onCreate: {
                    createProject(importAfterCreate: false)
                },
                onCreateAndImport: {
                    createProject(importAfterCreate: true)
                }
            )
        }
        .confirmationDialog(
            model.localized(
                model.assetsPendingRemoval.count > 1
                    ? .removeAssetsConfirmationTitle
                    : .removeAssetConfirmationTitle
            ),
            isPresented: Binding(
                get: { model.assetsPendingRemoval.isEmpty == false },
                set: { isPresented in
                    if isPresented == false {
                        model.cancelAssetRemoval()
                    }
                }
            ),
            titleVisibility: .visible,
            presenting: model.assetsPendingRemoval.isEmpty ? nil : model.assetsPendingRemoval
        ) { requestedAssets in
            Button(model.localized(.removeAssetAction), role: .destructive) {
                model.confirmAssetRemoval(requestedAssets)
            }
            Button(model.localized(.cancelAction), role: .cancel) {
                model.cancelAssetRemoval()
            }
        } message: { requestedAssets in
            Text(assetRemovalConfirmationMessage(for: requestedAssets))
        }
        .confirmationDialog(
            model.localized(.removeArtifactConfirmationTitle),
            isPresented: Binding(
                get: { model.artifactPendingRemoval != nil },
                set: { isPresented in
                    if isPresented == false {
                        model.cancelArtifactRemoval()
                    }
                }
            ),
            titleVisibility: .visible,
            presenting: model.artifactPendingRemoval
        ) { artifact in
            Button(model.localized(.removeArtifactAction), role: .destructive) {
                model.confirmArtifactRemoval(artifact)
            }
            Button(model.localized(.cancelAction), role: .cancel) {
                model.cancelArtifactRemoval()
            }
        } message: { artifact in
            Text("\(artifact.name)\n\n\(model.localized(.removeArtifactConfirmationMessage))")
        }
        .confirmationDialog(
            model.localized(.unsavedChangesTitle),
            isPresented: Binding(
                get: { model.pendingProjectTransition != nil },
                set: { isPresented in
                    if isPresented == false, model.pendingProjectTransition != nil {
                        cancelPendingProjectTransition()
                    }
                }
            ),
            titleVisibility: .visible,
            presenting: model.pendingProjectTransition
        ) { _ in
            Button(model.localized(.saveChangesAction)) {
                saveChangesAndPerformPendingProjectTransition()
            }
            Button(model.localized(.discardChangesAction), role: .destructive) {
                if let transition = model.discardChangesAndPerformPendingProjectTransition() {
                    completeProjectTransition(transition)
                }
            }
            Button(model.localized(.cancelAction), role: .cancel) {
                cancelPendingProjectTransition()
            }
        } message: { _ in
            Text("\(model.project.name)\n\n\(model.localized(.unsavedChangesMessage))")
        }
        .modifier(PreviewCacheCleanupConfirmationModifier(model: model))
        .modifier(
            ProcessingInterruptionConfirmationModifier(
                model: model,
                onComplete: { completeProjectTransition($0) },
                onCancel: { cancelPendingProcessingInterruption($0) }
            )
        )
        .task {
            await model.loadUserSettings()
            let pendingURLs = PhotonStackApplicationActivity.drainPendingExternalFileImports()
            if pendingURLs.isEmpty == false {
                photonStackUILogger.notice(
                    "Importing \(pendingURLs.count, privacy: .public) queued asset URL(s) after UI startup"
                )
                model.importAssets(from: pendingURLs)
            }
        }
        .modifier(
            ApplicationStateSynchronizationModifier(
                model: model,
                commandState: applicationCommandState
            )
        )
        .onReceive(NotificationCenter.default.publisher(for: .photonStackImportAssetURLs)) { notification in
            guard let urls = notification.object as? [URL], urls.isEmpty == false else {
                return
            }
            photonStackUILogger.notice("Importing \(urls.count, privacy: .public) asset URL(s) from app-open event")
            PhotonStackApplicationActivity.enqueueExternalFileImports(urls)
            let pendingURLs = PhotonStackApplicationActivity.drainPendingExternalFileImports()
            guard pendingURLs.isEmpty == false else {
                return
            }
            model.importAssets(from: pendingURLs)
        }
        .onReceive(NotificationCenter.default.publisher(for: .photonStackRunBatchRegistration)) { _ in
            guard model.applicationCommandSnapshot.canRunBatchRegistration,
                  let reference = model.batchRegistrationAssets.first
            else {
                return
            }
            photonStackUILogger.notice("Running batch registration from app menu")
            model.startRegisterBatch(reference: reference, alignment: .distortion)
        }
        .onReceive(NotificationCenter.default.publisher(for: .photonStackExportProductManifest)) { _ in
            guard model.isProcessing == false else {
                pendingProductManifestExport = true
                photonStackUILogger.notice("Queued product manifest export until current processing completes")
                return
            }
            photonStackUILogger.notice("Exporting product manifest from app menu")
            model.startExportProductManifestToDesktop()
        }
        .onChange(of: model.isProcessing) { _, isProcessing in
            guard isProcessing == false else {
                return
            }
            if pendingLayerStackPreview {
                pendingLayerStackPreview = false
                if model.canPreviewVisibleLayerStack {
                    photonStackUILogger.notice("Previewing queued layer stack")
                    model.startPreviewLayerStack()
                    return
                }
            }
            if pendingProductManifestExport {
                pendingProductManifestExport = false
                photonStackUILogger.notice("Exporting queued product manifest")
                model.startExportProductManifestToDesktop()
            }
        }
        .onReceive(NotificationCenter.default.publisher(for: .photonStackPreviewLayerStack)) { _ in
            guard model.isProcessing == false else {
                pendingLayerStackPreview = true
                photonStackUILogger.notice("Queued layer stack preview until current processing completes")
                return
            }
            guard model.canPreviewVisibleLayerStack else {
                return
            }
            photonStackUILogger.notice("Previewing layer stack from app menu")
            model.startPreviewLayerStack()
        }
        .onReceive(NotificationCenter.default.publisher(for: .photonStackRequestApplicationTermination)) { _ in
            #if os(macOS)
            if model.requestApplicationTermination() {
                completeApplicationTermination()
            }
            #endif
        }
        #if os(macOS)
        .background(
            WindowCloseGuard(
                hasUnsavedChanges: model.hasUnsavedChanges,
                hasActiveProcessing: model.hasActiveProcessing,
                onRequestClose: {
                    if model.requestWindowClose() {
                        NotificationCenter.default.post(
                            name: .photonStackCloseWindowWithoutPrompt,
                            object: nil
                        )
                    }
                }
            )
        )
        #endif
        .frame(minWidth: 1120, minHeight: 720)
    }

    private func presentNewProjectSheet() {
        newProjectTemplate = model.selectedTemplate
        newProjectName = suggestedProjectName(for: model.selectedTemplate)
        isCreatingProject = true
    }

    private func assetRemovalConfirmationMessage(for assets: [PhotonStackAsset]) -> String {
        let names = assets.map(\.displayName).joined(separator: "\n")
        let consequence = model.localized(
            assets.count > 1
                ? .removeAssetsConfirmationMessage
                : .removeAssetConfirmationMessage
        )
        return names.isEmpty ? consequence : "\(names)\n\n\(consequence)"
    }

    private func suggestedProjectName(for template: ProjectTemplate) -> String {
        let formatter = DateFormatter()
        formatter.dateFormat = "yyyy-MM-dd HH.mm"
        return "\(template.defaultProjectName) \(formatter.string(from: Date()))"
    }

    private func createProject(importAfterCreate: Bool) {
        let created = model.requestCreateProject(
            template: newProjectTemplate,
            name: newProjectName,
            importAfterCreate: importAfterCreate
        )
        isCreatingProject = false
        if created && importAfterCreate {
            DispatchQueue.main.async {
                startImportFiles()
            }
        }
    }

    private func startImportFiles() {
        #if os(macOS)
        if let urls = NativeFilePanels.importAssetURLs(message: model.localized(.importFilesMessage)) {
            model.importAssets(from: urls)
        }
        #else
        isImporting = true
        #endif
    }

    private func startImportFolder() {
        #if os(macOS)
        if let url = NativeFilePanels.importAssetFolderURL(message: model.localized(.importFolderMessage)) {
            model.importAssets(from: [url])
        }
        #else
        isImporting = true
        #endif
    }

    private func openProject() {
        #if os(macOS)
        if let directory = NativeFilePanels.openProjectDirectory(
            message: model.localized(.openProjectMessage)
        ) {
            model.requestOpenProject(from: directory)
        }
        #endif
    }

    private func saveProject() {
        #if os(macOS)
        if let directory = model.currentProjectDirectory {
            model.startSaveProject(to: directory)
        } else if let directory = NativeFilePanels.saveProjectDirectory(
            message: model.localized(.saveProjectMessage)
        ) {
            model.startSaveProject(to: directory)
        }
        #endif
    }

    private func saveChangesAndPerformPendingProjectTransition() {
        #if os(macOS)
        guard let directory = model.currentProjectDirectory ?? NativeFilePanels.saveProjectDirectory(
            message: model.localized(.saveProjectMessage)
        ) else {
            cancelPendingProjectTransition()
            return
        }
        guard let transition = model.beginSavingPendingProjectTransition() else {
            return
        }
        let wasTermination = transition == .terminateApplication
        Task { @MainActor in
            guard let completedTransition = await model.saveChangesAndPerformProjectTransition(
                transition,
                to: directory
            ) else {
                if wasTermination {
                    NSApp.reply(toApplicationShouldTerminate: false)
                }
                return
            }
            completeProjectTransition(completedTransition)
        }
        #endif
    }

    private func cancelPendingProjectTransition() {
        #if os(macOS)
        let wasTermination = model.pendingProjectTransition == .terminateApplication
        model.cancelPendingProjectTransition()
        if wasTermination {
            NSApp.reply(toApplicationShouldTerminate: false)
        }
        #else
        model.cancelPendingProjectTransition()
        #endif
    }

    private func cancelPendingProcessingInterruption(_ interruption: PendingProcessingInterruption) {
        model.cancelPendingProcessingInterruption()
        #if os(macOS)
        if interruption == .terminateApplication {
            NSApp.reply(toApplicationShouldTerminate: false)
        }
        #endif
    }

    private func completeProjectTransition(_ transition: PendingProjectTransition) {
        switch transition {
        case let .create(_, _, importAfterCreate):
            if importAfterCreate {
                DispatchQueue.main.async {
                    startImportFiles()
                }
            }
        case .open:
            break
        case .closeWindow:
            #if os(macOS)
            NotificationCenter.default.post(name: .photonStackCloseWindowWithoutPrompt, object: nil)
            #endif
        case .terminateApplication:
            #if os(macOS)
            completeApplicationTermination()
            #endif
        }
    }

    private func completeApplicationTermination() {
        #if os(macOS)
        Task { @MainActor in
            await PhotonStackApplicationActivity.waitForPendingSettingsPersistence()
            NSApp.reply(toApplicationShouldTerminate: true)
        }
        #endif
    }

}

#if os(macOS)
private final class WindowAttachmentView: NSView {
    var onWindowChange: ((NSWindow?) -> Void)?

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        onWindowChange?(window)
    }
}

private struct WindowCloseGuard: NSViewRepresentable {
    var hasUnsavedChanges: Bool
    var hasActiveProcessing: Bool
    var onRequestClose: @MainActor () -> Void

    func makeCoordinator() -> Coordinator {
        Coordinator(
            hasUnsavedChanges: hasUnsavedChanges,
            hasActiveProcessing: hasActiveProcessing,
            onRequestClose: onRequestClose
        )
    }

    func makeNSView(context: Context) -> WindowAttachmentView {
        let view = WindowAttachmentView(frame: .zero)
        view.onWindowChange = { [weak coordinator = context.coordinator] window in
            coordinator?.attach(to: window)
        }
        return view
    }

    func updateNSView(_ nsView: WindowAttachmentView, context: Context) {
        context.coordinator.hasUnsavedChanges = hasUnsavedChanges
        context.coordinator.hasActiveProcessing = hasActiveProcessing
        context.coordinator.onRequestClose = onRequestClose
        context.coordinator.attach(to: nsView.window)
    }

    static func dismantleNSView(_ nsView: WindowAttachmentView, coordinator: Coordinator) {
        nsView.onWindowChange = nil
        coordinator.detach()
    }

    @MainActor
    final class Coordinator: NSObject, NSWindowDelegate {
        var hasUnsavedChanges: Bool
        var hasActiveProcessing: Bool
        var onRequestClose: @MainActor () -> Void
        private weak var attachedWindow: NSWindow?
        nonisolated(unsafe) private weak var originalDelegate: NSWindowDelegate?
        private var allowNextClose = false

        init(
            hasUnsavedChanges: Bool,
            hasActiveProcessing: Bool,
            onRequestClose: @escaping @MainActor () -> Void
        ) {
            self.hasUnsavedChanges = hasUnsavedChanges
            self.hasActiveProcessing = hasActiveProcessing
            self.onRequestClose = onRequestClose
        }

        func attach(to window: NSWindow?) {
            guard attachedWindow !== window else {
                return
            }
            detach()
            guard let window else {
                return
            }
            attachedWindow = window
            originalDelegate = window.delegate
            window.delegate = self
            NotificationCenter.default.addObserver(
                self,
                selector: #selector(closeWithoutPrompt),
                name: .photonStackCloseWindowWithoutPrompt,
                object: nil
            )
        }

        func detach() {
            NotificationCenter.default.removeObserver(
                self,
                name: .photonStackCloseWindowWithoutPrompt,
                object: nil
            )
            if attachedWindow?.delegate === self {
                attachedWindow?.delegate = originalDelegate
            }
            attachedWindow = nil
            originalDelegate = nil
            allowNextClose = false
        }

        func windowShouldClose(_ sender: NSWindow) -> Bool {
            if allowNextClose {
                allowNextClose = false
                return originalDelegate?.windowShouldClose?(sender) ?? true
            }
            guard hasUnsavedChanges || hasActiveProcessing else {
                return originalDelegate?.windowShouldClose?(sender) ?? true
            }
            onRequestClose()
            return false
        }

        override func responds(to selector: Selector!) -> Bool {
            super.responds(to: selector) || originalDelegate?.responds(to: selector) == true
        }

        override func forwardingTarget(for selector: Selector!) -> Any? {
            if originalDelegate?.responds(to: selector) == true {
                return originalDelegate
            }
            return super.forwardingTarget(for: selector)
        }

        @objc private func closeWithoutPrompt() {
            guard let attachedWindow else {
                return
            }
            allowNextClose = true
            attachedWindow.performClose(nil)
        }
    }
}
#endif

private struct AssetSidebar: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let onImportFiles: () -> Void
    let onImportFolder: () -> Void
    @State private var hoveredAssetID: PhotonStackAsset.ID?

    var body: some View {
        VStack(spacing: 0) {
            if model.project.assets.isEmpty {
                Spacer(minLength: 20)

                VStack(spacing: 12) {
                    Image(systemName: "photo.on.rectangle.angled")
                        .font(.system(size: 38, weight: .regular))
                        .foregroundStyle(.secondary)

                    VStack(spacing: 4) {
                        Text(model.localized(.emptyAssetsTitle))
                            .font(.headline)
                        Text(model.localized(.emptyAssetsMessage))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                            .multilineTextAlignment(.center)
                    }

                    Menu {
                        Button(action: onImportFiles) {
                            Label(model.localized(.importFilesAction), systemImage: "photo.on.rectangle.angled")
                        }
                        Button(action: onImportFolder) {
                            Label(model.localized(.importFolderAction), systemImage: "folder.badge.plus")
                        }
                    } label: {
                        Label(model.localized(.importAction), systemImage: "plus")
                            .frame(maxWidth: 180)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(model.isProcessing)
                }
                .padding(20)

                Spacer(minLength: 20)
            } else {
                List(selection: assetSelection) {
                    ForEach(assetGroups) { group in
                        Section {
                            ForEach(group.assets) { asset in
                                SidebarAssetRow(
                                    model: model,
                                    asset: asset,
                                    isSelected: asset.id == model.selectedAsset?.id,
                                    showsRemoveButton: hoveredAssetID == asset.id || asset.id == model.selectedAssetID
                                )
                                .tag(asset.id)
                                .contentShape(Rectangle())
                                .onHover { isHovering in
                                    hoveredAssetID = isHovering ? asset.id : nil
                                }
                                .contextMenu {
                                    Button {
                                        presentRelinkPanel(for: asset, model: model)
                                    } label: {
                                        Label(model.localized(.relinkAssetAction), systemImage: "link")
                                    }
                                    .disabled(model.isProcessing)

                                    Divider()

                                    Button(role: .destructive) {
                                        model.requestAssetRemoval(asset)
                                    } label: {
                                        Label(model.localized(.removeAssetAction), systemImage: "trash")
                                    }
                                    .disabled(model.isProcessing)
                                }
                            }
                            .onDelete { offsets in
                                model.requestAssetRemoval(
                                    offsets.sorted().map { group.assets[$0] }
                                )
                            }
                        } header: {
                            HStack(spacing: 6) {
                                Text(model.roleName(group.role))
                                Spacer()
                                Text("\(group.assets.count)")
                                    .font(.caption2.monospacedDigit())
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }
                }
                .listStyle(.sidebar)
                .onDeleteCommand {
                    if let asset = model.selectedAsset, model.isProcessing == false {
                        model.requestAssetRemoval(asset)
                    }
                }
            }

            Divider()

            BuildIdentityBadge()
                .padding(.horizontal, 10)
                .padding(.vertical, 8)
        }
        .navigationTitle(model.project.name)
        .navigationSplitViewColumnWidth(min: 220, ideal: 280, max: 360)
    }

    private var assetGroups: [SidebarAssetGroup] {
        CalibrationFrameRole.allCases.compactMap { role in
            let assets = model.project.assets.filter { $0.role == role }
            return assets.isEmpty ? nil : SidebarAssetGroup(role: role, assets: assets)
        }
    }

    private var assetSelection: Binding<PhotonStackAsset.ID?> {
        Binding(
            get: { model.selectedAssetID },
            set: { assetID in
                guard model.isProcessing == false,
                      let assetID,
                      let asset = model.project.assets.first(where: { $0.id == assetID })
                else {
                    return
                }
                model.select(asset)
            }
        )
    }
}

private struct SidebarAssetGroup: Identifiable {
    var role: CalibrationFrameRole
    var assets: [PhotonStackAsset]

    var id: CalibrationFrameRole {
        role
    }
}

private struct BuildIdentityBadge: View {
    private let identity = PhotonStackBuildIdentity.current

    var body: some View {
        HStack(spacing: 6) {
            Image(systemName: "number")
                .imageScale(.small)

            Text(identity.displayText)
                .lineLimit(1)
                .truncationMode(.middle)
        }
        .font(.caption2.monospaced())
        .foregroundStyle(.secondary)
        .frame(maxWidth: .infinity, alignment: .leading)
        .help(identity.helpText)
        .accessibilityLabel("PhotonStack \(identity.displayText)")
    }
}

private struct ActionBar: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        let canInspectAsset = model.selectedAsset != nil &&
            model.selectedAssetIsMissing == false &&
            model.isProcessing == false
        let canAdjustCurrentInput = model.canProcessCurrentInput &&
            model.isProcessing == false &&
            model.editGraphNeedsReplay == false

        HStack(spacing: 8) {
            Button {
                model.startInspectSelectedAsset()
            } label: {
                Label(model.localized(.inspectAction), systemImage: "info.circle")
            }
            .disabled(canInspectAsset == false)

            Button {
                model.startBuildPreview()
            } label: {
                Label(model.localized(.previewAction), systemImage: "photo")
            }
            .accessibilityLabel(
                "\(model.localized(.previewAction)): \(model.selectedAsset?.displayName ?? model.localized(.previewAction))"
            )
            .disabled(canInspectAsset == false)

            Menu {
                Button {
                    model.startAutoStretchPreview()
                } label: {
                    Label(model.localized(.stretchAction), systemImage: "camera.aperture")
                }

                Button {
                    model.startLocalContrastPreview()
                } label: {
                    Label(model.localized(.contrastAction), systemImage: "circle.lefthalf.filled")
                }

                Button {
                    model.startDenoisePreview()
                } label: {
                    Label(model.localized(.denoiseAction), systemImage: "wand.and.stars")
                }

                Button {
                    model.startSharpenPreview()
                } label: {
                    Label(model.localized(.sharpenAction), systemImage: "scope")
                }

                Divider()

                Button {
                    model.startReduceStarsPreview()
                } label: {
                    Label(model.localized(.starsAction), systemImage: "sparkles")
                }

                Button {
                    model.startReduceComaPreview()
                } label: {
                    Label(model.localized(.comaAction), systemImage: "oval.portrait")
                }
            } label: {
                Label(model.localized(.inspectorAdjustmentsGroup), systemImage: "slider.horizontal.3")
            }
            .disabled(canAdjustCurrentInput == false)

            Spacer()

            ControlGroup {
                Button {
                    model.stepBackPreview()
                } label: {
                    Label(model.localized(.backAction), systemImage: "arrow.uturn.backward")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.backAction))
                .disabled(model.canStepBack == false || model.isProcessing)

                Button {
                    model.resetPreviewToOriginal()
                } label: {
                    Label(model.localized(.resetAction), systemImage: "arrow.counterclockwise")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.resetAction))
                .disabled(model.canResetPreviewToOriginal == false || model.isProcessing)

                Button(role: .cancel) {
                    model.cancelCurrentTask()
                } label: {
                    Label(model.localized(.cancelAction), systemImage: "xmark.circle")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.cancelAction))
                .disabled(model.canCancel == false)

                #if os(macOS)
                Button {
                    if let output = NativeFilePanels.exportImageURL(
                        message: model.localized(.exportImageMessage)
                    ) {
                        model.startExportCurrentImage(to: output)
                    }
                } label: {
                    Label(model.localized(.exportAction), systemImage: "square.and.arrow.up")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(model.editGraphNeedsReplay ? .editGraphNeedsReplay : .exportAction))
                .disabled(model.canExportCurrentImage == false)
                #endif
            }
        }
        .buttonStyle(.bordered)
        .controlSize(.regular)
        .padding(12)
        .background(.bar)
    }
}

private struct InspectorPanel: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var expandedGroups: Set<InspectorToolGroup> = Set(InspectorToolGroup.defaultExpanded)
    private let groupAnimation = Animation.interactiveSpring(response: 0.28, dampingFraction: 0.9, blendDuration: 0.08)

    var body: some View {
        VStack(spacing: 0) {
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    InspectorToolbarHeader(
                        model: model,
                        onExpandAll: {
                            withAnimation(groupAnimation) {
                                expandedGroups = Set(InspectorToolGroup.allCases)
                            }
                        },
                        onCollapseAll: {
                            withAnimation(groupAnimation) {
                                expandedGroups.removeAll()
                            }
                        }
                    )

                    InspectorToolGroupView(
                        model: model,
                        group: .asset,
                        summary: summary(for: .asset),
                        isExpanded: binding(for: .asset)
                    ) {
                        SelectedAssetSection(model: model)
                    }

                    InspectorToolGroupView(
                        model: model,
                        group: .photoInfo,
                        summary: summary(for: .photoInfo),
                        isExpanded: binding(for: .photoInfo)
                    ) {
                        PhotoInfoSection(model: model)
                    }

                    InspectorToolGroupView(
                        model: model,
                        group: .workflow,
                        summary: summary(for: .workflow),
                        isExpanded: binding(for: .workflow)
                    ) {
                        WorkflowSection(model: model, parameters: processingParametersBinding)
                        RegistrationSection(model: model)
                        MosaicSection(model: model, parameters: processingParametersBinding)
                    }

                    InspectorToolGroupView(
                        model: model,
                        group: .adjustments,
                        summary: summary(for: .adjustments),
                        isExpanded: binding(for: .adjustments)
                    ) {
                        AdjustmentsInspectorSection(model: model, parameters: processingParametersBinding)
                    }

                    InspectorToolGroupView(
                        model: model,
                        group: .workspace,
                        summary: summary(for: .workspace),
                        isExpanded: binding(for: .workspace)
                    ) {
                        ProcessingWorkspaceSection(model: model)
                    }

                    InspectorToolGroupView(
                        model: model,
                        group: .batch,
                        summary: summary(for: .batch),
                        isExpanded: binding(for: .batch)
                    ) {
                        BatchQueueSection(model: model)
                    }

                    InspectorToolGroupView(
                        model: model,
                        group: .history,
                        summary: summary(for: .history),
                        isExpanded: binding(for: .history)
                    ) {
                        EditGraphSection(model: model)
                        JobsSection(jobs: model.jobs, model: model)
                    }

                    InspectorToolGroupView(
                        model: model,
                        group: .project,
                        summary: summary(for: .project),
                        isExpanded: binding(for: .project)
                    ) {
                        LanguageSection(model: model)
                        ProjectSection(model: model)
                    }
                }
                .padding(16)
            }

            Divider()

            StatusConsole(model: model)
        }
    }

    private func binding(for group: InspectorToolGroup) -> Binding<Bool> {
        Binding(
            get: { expandedGroups.contains(group) },
            set: { isExpanded in
                withAnimation(groupAnimation) {
                    if isExpanded {
                        expandedGroups.insert(group)
                    } else {
                        expandedGroups.remove(group)
                    }
                }
            }
        )
    }

    private var processingParametersBinding: Binding<ProcessingParameters> {
        Binding(
            get: { model.parameters },
            set: { model.setProcessingParameters($0) }
        )
    }

    private func summary(for group: InspectorToolGroup) -> String? {
        switch group {
        case .project:
            return model.project.name
        case .asset:
            return model.selectedAsset?.displayName ?? model.localized(.noAssetSelected)
        case .photoInfo:
            guard let metadata = model.selectedAsset?.metadata else {
                return model.localized(.metadataUnavailable)
            }
            return [dimensions(metadata), cameraName(metadata), exposure(metadata)]
                .compactMap(\.self)
                .joined(separator: " · ")
        case .workspace:
            return "\(model.project.layers.count) \(model.localized(.layersLabel)) · \(model.project.artifacts.count) \(model.localized(.artifactsLabel))"
        case .workflow:
            return "\(model.roleName(.light)) \(model.lightAssets.count) · \(model.roleName(.dark)) \(model.darkAssets.count) · \(model.roleName(.flat)) \(model.flatAssets.count)"
        case .adjustments:
            return "\(model.localized(.previewWidth)) \(model.parameters.previewWidth) px"
        case .batch:
            return "\(model.batchQueue.count) \(model.localized(.tasksSection))"
        case .history:
            return "\(model.project.editGraph.operations.count) \(model.localized(.editGraphSection)) · \(model.jobs.count) \(model.localized(.tasksSection))"
        }
    }

    private func dimensions(_ metadata: AssetMetadata) -> String? {
        guard let width = metadata.width, let height = metadata.height else {
            return nil
        }
        return "\(width) x \(height)"
    }

    private func cameraName(_ metadata: AssetMetadata) -> String? {
        let name = [metadata.cameraMake, metadata.cameraModel]
            .compactMap { value in
                let trimmed = value?.trimmingCharacters(in: .whitespacesAndNewlines)
                return trimmed?.isEmpty == false ? trimmed : nil
            }
            .joined(separator: " ")
        return name.isEmpty ? nil : name
    }

    private func exposure(_ metadata: AssetMetadata) -> String? {
        guard let exposure = metadata.exposureTimeSeconds, exposure.isFinite, exposure > 0 else {
            return nil
        }
        if exposure > 0, exposure < 1 {
            let denominator = (1.0 / exposure).rounded()
            if denominator.isFinite, denominator <= Double(Int.max) {
                return "1/\(Int(denominator)) s"
            }
        }
        return exposure < 0.01 ? String(format: "%.3g s", exposure) : String(format: "%.2f s", exposure)
    }
}

private enum InspectorToolGroup: String, CaseIterable, Hashable {
    case project
    case asset
    case photoInfo
    case workspace
    case workflow
    case adjustments
    case batch
    case history

    static var defaultExpanded: [InspectorToolGroup] {
        [.asset, .workflow, .adjustments]
    }

    var titleKey: LocalizedTextKey {
        switch self {
        case .project:
            return .inspectorProjectGroup
        case .asset:
            return .inspectorAssetGroup
        case .photoInfo:
            return .inspectorPhotoInfoGroup
        case .workspace:
            return .inspectorWorkspaceGroup
        case .workflow:
            return .inspectorWorkflowGroup
        case .adjustments:
            return .inspectorAdjustmentsGroup
        case .batch:
            return .inspectorBatchGroup
        case .history:
            return .inspectorHistoryGroup
        }
    }

    var iconName: String {
        switch self {
        case .project:
            return "folder"
        case .asset:
            return "photo.on.rectangle"
        case .photoInfo:
            return "camera.metering.matrix"
        case .workspace:
            return "rectangle.3.group"
        case .workflow:
            return "point.3.connected.trianglepath.dotted"
        case .adjustments:
            return "slider.horizontal.3"
        case .batch:
            return "tray.full"
        case .history:
            return "clock.arrow.circlepath"
        }
    }

    var introKey: LocalizedTextKey {
        switch self {
        case .project:
            return .inspectorProjectGroupIntro
        case .asset:
            return .inspectorAssetGroupIntro
        case .photoInfo:
            return .inspectorPhotoInfoGroupIntro
        case .workspace:
            return .inspectorWorkspaceGroupIntro
        case .workflow:
            return .inspectorWorkflowGroupIntro
        case .adjustments:
            return .inspectorAdjustmentsGroupIntro
        case .batch:
            return .inspectorBatchGroupIntro
        case .history:
            return .inspectorHistoryGroupIntro
        }
    }
}

private struct InspectorToolbarHeader: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let onExpandAll: () -> Void
    let onCollapseAll: () -> Void

    var body: some View {
        HStack(spacing: 8) {
            Label(model.localized(.toolsPanel), systemImage: "sidebar.right")
                .font(.headline)

            Spacer()

            Button(action: onExpandAll) {
                Label(model.localized(.expandAllTools), systemImage: "rectangle.expand.vertical")
                    .labelStyle(.iconOnly)
            }
            .help(model.localized(.expandAllTools))

            Button(action: onCollapseAll) {
                Label(model.localized(.collapseAllTools), systemImage: "rectangle.compress.vertical")
                    .labelStyle(.iconOnly)
            }
            .help(model.localized(.collapseAllTools))
        }
        .buttonStyle(.borderless)
    }
}

private struct InspectorToolGroupView<Content: View>: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let group: InspectorToolGroup
    var summary: String? = nil
    @Binding var isExpanded: Bool
    @ViewBuilder let content: () -> Content
    @State private var showIntro = false
    @State private var hoverIntroTask: Task<Void, Never>?
    private let groupAnimation = Animation.interactiveSpring(response: 0.28, dampingFraction: 0.9, blendDuration: 0.08)

    var body: some View {
        VStack(alignment: .leading, spacing: 0) {
            Button {
                isExpanded.toggle()
            } label: {
                HStack(spacing: 8) {
                    Image(systemName: group.iconName)
                        .foregroundStyle(Color.accentColor)
                        .frame(width: 18)
                        .transaction { transaction in
                            transaction.animation = nil
                        }

                    VStack(alignment: .leading, spacing: 2) {
                        Text(model.localized(group.titleKey))
                            .font(.subheadline)
                            .fontWeight(.semibold)
                            .transaction { transaction in
                                transaction.animation = nil
                            }

                        if let summary, summary.isEmpty == false {
                            Text(summary)
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                                .truncationMode(.middle)
                                .transaction { transaction in
                                    transaction.animation = nil
                                }
                        }
                    }

                    Spacer()

                    Image(systemName: "chevron.right")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.secondary)
                        .frame(width: 14)
                        .rotationEffect(.degrees(isExpanded ? 90 : 0))
                        .animation(groupAnimation, value: isExpanded)
                }
                .contentShape(Rectangle())
            }
            .buttonStyle(InspectorHeaderButtonStyle())
            .onHover { isHovering in
                if isHovering {
                    scheduleIntro()
                } else {
                    cancelIntro()
                }
            }
            .popover(isPresented: $showIntro, arrowEdge: .trailing) {
                Text(model.localized(group.introKey))
                    .font(.callout)
                    .lineSpacing(3)
                    .foregroundStyle(.primary)
                    .frame(width: 280, alignment: .leading)
                    .padding(14)
                    .presentationCompactAdaptation(.popover)
            }

            CollapsibleInspectorContent(isExpanded: isExpanded) {
                content()
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.quaternary.opacity(0.45), in: RoundedRectangle(cornerRadius: 8))
        .overlay {
            RoundedRectangle(cornerRadius: 8)
                .stroke(.secondary.opacity(0.16), lineWidth: 1)
        }
        .onDisappear {
            cancelIntro()
        }
    }

    private func scheduleIntro() {
        hoverIntroTask?.cancel()
        showIntro = false
        hoverIntroTask = Task {
            try? await Task.sleep(nanoseconds: 5_000_000_000)
            guard Task.isCancelled == false else {
                return
            }
            await MainActor.run {
                withAnimation(.easeOut(duration: 0.16)) {
                    showIntro = true
                }
            }
        }
    }

    private func cancelIntro() {
        hoverIntroTask?.cancel()
        hoverIntroTask = nil
        showIntro = false
    }
}

private struct InspectorHeaderButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
    }
}

private struct CollapsibleInspectorContent<Content: View>: View {
    let isExpanded: Bool
    @ViewBuilder let content: () -> Content
    @State private var measuredHeight: CGFloat = 0
    private let groupAnimation = Animation.interactiveSpring(response: 0.28, dampingFraction: 0.9, blendDuration: 0.08)

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            content()
        }
        .padding(.top, 12)
        .fixedSize(horizontal: false, vertical: true)
        .background {
            GeometryReader { proxy in
                Color.clear
                    .preference(key: InspectorContentHeightKey.self, value: proxy.size.height)
            }
        }
        .onPreferenceChange(InspectorContentHeightKey.self) { height in
            guard abs(measuredHeight - height) > 0.5 else {
                return
            }
            measuredHeight = height
        }
        .frame(height: isExpanded ? measuredHeight : 0, alignment: .top)
        .opacity(isExpanded ? 1 : 0)
        .clipped()
        .allowsHitTesting(isExpanded)
        .accessibilityHidden(isExpanded == false)
        .animation(groupAnimation, value: isExpanded)
    }
}

private struct InspectorContentHeightKey: PreferenceKey {
    static let defaultValue: CGFloat = 0

    static func reduce(value: inout CGFloat, nextValue: () -> CGFloat) {
        value = max(value, nextValue())
    }
}

private struct SelectedAssetSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(model.localized(.assetSection))
                .font(.headline)

            if let asset = model.selectedAsset {
                LabeledContent(model.localized(.nameLabel), value: asset.displayName)
                LabeledContent(model.localized(.kindLabel), value: asset.kind.rawValue.uppercased())
                LabeledContent(model.localized(.pathLabel), value: asset.originalURL.deletingLastPathComponent().path)
                    .font(.caption)

                if model.isAssetMissing(asset) {
                    Label(model.localized(.missingAssetStatus), systemImage: "exclamationmark.triangle.fill")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.orange)
                }

                Button {
                    presentRelinkPanel(for: asset, model: model)
                } label: {
                    Label(model.localized(.relinkAssetAction), systemImage: "link")
                }
                .help(model.localized(.relinkAssetHelp))
                .disabled(model.isProcessing)

                Picker(
                    model.localized(.roleLabel),
                    selection: Binding(
                        get: { model.selectedAsset?.role ?? .light },
                        set: { role in
                            if let selected = model.selectedAsset {
                                model.setRole(role, for: selected)
                            }
                        }
                    )
                ) {
                    ForEach(CalibrationFrameRole.allCases, id: \.self) { role in
                        Text(model.roleName(role))
                            .tag(role)
                    }
                }
                .pickerStyle(.menu)
                .disabled(model.canModifyProcessingConfiguration == false)

                if asset.kind == .raw {
                    RawPipelineSection(model: model, asset: asset)
                        .disabled(
                            model.isAssetMissing(asset) ||
                                model.canModifyProcessingConfiguration == false
                        )
                }
            } else {
                Text(model.localized(.noAssetSelected))
                    .foregroundStyle(.secondary)
            }
        }
    }
}

private struct RawPipelineSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    var asset: PhotonStackAsset

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Divider()

            Text(model.localized(.rawPipelineSection))
                .font(.subheadline.weight(.semibold))

            Picker(model.localized(.rawWhiteBalance), selection: rawWhiteBalanceBinding) {
                ForEach(RawWhiteBalanceMode.allCases) { mode in
                    Text(model.rawWhiteBalanceModeName(mode))
                        .tag(mode)
                }
            }
            .pickerStyle(.menu)

            if model.parameters.rawProcessingOptions.whiteBalanceMode == .manual {
                ParameterSlider(
                    title: model.localized(.rawTemperature),
                    value: rawTemperatureBinding,
                    range: 2000...50000
                )
                ParameterSlider(
                    title: model.localized(.rawTint),
                    value: rawTintBinding,
                    range: -150...150
                )
            }

            HStack(spacing: 6) {
                Picker(
                    model.localized(.fitsValueMode),
                    selection: Binding(
                        get: { model.exportOptions.fitsValueMode },
                        set: { model.setFITSValueMode($0) }
                    )
                ) {
                    ForEach(FITSValueMode.allCases) { mode in
                        Text(model.fitsValueModeName(mode))
                            .tag(mode)
                    }
                }
                .pickerStyle(.menu)

                Image(systemName: "info.circle")
                    .foregroundStyle(.secondary)
                    .help(model.localized(.fitsValueModeHelp))
            }

            ParameterSlider(
                title: model.localized(.rawExposureBias),
                value: rawExposureBiasBinding,
                range: -2.0...2.0
            )

            Picker(model.localized(.rawBlackLevel), selection: rawBlackLevelBinding) {
                ForEach(RawBlackLevelMode.allCases) { mode in
                    Text(model.rawBlackLevelModeName(mode))
                        .tag(mode)
                }
            }
            .pickerStyle(.segmented)

            if model.parameters.rawProcessingOptions.blackLevelMode == .manual {
                ParameterSlider(
                    title: model.localized(.rawManualBlackLevel),
                    value: rawManualBlackLevelBinding,
                    range: 0...1
                )
            }

            Picker(model.localized(.rawDemosaicQuality), selection: rawDemosaicBinding) {
                ForEach(RawDemosaicQuality.allCases) { quality in
                    Text(model.rawDemosaicQualityName(quality))
                        .tag(quality)
                }
            }
            .pickerStyle(.segmented)

            Toggle(model.localized(.rawLinearOutput), isOn: rawLinearOutputBinding)

            if model.isRawPreviewUpdating {
                ProgressView()
                    .controlSize(.small)
            }

            if let status = model.rawDecodeStatus {
                Label(
                    model.localized(
                        status.backend == .appleRaw ? .rawDecoderAppleActive : .rawDecoderImageIOFallback
                    ),
                    systemImage: status.usedFallback ? "exclamationmark.triangle.fill" : "checkmark.circle"
                )
                .font(.caption)
                .foregroundStyle(status.usedFallback ? .orange : .secondary)
                .help(
                    status.usedFallback
                        ? "\(status.fallbackErrorCode): \(status.fallbackMessage)"
                        : model.localized(.rawDecoderAppleActive)
                )
            }

            VStack(alignment: .leading, spacing: 4) {
                Text(model.localized(.rawAutoPreview))
                Text(model.localized(.rawAppliedParameters))
                Text(model.localized(.rawDeferredParameters))
            }
            .font(.caption)
            .foregroundStyle(.secondary)

            Text(model.localized(.rawProfessionalNote))
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private var rawWhiteBalanceBinding: Binding<RawWhiteBalanceMode> {
        Binding(
            get: { model.parameters.rawProcessingOptions.whiteBalanceMode },
            set: { model.setRawWhiteBalanceMode($0) }
        )
    }

    private var rawExposureBiasBinding: Binding<Double> {
        Binding(
            get: { model.parameters.rawProcessingOptions.exposureBias },
            set: { model.setRawExposureBias($0) }
        )
    }

    private var rawTemperatureBinding: Binding<Double> {
        Binding(
            get: { model.parameters.rawProcessingOptions.manualWhiteBalanceTemperature },
            set: { model.setRawManualWhiteBalanceTemperature($0) }
        )
    }

    private var rawTintBinding: Binding<Double> {
        Binding(
            get: { model.parameters.rawProcessingOptions.manualWhiteBalanceTint },
            set: { model.setRawManualWhiteBalanceTint($0) }
        )
    }

    private var rawBlackLevelBinding: Binding<RawBlackLevelMode> {
        Binding(
            get: { model.parameters.rawProcessingOptions.blackLevelMode },
            set: { model.setRawBlackLevelMode($0) }
        )
    }

    private var rawManualBlackLevelBinding: Binding<Double> {
        Binding(
            get: { model.parameters.rawProcessingOptions.manualBlackLevel },
            set: { model.setRawManualBlackLevel($0) }
        )
    }

    private var rawDemosaicBinding: Binding<RawDemosaicQuality> {
        Binding(
            get: { model.parameters.rawProcessingOptions.demosaicQuality },
            set: { model.setRawDemosaicQuality($0) }
        )
    }

    private var rawLinearOutputBinding: Binding<Bool> {
        Binding(
            get: { model.parameters.rawProcessingOptions.linearOutput },
            set: { model.setRawLinearOutput($0) }
        )
    }
}

private struct PhotoInfoSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.photoInfoSection))
                .font(.headline)

            if let asset = model.selectedAsset {
                VStack(alignment: .leading, spacing: 7) {
                    LabeledContent(model.localized(.nameLabel), value: asset.displayName)
                    LabeledContent(model.localized(.kindLabel), value: asset.kind.rawValue.uppercased())

                    if let metadata = asset.metadata {
                        if let dimensions = dimensions(metadata) {
                            LabeledContent(model.localized(.imageSizeLabel), value: dimensions)
                        }
                        if let format = metadata.formatDescription {
                            LabeledContent(model.localized(.formatLabel), value: format)
                        }
                        if let channels = metadata.channels {
                            LabeledContent(model.localized(.channelsLabel), value: "\(channels)")
                        }
                        if let bits = metadata.bitsPerChannel {
                            LabeledContent(model.localized(.bitDepthLabel), value: "\(bits)-bit")
                        }
                        if let camera = cameraName(metadata) {
                            LabeledContent(model.localized(.cameraLabel), value: camera)
                        }
                        if let lens = metadata.lensModel {
                            LabeledContent(model.localized(.lensLabel), value: lens)
                        }
                        if let captureDate = metadata.captureDate {
                            LabeledContent(model.localized(.captureDateLabel), value: captureDate)
                        }
                        if let whiteBalance = formattedWhiteBalance(metadata.whiteBalance) {
                            LabeledContent(model.localized(.rawWhiteBalance), value: whiteBalance)
                        }
                        if let exposure = metadata.exposureTimeSeconds {
                            LabeledContent(model.localized(.exposureLabel), value: formatExposure(exposure))
                        }
                        if let exposureBias = metadata.exposureBias {
                            LabeledContent(model.localized(.exposureBiasLabel), value: exposureBias)
                        }
                        if let fNumber = metadata.fNumber {
                            LabeledContent(model.localized(.fNumberLabel), value: "f/\(formatNumber(fNumber, fractionDigits: 1))")
                        }
                        if let iso = metadata.iso {
                            LabeledContent(model.localized(.isoLabel), value: "\(iso)")
                        }
                        if let focalLength = metadata.focalLengthMM {
                            LabeledContent(model.localized(.focalLengthLabel), value: "\(formatNumber(focalLength, fractionDigits: 1)) mm")
                        }
                        if let orientation = metadata.orientation {
                            LabeledContent(model.localized(.orientationLabel), value: "\(orientation)")
                        }
                        if let profile = metadata.colorProfile ?? metadata.colorModel {
                            LabeledContent(model.localized(.colorProfileLabel), value: profile)
                        }
                        if let decoder = metadata.rawDecoder {
                            LabeledContent(model.localized(.rawDecoderLabel), value: decoder)
                        }
                    } else {
                        Text(model.localized(.metadataUnavailable))
                            .foregroundStyle(.secondary)
                    }
                }
                .font(.caption)
            } else {
                Text(model.localized(.noAssetSelected))
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func dimensions(_ metadata: AssetMetadata) -> String? {
        guard let width = metadata.width, let height = metadata.height else {
            return nil
        }
        return "\(width) x \(height)"
    }

    private func cameraName(_ metadata: AssetMetadata) -> String? {
        let name = [metadata.cameraMake, metadata.cameraModel]
            .compactMap { value in
                let trimmed = value?.trimmingCharacters(in: .whitespacesAndNewlines)
                return trimmed?.isEmpty == false ? trimmed : nil
            }
            .joined(separator: " ")
        return name.isEmpty ? nil : name
    }

    private func formatExposure(_ seconds: Double) -> String {
        guard seconds.isFinite, seconds > 0 else {
            return "-"
        }
        if seconds < 1 {
            let denominator = (1.0 / seconds).rounded()
            if denominator.isFinite, denominator <= Double(Int.max) {
                return "1/\(Int(denominator)) s"
            }
        }
        return seconds < 0.01 ? String(format: "%.3g s", seconds) : "\(formatNumber(seconds, fractionDigits: 2)) s"
    }

    private func formattedWhiteBalance(_ value: String?) -> String? {
        guard let value else {
            return nil
        }
        switch value.lowercased() {
        case "auto":
            return model.rawWhiteBalanceModeName(.auto)
        case "manual":
            return model.rawWhiteBalanceModeName(.manual)
        default:
            return value
        }
    }

    private func formatNumber(_ value: Double, fractionDigits: Int) -> String {
        let formatter = NumberFormatter()
        formatter.minimumFractionDigits = 0
        formatter.maximumFractionDigits = fractionDigits
        return formatter.string(from: NSNumber(value: value)) ?? "\(value)"
    }
}

private struct LanguageSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        Picker(model.localized(.languageLabel), selection: $model.language) {
            ForEach(AppLanguage.allCases, id: \.self) { language in
                Text(language.displayName)
                    .tag(language)
            }
        }
        .pickerStyle(.segmented)
        .onChange(of: model.language) { _, _ in
            model.persistUserSettings()
        }
    }
}

private struct NewProjectSheet: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var name: String
    @Binding var template: ProjectTemplate
    let onCancel: () -> Void
    let onCreate: () -> Void
    let onCreateAndImport: () -> Void

    private var canCreate: Bool {
        name.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty == false
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            Text(model.localized(.newProject))
                .font(.title3.weight(.semibold))

            VStack(alignment: .leading, spacing: 10) {
                TextField(model.localized(.projectName), text: $name)
                    .textFieldStyle(.roundedBorder)
                    .onSubmit {
                        if canCreate {
                            onCreate()
                        }
                    }

                Picker(model.localized(.templateLabel), selection: $template) {
                    ForEach(ProjectTemplate.allCases, id: \.self) { projectTemplate in
                        Text(model.projectTemplateName(projectTemplate))
                            .tag(projectTemplate)
                    }
                }
                .pickerStyle(.menu)
            }

            HStack(spacing: 10) {
                Button(model.localized(.cancelAction), action: onCancel)

                Spacer()

                Button(model.localized(.createProject), action: onCreate)
                    .disabled(canCreate == false)

                Button(model.localized(.createAndImport), action: onCreateAndImport)
                    .buttonStyle(.borderedProminent)
                    .disabled(canCreate == false)
            }
        }
        .padding(22)
        .frame(width: 420)
    }
}

private struct ProjectSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.projectSection))
                .font(.headline)

            Picker(model.localized(.templateLabel), selection: $model.selectedTemplate) {
                ForEach(ProjectTemplate.allCases, id: \.self) { template in
                    Text(model.projectTemplateName(template))
                        .tag(template)
                }
            }
            .pickerStyle(.menu)
            .onChange(of: model.selectedTemplate) { _, _ in
                model.persistUserSettings()
            }

            Toggle(
                model.localized(.autosaveLabel),
                isOn: Binding(
                    get: { model.autosaveEnabled },
                    set: { model.setAutosaveEnabled($0) }
                )
            )

            if model.hasUnsavedChanges {
                Label(model.localized(.unsavedChangesStatus), systemImage: "circle.fill")
                    .font(.caption)
                    .foregroundStyle(.orange)
            }

            VStack(alignment: .leading, spacing: 6) {
                Text(model.localized(.recentProjects))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                if model.recentProjects.isEmpty {
                    Text(model.localized(.noRecentProjects))
                        .font(.caption)
                        .foregroundStyle(.secondary)
                } else {
                    ForEach(model.recentProjects.prefix(5)) { recentProject in
                        Button {
                            model.openRecentProject(recentProject)
                        } label: {
                            VStack(alignment: .leading, spacing: 2) {
                                Text(recentProject.name)
                                    .lineLimit(1)
                                Text(recentProject.directory.path)
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                            }
                            .frame(maxWidth: .infinity, alignment: .leading)
                        }
                        .buttonStyle(.plain)
                        .disabled(model.isProcessing)
                    }
                }
            }
        }
    }
}

private struct WorkflowSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var parameters: ProcessingParameters

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(model.localized(.workflowSection))
                .font(.headline)

            HStack {
                FrameCountBadge(title: model.roleName(.light), count: model.lightAssets.count)
                FrameCountBadge(title: model.roleName(.dark), count: model.darkAssets.count)
            }
            HStack {
                FrameCountBadge(title: model.roleName(.bias), count: model.biasAssets.count)
                FrameCountBadge(title: model.roleName(.flat), count: model.flatAssets.count)
            }

            Picker(model.localized(.masterLabel), selection: $parameters.workflowMasterMethod) {
                Text(model.stackMethodName(.median)).tag(StackMethod.median)
                Text(model.stackMethodName(.average)).tag(StackMethod.average)
            }
            .pickerStyle(.menu)

            if model.darkAssets.isEmpty == false && model.biasAssets.isEmpty == false {
                Picker(model.localized(.darkBiasStateLabel), selection: $parameters.workflowDarkBiasState) {
                    ForEach(CalibrationBiasState.allCases) { state in
                        Text(model.calibrationBiasStateName(state)).tag(state)
                    }
                }
                .pickerStyle(.menu)
                .help(model.localized(.calibrationBiasStateHelp))
            }

            if model.flatAssets.isEmpty == false {
                Picker(model.localized(.flatBiasStateLabel), selection: $parameters.workflowFlatBiasState) {
                    ForEach(CalibrationBiasState.allCases) { state in
                        Text(model.calibrationBiasStateName(state)).tag(state)
                    }
                }
                .pickerStyle(.menu)
                .help(model.localized(.calibrationBiasStateHelp))
            }

            if let warning = model.calibrationConfigurationWarning {
                Label(warning, systemImage: "exclamationmark.triangle.fill")
                    .font(.caption)
                    .foregroundStyle(.orange)
            }

            Picker(model.localized(.stackLabel), selection: $parameters.workflowStackMethod) {
                ForEach(StackMethod.allCases, id: \.self) { method in
                    Text(model.stackMethodName(method)).tag(method)
                }
            }
            .pickerStyle(.menu)

            Picker(model.localized(.alignLabel), selection: $parameters.workflowAlignment) {
                ForEach(AlignmentMethod.allCases, id: \.self) { method in
                    Text(model.alignmentMethodName(method)).tag(method)
                }
            }
            .pickerStyle(.menu)

            Toggle(model.localized(.restoreMeteorsAfterStack), isOn: $parameters.workflowRestoreMeteors)
                .toggleStyle(.checkbox)

            Button {
                model.startRunStackWorkflow()
            } label: {
                Label(model.localized(.runStackWorkflow), systemImage: "play.circle")
                    .frame(maxWidth: .infinity)
            }
            .buttonStyle(.borderedProminent)
            .disabled(model.canRunStackWorkflow == false || model.isProcessing)
        }
        .disabled(model.canModifyProcessingConfiguration == false)
    }
}

private struct RegistrationSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var referenceID: PhotonStackAsset.ID?
    @State private var movingID: PhotonStackAsset.ID?
    @State private var alignment: AlignmentMethod = .distortion

    var body: some View {
        let assets = model.batchRegistrationAssets.isEmpty ? model.project.assets : model.batchRegistrationAssets
        let reference = selectedAsset(referenceID, in: assets) ?? assets.first
        let moving = selectedAsset(movingID, in: assets) ?? defaultMovingAsset(in: assets, reference: reference)
        let batchMovingCandidates = model.batchRegistrationAssets.filter { $0.id != reference?.id }
        let selectedBatchMovingCount = model.batchRegistrationSelectedMovingCount(reference: reference)

        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.registrationSection))
                .font(.headline)

            if assets.count < 2 {
                Text(model.localized(.registrationNeedsTwoFrames))
                    .foregroundStyle(.secondary)
            } else {
                Picker(model.localized(.referenceFrame), selection: referenceBinding(assets: assets)) {
                    ForEach(assets) { asset in
                        Text(asset.displayName)
                            .tag(Optional(asset.id))
                    }
                }
                .pickerStyle(.menu)

                Picker(model.localized(.movingFrame), selection: movingBinding(assets: assets)) {
                    ForEach(assets) { asset in
                        Text(asset.displayName)
                            .tag(Optional(asset.id))
                    }
                }
                .pickerStyle(.menu)

                Picker(model.localized(.alignLabel), selection: $alignment) {
                    Text(model.alignmentMethodName(.translation)).tag(AlignmentMethod.translation)
                    Text(model.alignmentMethodName(.similarity)).tag(AlignmentMethod.similarity)
                    Text(model.alignmentMethodName(.affine)).tag(AlignmentMethod.affine)
                    Text(model.alignmentMethodName(.distortion)).tag(AlignmentMethod.distortion)
                }
                .pickerStyle(.segmented)

                Button {
                    if let reference, let moving {
                        model.startRegister(reference: reference, moving: moving, alignment: alignment)
                    }
                } label: {
                    Label(model.localized(.registerFrames), systemImage: "scope")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canRegister(reference: reference, moving: moving) == false || model.isProcessing)

                Button {
                    if let reference {
                        model.startRegisterBatch(reference: reference, alignment: alignment)
                    }
                } label: {
                    Label(model.localized(.registerBatchFrames), systemImage: "rectangle.stack")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canRegisterBatch(reference: reference) == false || model.isProcessing)

                BatchRegistrationFrameSelection(
                    model: model,
                    reference: reference,
                    movingCandidates: batchMovingCandidates,
                    selectedCount: selectedBatchMovingCount
                )

                if let report = model.registrationReport {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(model.localized(.registrationReport))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        LabeledContent(model.localized(.registrationMatches), value: "\(report.matches)")
                        LabeledContent(model.localized(.registrationOffset), value: String(format: "%.2f, %.2f", report.dx, report.dy))
                        LabeledContent(model.localized(.registrationScale), value: String(format: "%.4f", report.scale))
                        LabeledContent(model.localized(.registrationRotation), value: String(format: "%.3f°", report.rotationDegrees))
                    }
                    .font(.caption)
                }
            }
        }
        .disabled(model.canModifyProcessingConfiguration == false)
        .onAppear {
            initializeSelection(from: assets)
        }
        .onChange(of: assets.map(\.id)) { _, _ in
            initializeSelection(from: assets)
        }
    }

    private func referenceBinding(assets: [PhotonStackAsset]) -> Binding<PhotonStackAsset.ID?> {
        Binding(
            get: { referenceID ?? assets.first?.id },
            set: { newValue in
                referenceID = newValue
                if movingID == newValue {
                    movingID = assets.first { $0.id != newValue }?.id
                }
            }
        )
    }

    private func movingBinding(assets: [PhotonStackAsset]) -> Binding<PhotonStackAsset.ID?> {
        Binding(
            get: { movingID ?? assets.dropFirst().first?.id ?? assets.first?.id },
            set: { newValue in
                movingID = newValue
                if referenceID == newValue {
                    referenceID = assets.first { $0.id != newValue }?.id
                }
            }
        )
    }

    private func selectedAsset(_ id: PhotonStackAsset.ID?, in assets: [PhotonStackAsset]) -> PhotonStackAsset? {
        guard let id else {
            return nil
        }
        return assets.first { $0.id == id }
    }

    private func defaultMovingAsset(in assets: [PhotonStackAsset], reference: PhotonStackAsset?) -> PhotonStackAsset? {
        assets.first { $0.id != reference?.id }
    }

    private func initializeSelection(from assets: [PhotonStackAsset]) {
        guard assets.isEmpty == false else {
            referenceID = nil
            movingID = nil
            return
        }
        if referenceID == nil || selectedAsset(referenceID, in: assets) == nil {
            referenceID = assets.first?.id
        }
        if movingID == nil || selectedAsset(movingID, in: assets) == nil || movingID == referenceID {
            movingID = assets.first { $0.id != referenceID }?.id
        }
    }
}

private struct BatchRegistrationFrameSelection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let reference: PhotonStackAsset?
    let movingCandidates: [PhotonStackAsset]
    let selectedCount: Int

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(model.localized(.batchRegistrationFrames))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Spacer()

                Text("\(selectedCount)/\(movingCandidates.count)")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }

            HStack(spacing: 8) {
                Button(model.localized(.selectAllFrames)) {
                    model.selectAllBatchRegistrationFrames()
                }
                .disabled(movingCandidates.isEmpty || model.isProcessing)

                Button(model.localized(.clearFrameSelection)) {
                    model.clearBatchRegistrationFrames()
                }
                .disabled(movingCandidates.isEmpty || model.isProcessing)

                if let selectedAsset = model.selectedAsset,
                   selectedAsset.id != reference?.id,
                   movingCandidates.contains(where: { $0.id == selectedAsset.id }) {
                    Button(model.localized(.onlyCurrentFrame)) {
                        model.selectOnlyBatchRegistrationFrame(selectedAsset)
                    }
                    .disabled(model.isProcessing)
                }
            }
            .buttonStyle(.bordered)
            .font(.caption)

            if movingCandidates.isEmpty {
                Text(model.localized(.registrationNeedsTwoFrames))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 6) {
                        ForEach(movingCandidates) { asset in
                            Toggle(
                                isOn: Binding(
                                    get: { model.isBatchRegistrationFrameSelected(asset) },
                                    set: { model.setBatchRegistrationFrame(asset, isSelected: $0) }
                                )
                            ) {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(asset.displayName)
                                        .lineLimit(1)
                                    Text(asset.originalURL.deletingLastPathComponent().path)
                                        .font(.caption2)
                                        .foregroundStyle(.secondary)
                                        .lineLimit(1)
                                        .truncationMode(.middle)
                                }
                            }
                            .toggleStyle(.checkbox)
                            .disabled(model.isProcessing)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 128)

                if selectedCount == 0 {
                    Text(model.localized(.registrationNoFramesSelected))
                        .font(.caption2)
                        .foregroundStyle(.red)
                }
            }
        }
    }
}

private struct FrameCountBadge: View {
    let title: String
    let count: Int

    var body: some View {
        HStack {
            Text(title)
            Spacer()
            Text("\(count)")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)
        }
        .font(.caption)
        .padding(.horizontal, 8)
        .padding(.vertical, 6)
        .background(.quaternary, in: RoundedRectangle(cornerRadius: 6))
    }
}

private struct AdjustmentsInspectorSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var parameters: ProcessingParameters
    @State private var selectedTab: AdjustmentInspectorTab = .curves

    private let columns = [
        GridItem(.flexible(), spacing: 6),
        GridItem(.flexible(), spacing: 6)
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            LazyVGrid(columns: columns, spacing: 6) {
                ForEach(AdjustmentInspectorTab.allCases) { tab in
                    Button {
                        selectedTab = tab
                    } label: {
                        Label(model.localized(tab.titleKey), systemImage: tab.iconName)
                            .font(.caption)
                            .lineLimit(1)
                            .frame(maxWidth: .infinity, minHeight: 28)
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(selectedTab == tab ? Color.accentColor : Color.primary)
                    .background(selectedTab == tab ? Color.accentColor.opacity(0.14) : Color.clear, in: RoundedRectangle(cornerRadius: 6))
                    .overlay {
                        RoundedRectangle(cornerRadius: 6)
                            .stroke(selectedTab == tab ? Color.accentColor.opacity(0.28) : Color.secondary.opacity(0.16), lineWidth: 1)
                    }
                }
            }

            Divider()

            selectedContent
        }
        .disabled(model.canModifyProcessingConfiguration == false)
    }

    @ViewBuilder
    private var selectedContent: some View {
        switch selectedTab {
        case .curves:
            HistogramCurvesSection(model: model, parameters: $parameters)
        case .parameters:
            ParametersSection(parameters: $parameters, model: model)
        case .export:
            ExportSettingsSection(model: model)
        case .advanced:
            AdvancedProcessingSection(model: model, parameters: $parameters)
        }
    }
}

private enum AdjustmentInspectorTab: String, CaseIterable, Identifiable {
    case curves
    case parameters
    case export
    case advanced

    var id: String {
        rawValue
    }

    var titleKey: LocalizedTextKey {
        switch self {
        case .curves:
            return .histogramCurvesSection
        case .parameters:
            return .parametersSection
        case .export:
            return .exportSettingsSection
        case .advanced:
            return .advancedProcessingSection
        }
    }

    var iconName: String {
        switch self {
        case .curves:
            return "point.topleft.down.curvedto.point.bottomright.up"
        case .parameters:
            return "slider.horizontal.3"
        case .export:
            return "square.and.arrow.up"
        case .advanced:
            return "wand.and.stars"
        }
    }
}

private struct AdvancedProcessingSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var parameters: ProcessingParameters
    @State private var backgroundModel = "grid"
    @State private var backgroundMode = "subtract"
    @State private var backgroundStrength = 0.35
    @State private var preserveBackgroundBrightness = true
    @State private var protectBrightBackgroundTargets = true
    @State private var selectedCloudIndices: Set<Int> = []
    @State private var targetBackground = 0.25
    @State private var targetScale = 1.0
    @State private var neutralizeStrength = 1.0
    @State private var saturationAmount = 0.2
    @State private var starDetectionThreshold = 3.0
    @State private var starMinimumPeak = 0.05
    @State private var starMaskRadius = 2
    @State private var largeStarMaskRadius = 6
    @State private var layeredStarMask = true
    @State private var deconvolutionIterations = 8
    @State private var deconvolutionRadius = 2
    @State private var deconvolutionSigma = 1.2
    @State private var drizzleScale = 2
    @State private var drizzlePixfrac = 1.0
    @State private var drizzleAlignment: AlignmentMethod = .distortion
    @State private var selectedArtifactIndices: Set<Int> = []
    @State private var removeSelectedMeteors = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(model.localized(.advancedProcessingSection))
                .font(.headline)

            VStack(alignment: .leading, spacing: 8) {
                Picker(model.localized(.backgroundModel), selection: $backgroundModel) {
                    Text(model.localized(.globalModel)).tag("global")
                    Text(model.localized(.gridModel)).tag("grid")
                }
                .pickerStyle(.segmented)

                Picker(model.localized(.backgroundMode), selection: $backgroundMode) {
                    Text(model.localized(.subtractMode)).tag("subtract")
                    Text(model.localized(.divideMode)).tag("divide")
                }
                .pickerStyle(.segmented)

                ParameterSlider(title: model.localized(.backgroundStrength), value: $backgroundStrength, range: 0.0...1.0)

                if backgroundMode == "divide" {
                    Toggle(
                        model.localized(.preserveBackgroundBrightness),
                        isOn: $preserveBackgroundBrightness
                    )
                    .toggleStyle(.checkbox)
                    .help(model.localized(.preserveBackgroundBrightnessHelp))
                }

                if backgroundModel == "grid" {
                    Toggle(
                        model.localized(.protectBrightBackgroundTargets),
                        isOn: $protectBrightBackgroundTargets
                    )
                    .toggleStyle(.checkbox)
                    .help(model.localized(.protectBrightBackgroundTargetsHelp))
                }

                Button {
                    model.startBackgroundPreview(
                        model: backgroundModel,
                        mode: backgroundMode,
                        strength: backgroundStrength,
                        preserveBrightness: preserveBackgroundBrightness,
                        protectBrightTargets: protectBrightBackgroundTargets
                    )
                } label: {
                    Label(model.localized(.applyBackground), systemImage: "circle.lefthalf.filled")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

            }

            VStack(alignment: .leading, spacing: 8) {
                Text(model.localized(.cloudsSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                ParameterSlider(title: model.localized(.cloudRemovalStrength), value: $parameters.cloudRemovalStrength, range: 0.0...1.0)

                Button {
                    model.startDetectClouds()
                } label: {
                    Label(model.localized(.detectClouds), systemImage: "cloud")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

                Button {
                    model.startDetectCloudsTemporally()
                } label: {
                    Label("多帧辅助识别", systemImage: "rectangle.3.group.bubble")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.temporalCloudReferenceCount < 2 || model.canProcessCurrentInput == false || model.isProcessing)

                if model.temporalCloudReferenceCount < 2 {
                    Text("至少载入 3 张同尺寸的光照/参考帧后可启用弱云辅助识别。")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }

                if let report = model.cloudRegionReport {
                    Text("\(model.localized(.cloudSummary)): \(report.clouds)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    if report.temporalFramesUsed > 0 {
                        Text("多帧：\(report.temporalFramesUsed) 帧，确认 \(report.temporallyConfirmedClouds) 个暗云候选")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }

                    if report.topItems.isEmpty == false {
                        VStack(alignment: .leading, spacing: 6) {
                            Text(model.localized(.topCloudRegions))
                                .font(.caption)
                                .foregroundStyle(.secondary)

                            HStack(spacing: 8) {
                                Button {
                                    selectVisibleClouds(report)
                                } label: {
                                    Label(model.localized(.selectAllClouds), systemImage: "checklist.checked")
                                        .frame(maxWidth: .infinity)
                                }

                                Button {
                                    clearCloudSelection()
                                } label: {
                                    Label(model.localized(.clearCloudSelection), systemImage: "xmark.circle")
                                        .frame(maxWidth: .infinity)
                                }
                            }
                            .buttonStyle(.bordered)
                            .controlSize(.small)

                            Text(model.localized(.selectedCloudPreviewHint))
                                .font(.caption2)
                                .foregroundStyle(.secondary)

                            ForEach(report.topItems) { item in
                                Toggle(isOn: cloudSelectionBinding(for: item.index)) {
                                    HStack {
                                        Image(systemName: "cloud")
                                            .foregroundStyle(.cyan)
                                        VStack(alignment: .leading, spacing: 2) {
                                            Text(item.displayLabel)
                                                .font(.caption)
                                            Text(item.temporalSupport > 0
                                                ? "A \(item.coverage * 100, format: .number.precision(.fractionLength(1)))%  C \(item.confidence, format: .number.precision(.fractionLength(2)))  T \(item.temporalSupport)"
                                                : "A \(item.coverage * 100, format: .number.precision(.fractionLength(1)))%  C \(item.confidence, format: .number.precision(.fractionLength(2)))")
                                                .font(.caption2)
                                                .foregroundStyle(.secondary)
                                        }
                                        Spacer()
                                    }
                                }
                                .toggleStyle(.checkbox)
                            }

                            if report.items.count > report.topItems.count {
                                Text(model.localized(.cloudTop20Hint))
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }

                    Button {
                        model.startRemoveClouds(
                            selectedIndices: Array(selectedCloudIndices).sorted(),
                            strength: model.parameters.cloudRemovalStrength
                        )
                    } label: {
                        Label(model.localized(.removeSelectedClouds), systemImage: "cloud.sun")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(selectedCloudIndices.isEmpty || model.canProcessCurrentInput == false || model.isProcessing)
                    if report.temporalFramesUsed > 0 {
                        Text("去除会复用这次多帧确认的掩膜；需要至少保留 3 张同组帧。")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                }
            }

            VStack(alignment: .leading, spacing: 8) {
                ParameterSlider(title: model.localized(.targetBackground), value: $targetBackground, range: 0.02...0.6)
                ParameterSlider(title: model.localized(.targetScale), value: $targetScale, range: 0.5...2.0)

                Button {
                    model.startNormalizePreview(targetBackground: targetBackground, targetScale: targetScale)
                } label: {
                    Label(model.localized(.normalizeAction), systemImage: "equal.circle")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)
            }

            VStack(alignment: .leading, spacing: 8) {
                Text(model.localized(.colorSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                ParameterSlider(title: model.localized(.neutralizeStrength), value: $neutralizeStrength, range: 0.0...1.0)
                ParameterSlider(title: model.localized(.saturationAmount), value: $saturationAmount, range: -0.5...1.5)

                HStack {
                    Button {
                        model.startColorNeutralizePreview(strength: neutralizeStrength)
                    } label: {
                        Label(model.localized(.neutralizeAction), systemImage: "eyedropper.halffull")
                            .frame(maxWidth: .infinity)
                    }

                    Button {
                        model.startColorSaturatePreview(amount: saturationAmount)
                    } label: {
                        Label(model.localized(.saturationAction), systemImage: "paintpalette")
                            .frame(maxWidth: .infinity)
                    }
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)
            }

            VStack(alignment: .leading, spacing: 8) {
                Text(model.localized(.artifactTrailsSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                HStack {
                    Button {
                        model.startDetectArtifactTrails()
                    } label: {
                        Label(model.localized(.detectArtifactTrails), systemImage: "scope")
                            .frame(maxWidth: .infinity)
                    }

                    Button {
                        model.startRemoveArtifactTrails(selectedIndices: nil, removeMeteors: false)
                    } label: {
                        Label(model.localized(.removeArtificialTrails), systemImage: "wand.and.stars")
                            .frame(maxWidth: .infinity)
                    }
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

                Button {
                    model.startCleanTimelapseArtifactSequence()
                } label: {
                    Label(model.localized(.cleanTimelapseTrails), systemImage: "film.stack")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canCleanTimelapseArtifactSequence == false || model.isProcessing)

                HStack {
                    Button {
                        model.startExtractMeteorLayer()
                    } label: {
                        Label(model.localized(.extractMeteorLayer), systemImage: "sparkles")
                            .frame(maxWidth: .infinity)
                    }

                    Button {
                        model.startRestoreMeteorsFromSelectedAsset()
                    } label: {
                        Label(model.localized(.restoreMeteorsAction), systemImage: "rectangle.stack.badge.plus")
                            .frame(maxWidth: .infinity)
                    }
                }
                .buttonStyle(.bordered)
                .disabled(model.canRestoreMeteorsFromSelectedAsset == false || model.isProcessing)

                if let report = model.artifactTrailReport {
                    Text("\(model.localized(.artifactTrailSummary)): \(report.trails)   \(model.localized(.protectedMeteors)): \(report.protectedMeteors)")
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    if report.topItems.isEmpty == false {
                        VStack(alignment: .leading, spacing: 6) {
                            Text(model.localized(.topArtifactTrails))
                                .font(.caption)
                                .foregroundStyle(.secondary)

                            HStack(spacing: 8) {
                                Button {
                                    selectVisibleArtificialArtifacts(report)
                                } label: {
                                    Label(model.localized(.selectAllArtificialTrails), systemImage: "checklist.checked")
                                        .frame(maxWidth: .infinity)
                                }

                                Button {
                                    clearArtifactSelection()
                                } label: {
                                    Label(model.localized(.clearArtifactSelection), systemImage: "xmark.circle")
                                        .frame(maxWidth: .infinity)
                                }
                            }
                            .buttonStyle(.bordered)
                            .controlSize(.small)

                            Text(model.localized(.selectedArtifactPreviewHint))
                                .font(.caption2)
                                .foregroundStyle(.secondary)

                            ForEach(report.topItems) { item in
                                Toggle(isOn: artifactSelectionBinding(for: item.index)) {
                                    HStack {
                                        Image(systemName: artifactIcon(for: item))
                                            .foregroundStyle(artifactColor(for: item))
                                        VStack(alignment: .leading, spacing: 2) {
                                            Text(artifactKindName(item.kind))
                                                .font(.caption)
                                            Text("\(model.localized(.artifactTrailWeight)) \(item.weight, format: .number.precision(.fractionLength(2)))  L \(item.length, format: .number.precision(.fractionLength(0)))  B \(item.meanBrightness, format: .number.precision(.fractionLength(2)))  C \(item.confidence, format: .number.precision(.fractionLength(2)))")
                                                .font(.caption2)
                                                .foregroundStyle(.secondary)
                                        }
                                        Spacer()
                                        Text(item.displayLabel)
                                            .font(.caption2.monospaced())
                                            .foregroundStyle(.secondary)
                                    }
                                }
                                .toggleStyle(.checkbox)
                            }

                            if report.items.count > report.topItems.count {
                                Text(model.localized(.artifactTrailTop20Hint))
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }

                    Toggle(model.localized(.allowMeteorRemoval), isOn: $removeSelectedMeteors)
                        .toggleStyle(.checkbox)

                    Button {
                        model.startRemoveArtifactTrails(
                            selectedIndices: Array(selectedArtifactIndices).sorted(),
                            removeMeteors: removeSelectedMeteors
                        )
                    } label: {
                        Label(model.localized(.removeSelectedTrails), systemImage: "checklist.checked")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(selectedArtifactIndices.isEmpty || model.canProcessCurrentInput == false || model.isProcessing)
                }
            }

            VStack(alignment: .leading, spacing: 8) {
                Text(model.localized(.starToolsSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                ParameterSlider(
                    title: model.localized(.starDetectionThreshold),
                    value: $starDetectionThreshold,
                    range: 1.0...8.0
                )
                ParameterSlider(
                    title: model.localized(.starMinimumPeak),
                    value: $starMinimumPeak,
                    range: 0.0...0.5
                )
                Stepper(
                    "\(model.localized(.starMaskRadius)) \(starMaskRadius)",
                    value: $starMaskRadius,
                    in: 1...16
                )
                Stepper(
                    "\(model.localized(.largeStarMaskRadius)) \(largeStarMaskRadius)",
                    value: $largeStarMaskRadius,
                    in: max(starMaskRadius, 2)...32
                )
                Toggle(model.localized(.layeredStarMask), isOn: $layeredStarMask)

                HStack {
                    Button {
                        model.startDetectStars(
                            sigmaThreshold: starDetectionThreshold,
                            minPeak: starMinimumPeak
                        )
                    } label: {
                        Label(model.localized(.detectStarsAction), systemImage: "sparkle.magnifyingglass")
                            .frame(maxWidth: .infinity)
                    }

                    Button {
                        model.startCreateStarMask(
                            radius: starMaskRadius,
                            largeRadius: largeStarMaskRadius,
                            layered: layeredStarMask,
                            sigmaThreshold: starDetectionThreshold,
                            minPeak: starMinimumPeak
                        )
                    } label: {
                        Label(model.localized(.createStarMaskAction), systemImage: "circle.grid.cross")
                            .frame(maxWidth: .infinity)
                    }
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

                if let count = model.detectedStarCount {
                    Text("\(model.localized(.detectedStarsLabel)): \(count)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            }

            VStack(alignment: .leading, spacing: 8) {
                Text(model.localized(.deconvolutionSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                Stepper(
                    "\(model.localized(.deconvolutionIterations)) \(deconvolutionIterations)",
                    value: $deconvolutionIterations,
                    in: 1...32
                )
                Stepper(
                    "\(model.localized(.deconvolutionRadius)) \(deconvolutionRadius)",
                    value: $deconvolutionRadius,
                    in: 1...8
                )
                ParameterSlider(title: model.localized(.deconvolutionSigma), value: $deconvolutionSigma, range: 0.5...3.5)

                Button {
                    model.startDeconvolvePreview(
                        iterations: deconvolutionIterations,
                        radius: deconvolutionRadius,
                        sigma: deconvolutionSigma
                    )
                } label: {
                    Label(model.localized(.deconvolveAction), systemImage: "scope")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)
            }

            VStack(alignment: .leading, spacing: 8) {
                Text(model.localized(.drizzleSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                Stepper("\(model.localized(.drizzleScale)) \(drizzleScale)x", value: $drizzleScale, in: 1...4)
                ParameterSlider(
                    title: model.localized(.drizzlePixfrac),
                    value: $drizzlePixfrac,
                    range: 0.1...1.0
                )
                Picker(model.localized(.drizzleAlign), selection: $drizzleAlignment) {
                    ForEach(AlignmentMethod.allCases, id: \.self) { method in
                        Text(model.alignmentMethodName(method)).tag(method)
                    }
                }
                .pickerStyle(.menu)

                Button {
                    model.startRunDrizzle(
                        scale: drizzleScale,
                        pixfrac: drizzlePixfrac,
                        alignment: drizzleAlignment
                    )
                } label: {
                    Label(model.localized(.drizzleAction), systemImage: "rectangle.grid.2x2")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .disabled(model.canRunDrizzle == false || model.isProcessing)
            }
        }
        .onChange(of: model.artifactTrailReport) { _, report in
            selectedArtifactIndices = Set(
                report?.topItems
                    .filter { $0.kind != "meteor" }
                    .prefix(8)
                    .map(\.index) ?? []
            )
            removeSelectedMeteors = false
            model.refreshArtifactMarkedPreview(selectedIndices: selectedArtifactIndices)
        }
        .onChange(of: model.cloudRegionReport) { _, report in
            selectedCloudIndices.removeAll()
            model.refreshCloudMarkedPreview(selectedIndices: selectedCloudIndices)
        }
        .onChange(of: starMaskRadius) { _, radius in
            largeStarMaskRadius = max(largeStarMaskRadius, radius)
        }
    }

    private func cloudSelectionBinding(for index: Int) -> Binding<Bool> {
        Binding {
            selectedCloudIndices.contains(index)
        } set: { isSelected in
            if isSelected {
                selectedCloudIndices.insert(index)
            } else {
                selectedCloudIndices.remove(index)
            }
            model.refreshCloudMarkedPreview(selectedIndices: selectedCloudIndices)
        }
    }

    private func selectVisibleClouds(_ report: CloudRegionReport) {
        selectedCloudIndices = Set(report.topItems.map(\.index))
        model.refreshCloudMarkedPreview(selectedIndices: selectedCloudIndices)
    }

    private func clearCloudSelection() {
        selectedCloudIndices.removeAll()
        model.refreshCloudMarkedPreview(selectedIndices: selectedCloudIndices)
    }

    private func artifactSelectionBinding(for index: Int) -> Binding<Bool> {
        Binding {
            selectedArtifactIndices.contains(index)
        } set: { isSelected in
            if isSelected {
                selectedArtifactIndices.insert(index)
            } else {
                selectedArtifactIndices.remove(index)
            }
            model.refreshArtifactMarkedPreview(selectedIndices: selectedArtifactIndices)
        }
    }

    private func selectVisibleArtificialArtifacts(_ report: ArtifactTrailReport) {
        selectedArtifactIndices = Set(
            report.topItems
                .filter { $0.kind != "meteor" }
                .map(\.index)
        )
        model.refreshArtifactMarkedPreview(selectedIndices: selectedArtifactIndices)
    }

    private func clearArtifactSelection() {
        selectedArtifactIndices.removeAll()
        model.refreshArtifactMarkedPreview(selectedIndices: selectedArtifactIndices)
    }

    private func artifactKindName(_ kind: String) -> String {
        switch kind {
        case "airplane":
            return model.localized(.airplaneTrail)
        case "drone":
            return model.localized(.droneTrail)
        case "satellite":
            return model.localized(.satelliteTrail)
        case "meteor":
            return model.localized(.meteorTrail)
        default:
            return kind
        }
    }

    private func artifactIcon(for item: ArtifactTrailItem) -> String {
        switch item.kind {
        case "airplane":
            return "airplane"
        case "drone":
            return "dot.radiowaves.left.and.right"
        case "satellite":
            return "dot.circle"
        case "meteor":
            return "sparkles"
        default:
            return "line.diagonal"
        }
    }

    private func artifactColor(for item: ArtifactTrailItem) -> Color {
        switch item.kind {
        case "meteor":
            return .green
        case "drone":
            return .orange
        case "satellite":
            return .cyan
        default:
            return .blue
        }
    }
}

private struct ParametersSection: View {
    @Binding var parameters: ProcessingParameters
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(model.localized(.parametersSection))
                .font(.headline)

            Stepper("\(model.localized(.previewWidth)) \(parameters.previewWidth) px", value: $parameters.previewWidth, in: 512...4096, step: 256)

            ParameterSlider(
                title: model.localized(.stretchParameter),
                value: $parameters.stretchTargetBackground,
                range: 0.05...0.65
            )

            ParameterSlider(
                title: model.localized(.starReduction),
                value: $parameters.starReductionAmount,
                range: 0.05...0.9
            )

            Toggle(model.localized(.profileAwareStars), isOn: $parameters.profileAwareStarReduction)

            Toggle(model.localized(.edgeAwareStars), isOn: $parameters.edgeAwareStarReduction)

            ParameterSlider(
                title: model.localized(.comaReduction),
                value: $parameters.comaReductionAmount,
                range: 0.0...1.0
            )

            Stepper("\(model.localized(.comaRadius)) \(parameters.comaReductionRadius)", value: $parameters.comaReductionRadius, in: 2...16)

            ParameterSlider(
                title: model.localized(.comaEccentricity),
                value: $parameters.comaReductionEccentricity,
                range: 0.1...0.9
            )

            Toggle(model.localized(.edgeAwareComa), isOn: $parameters.edgeAwareComaReduction)

            ParameterSlider(
                title: model.localized(.localContrast),
                value: $parameters.localContrastAmount,
                range: 0.0...1.0
            )

            Stepper("\(model.localized(.contrastRadius)) \(parameters.localContrastRadius)", value: $parameters.localContrastRadius, in: 1...64)

            ParameterSlider(
                title: model.localized(.denoiseParameter),
                value: $parameters.denoiseAmount,
                range: 0.0...1.0
            )

            ParameterSlider(
                title: model.localized(.chromaDenoiseParameter),
                value: $parameters.denoiseChromaAmount,
                range: 0.0...1.0
            )

            Stepper("\(model.localized(.denoiseRadius)) \(parameters.denoiseRadius)", value: $parameters.denoiseRadius, in: 1...8)

            ParameterSlider(
                title: model.localized(.sharpenParameter),
                value: $parameters.sharpenAmount,
                range: 0.0...1.0
            )

            Stepper("\(model.localized(.sharpenRadius)) \(parameters.sharpenRadius)", value: $parameters.sharpenRadius, in: 1...8)
        }
    }
}

private struct ExportSettingsSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.exportSettingsSection))
                .font(.headline)

            Picker(
                model.localized(.exportBitDepth),
                selection: Binding(
                    get: { model.exportOptions.bitDepth },
                    set: { model.setExportBitDepth($0) }
                )
            ) {
                ForEach(ExportBitDepth.allCases) { bitDepth in
                    Text(model.exportBitDepthName(bitDepth))
                        .tag(bitDepth)
                }
            }
            .pickerStyle(.segmented)

            Picker(
                model.localized(.exportColorSpace),
                selection: Binding(
                    get: { model.exportOptions.colorSpace },
                    set: { model.setExportColorSpace($0) }
                )
            ) {
                ForEach(ExportColorSpace.allCases) { colorSpace in
                    Text(model.exportColorSpaceName(colorSpace))
                        .tag(colorSpace)
                }
            }
            .pickerStyle(.menu)

            ParameterSlider(
                title: model.localized(.jpegQuality),
                value: Binding(
                    get: { model.exportOptions.jpegQuality },
                    set: { model.setExportJPEGQuality($0) }
                ),
                range: 0.1...1.0
            )
        }
    }
}

private struct BatchQueueSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.batchQueueSection))
                .font(.headline)

            HStack {
                Button(model.localized(.queueStretch)) {
                    model.enqueueCurrent(.stretch)
                }
                Button(model.localized(.queueDenoise)) {
                    model.enqueueCurrent(.denoise)
                }
            }
            .buttonStyle(.bordered)
            .disabled(model.canEnqueueCurrentBatchItem == false || model.isBatchRunning)

            HStack {
                Button(model.localized(.queueSharpen)) {
                    model.enqueueCurrent(.sharpen)
                }
                Button(model.localized(.queueStars)) {
                    model.enqueueCurrent(.starReduce)
                }
            }
            .buttonStyle(.bordered)
            .disabled(model.canEnqueueCurrentBatchItem == false || model.isBatchRunning)

            HStack {
                Button(model.localized(.queueAllStretch)) {
                    model.enqueueAllAssets(.stretch)
                }
                Button(model.localized(.queueAllDenoise)) {
                    model.enqueueAllAssets(.denoise)
                }
            }
            .buttonStyle(.bordered)
            .disabled(model.canEnqueueAllAssets == false || model.isBatchRunning)

            HStack {
                Button(model.localized(.queueAllSharpen)) {
                    model.enqueueAllAssets(.sharpen)
                }
                Button(model.localized(.queueAllStars)) {
                    model.enqueueAllAssets(.starReduce)
                }
            }
            .buttonStyle(.bordered)
            .disabled(model.canEnqueueAllAssets == false || model.isBatchRunning)

            HStack {
                Button(model.localized(.startQueue)) {
                    model.startBatchQueue()
                }
                .disabled(model.batchQueueInputsAvailable == false || model.isProcessing)

                Button(model.isBatchPaused ? model.localized(.resumeQueue) : model.localized(.pauseQueue)) {
                    if model.isBatchPaused {
                        model.resumeBatchQueue()
                    } else {
                        model.pauseBatchQueue()
                    }
                }
                .disabled(model.isBatchRunning == false)
            }
            .buttonStyle(.bordered)

            HStack {
                Button(model.localized(.retryFailed)) {
                    model.retryFailedBatchItems()
                }
                .disabled(model.batchQueue.contains { $0.status == .failed || $0.status == .cancelled } == false || model.isBatchRunning)

                Button(model.localized(.clearQueue)) {
                    model.clearBatchQueue()
                }
                .disabled(model.batchQueue.isEmpty || model.isBatchRunning)
            }
            .buttonStyle(.bordered)

            VStack(alignment: .leading, spacing: 6) {
                Picker(
                    model.localized(.batchOutputFormat),
                    selection: Binding(
                        get: { model.project.batchOutputFormat },
                        set: { model.setBatchOutputFormat($0) }
                    )
                ) {
                    ForEach(BatchOutputFormat.allCases) { format in
                        Text(model.batchOutputFormatName(format))
                            .tag(format)
                    }
                }
                .pickerStyle(.segmented)
                .disabled(model.isBatchRunning)

                Stepper(
                    "\(model.localized(.batchConcurrency)) \(model.project.batchMaxConcurrentTasks)",
                    value: Binding(
                        get: { model.project.batchMaxConcurrentTasks },
                        set: { model.setBatchMaxConcurrentTasks($0) }
                    ),
                    in: 1...4
                )
                .disabled(model.isBatchRunning)

                HStack {
                    Text(model.localized(.batchOutputDirectory))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    Spacer()

                    #if os(macOS)
                    Button {
                        if let directory = NativeFilePanels.openBatchOutputDirectory(
                            message: model.localized(.chooseBatchOutputDirectoryMessage)
                        ) {
                            model.setBatchOutputDirectory(directory)
                        }
                    } label: {
                        Label(model.localized(.chooseOutputDirectory), systemImage: "folder.badge.plus")
                            .labelStyle(.iconOnly)
                    }
                    .help(model.localized(.chooseOutputDirectory))
                    .disabled(model.isBatchRunning)

                    Button {
                        model.clearBatchOutputDirectory()
                    } label: {
                        Label(model.localized(.clearOutputDirectory), systemImage: "xmark.circle")
                            .labelStyle(.iconOnly)
                    }
                    .help(model.localized(.clearOutputDirectory))
                    .disabled(model.project.batchOutputDirectory == nil || model.isBatchRunning)
                    #endif

                    Button {
                        model.requestPreviewCacheCleanup()
                    } label: {
                        Label(model.localized(.clearPreviewCache), systemImage: "trash")
                            .labelStyle(.iconOnly)
                    }
                    .help(model.localized(.clearPreviewCache))
                    .disabled(model.isBatchRunning || model.isProcessing || model.isRawPreviewUpdating)
                }

                Text(model.project.batchOutputDirectory?.path ?? model.localized(.previewCache))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }

            if model.batchQueue.isEmpty {
                Text(model.localized(.noQueuedTasks))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                ForEach(model.batchQueue.suffix(8).reversed()) { item in
                    HStack(spacing: 8) {
                        Image(systemName: iconName(for: item.status))
                            .foregroundStyle(color(for: item.status))
                            .frame(width: 18)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(item.title)
                                .lineLimit(1)
                            Text(item.inputURL.lastPathComponent)
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .lineLimit(1)
                        }
                        Spacer()
                        if item.attempts > 0 {
                            Text("\(item.attempts)")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                    }
                    .font(.caption)
                    .padding(.vertical, 2)
                }
            }
        }
    }

    private func iconName(for status: ProcessingJobStatus) -> String {
        switch status {
        case .queued:
            return "clock"
        case .running:
            return "progress.indicator"
        case .succeeded:
            return "checkmark.circle.fill"
        case .failed:
            return "xmark.octagon.fill"
        case .cancelled:
            return "minus.circle"
        }
    }

    private func color(for status: ProcessingJobStatus) -> Color {
        switch status {
        case .queued, .cancelled:
            return .secondary
        case .running:
            return .blue
        case .succeeded:
            return .green
        case .failed:
            return .red
        }
    }
}

private struct MosaicSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var parameters: ProcessingParameters
    @State private var draggedMosaicPanelID: PhotonStackAsset.ID?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            let plan = model.mosaicPlan

            Text(model.localized(.mosaicSection))
                .font(.headline)

            HStack {
                Text(model.localized(.mosaicPanels))
                Spacer()
                Text("\(model.mosaicAssets.count)")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
            }

            Stepper(
                "\(model.localized(.overlapPixels)) \(parameters.mosaicOverlapPixels)",
                value: $parameters.mosaicOverlapPixels,
                in: 0...4096,
                step: 8
            )

            Picker(model.localized(.mosaicProjection), selection: $parameters.mosaicProjection) {
                ForEach(MosaicProjection.allCases) { projection in
                    Text(model.mosaicProjectionName(projection))
                        .tag(projection)
                }
            }
            .pickerStyle(.segmented)

            Picker(model.localized(.mosaicLayout), selection: $parameters.mosaicLayout) {
                ForEach(MosaicLayoutMode.allCases) { layout in
                    Text(model.mosaicLayoutName(layout))
                        .tag(layout)
                }
            }
            .pickerStyle(.segmented)

            if parameters.mosaicLayout == .grid {
                Stepper(
                    "\(model.localized(.mosaicColumns)) \(parameters.mosaicColumns)",
                    value: $parameters.mosaicColumns,
                    in: 1...12
                )
            }

            Picker(model.localized(.mosaicAlignment), selection: $parameters.mosaicAlignment) {
                ForEach(MosaicAlignmentMode.allCases) { alignment in
                    Text(model.mosaicAlignmentName(alignment))
                        .tag(alignment)
                }
            }
            .pickerStyle(.segmented)

            Picker(model.localized(.mosaicBlend), selection: $parameters.mosaicBlendMode) {
                ForEach(MosaicBlendMode.allCases) { blendMode in
                    Text(model.mosaicBlendModeName(blendMode))
                        .tag(blendMode)
                }
            }
            .pickerStyle(.segmented)

            Toggle(model.localized(.mosaicExposureMatch), isOn: $parameters.mosaicExposureMatching)

            Stepper(
                "\(model.localized(.mosaicPreviewWidth)) \(parameters.mosaicPreviewWidth)",
                value: $parameters.mosaicPreviewWidth,
                in: 320...4096,
                step: 80
            )

            if model.mosaicAssets.isEmpty {
                Text(model.localized(.mosaicNoPanels))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                Button {
                    if let suggestedOverlap = plan.suggestedOverlapPixels {
                        parameters.mosaicOverlapPixels = suggestedOverlap
                    }
                } label: {
                    Label(model.localized(.estimateOverlap), systemImage: "wand.and.stars")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(plan.suggestedOverlapPixels == nil || model.isProcessing)

                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Text(model.localized(.mosaicEstimatedSize))
                        Spacer()
                        if let width = plan.estimatedWidth, let height = plan.estimatedHeight {
                            Text("\(width) x \(height)")
                                .font(.system(.caption, design: .monospaced))
                        } else {
                            Text(model.localized(.mosaicMissingMetadata))
                                .foregroundStyle(.secondary)
                        }
                    }

                    if plan.missingMetadataCount > 0 {
                        Text("\(model.localized(.mosaicMissingMetadata)): \(plan.missingMetadataCount)")
                            .foregroundStyle(.secondary)
                    }

                    if plan.hasHeightMismatch {
                        Text(model.localized(.mosaicHeightMismatch))
                            .foregroundStyle(.orange)
                    }
                }
                .font(.caption)

                MosaicLayoutPreviewView(plan: plan)
                    .frame(height: 72)

                ForEach(Array(model.mosaicAssets.enumerated()), id: \.element.id) { index, asset in
                    HStack(spacing: 8) {
                        Text("\(index + 1)")
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .frame(width: 18)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(asset.displayName)
                                .font(.caption)
                                .lineLimit(1)
                            if let width = asset.metadata?.width, let height = asset.metadata?.height {
                                Text("\(width) x \(height)")
                                    .font(.caption2.monospacedDigit())
                                    .foregroundStyle(.secondary)
                            }
                        }
                        Spacer()
                        HStack(spacing: 4) {
                            Button {
                                model.moveMosaicPanel(asset.id, direction: .up)
                            } label: {
                                Label(model.localized(.moveOperationUp), systemImage: "chevron.up")
                                    .labelStyle(.iconOnly)
                            }
                            .disabled(model.isProcessing || index == 0)

                            Button {
                                model.moveMosaicPanel(asset.id, direction: .down)
                            } label: {
                                Label(model.localized(.moveOperationDown), systemImage: "chevron.down")
                                    .labelStyle(.iconOnly)
                            }
                            .disabled(model.isProcessing || index == model.mosaicAssets.count - 1)
                        }
                        .buttonStyle(.borderless)
                    }
                    .onDrag {
                        draggedMosaicPanelID = asset.id
                        return NSItemProvider(object: asset.id.uuidString as NSString)
                    }
                    .onDrop(of: [.text], delegate: MosaicPanelDropDelegate(
                        targetID: asset.id,
                        draggedID: $draggedMosaicPanelID,
                        model: model
                    ))
                    .padding(.vertical, 2)
                }

                if let report = model.mosaicQualityReport {
                    MosaicQualityReportView(model: model, report: report)
                }
            }

            HStack {
                Button {
                    model.startRunMosaicPreview()
                } label: {
                    Label(model.localized(.previewMosaic), systemImage: "eye")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canRunMosaic == false || model.isProcessing)

                Button {
                    model.startRunMosaic()
                } label: {
                    Label(model.localized(.runMosaic), systemImage: "rectangle.3.group")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .disabled(model.canRunMosaic == false || model.isProcessing)
            }
        }
        .disabled(model.canModifyProcessingConfiguration == false)
    }
}

private struct MosaicQualityReportView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let report: MosaicQualityReport

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(model.localized(.mosaicQualityReport))
                .font(.caption)
                .fontWeight(.semibold)

            HStack {
                Label("\(report.matches)", systemImage: "sparkles")
                Spacer()
                Text("\(model.localized(.mosaicFallbackPanels)): \(report.fallbackPanels)")
            }

            HStack {
                Text(report.autoAligned ? model.mosaicAlignmentName(.auto) : model.mosaicAlignmentName(.manual))
                Spacer()
                Text(report.exposureMatched ? model.localized(.mosaicExposureMatch) : report.blend)
            }

            ForEach(report.placements.prefix(4)) { placement in
                HStack(alignment: .top) {
                    Text("#\(placement.index + 1)")
                    Spacer()
                    VStack(alignment: .trailing, spacing: 1) {
                        Text("dx \(placement.dx, specifier: "%.1f") dy \(placement.dy, specifier: "%.1f")")
                        Text("\(transformName(placement.model))  \(placement.scale, specifier: "%.3f")x  \(placement.rotationDegrees, specifier: "%.2f")°\(placement.coarseAlignment ? "  2K" : "")")
                    }
                    VStack(alignment: .trailing, spacing: 1) {
                        Text("\(placement.matches)")
                        if placement.references > 0 {
                            Label("\(placement.references)", systemImage: "link")
                        }
                    }
                }
            }
        }
        .font(.caption2.monospacedDigit())
        .foregroundStyle(.secondary)
        .padding(.top, 4)
    }

    private func transformName(_ value: String) -> String {
        switch value {
        case "translation":
            return model.alignmentMethodName(.translation)
        case "similarity":
            return model.alignmentMethodName(.similarity)
        case "affine":
            return model.alignmentMethodName(.affine)
        default:
            return model.mosaicAlignmentName(.manual)
        }
    }
}

private struct HistogramCurvesSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var parameters: ProcessingParameters
    @State private var selectedCurvePointID: CurveEditorPoint.ID?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.histogramCurvesSection))
                .font(.headline)

            Picker(
                model.localized(.curveChannel),
                selection: $parameters.curveChannel
            ) {
                ForEach(CurveChannel.allCases) { channel in
                    Text(model.curveChannelName(channel))
                        .tag(channel)
                }
            }
            .pickerStyle(.segmented)
            .onChange(of: parameters.curveChannel) { _, _ in
                model.startCurvePreview()
            }

            CurveEditorView(
                model: model,
                snapshot: model.curveReferenceHistogram ?? model.histogram,
                parameters: $parameters,
                selectedPointID: $selectedCurvePointID
            )
                .frame(height: 156)

            HStack(spacing: 8) {
                if let selectedPoint = parameters.curveEditorPoints.first(where: { $0.id == selectedCurvePointID }) {
                    Text("x \(selectedPoint.input, format: .number.precision(.fractionLength(3)))")
                    Text("y \(selectedPoint.output, format: .number.precision(.fractionLength(3)))")
                } else {
                    Text(model.localized(.addCurvePoint))
                }

                Spacer()

                Button {
                    parameters.addCurvePoint(input: 0.5, output: 0.5)
                    selectedCurvePointID = parameters.curveEditorPoints.min { left, right in
                        abs(left.input - 0.5) < abs(right.input - 0.5)
                    }?.id
                    model.startCurvePreview()
                } label: {
                    Label(model.localized(.addCurvePoint), systemImage: "plus")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.addCurvePoint))

                Button(role: .destructive) {
                    if let selectedCurvePointID {
                        parameters.deleteCurvePoint(selectedCurvePointID)
                        self.selectedCurvePointID = parameters.curveEditorPoints.first?.id
                        model.startCurvePreview()
                    }
                } label: {
                    Label(model.localized(.deleteCurvePoint), systemImage: "trash")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.deleteCurvePoint))
                .disabled(selectedCurvePointID == nil || parameters.curveEditorPoints.count <= 2)
            }
            .font(.caption2.monospacedDigit())
            .foregroundStyle(.secondary)
            .buttonStyle(.bordered)

            if let histogram = model.curveReferenceHistogram ?? model.histogram {
                HStack {
                    Text("min \(histogram.minimum, format: .number.precision(.fractionLength(3)))")
                    Spacer()
                    Text("mean \(histogram.mean, format: .number.precision(.fractionLength(3)))")
                    Spacer()
                    Text("max \(histogram.maximum, format: .number.precision(.fractionLength(3)))")
                }
                .font(.caption2)
                .foregroundStyle(.secondary)
            }

            HStack {
                Button(model.localized(.refreshHistogram)) {
                    model.startRefreshHistogram()
                }
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

                Button(model.localized(.previewAction)) {
                    model.startCurvePreview()
                }
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

                Button(model.localized(.applyCurve)) {
                    model.startApplyCurve()
                }
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

                Spacer()

                Button {
                    model.resetCurveEditor()
                    selectedCurvePointID = parameters.curveEditorPoints[1].id
                } label: {
                    Label(model.localized(.resetAction), systemImage: "arrow.counterclockwise")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.resetAction))
            }
            .buttonStyle(.bordered)

            Picker(
                model.localized(.curvePreset),
                selection: Binding(
                    get: { parameters.matchingCurvePreset },
                    set: { preset in
                        parameters.applyCurvePreset(preset)
                        model.startCurvePreview()
                    }
                )
            ) {
                ForEach(CurvePreset.allCases) { preset in
                    Text(model.curvePresetName(preset))
                        .tag(preset)
                }
            }
            .pickerStyle(.menu)

            HStack {
                Button {
                    model.saveCurrentCurvePreset()
                } label: {
                    Label(model.localized(.saveCurvePreset), systemImage: "plus")
                }

                Spacer()

                #if os(macOS)
                Button {
                    if let url = NativeFilePanels.openCurvePresetLibraryURL(
                        message: model.localized(.importCurvePresetsMessage)
                    ) {
                        model.importCustomCurvePresets(from: url)
                    }
                } label: {
                    Label(model.localized(.importCurvePresets), systemImage: "square.and.arrow.down")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.importCurvePresets))

                Button {
                    if let url = NativeFilePanels.exportCurvePresetLibraryURL(
                        message: model.localized(.exportCurvePresetsMessage)
                    ) {
                        model.exportCustomCurvePresets(to: url)
                    }
                } label: {
                    Label(model.localized(.exportCurvePresets), systemImage: "square.and.arrow.up")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.exportCurvePresets))
                .disabled(model.customCurvePresets.isEmpty)
                #endif
            }
            .buttonStyle(.bordered)

            if model.customCurvePresets.isEmpty == false {
                VStack(alignment: .leading, spacing: 6) {
                    Text(model.localized(.customCurvePresets))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    ForEach(model.customCurvePresets) { preset in
                        HStack(spacing: 6) {
                            TextField(
                                model.localized(.nameLabel),
                                text: Binding(
                                    get: { preset.name },
                                    set: { model.renameCustomCurvePreset(preset.id, name: $0) }
                                )
                            )
                            .textFieldStyle(.roundedBorder)
                            .font(.caption)

                            Button {
                                model.applyCustomCurvePreset(preset)
                            } label: {
                                Label(model.localized(.applyCurve), systemImage: "paintbrush")
                                    .labelStyle(.iconOnly)
                            }
                            .help(model.localized(.applyCurve))

                            Button(role: .destructive) {
                                model.deleteCustomCurvePreset(preset.id)
                            } label: {
                                Label(model.localized(.deleteOperation), systemImage: "trash")
                                    .labelStyle(.iconOnly)
                            }
                            .help(model.localized(.deleteOperation))
                        }
                    }
                }
            }
        }
    }
}

private struct MosaicPanelDropDelegate: DropDelegate {
    let targetID: PhotonStackAsset.ID
    @Binding var draggedID: PhotonStackAsset.ID?
    @ObservedObject var model: PhotonStackWorkspaceModel

    func dropEntered(info: DropInfo) {
        guard let draggedID, draggedID != targetID else {
            return
        }
        model.moveMosaicPanel(draggedID, before: targetID)
    }

    func performDrop(info: DropInfo) -> Bool {
        draggedID = nil
        return true
    }

    func dropUpdated(info: DropInfo) -> DropProposal? {
        DropProposal(operation: .move)
    }
}

private struct MosaicLayoutPreviewView: View {
    let plan: MosaicPlanSummary

    var body: some View {
        GeometryReader { proxy in
            let layout = panelFrames(in: proxy.size)

            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 6)
                    .fill(.quaternary)
                    .overlay {
                        RoundedRectangle(cornerRadius: 6)
                            .stroke(.secondary.opacity(0.22), lineWidth: 1)
                    }

                ForEach(layout) { panel in
                    RoundedRectangle(cornerRadius: 4)
                        .fill(panel.color.opacity(0.38))
                        .overlay {
                            RoundedRectangle(cornerRadius: 4)
                                .stroke(panel.color.opacity(0.75), lineWidth: 1)
                        }
                        .frame(width: panel.frame.width, height: panel.frame.height)
                        .position(x: panel.frame.midX, y: panel.frame.midY)

                    Text("\(panel.index)")
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.primary)
                        .position(x: panel.frame.midX, y: panel.frame.midY)
                }
            }
        }
    }

    private func panelFrames(in size: CGSize) -> [LayoutPanel] {
        guard plan.panels.isEmpty == false else {
            return []
        }

        let widths = plan.panels.map { CGFloat($0.width ?? 1) }
        let heights = plan.panels.map { CGFloat($0.height ?? 1) }
        let effectiveOverlap = min(CGFloat(plan.overlapPixels), max((widths.min() ?? 1) - 1, 0))
        let rows = layoutRows(widths: widths, heights: heights, overlap: effectiveOverlap)
        let rawWidth = max(rows.map(\.width).max() ?? 1, 1)
        let rawHeight = max(rows.map(\.height).reduce(0, +) - effectiveOverlap * CGFloat(max(rows.count - 1, 0)), 1)
        let scale = min(size.width / rawWidth, size.height / rawHeight)
        let previewWidth = rawWidth * scale
        let previewHeight = rawHeight * scale
        let xInset = max((size.width - previewWidth) / 2.0, 0)
        let yInset = max((size.height - previewHeight) / 2.0, 0)

        var result: [LayoutPanel] = []
        var panelOffset = 0
        var y = yInset
        for row in rows {
            var x = xInset
            for column in 0..<row.count {
                let panel = plan.panels[panelOffset + column]
                let panelWidth = max(CGFloat(panel.width ?? 1) * scale, 8)
                let panelHeight = max(CGFloat(panel.height ?? 1) * scale, 12)
                let frame = CGRect(x: x, y: y + (row.height * scale - panelHeight) / 2.0, width: panelWidth, height: panelHeight)
                result.append(LayoutPanel(index: panel.index, frame: frame, color: palette[(panelOffset + column) % palette.count]))
                x += panelWidth - effectiveOverlap * scale
            }
            panelOffset += row.count
            y += row.height * scale - effectiveOverlap * scale
        }
        return result
    }

    private func layoutRows(widths: [CGFloat], heights: [CGFloat], overlap: CGFloat) -> [(width: CGFloat, height: CGFloat, count: Int)] {
        let columns = plan.layout == .horizontal ? max(plan.panels.count, 1) : max(plan.columns, 1)
        var rows: [(width: CGFloat, height: CGFloat, count: Int)] = []
        for start in stride(from: 0, to: plan.panels.count, by: columns) {
            let end = min(start + columns, plan.panels.count)
            let rowWidths = widths[start..<end]
            let rowHeights = heights[start..<end]
            let width = rowWidths.reduce(0, +) - overlap * CGFloat(max(rowWidths.count - 1, 0))
            rows.append((width: width, height: rowHeights.max() ?? 1, count: rowWidths.count))
        }
        return rows
    }

    private var palette: [Color] {
        [.blue, .green, .orange, .purple, .pink, .teal]
    }

    private struct LayoutPanel: Identifiable {
        var index: Int
        var frame: CGRect
        var color: Color

        var id: Int {
            index
        }
    }
}

private struct CurveEditorView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let snapshot: HistogramSnapshot?
    @Binding var parameters: ProcessingParameters
    @Binding var selectedPointID: CurveEditorPoint.ID?
    @State private var draftPoints: [CurveEditorPoint] = []
    @State private var isDraggingPoint = false

    var body: some View {
        GeometryReader { proxy in
            let size = proxy.size
            let points = currentPoints

            ZStack {
                RoundedRectangle(cornerRadius: 6)
                    .fill(.quaternary)
                    .overlay {
                        RoundedRectangle(cornerRadius: 6)
                            .stroke(.secondary.opacity(0.22), lineWidth: 1)
                    }
                    .contentShape(Rectangle())
                    .gesture(
                        SpatialTapGesture()
                            .onEnded { value in
                                addPoint(at: value.location, in: size)
                            }
                    )

                gridPath(in: size)
                    .stroke(.secondary.opacity(0.22), style: StrokeStyle(lineWidth: 1, dash: [3, 4]))

                if let snapshot, snapshot.bins.isEmpty == false {
                    let maximum = max(snapshot.bins.max() ?? 1, 1)
                    Path { path in
                        let width = size.width
                        let height = size.height
                        let step = width / CGFloat(max(snapshot.bins.count - 1, 1))

                        path.move(to: CGPoint(x: 0, y: height))
                        for index in snapshot.bins.indices {
                            let normalized = CGFloat(snapshot.bins[index]) / CGFloat(maximum)
                            let x = CGFloat(index) * step
                            let y = height - normalized * height
                            path.addLine(to: CGPoint(x: x, y: y))
                        }
                        path.addLine(to: CGPoint(x: width, y: height))
                        path.closeSubpath()
                    }
                    .fill(Color.accentColor.opacity(0.45))

                    Path { path in
                        let width = size.width
                        let height = size.height
                        let step = width / CGFloat(max(snapshot.bins.count - 1, 1))

                        for index in snapshot.bins.indices {
                            let normalized = CGFloat(snapshot.bins[index]) / CGFloat(maximum)
                            let x = CGFloat(index) * step
                            let y = height - normalized * height
                            if index == snapshot.bins.startIndex {
                                path.move(to: CGPoint(x: x, y: y))
                            } else {
                                path.addLine(to: CGPoint(x: x, y: y))
                            }
                        }
                    }
                    .stroke(Color.accentColor, lineWidth: 1.5)
                } else {
                    Text("Histogram")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                referencePath(in: size)
                    .stroke(.secondary.opacity(0.35), style: StrokeStyle(lineWidth: 1, dash: [4, 4]))

                curvePath(points: points, in: size)
                    .stroke(channelColor.opacity(0.95), style: StrokeStyle(lineWidth: 2.5, lineCap: .round, lineJoin: .round))

                ForEach(points) { point in
                    let selected = point.id == selectedPointID
                    Circle()
                        .fill(selected ? channelColor : Color.primary)
                        .frame(width: selected ? 15 : 12, height: selected ? 15 : 12)
                        .overlay {
                            Circle()
                                .stroke(Color.primary.opacity(selected ? 0.32 : 0.18), lineWidth: 2)
                        }
                        .shadow(radius: 2, y: 1)
                        .position(position(for: point, in: size))
                        .gesture(
                            DragGesture(minimumDistance: 0)
                                .onChanged { value in
                                    isDraggingPoint = true
                                    selectedPointID = point.id
                                    updateDraft(point.id, with: value.location, in: size)
                                    scheduleInteractivePreviewIfNeeded()
                                }
                                .onEnded { value in
                                    selectedPointID = point.id
                                    updateDraft(point.id, with: value.location, in: size)
                                    commitDraftPointsAndKeepPreview()
                                    isDraggingPoint = false
                                }
                        )
                }
            }
        }
        .onAppear {
            syncDraftPoints()
        }
        .onChange(of: parameters.curvePoints) { _, _ in
            guard isDraggingPoint == false else {
                return
            }
            syncDraftPoints()
        }
        .onDisappear {
            model.cancelCurvePreview()
        }
    }

    private var channelColor: Color {
        switch parameters.curveChannel {
        case .rgb:
            return .accentColor
        case .red:
            return .red
        case .green:
            return .green
        case .blue:
            return .blue
        case .luminance:
            return .primary
        }
    }

    private var currentPoints: [CurveEditorPoint] {
        normalizedDraftPoints(draftPoints.isEmpty ? parameters.curveEditorPoints : draftPoints)
    }

    private func syncDraftPoints() {
        draftPoints = parameters.curveEditorPoints
    }

    private func ensureDraftPoints() {
        if draftPoints.isEmpty {
            draftPoints = parameters.curveEditorPoints
        }
    }

    private func gridPath(in size: CGSize) -> Path {
        Path { path in
            for fraction in [0.25, 0.5, 0.75] {
                let x = size.width * CGFloat(fraction)
                path.move(to: CGPoint(x: x, y: 0))
                path.addLine(to: CGPoint(x: x, y: size.height))

                let y = size.height * CGFloat(fraction)
                path.move(to: CGPoint(x: 0, y: y))
                path.addLine(to: CGPoint(x: size.width, y: y))
            }
        }
    }

    private func referencePath(in size: CGSize) -> Path {
        Path { path in
            path.move(to: CGPoint(x: 0, y: size.height))
            path.addLine(to: CGPoint(x: size.width, y: 0))
        }
    }

    private func curvePath(points: [CurveEditorPoint], in size: CGSize) -> Path {
        Path { path in
            let samples = 96
            for sample in 0...samples {
                let input = Double(sample) / Double(samples)
                let output = curveOutput(for: input, points: points)
                let point = CGPoint(
                    x: CGFloat(input) * size.width,
                    y: (1.0 - CGFloat(output)) * size.height
                )
                if sample == 0 {
                    path.move(to: point)
                } else {
                    path.addLine(to: point)
                }
            }
        }
    }

    private func curveOutput(for input: Double, points: [CurveEditorPoint]) -> Double {
        sampleCurve(input, points: points)
    }

    private func position(for point: CurveEditorPoint, in size: CGSize) -> CGPoint {
        CGPoint(
            x: CGFloat(point.input) * size.width,
            y: (1.0 - CGFloat(point.output)) * size.height
        )
    }

    private func updateDraft(_ id: CurveEditorPoint.ID, with location: CGPoint, in size: CGSize) {
        ensureDraftPoints()
        let width = max(size.width, 1.0)
        let height = max(size.height, 1.0)
        let input = min(max(Double(location.x / width), 0.0), 1.0)
        let output = min(max(1.0 - Double(location.y / height), 0.0), 1.0)
        guard let index = draftPoints.firstIndex(where: { $0.id == id }) else {
            return
        }
        draftPoints[index].input = input
        draftPoints[index].output = output
    }

    private func scheduleInteractivePreviewIfNeeded() {
        let normalized = normalizedDraftPoints(draftPoints)
        guard normalized != parameters.curveEditorPoints else {
            return
        }
        model.scheduleCurvePreview(points: normalized)
    }

    private func commitDraftPointsAndKeepPreview() {
        let normalized = normalizedDraftPoints(draftPoints)
        guard normalized != parameters.curveEditorPoints else {
            draftPoints = parameters.curveEditorPoints
            return
        }
        parameters.replaceCurvePoints(normalized)
        draftPoints = parameters.curveEditorPoints
        // Dragging edits one pending curve against the session's original image.
        // The explicit Apply button is the only action that commits an operation.
        model.scheduleCurvePreview(points: parameters.curveEditorPoints)
    }

    private func addPoint(at location: CGPoint, in size: CGSize) {
        let width = max(size.width, 1.0)
        let height = max(size.height, 1.0)
        let input = min(max(Double(location.x / width), 0.0), 1.0)
        let output = min(max(1.0 - Double(location.y / height), 0.0), 1.0)
        parameters.addCurvePoint(input: input, output: output)
        draftPoints = parameters.curveEditorPoints
        selectedPointID = parameters.curveEditorPoints.min { left, right in
            squaredDistance(left, input: input, output: output) < squaredDistance(right, input: input, output: output)
        }?.id
    }

    private func normalizedDraftPoints(_ points: [CurveEditorPoint]) -> [CurveEditorPoint] {
        let source = points.count >= 2 ? points : parameters.curveEditorPoints
        var normalized = source.map { point in
            CurveEditorPoint(
                id: point.id,
                input: min(max(point.input, 0.0), 1.0),
                output: min(max(point.output, 0.0), 1.0)
            )
        }
        normalized.sort { left, right in
            if abs(left.input - right.input) < 0.000_001 {
                return left.id.uuidString < right.id.uuidString
            }
            return left.input < right.input
        }

        for index in normalized.indices.dropFirst() where normalized[index].input <= normalized[index - 1].input {
            normalized[index].input = min(normalized[index - 1].input + 0.01, 1.0)
        }
        for index in normalized.indices.dropLast().reversed() where normalized[index].input >= normalized[index + 1].input {
            normalized[index].input = max(normalized[index + 1].input - 0.01, 0.0)
        }
        return normalized
    }

    private func squaredDistance(_ point: CurveEditorPoint, input: Double, output: Double) -> Double {
        let dx = point.input - input
        let dy = point.output - output
        return dx * dx + dy * dy
    }
}

private struct ParameterSlider: View {
    let title: String
    @Binding var value: Double
    let range: ClosedRange<Double>

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(title)
                Spacer()
                Text(value, format: .number.precision(.fractionLength(2)))
                    .foregroundStyle(.secondary)
            }
            Slider(value: $value, in: range)
        }
    }
}

private struct JobsSection: View {
    let jobs: [ProcessingJob]
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.tasksSection))
                .font(.headline)

            if jobs.isEmpty {
                Text(model.localized(.noTasksYet))
                    .foregroundStyle(.secondary)
            } else {
                ForEach(jobs.suffix(8).reversed()) { job in
                    HStack(spacing: 8) {
                        Image(systemName: iconName(for: job.status))
                            .foregroundStyle(color(for: job.status))
                            .frame(width: 18)

                        VStack(alignment: .leading, spacing: 2) {
                            Text(job.title)
                                .lineLimit(1)
                            if let outputURL = job.outputURL {
                                Text(outputURL.lastPathComponent)
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                    .lineLimit(1)
                            }
                            HStack(spacing: 6) {
                                if job.cacheHit {
                                    Text(model.localized(.cacheHitLabel))
                                        .foregroundStyle(.green)
                                } else if job.status == .succeeded {
                                    Text(model.localized(.executedLabel))
                                        .foregroundStyle(.blue)
                                }
                                if let duration = job.durationMilliseconds {
                                    Text(formatDuration(duration))
                                        .foregroundStyle(.secondary)
                                }
                            }
                            .font(.caption2)
                        }

                        Spacer()
                    }
                    .font(.caption)
                    .padding(.vertical, 3)
                }
            }
        }
    }

    private func iconName(for status: ProcessingJobStatus) -> String {
        switch status {
        case .queued:
            return "clock"
        case .running:
            return "progress.indicator"
        case .succeeded:
            return "checkmark.circle.fill"
        case .failed:
            return "xmark.octagon.fill"
        case .cancelled:
            return "minus.circle"
        }
    }

    private func color(for status: ProcessingJobStatus) -> Color {
        switch status {
        case .queued, .cancelled:
            return .secondary
        case .running:
            return .blue
        case .succeeded:
            return .green
        case .failed:
            return .red
        }
    }

    private func formatDuration(_ milliseconds: Double) -> String {
        if milliseconds < 1000 {
            return "\(Int(milliseconds.rounded())) ms"
        }
        return String(format: "%.2f s", milliseconds / 1000)
    }
}

private struct ProcessingWorkspaceSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var selectedTab: WorkspaceTab = .layers

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Picker("", selection: $selectedTab) {
                ForEach(WorkspaceTab.allCases) { tab in
                    Text(model.localized(tab.titleKey))
                        .tag(tab)
                }
            }
            .labelsHidden()
            .pickerStyle(.segmented)

            selectedContent
        }
    }

    @ViewBuilder
    private var selectedContent: some View {
        switch selectedTab {
        case .assets:
            SourceAssetsWorkspaceView(model: model)
        case .objects:
            SmartObjectsWorkspaceView(model: model, artifactIcon: artifactIcon)
        case .layers:
            LayersWorkspaceView(model: model, artifactIcon: artifactIcon)
        }
    }

    private func artifactIcon(for kind: ProcessingArtifactKind) -> String {
        switch kind {
        case .rawSequence, .decodedSequence, .calibratedSequence, .registeredSequence, .cleanedTimelapseSequence:
            return "rectangle.stack"
        case .stackMaster:
            return "square.stack.3d.up"
        case .editedImage:
            return "photo"
        case .mask:
            return "circle.dashed"
        case .meteorLayer:
            return "sparkles"
        case .starLayer:
            return "star"
        }
    }
}

private enum WorkspaceTab: String, CaseIterable, Identifiable {
    case assets
    case objects
    case layers

    var id: String {
        rawValue
    }

    var titleKey: LocalizedTextKey {
        switch self {
        case .assets:
            return .workspaceAssetsTab
        case .objects:
            return .workspaceObjectsTab
        case .layers:
            return .workspaceLayersTab
        }
    }
}

private struct SourceAssetsWorkspaceView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if model.project.assets.isEmpty {
                Text(model.localized(.noSourceAssets))
                    .foregroundStyle(.secondary)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 8) {
                        ForEach(model.project.assets) { asset in
                            SourceAssetWorkspaceRow(model: model, asset: asset)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 220)
            }
        }
    }
}

private struct SourceAssetWorkspaceRow: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let asset: PhotonStackAsset

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: model.isAssetMissing(asset) ? "exclamationmark.triangle.fill" : "photo")
                .frame(width: 18)
                .foregroundStyle(
                    model.isAssetMissing(asset)
                        ? Color.orange
                        : (model.selectedAssetID == asset.id ? Color.accentColor : Color.secondary)
                )

            VStack(alignment: .leading, spacing: 2) {
                Text(asset.displayName)
                    .lineLimit(1)
                Text("\(model.roleName(asset.role)) / \(asset.kind.rawValue.uppercased())")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                if model.isAssetMissing(asset) {
                    Text(model.localized(.missingAssetStatus))
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(.orange)
                }
            }

            Spacer()

            Button {
                model.select(asset)
            } label: {
                Label(model.localized(.previewProduct), systemImage: "eye")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(model.localized(.previewProduct))
            .accessibilityLabel("\(model.localized(.previewProduct)): \(asset.displayName)")
            .disabled(model.isProcessing || model.isAssetMissing(asset))

            Button {
                model.createLayer(from: asset)
            } label: {
                Label(model.localized(.addLayerFromSource), systemImage: "square.stack.3d.up")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(model.localized(.addLayerFromSource))
            .disabled(model.canModifyWorkspaceProducts == false || model.isAssetMissing(asset))

            Button {
                presentRelinkPanel(for: asset, model: model)
            } label: {
                Label(model.localized(.relinkAssetAction), systemImage: "link")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(model.localized(.relinkAssetHelp))
            .disabled(model.isProcessing)

            Button {
                model.requestAssetRemoval(asset)
            } label: {
                Label(model.localized(.removeAssetAction), systemImage: "trash")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(model.localized(.removeAssetHelp))
            .disabled(model.isProcessing)
        }
        .padding(.vertical, 3)
        .contextMenu {
            Button {
                presentRelinkPanel(for: asset, model: model)
            } label: {
                Label(model.localized(.relinkAssetAction), systemImage: "link")
            }
            .disabled(model.isProcessing)

            Divider()

            Button(role: .destructive) {
                model.requestAssetRemoval(asset)
            } label: {
                Label(model.localized(.removeAssetAction), systemImage: "trash")
            }
            .disabled(model.isProcessing)
        }
    }
}

private struct SmartObjectsWorkspaceView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let artifactIcon: (ProcessingArtifactKind) -> String

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if model.project.artifacts.isEmpty {
                Text(model.localized(.noArtifacts))
                    .foregroundStyle(.secondary)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 8) {
                        ForEach(model.project.artifacts.reversed()) { artifact in
                            SmartObjectWorkspaceRow(model: model, artifact: artifact, iconName: artifactIcon(artifact.kind))
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 240)
            }
        }
        .disabled(model.canModifyWorkspaceProducts == false)
    }
}

private struct SmartObjectWorkspaceRow: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let artifact: ProcessingArtifact
    let iconName: String

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 8) {
                Image(systemName: iconName)
                    .frame(width: 18)
                    .foregroundStyle(.blue)

                VStack(alignment: .leading, spacing: 1) {
                    Text(artifact.name)
                        .lineLimit(1)
                    Text(model.artifactKindName(artifact.kind))
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }

                Spacer()

                Button {
                    model.previewArtifact(artifact)
                } label: {
                    Label(model.localized(.previewProduct), systemImage: "eye")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .help(model.localized(.previewProduct))
                .accessibilityLabel("\(model.localized(.previewProduct)): \(artifact.name)")
                .disabled(model.isArtifactPreviewAvailable(artifact) == false || model.isProcessing)

                Button {
                    model.createLayer(from: artifact)
                } label: {
                    Label(model.localized(.addLayerFromObject), systemImage: "square.stack.3d.up")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .help(model.localized(.addLayerFromObject))
                .disabled(model.canCreateLayer(from: artifact) == false || model.isProcessing)

                Button(role: .destructive) {
                    model.requestArtifactRemoval(artifact)
                } label: {
                    Label(model.localized(.removeArtifactAction), systemImage: "trash")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .help(model.localized(.removeArtifactHelp))
                .accessibilityLabel("\(model.localized(.removeArtifactAction)): \(artifact.name)")
                .disabled(model.isProcessing)
            }

            if artifact.previewURL != nil && model.isArtifactPreviewAvailable(artifact) == false {
                Label(model.localized(.missingWorkspaceSourceError), systemImage: "exclamationmark.triangle.fill")
                    .font(.caption2)
                    .foregroundStyle(.orange)
                    .lineLimit(2)
            }

            HStack(spacing: 8) {
                Text("\(model.localized(.framesLabel)): \(artifact.frameCount)")
                if let output = artifact.outputDirectory ?? artifact.previewURL?.deletingLastPathComponent() {
                    Text(output.lastPathComponent)
                        .lineLimit(1)
                }
            }
            .font(.caption2)
            .foregroundStyle(.secondary)

            if let matches = artifact.frames.first(where: { $0.isReference == false })?.metrics["matches"] {
                Text("\(model.localized(.registrationMatches)): \(matches)")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 3)
    }
}

private struct LayersWorkspaceView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let artifactIcon: (ProcessingArtifactKind) -> String

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(model.localized(.workspaceLayersTab))
                    .font(.headline)

                Text("\(model.project.layers.count)")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)

                Spacer()

                addLayerMenu

                Button {
                    model.startPreviewLayerStack()
                } label: {
                    Label(model.localized(.previewLayerStack), systemImage: "square.3.layers.3d")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .help(model.localized(.previewLayerStack))
                .disabled(model.canPreviewVisibleLayerStack == false || model.isProcessing)
            }

            if let activeLayer = model.activeLayer {
                Label("\(model.localized(.activeLayerLabel)): \(activeLayer.name)", systemImage: "target")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            } else {
                Text(model.localized(.noActiveLayer))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }

            if model.project.layers.isEmpty {
                Text(model.localized(.noLayers))
                    .foregroundStyle(.secondary)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 8) {
                        ForEach(model.project.layers.reversed()) { layer in
                            ProcessingLayerRow(model: model, layer: layer)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 260)
            }
        }
        .disabled(model.canModifyWorkspaceProducts == false)
    }

    private var addLayerMenu: some View {
        Menu {
            if model.project.assets.isEmpty == false {
                Section(model.localized(.workspaceAssetsTab)) {
                    ForEach(model.project.assets) { asset in
                        Button {
                            model.createLayer(from: asset)
                        } label: {
                            Label(asset.displayName, systemImage: asset.id == model.selectedAssetID ? "checkmark.circle" : "photo")
                        }
                        .disabled(model.isAssetMissing(asset))
                    }
                }
            }

            let artifacts = model.project.artifacts
                .filter { $0.previewURL != nil }
                .reversed()
            if artifacts.isEmpty == false {
                Section(model.localized(.workspaceObjectsTab)) {
                    ForEach(Array(artifacts)) { artifact in
                        Button {
                            model.createLayer(from: artifact)
                        } label: {
                            Label(artifact.name, systemImage: artifactIcon(artifact.kind))
                        }
                        .disabled(model.canCreateLayer(from: artifact) == false)
                    }
                }
            }
        } label: {
            Label(model.localized(.addLayerMenu), systemImage: "plus.square.on.square")
                .labelStyle(.iconOnly)
        }
        .menuStyle(.borderlessButton)
        .help(model.localized(.addLayerMenu))
        .disabled(model.isProcessing || model.hasAvailableLayerSource == false)
    }

}

private struct ProcessingLayerRow: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let layer: ProcessingLayer
    @State private var showingMaskEditor = false

    private var isActive: Bool {
        model.activeLayerID == layer.id
    }

    private var isInputAvailable: Bool {
        model.isLayerInputAvailable(layer)
    }

    private var isMaskAvailable: Bool {
        model.isLayerMaskInputAvailable(layer)
    }

    private var isWorkspaceSourceAvailable: Bool {
        isInputAvailable && isMaskAvailable
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                if layer.kind == .mask {
                    Image(systemName: "circle.lefthalf.filled")
                        .foregroundStyle(.secondary)
                        .accessibilityHidden(true)
                        .frame(width: 20)
                } else {
                    Button {
                        model.setLayerVisibility(layer.id, isVisible: model.layerIsVisible(layer.id) == false)
                    } label: {
                        Label(
                            model.localized(model.layerIsVisible(layer.id) ? .hideLayer : .showLayer),
                            systemImage: model.layerIsVisible(layer.id) ? "eye" : "eye.slash"
                        )
                        .labelStyle(.iconOnly)
                        .frame(width: 20)
                    }
                    .buttonStyle(.borderless)
                    .help(model.localized(
                        model.layerIsVisible(layer.id)
                            ? .hideLayer
                            : (model.canShowLayer(layer.id) ? .showLayer : .disabledOperationLayer)
                    ))
                    .disabled(
                        model.isProcessing ||
                            (model.layerIsVisible(layer.id) == false && model.canShowLayer(layer.id) == false)
                    )
                }

                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 4) {
                        Text(layer.name)
                            .lineLimit(1)
                        if isActive {
                            Image(systemName: "target")
                                .font(.caption2)
                                .foregroundStyle(Color.accentColor)
                        }
                    }
                    Text(
                        layer.kind == .mask
                            ? model.layerKindName(layer.kind)
                            : "\(model.layerKindName(layer.kind)) · \(model.layerBlendModeName(model.layerBlendMode(layer.id)))"
                    )
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }

                if isWorkspaceSourceAvailable == false {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                        .help(model.localized(.missingWorkspaceSourceError))
                }

                Spacer()

                if layer.kind != .mask {
                    Button {
                        model.moveProcessingLayer(layer.id, direction: .up)
                    } label: {
                        Label(model.localized(.moveOperationUp), systemImage: "chevron.up")
                            .labelStyle(.iconOnly)
                    }
                    .buttonStyle(.borderless)
                    .disabled(model.isProcessing || model.canMoveProcessingLayer(layer.id, direction: .up) == false)

                    Button {
                        model.moveProcessingLayer(layer.id, direction: .down)
                    } label: {
                        Label(model.localized(.moveOperationDown), systemImage: "chevron.down")
                            .labelStyle(.iconOnly)
                    }
                    .buttonStyle(.borderless)
                    .disabled(model.isProcessing || model.canMoveProcessingLayer(layer.id, direction: .down) == false)
                }

                Button {
                    model.selectLayer(layer)
                } label: {
                    Label(model.localized(.previewProduct), systemImage: "eye")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .accessibilityLabel("\(model.localized(.previewProduct)): \(layer.name)")
                .disabled(isInputAvailable == false || model.isProcessing)

                Button {
                    model.deleteProcessingLayer(layer.id)
                } label: {
                    Label(model.localized(.deleteOperation), systemImage: "trash")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .help(model.localized(.deleteOperation))
                .disabled(model.isProcessing)
            }

            if layer.kind != .mask {
                HStack(spacing: 8) {
                    Picker(model.localized(.blendModeLabel), selection: blendModeBinding) {
                        ForEach(ProcessingLayerBlendMode.allCases) { mode in
                            Text(model.layerBlendModeName(mode)).tag(mode)
                        }
                    }
                    .labelsHidden()
                    .pickerStyle(.menu)
                    .frame(width: 94)
                    .disabled(model.isProcessing || isWorkspaceSourceAvailable == false)

                    Slider(value: opacityBinding, in: 0.0...1.0) { editing in
                        if editing {
                            model.beginLayerOpacityAdjustment(layer.id)
                        } else {
                            model.endLayerOpacityAdjustment(layer.id)
                        }
                    }
                        .disabled(model.isProcessing || isWorkspaceSourceAvailable == false)

                    Text(formatPercent(model.layerOpacity(layer.id)))
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .frame(width: 36, alignment: .trailing)
                }
                .padding(.leading, 54)
            }

            if layer.kind != .mask {
                VStack(alignment: .leading, spacing: 6) {
                    HStack(spacing: 8) {
                        Menu {
                            if model.layerMaskArtifact(layer.id) != nil {
                                Button {
                                    model.setLayerMask(layer.id, artifactID: nil)
                                } label: {
                                    Label(model.localized(.detachLayerMask), systemImage: "xmark.circle")
                                }
                                Divider()
                            }

                            ForEach(model.availableMaskArtifacts) { artifact in
                                Button {
                                    model.setLayerMask(layer.id, artifactID: artifact.id)
                                } label: {
                                    Label(
                                        artifact.name,
                                        systemImage: model.layerMaskArtifact(layer.id)?.id == artifact.id
                                            ? "checkmark.circle.fill"
                                            : "circle.lefthalf.filled"
                                    )
                                }
                            }
                        } label: {
                            Label(model.localized(.layerMaskLabel), systemImage: "circle.lefthalf.filled")
                                .labelStyle(.iconOnly)
                        }
                        .menuStyle(.borderlessButton)
                        .help(model.localized(.layerMaskLabel))
                        .disabled(
                            model.isProcessing ||
                                (model.layerMaskArtifact(layer.id) == nil && model.availableMaskArtifacts.isEmpty)
                        )

                        Text(model.layerMaskArtifact(layer.id)?.name ?? model.localized(.noLayerMask))
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)

                        Spacer()

                        if model.layerMaskArtifact(layer.id) != nil && isMaskAvailable {
                            Button {
                                showingMaskEditor = true
                            } label: {
                                Label(model.localized(.editLayerMask), systemImage: "paintbrush.pointed")
                                    .labelStyle(.iconOnly)
                            }
                            .buttonStyle(.borderless)
                            .help(model.localized(.editLayerMask))
                            .disabled(model.isProcessing)

                            Button {
                                model.setLayerMaskInverted(
                                    layer.id,
                                    inverted: model.layerMaskIsInverted(layer.id) == false
                                )
                            } label: {
                                Label(model.localized(.invertLayerMask), systemImage: "circle.lefthalf.filled")
                                    .labelStyle(.iconOnly)
                                    .foregroundStyle(
                                        model.layerMaskIsInverted(layer.id) ? Color.accentColor : Color.secondary
                                    )
                            }
                            .buttonStyle(.borderless)
                            .help(model.localized(.invertLayerMask))
                            .disabled(model.isProcessing)
                        }
                    }

                    if model.layerMaskArtifact(layer.id) != nil && isMaskAvailable {
                        HStack(spacing: 8) {
                            Text(model.localized(.maskDensity))
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .frame(width: 48, alignment: .leading)
                            Slider(value: maskDensityBinding, in: 0...1) { editing in
                                if editing {
                                    model.beginLayerMaskAdjustment(layer.id)
                                } else {
                                    model.endLayerMaskAdjustment(layer.id)
                                }
                            }
                                .disabled(model.isProcessing)
                            Text(formatPercent(model.layerMaskDensity(layer.id)))
                                .font(.caption2.monospacedDigit())
                                .foregroundStyle(.secondary)
                                .frame(width: 36, alignment: .trailing)
                        }

                        HStack(spacing: 8) {
                            Text(model.localized(.maskFeather))
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .frame(width: 48, alignment: .leading)
                            Slider(value: maskFeatherBinding, in: maskFeatherRange) { editing in
                                if editing {
                                    model.beginLayerMaskAdjustment(layer.id)
                                } else {
                                    model.endLayerMaskAdjustment(layer.id)
                                }
                            }
                                .disabled(model.isProcessing)
                            Text("\(Int(model.layerMaskFeatherRadius(layer.id).rounded())) px")
                                .font(.caption2.monospacedDigit())
                                .foregroundStyle(.secondary)
                                .frame(width: 48, alignment: .trailing)
                        }
                    }
                }
                .padding(.leading, 54)
            }
        }
        .padding(.vertical, 4)
        .padding(.horizontal, 6)
        .opacity(layer.kind == .mask || model.layerIsVisible(layer.id) ? 1.0 : 0.55)
        .contentShape(Rectangle())
        .background(isActive ? Color.accentColor.opacity(0.14) : Color.clear, in: RoundedRectangle(cornerRadius: 6))
        .onTapGesture {
            guard model.canModifyWorkspaceProducts else {
                return
            }
            model.selectLayer(layer)
        }
        .sheet(isPresented: $showingMaskEditor) {
            if let maskURL = model.layerMaskArtifact(layer.id)?.previewURL {
                MaskBrushEditorSheet(model: model, layerID: layer.id, maskURL: maskURL)
            }
        }
    }

    private var opacityBinding: Binding<Double> {
        Binding(
            get: { model.layerOpacity(layer.id) },
            set: { model.setLayerOpacity(layer.id, opacity: $0) }
        )
    }

    private var blendModeBinding: Binding<ProcessingLayerBlendMode> {
        Binding(
            get: { model.layerBlendMode(layer.id) },
            set: { model.setLayerBlendMode(layer.id, blendMode: $0) }
        )
    }

    private var maskDensityBinding: Binding<Double> {
        Binding(
            get: { model.layerMaskDensity(layer.id) },
            set: { model.setLayerMaskDensity(layer.id, density: $0) }
        )
    }

    private var maskFeatherBinding: Binding<Double> {
        Binding(
            get: { model.layerMaskFeatherRadius(layer.id) },
            set: { model.setLayerMaskFeatherRadius(layer.id, radius: $0) }
        )
    }

    private var maskFeatherRange: ClosedRange<Double> {
        guard let size = model.layerMaskPixelSize(layer.id) else {
            return 0...1
        }
        return 0...max(1, min(size.width, size.height) * 0.1)
    }

    private func formatPercent(_ value: Double) -> String {
        "\(Int((value * 100).rounded()))%"
    }
}

private struct EditGraphSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(model.localized(.editGraphSection))
                    .font(.headline)

                Spacer()

                Button {
                    model.undoEditGraphChange()
                    replayIfNeeded()
                } label: {
                    Label(model.localized(.undoEditGraph), systemImage: "arrow.uturn.backward")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .keyboardShortcut("z", modifiers: [.command])
                .help(model.localized(.undoEditGraph))
                .disabled(model.canUndoEditGraph == false || model.isProcessing)

                Button {
                    model.redoEditGraphChange()
                    replayIfNeeded()
                } label: {
                    Label(model.localized(.redoEditGraph), systemImage: "arrow.uturn.forward")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .keyboardShortcut("z", modifiers: [.command, .shift])
                .help(model.localized(.redoEditGraph))
                .disabled(model.canRedoEditGraph == false || model.isProcessing)

                Button {
                    model.startReplayEditGraph()
                } label: {
                    Label(model.localized(.replayEditGraph), systemImage: "play.fill")
                        .labelStyle(.iconOnly)
                        .foregroundStyle(model.editGraphNeedsReplay ? Color.accentColor : Color.primary)
                }
                .buttonStyle(.borderless)
                .help(model.localized(model.editGraphNeedsReplay ? .editGraphNeedsReplay : .replayEditGraph))
                .disabled(model.canReplayEditGraph == false)
            }

            if model.project.editGraph.operations.isEmpty {
                Text(model.localized(.noEditOperations))
                    .foregroundStyle(.secondary)
            } else {
                ForEach(model.project.editGraph.operations) { operation in
                    VStack(alignment: .leading, spacing: 6) {
                        HStack(spacing: 8) {
                            Button {
                                model.toggleEditOperation(operation.id)
                                replayIfNeeded()
                            } label: {
                                Image(systemName: operation.isEnabled ? "checkmark.circle" : "circle")
                                    .foregroundStyle(operation.isEnabled ? .green : .secondary)
                                    .frame(width: 18)
                            }
                            .buttonStyle(.plain)
                            .disabled(model.isProcessing)

                            VStack(alignment: .leading, spacing: 2) {
                                Text(operation.kind.rawValue)
                                    .lineLimit(1)
                                if let output = operation.parameters["output"] {
                                    Text(URL(fileURLWithPath: output).lastPathComponent)
                                        .font(.caption2)
                                        .foregroundStyle(.secondary)
                                        .lineLimit(1)
                                }
                            }

                            Spacer()

                            HStack(spacing: 4) {
                                Button {
                                    model.moveEditOperation(operation.id, direction: .up)
                                    replayIfNeeded()
                                } label: {
                                    Label(model.localized(.moveOperationUp), systemImage: "chevron.up")
                                        .labelStyle(.iconOnly)
                                }
                                .disabled(model.isProcessing || model.canMoveEditOperation(operation.id, direction: .up) == false)

                                Button {
                                    model.moveEditOperation(operation.id, direction: .down)
                                    replayIfNeeded()
                                } label: {
                                    Label(model.localized(.moveOperationDown), systemImage: "chevron.down")
                                        .labelStyle(.iconOnly)
                                }
                                .disabled(model.isProcessing || model.canMoveEditOperation(operation.id, direction: .down) == false)

                                Button(role: .destructive) {
                                    model.deleteEditOperation(operation.id)
                                    replayIfNeeded()
                                } label: {
                                    Label(model.localized(.deleteOperation), systemImage: "trash")
                                        .labelStyle(.iconOnly)
                                }
                                .disabled(model.isProcessing)
                            }
                            .buttonStyle(.borderless)
                        }

                        let editableParameters = model.editableEditOperationParameters(for: operation)
                        if let curveParameters = model.curveParameters(for: operation) {
                            CurveNodePreviewView(points: curveParameters.curveEditorPoints)
                                .frame(height: 54)
                                .padding(.leading, 26)

                            if let presetName = curvePresetName(for: operation) {
                                LabeledContent(model.localized(.curvePreset), value: presetName)
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                    .padding(.leading, 26)
                            }
                        }

                        if editableParameters.isEmpty == false {
                            LazyVGrid(columns: [GridItem(.adaptive(minimum: 88), spacing: 6)], alignment: .leading, spacing: 6) {
                                ForEach(editableParameters) { parameter in
                                    EditOperationParameterControl(
                                        model: model,
                                        operationID: operation.id,
                                        parameter: parameter,
                                        onCommit: replayIfNeeded
                                    )
                                }
                            }
                            .padding(.leading, 26)
                        }
                    }
                    .font(.caption)
                    .padding(.vertical, 3)
                }
            }
        }
        .disabled(model.canModifyWorkspaceProducts == false)
    }

    private func replayIfNeeded() {
        if model.editGraphNeedsReplay {
            model.startReplayEditGraph()
        }
    }

    private func curvePresetName(for operation: EditOperation) -> String? {
        model.editOperationPresetName(operation)
    }
}

private struct EditOperationParameterControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let operationID: EditOperation.ID
    let parameter: EditableEditOperationParameter
    let onCommit: () -> Void

    @State private var draft: String
    @FocusState private var isFocused: Bool

    init(
        model: PhotonStackWorkspaceModel,
        operationID: EditOperation.ID,
        parameter: EditableEditOperationParameter,
        onCommit: @escaping () -> Void
    ) {
        self.model = model
        self.operationID = operationID
        self.parameter = parameter
        self.onCommit = onCommit
        _draft = State(initialValue: parameter.value)
    }

    @ViewBuilder
    var body: some View {
        switch parameter.editor {
        case .toggle:
            Toggle(parameter.key, isOn: toggleBinding)
                .toggleStyle(.checkbox)
                .font(.caption2)
        case let .options(values):
            Picker(parameter.key, selection: optionBinding) {
                ForEach(values, id: \.self) { value in
                    Text(value).tag(value)
                }
            }
            .pickerStyle(.menu)
            .font(.caption2)
        case .decimal, .integer, .text:
            TextField(parameter.key, text: $draft)
                .textFieldStyle(.roundedBorder)
                .font(.caption2.monospacedDigit())
                .focused($isFocused)
                .onSubmit(commitDraft)
                .onChange(of: isFocused) { wasFocused, isFocused in
                    if wasFocused && isFocused == false {
                        commitDraft()
                    }
                }
                .onChange(of: parameter.value) { _, value in
                    if isFocused == false {
                        draft = value
                    }
                }
        }
    }

    private var toggleBinding: Binding<Bool> {
        Binding(
            get: { model.parameterValue(operationID, key: parameter.key) == "true" },
            set: { commit(String($0)) }
        )
    }

    private var optionBinding: Binding<String> {
        Binding(
            get: { model.parameterValue(operationID, key: parameter.key) },
            set: { value in
                commit(value)
            }
        )
    }

    private func commitDraft() {
        commit(draft)
        draft = model.parameterValue(operationID, key: parameter.key)
    }

    private func commit(_ value: String) {
        let previousValue = model.parameterValue(operationID, key: parameter.key)
        if model.updateEditOperationParameter(operationID, key: parameter.key, value: value),
           model.parameterValue(operationID, key: parameter.key) != previousValue {
            onCommit()
        }
    }
}

private struct CurveNodePreviewView: View {
    let points: [CurveEditorPoint]

    var body: some View {
        GeometryReader { proxy in
            let size = proxy.size

            ZStack {
                RoundedRectangle(cornerRadius: 6)
                    .fill(.quaternary.opacity(0.7))

                Path { path in
                    path.move(to: CGPoint(x: 0, y: size.height))
                    path.addLine(to: CGPoint(x: size.width, y: 0))
                }
                .stroke(.secondary.opacity(0.35), style: StrokeStyle(lineWidth: 1, dash: [3, 4]))

                curvePath(in: size)
                    .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))

                ForEach(points) { point in
                    Circle()
                        .fill(Color.accentColor)
                        .frame(width: 8, height: 8)
                        .position(position(for: point, in: size))
                }
            }
        }
    }

    private func curvePath(in size: CGSize) -> Path {
        Path { path in
            let samples = 48
            for sample in 0...samples {
                let input = Double(sample) / Double(samples)
                let output = curveOutput(for: input)
                let point = CGPoint(x: CGFloat(input) * size.width, y: (1.0 - CGFloat(output)) * size.height)
                if sample == 0 {
                    path.move(to: point)
                } else {
                    path.addLine(to: point)
                }
            }
        }
    }

    private func curveOutput(for input: Double) -> Double {
        sampleCurve(input, points: points)
    }

    private func position(for point: CurveEditorPoint, in size: CGSize) -> CGPoint {
        CGPoint(x: CGFloat(point.input) * size.width, y: (1.0 - CGFloat(point.output)) * size.height)
    }
}

private func sampleCurve(_ input: Double, points: [CurveEditorPoint]) -> Double {
    let sorted = points.sorted { $0.input < $1.input }
    guard sorted.count >= 2 else {
        return min(max(input, 0.0), 1.0)
    }
    let x = min(max(input, 0.0), 1.0)
    if x <= sorted[0].input {
        return min(max(sorted[0].output, 0.0), 1.0)
    }

    let tangents = curveTangents(for: sorted)
    for index in sorted.indices.dropFirst() where x <= sorted[index].input {
        return min(max(hermiteInterpolate(
            x,
            from: sorted[index - 1],
            to: sorted[index],
            startTangent: tangents[index - 1],
            endTangent: tangents[index]
        ), 0.0), 1.0)
    }
    return min(max(sorted[sorted.count - 1].output, 0.0), 1.0)
}

private func curveTangents(for points: [CurveEditorPoint]) -> [Double] {
    guard points.count >= 2 else {
        return Array(repeating: 0.0, count: points.count)
    }

    let slopes = points.indices.dropLast().map { index in
        let span = max(points[index + 1].input - points[index].input, 0.000_001)
        return (points[index + 1].output - points[index].output) / span
    }

    var tangents = Array(repeating: 0.0, count: points.count)
    tangents[0] = slopes[0]
    tangents[points.count - 1] = slopes[slopes.count - 1]
    for index in 1..<(points.count - 1) {
        tangents[index] = limitedCurveTangent(leftSlope: slopes[index - 1], rightSlope: slopes[index])
    }
    return tangents
}

private func limitedCurveTangent(leftSlope: Double, rightSlope: Double) -> Double {
    guard leftSlope * rightSlope > 0 else {
        return 0.0
    }
    let sign = leftSlope < 0 ? -1.0 : 1.0
    let average = 0.5 * (leftSlope + rightSlope)
    let limit = 3.0 * min(abs(leftSlope), abs(rightSlope))
    return sign * min(abs(average), limit)
}

private func hermiteInterpolate(
    _ input: Double,
    from start: CurveEditorPoint,
    to end: CurveEditorPoint,
    startTangent: Double,
    endTangent: Double
) -> Double {
    let span = max(end.input - start.input, 0.000_001)
    let t = min(max((input - start.input) / span, 0.0), 1.0)
    let t2 = t * t
    let t3 = t2 * t
    let h00 = 2.0 * t3 - 3.0 * t2 + 1.0
    let h10 = t3 - 2.0 * t2 + t
    let h01 = -2.0 * t3 + 3.0 * t2
    let h11 = t3 - t2
    return h00 * start.output + h10 * span * startTangent + h01 * end.output + h11 * span * endTangent
}

private struct StatusConsole: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var commandInput = ""
    @AppStorage("PhotonStackStatusConsole.isCollapsed") private var isCollapsed = false
    @AppStorage("PhotonStackStatusConsole.isExpanded") private var isExpanded = false
    #if os(macOS)
    @State private var detachedWindow: NSWindow?
    #endif

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(model.isProcessing ? model.localized(.processingStatus) : model.localized(.consoleStatus))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Spacer()

                if model.isProcessing, let progress = model.progressFraction {
                    Text("\(Int((progress * 100).rounded()))%")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }

                if let errorMessage = model.errorMessage {
                    Button {
                        withAnimation(.easeInOut(duration: 0.18)) {
                            isCollapsed = false
                        }
                    } label: {
                        Image(systemName: "exclamationmark.triangle.fill")
                            .foregroundStyle(.red)
                    }
                    .buttonStyle(.borderless)
                    .frame(width: 28, height: 24)
                    .help(errorMessage)
                    .accessibilityLabel(Text(errorMessage))
                }

                Button {
                    withAnimation(.easeInOut(duration: 0.18)) {
                        isCollapsed.toggle()
                        if isCollapsed {
                            isExpanded = false
                        }
                    }
                } label: {
                    Label(
                        isCollapsed ? model.localized(.consoleExpand) : model.localized(.consoleCollapse),
                        systemImage: isCollapsed ? "chevron.up" : "chevron.down"
                    )
                    .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .frame(width: 28, height: 24)
                .help(isCollapsed ? model.localized(.consoleExpand) : model.localized(.consoleCollapse))

                if isCollapsed == false {
                    Button {
                        withAnimation(.easeInOut(duration: 0.18)) {
                            isExpanded.toggle()
                        }
                    } label: {
                        Label(
                            isExpanded ? model.localized(.consoleZoomOut) : model.localized(.consoleZoomIn),
                            systemImage: isExpanded ? "arrow.down.right.and.arrow.up.left" : "arrow.up.left.and.arrow.down.right"
                        )
                        .labelStyle(.iconOnly)
                    }
                    .buttonStyle(.borderless)
                    .frame(width: 28, height: 24)
                    .help(isExpanded ? model.localized(.consoleZoomOut) : model.localized(.consoleZoomIn))
                }

                #if os(macOS)
                Button {
                    openDetachedConsole()
                } label: {
                    Label(model.localized(.consolePopOut), systemImage: "macwindow")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .frame(width: 28, height: 24)
                .help(model.localized(.consolePopOut))
                #endif
            }

            if isCollapsed == false {
                if model.isProcessing {
                    ProgressView(value: model.progressFraction ?? 0)
                        .controlSize(.small)
                    if let progressMessage = model.progressMessage {
                        Text(progressMessage)
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)
                    }
                }

                HStack(spacing: 8) {
                    Text(">")
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.secondary)

                    TextField(model.localized(.consoleInputPlaceholder), text: $commandInput)
                        .textFieldStyle(.roundedBorder)
                        .font(.system(.caption, design: .monospaced))
                        .disabled(model.isProcessing)
                        .onSubmit {
                            submitCommand()
                        }

                    Button {
                        submitCommand()
                    } label: {
                        Label(model.localized(.consoleRunCommand), systemImage: "terminal")
                    }
                    .help(model.localized(.consoleRunCommand))
                    .disabled(model.isProcessing || commandInput.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
                }

                if let errorMessage = model.errorMessage {
                    Text(errorMessage)
                        .font(.system(.caption, design: .monospaced))
                        .foregroundStyle(.red)
                        .textSelection(.enabled)
                        .frame(maxWidth: .infinity, alignment: .leading)
                } else {
                    ScrollView {
                        Text(model.commandOutput.isEmpty ? model.localized(.readyStatus) : model.commandOutput)
                            .font(.system(.caption, design: .monospaced))
                            .foregroundStyle(model.commandOutput.isEmpty ? .secondary : .primary)
                            .textSelection(.enabled)
                            .frame(maxWidth: .infinity, alignment: .leading)
                    }
                }
            }
        }
        .padding(12)
        .frame(height: consoleHeight, alignment: .top)
        .clipped()
    }

    private var consoleHeight: CGFloat {
        if isCollapsed {
            return 48
        }
        return isExpanded ? 420 : 220
    }

    private func submitCommand() {
        let command = commandInput.trimmingCharacters(in: .whitespacesAndNewlines)
        guard command.isEmpty == false else {
            return
        }
        model.startRunConsoleCommand(command)
    }

    #if os(macOS)
    private func openDetachedConsole() {
        if let detachedWindow, detachedWindow.isVisible {
            detachedWindow.makeKeyAndOrderFront(nil)
            return
        }

        let window = NSWindow(
            contentRect: NSRect(x: 0, y: 0, width: 860, height: 560),
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.title = model.localized(.consoleStatus)
        window.isReleasedWhenClosed = false
        window.contentViewController = NSHostingController(rootView: DetachedConsoleWindow(model: model))
        window.setFrameAutosaveName("PhotonStackDetachedConsole")
        window.center()
        window.makeKeyAndOrderFront(nil)
        detachedWindow = window
    }
    #endif
}

private struct DetachedConsoleWindow: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var commandInput = ""

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(model.isProcessing ? model.localized(.processingStatus) : model.localized(.consoleStatus))
                    .font(.headline)

                Spacer()

                if model.isProcessing, let progress = model.progressFraction {
                    Text("\(Int((progress * 100).rounded()))%")
                        .font(.caption.monospacedDigit())
                        .foregroundStyle(.secondary)
                }
            }

            if model.isProcessing {
                ProgressView(value: model.progressFraction ?? 0)
                    .controlSize(.small)
                if let progressMessage = model.progressMessage {
                    Text(progressMessage)
                        .font(.caption)
                        .foregroundStyle(.secondary)
                        .lineLimit(1)
                }
            }

            HStack(spacing: 8) {
                Text(">")
                    .font(.system(.body, design: .monospaced))
                    .foregroundStyle(.secondary)

                TextField(model.localized(.consoleInputPlaceholder), text: $commandInput)
                    .textFieldStyle(.roundedBorder)
                    .font(.system(.body, design: .monospaced))
                    .disabled(model.isProcessing)
                    .onSubmit {
                        submitCommand()
                    }

                Button {
                    submitCommand()
                } label: {
                    Label(model.localized(.consoleRunCommand), systemImage: "terminal")
                }
                .disabled(model.isProcessing || commandInput.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty)
            }

            ConsoleOutputView(model: model)
        }
        .padding(16)
        .frame(minWidth: 720, minHeight: 460)
    }

    private func submitCommand() {
        let command = commandInput.trimmingCharacters(in: .whitespacesAndNewlines)
        guard command.isEmpty == false else {
            return
        }
        model.startRunConsoleCommand(command)
    }
}

private struct ConsoleOutputView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        if let errorMessage = model.errorMessage {
            ScrollView {
                Text(errorMessage)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.red)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        } else {
            ScrollView {
                Text(model.commandOutput.isEmpty ? model.localized(.readyStatus) : model.commandOutput)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(model.commandOutput.isEmpty ? .secondary : .primary)
                    .textSelection(.enabled)
                    .frame(maxWidth: .infinity, alignment: .leading)
            }
        }
    }
}

private struct SidebarAssetRow: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let asset: PhotonStackAsset
    let isSelected: Bool
    let showsRemoveButton: Bool

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: iconName)
                .foregroundStyle(
                    model.isAssetMissing(asset)
                        ? Color.orange
                        : (isSelected ? Color.accentColor : Color.secondary)
                )
                .frame(width: 18)

            VStack(alignment: .leading, spacing: 4) {
                Text(asset.displayName)
                    .lineLimit(1)
                Text(subtitle)
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
            }

            Spacer(minLength: 6)

            Button {
                model.requestAssetRemoval(asset)
            } label: {
                Label(model.localized(.removeAssetAction), systemImage: "trash")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(model.localized(.removeAssetHelp))
            .disabled(model.isProcessing)
            .opacity(showsRemoveButton ? 1 : 0)
            .accessibilityHidden(showsRemoveButton == false)
        }
        .padding(.vertical, 4)
    }

    private var subtitle: String {
        if model.isAssetMissing(asset) {
            return model.localized(.missingAssetStatus)
        }
        var parts = [asset.kind.rawValue.uppercased()]
        if let width = asset.metadata?.width, let height = asset.metadata?.height {
            parts.append("\(width) x \(height)")
        } else {
            let folder = asset.originalURL.deletingLastPathComponent().lastPathComponent
            if folder.isEmpty == false {
                parts.append(folder)
            }
        }
        return parts.joined(separator: " · ")
    }

    private var iconName: String {
        if model.isAssetMissing(asset) {
            return "exclamationmark.triangle.fill"
        }
        switch asset.kind {
        case .raw:
            return "camera"
        case .fits:
            return "sparkles"
        case .tiff, .jpeg, .heif, .png:
            return "photo"
        case .unknown:
            return "doc"
        }
    }
}

private struct PhotonStackPreview: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var magnification = 1.0
    @State private var fitToWindow = true
    @State private var showArtifactMarkedPreview = false

    var body: some View {
        VStack(spacing: 0) {
            PreviewZoomBar(
                model: model,
                magnification: $magnification,
                fitToWindow: $fitToWindow,
                showArtifactMarkedPreview: $showArtifactMarkedPreview,
                hasArtifactMarkedPreview: model.artifactMarkedPreviewURL != nil,
                hasImage: currentURL != nil
            )

            ZStack {
                Color.black

                if let url = currentURL {
                    PlatformImageView(url: url, magnification: $magnification, fitToWindow: $fitToWindow)
                        .padding(16)
                } else {
                    ContentUnavailableView(model.localized(.noImage), systemImage: "photo")
                }
            }
            .onChange(of: model.selectedAssetID) { _, _ in
                magnification = 1.0
                fitToWindow = true
                showArtifactMarkedPreview = false
            }
            .onChange(of: model.previewURL) { oldValue, newValue in
                guard oldValue != newValue, newValue != nil else {
                    return
                }
                magnification = 1.0
                fitToWindow = true
            }
            .onChange(of: model.artifactMarkedPreviewURL) { _, url in
                showArtifactMarkedPreview = url != nil
            }
        }
    }

    private var currentURL: URL? {
        if showArtifactMarkedPreview, let markedPreview = model.artifactMarkedPreviewURL {
            return markedPreview
        }
        return model.previewURL ?? model.selectedAsset?.originalURL
    }
}

private struct PreviewZoomBar: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var magnification: Double
    @Binding var fitToWindow: Bool
    @Binding var showArtifactMarkedPreview: Bool
    let hasArtifactMarkedPreview: Bool
    let hasImage: Bool

    var body: some View {
        HStack(spacing: 8) {
            Button {
                fitToWindow = true
            } label: {
                Label(model.localized(.fitToWindow), systemImage: "arrow.up.left.and.arrow.down.right")
                    .labelStyle(.iconOnly)
            }
            .help(model.localized(.fitToWindow))

            Button {
                fitToWindow = false
                magnification = 1.0
            } label: {
                Text(model.localized(.actualSize))
                    .font(.caption)
                    .frame(minWidth: 34)
            }
            .help(model.localized(.actualSize))

            Divider()
                .frame(height: 18)

            Button {
                fitToWindow = false
                magnification = max(0.1, magnification / 1.25)
            } label: {
                Label(model.localized(.zoomOut), systemImage: "minus.magnifyingglass")
                    .labelStyle(.iconOnly)
            }
            .help(model.localized(.zoomOut))

            Slider(
                value: Binding(
                    get: { magnification },
                    set: { value in
                        fitToWindow = false
                        magnification = value
                    }
                ),
                in: 0.1...8.0
            )
            .frame(width: 150)

            Button {
                fitToWindow = false
                magnification = min(8.0, magnification * 1.25)
            } label: {
                Label(model.localized(.zoomIn), systemImage: "plus.magnifyingglass")
                    .labelStyle(.iconOnly)
            }
            .help(model.localized(.zoomIn))

            Text("\(Int((magnification * 100).rounded()))%")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(width: 48, alignment: .trailing)

            if hasArtifactMarkedPreview {
                Divider()
                    .frame(height: 18)

                Picker("", selection: $showArtifactMarkedPreview) {
                    Text(model.localized(.originalPreview)).tag(false)
                    Text(model.localized(.markedPreview)).tag(true)
                }
                .pickerStyle(.segmented)
                .frame(width: 128)
                .help(model.localized(.artifactMarkedPreview))
            }

            Spacer()
        }
        .buttonStyle(.bordered)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.bar)
        .disabled(hasImage == false)
    }
}

private struct PlatformImageView: View {
    let url: URL
    @Binding var magnification: Double
    @Binding var fitToWindow: Bool

    var body: some View {
        #if os(macOS)
        NativeImagePreview(url: url, magnification: $magnification, fitToWindow: $fitToWindow)
        #else
        if let data = try? Data(contentsOf: url), let image = UIImage(data: data) {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
        } else {
            ContentUnavailableView(url.lastPathComponent, systemImage: "exclamationmark.triangle")
        }
        #endif
    }
}
