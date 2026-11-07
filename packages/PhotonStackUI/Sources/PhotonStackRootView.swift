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
func presentRelinkPanel(for asset: PhotonStackAsset, model: PhotonStackWorkspaceModel) {
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
    static let photonStackOpenDeepSky = Notification.Name("dev.photonstack.openDeepSky")
    static let photonStackOpenFinishing = Notification.Name("dev.photonstack.openFinishing")
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
            let tasks = pendingSettingsPersistence
            for (id, task) in tasks {
                await task.value
                // An already-completed task need not suspend. Drain it here
                // instead of starving the separate MainActor cleanup task.
                pendingSettingsPersistence.removeValue(forKey: id)
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

public struct PhotonStackRootView: View {
    @StateObject private var model: PhotonStackWorkspaceModel
    private let applicationCommandState: PhotonStackApplicationCommandState
    private let workflowRepository: WorkflowRepository
    @State private var isImporting = false
    @State private var isCreatingProject = false
    @State private var isDeepSkyPresented = false
    @State private var deepSkyReviewFilter: DeepSkyReviewFilter?
    @State private var isWorkflowLibraryPresented = false
    @StateObject private var workflowBatch = WorkflowBatchRun()
    @State private var newProjectDraft = NewProjectDraft(name: "", template: .deepSky)
    @State private var pendingProjectCreation: NewProjectDraft?
    @State private var pendingProductManifestExport = false
    @State private var pendingLayerStackPreview = false
    @State private var inspectorPage: WorkspaceInspectorPage = .processing
    @State private var finishingRequested = false

    public init(
        processingService: any ProcessingService,
        applicationCommandState: PhotonStackApplicationCommandState = PhotonStackApplicationCommandState()
    ) {
        _model = StateObject(wrappedValue: PhotonStackWorkspaceModel(processingService: processingService))
        self.applicationCommandState = applicationCommandState
        self.workflowRepository = .shared
    }

    /// Hosts the same workspace with explicitly isolated storage, for integration
    /// tests and embedded workspaces. Production initialization remains unchanged.
    public init(
        workspaceModel: PhotonStackWorkspaceModel,
        workflowRepository: WorkflowRepository,
        applicationCommandState: PhotonStackApplicationCommandState = PhotonStackApplicationCommandState()
    ) {
        _model = StateObject(wrappedValue: workspaceModel)
        self.workflowRepository = workflowRepository
        self.applicationCommandState = applicationCommandState
    }

    private var workspaceLayout: some View {
        NavigationSplitView {
            AssetSidebar(
                model: model,
                onImportFiles: { startImportFiles() },
                onImportFolder: { startImportFolder() }
            )
        } detail: {
            VStack(spacing: 0) {
                WorkspaceJourneyBar(model: model, finishingRequested: finishingRequested, onSelect: navigateJourney)
                Divider()
                if model.project.assets.isEmpty {
                    WorkspaceWelcomeView(chinese: model.language == .simplifiedChinese,
                        onImport: startImportFiles, onImportFolder: startImportFolder,
                        onEditMaster: startImportMaster, onOpenProject: openProject)
                } else {
                    HStack(spacing: 0) {
                        VStack(spacing: 0) {
                            PhotonStackPreview(model: model)
                            Divider()
                            ActionBar(model: model)
                        }
                        Divider()
                        InspectorPanel(model: model, page: $inspectorPage,
                            isFinishing: finishingRequested || model.journeyResultIsCurrent)
                            .frame(width: 320)
                    }
                }
            }
            .frame(minWidth: 0, maxWidth: .infinity, minHeight: 0, maxHeight: .infinity)
            .navigationTitle(model.project.name)
        }
        .toolbar {
            WorkspaceToolbar(
                model: model,
                onNewProject: presentNewProjectSheet,
                onImportFiles: startImportFiles,
                onImportFolder: startImportFolder,
                onOpenProject: openProject,
                onSaveProject: saveProject,
                onOpenWorkflows: { isWorkflowLibraryPresented = true }
            )
        }
    }

    private var workspaceSheets: some View {
        workspaceLayout
        .fileImporter(
            isPresented: $isImporting,
            allowedContentTypes: [.item],
            allowsMultipleSelection: true
        ) { result in
            if case let .success(urls) = result {
                model.importAssets(from: urls)
            }
        }
        .sheet(isPresented: $isDeepSkyPresented) {
            DeepSkyRecipeSheet(model: model, presented: $isDeepSkyPresented, reviewFilter: $deepSkyReviewFilter, onImport: {
                startImportFolder()
            })
        }
        .sheet(isPresented: $isWorkflowLibraryPresented) {
            WorkflowLibrarySheet(model: model, batch: workflowBatch, presented: $isWorkflowLibraryPresented,
                                 repository: workflowRepository)
        }
        .sheet(isPresented: $isCreatingProject, onDismiss: createPendingProject) {
            NewProjectSheet(
                model: model,
                draft: $newProjectDraft,
                onCancel: {
                    pendingProjectCreation = nil
                    isCreatingProject = false
                },
                onCreate: {
                    guard newProjectDraft.canCreate, model.hasActiveProcessing == false else { return }
                    pendingProjectCreation = newProjectDraft
                    isCreatingProject = false
                }
            )
        }
    }

    private var workspaceDialogs: some View {
        workspaceSheets
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
    }

    public var body: some View {
        workspaceDialogs
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
        .onReceive(NotificationCenter.default.publisher(for: .photonStackOpenDeepSky)) { notification in
            deepSkyReviewFilter = notification.object as? DeepSkyReviewFilter
            isDeepSkyPresented = true
        }
        .onReceive(NotificationCenter.default.publisher(for: .photonStackOpenFinishing)) { _ in
            navigateJourney(.finish)
        }
        .onChange(of: model.project.id) { _, _ in
            finishingRequested = false
            inspectorPage = .processing
            isDeepSkyPresented = false
            isWorkflowLibraryPresented = false
            pendingProductManifestExport = false
            pendingLayerStackPreview = false
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
        // Window scenes own the minimum window size. The split view must
        // accept the actual content area after the toolbar's safe area.
        .frame(minWidth: 0, maxWidth: .infinity, minHeight: 0, maxHeight: .infinity)
    }

    private func navigateJourney(_ step: WorkspaceJourneyStep) {
        guard !model.hasActiveProcessing else { return }
        switch step {
        case .importPhotos:
            startImportFiles()
        case .review, .stack:
            deepSkyReviewFilter = nil
            finishingRequested = false
            isDeepSkyPresented = true
        case .finish:
            // Returning to the editor must not reset a user's later adjustments.
            if model.isShowingSelectedOriginal, model.journeyResultIsCurrent {
                model.showLatestDeepSkyResult()
            }
            finishingRequested = true
            inspectorPage = .processing
        case .export:
            #if os(macOS)
            if model.canExportCurrentImage,
               let output = NativeFilePanels.exportImageURL(message: model.localized(.exportImageMessage)) {
                model.startExportCurrentImage(to: output)
            }
            #endif
        }
    }

    private func presentNewProjectSheet() {
        guard model.hasActiveProcessing == false else { return }
        pendingProjectCreation = nil
        newProjectDraft = NewProjectDraft(
            name: suggestedProjectName(for: model.selectedTemplate),
            template: model.selectedTemplate
        )
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

    private func createPendingProject() {
        guard let draft = pendingProjectCreation else { return }
        pendingProjectCreation = nil
        // Present the unsaved-changes decision only after the creation sheet
        // has actually dismissed; two modal surfaces must not compete.
        _ = model.requestCreateProject(
            template: draft.template,
            name: draft.name,
            reuseCurrentAssets: draft.reuseCurrentAssets
        )
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

    private func startImportMaster() {
        #if os(macOS)
        if let urls = NativeFilePanels.importAssetURLs(message: model.language == .simplifiedChinese
            ? "选择已堆栈的 FITS / TIFF 或要调整的单张照片。" : "Choose a FITS / TIFF master or a single image to finish.") {
            model.importAssets(from: urls)
            finishingRequested = true
            inspectorPage = .processing
        }
        #else
        finishingRequested = true
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
        case .open, .createUsingCurrentAssets:
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
