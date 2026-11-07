import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct ApplicationStateSynchronizationModifier: ViewModifier {
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

struct PreviewCacheCleanupConfirmationModifier: ViewModifier {
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

struct ProcessingInterruptionConfirmationModifier: ViewModifier {
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

#if os(macOS)
final class WindowAttachmentView: NSView {
    var onWindowChange: ((NSWindow?) -> Void)?

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        onWindowChange?(window)
    }
}

struct WindowCloseGuard: NSViewRepresentable {
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
