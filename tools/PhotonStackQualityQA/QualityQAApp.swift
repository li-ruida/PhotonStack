import AppKit
import PhotonStackAppCore
import PhotonStackProcessing
import PhotonStackUI
import SwiftUI

/// Manual integration harness, not a replacement production application.
/// The staging script supplies a unique bundle and writable storage directory.
@main
struct QualityQAApp: App {
    @StateObject private var model: PhotonStackWorkspaceModel
    @StateObject private var commands = PhotonStackApplicationCommandState()
    private let workflows: WorkflowRepository

    init() {
        guard Bundle.main.bundleIdentifier == "dev.photonstack.qualityqa",
              let path = Bundle.main.object(forInfoDictionaryKey: "PhotonStackQADataRoot") as? String,
              path.hasPrefix("/"), path != "/" else {
            fatalError("Launch the staged QA app with its explicit isolated data directory.")
        }
        let root = URL(fileURLWithPath: path, isDirectory: true)
        let service = CLIProcessingService(executableURL:
            Bundle.main.bundleURL.appendingPathComponent("Contents/MacOS/photonstack"))
        _model = StateObject(wrappedValue: PhotonStackWorkspaceModel(
            project: PhotonStackProject(name: "QA · Synthetic workflow check"),
            language: .simplifiedChinese, autosaveEnabled: false,
            processingService: service,
            settingsRepository: AppSettingsRepository(settingsURL: root.appendingPathComponent("settings.json")),
            previewDirectory: root.appendingPathComponent("previews", isDirectory: true)))
        workflows = WorkflowRepository(url: root.appendingPathComponent("workflows.json"))
    }

    var body: some Scene {
        Window("PhotonStack Quality QA — isolated", id: "qa") {
            PhotonStackRootView(workspaceModel: model, workflowRepository: workflows,
                               applicationCommandState: commands)
                .frame(minWidth: 1120, minHeight: 720)
                .background(QAWindowLayoutProbe())
                .onAppear { NSApp.setActivationPolicy(.regular); NSApp.activate(ignoringOtherApps: true) }
        }
        .commands { PhotonStackProcessCommands(state: commands) }
    }
}

/// Records actual AppKit geometry for small-window layout regression checks.
/// This lives only in the isolated QA harness and never changes window sizing.
private struct QAWindowLayoutProbe: NSViewRepresentable {
    func makeNSView(context: Context) -> ProbeView { ProbeView() }
    func updateNSView(_ nsView: ProbeView, context: Context) {}

    final class ProbeView: NSView {
        private var pending = false

        override func layout() {
            super.layout()
            recordLayout()
        }

        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            recordLayout()
        }

        private func recordLayout() {
            guard !pending else { return }
            pending = true
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.2) { [weak self] in
                guard let self else { return }
                self.pending = false
                guard let window = self.window, let content = window.contentView,
                      let path = Bundle.main.object(forInfoDictionaryKey: "PhotonStackQADataRoot") as? String else { return }
                func tree(_ view: NSView, depth: Int) -> [String] {
                    let inset = view.safeAreaInsets
                    let row = "\(String(repeating: "  ", count: depth))\(type(of: view)) frame=\(view.frame) bounds=\(view.bounds) safe=\(inset) intrinsic=\(view.intrinsicContentSize)"
                    return [row] + (depth < 12 ? view.subviews.flatMap { tree($0, depth: depth + 1) } : [])
                }
                let report = ["window=\(window.frame) contentLayout=\(window.contentLayoutRect) minSize=\(window.minSize) contentMinSize=\(window.contentMinSize)"] + tree(content, depth: 0)
                try? report.joined(separator: "\n").write(to: URL(fileURLWithPath: path).appendingPathComponent("window-layout.txt"), atomically: true, encoding: .utf8)
            }
        }
    }
}
