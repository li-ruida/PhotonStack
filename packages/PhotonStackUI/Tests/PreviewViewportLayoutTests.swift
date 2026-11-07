#if os(macOS)
import AppKit
import ImageIO
import PhotonStackAppCore
import PhotonStackProcessing
import SwiftUI
import Testing
import UniformTypeIdentifiers
@testable import PhotonStackUI

@MainActor private func previewDescendants(of view: NSView) -> [NSView] {
    [view] + view.subviews.flatMap { previewDescendants(of: $0) }
}

private func writeLargePreviewFixture(to url: URL) throws {
    let context = try #require(CGContext(data: nil, width: 2400, height: 1800,
        bitsPerComponent: 8, bytesPerRow: 0, space: CGColorSpaceCreateDeviceRGB(),
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue))
    context.setFillColor(CGColor(red: 0.1, green: 0.3, blue: 0.5, alpha: 1))
    context.fill(CGRect(x: 0, y: 0, width: 2400, height: 1800))
    let image = try #require(context.makeImage())
    let output = try #require(CGImageDestinationCreateWithURL(url as CFURL,
        UTType.png.identifier as CFString, 1, nil))
    CGImageDestinationAddImage(output, image, nil)
    try #require(CGImageDestinationFinalize(output))
}

@Test @MainActor func loadedRootWorkspaceShrinksWithinWindowContent() async throws {
    _ = NSApplication.shared
    let root = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackRootLayout-\(UUID().uuidString)")
    try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
    defer { try? FileManager.default.removeItem(at: root) }
    let input = root.appendingPathComponent("large-original.png")
    try writeLargePreviewFixture(to: input)
    var project = PhotonStackProject(name: "Root workspace layout regression")
    project.addAssets(from: [input])
    let model = PhotonStackWorkspaceModel(project: project, language: .simplifiedChinese,
        autosaveEnabled: false, processingService: UnavailableProcessingService(),
        settingsRepository: AppSettingsRepository(settingsURL: root.appendingPathComponent("settings.json")),
        previewDirectory: root.appendingPathComponent("previews"))
    let workflows = WorkflowRepository(url: root.appendingPathComponent("workflows.json"))
    // Host the production root, including sidebar, journey, inspector, preview
    // and action bar. Keep only the Scene's window minimum outside the root.
    let content = PhotonStackRootView(workspaceModel: model, workflowRepository: workflows)
        .frame(minWidth: 1120, minHeight: 720)
    let host = NSHostingView(rootView: content)
    let window = NSWindow(contentRect: CGRect(x: 0, y: 0, width: 1728, height: 1032),
        styleMask: [.titled, .closable, .resizable, .fullSizeContentView], backing: .buffered, defer: false)
    window.isReleasedWhenClosed = false
    window.title = "Isolated root layout regression"
    window.toolbar = NSToolbar(identifier: "PhotonStackRootLayoutToolbar")
    window.toolbarStyle = .unified
    window.contentView = host
    defer { window.contentView = nil; window.close() }

    func resize(_ size: CGSize) {
        window.setFrame(CGRect(origin: window.frame.origin, size: size), display: false)
        host.needsLayout = true
        host.layoutSubtreeIfNeeded()
    }
    resize(CGSize(width: 1728, height: 1084))

    // Exercise actual asynchronous decoding and AppKit document assignment;
    // an empty workspace does not reproduce the populated-root overflow.
    var loadedScroll: NativePreviewScrollView?
    for _ in 0..<120 {
        host.layoutSubtreeIfNeeded()
        loadedScroll = previewDescendants(of: host).compactMap { $0 as? NativePreviewScrollView }
            .first { ($0.documentView as? NSImageView)?.image?.size == CGSize(width: 2400, height: 1800) }
        if loadedScroll != nil { break }
        try await Task.sleep(for: .milliseconds(25))
    }
    let scroll = try #require(loadedScroll, "The real image must be loaded before testing root resize")

    for size in [CGSize(width: 1120, height: 772), CGSize(width: 1728, height: 1084), CGSize(width: 1120, height: 772)] {
        resize(size)
        try await Task.sleep(for: .milliseconds(100))
        host.layoutSubtreeIfNeeded()
        #expect(abs(window.frame.width - size.width) <= 1 && abs(window.frame.height - size.height) <= 1,
            "The populated workspace must not enlarge the requested window")
        let views = previewDescendants(of: host)
        let splits = views.compactMap { $0 as? NSSplitView }
        try #require(!splits.isEmpty, "Exercise the real NavigationSplitView")
        let workspace = try #require(views.first { $0 is WindowAttachmentView },
            "The production root's background observes its actual layout bounds")
        #expect(host.safeAreaInsets.top >= 22,
            "The test must reserve an actual titlebar/toolbar safe area")
        let usable = host.safeAreaRect.insetBy(dx: -1, dy: -1)
        let outer = host.bounds.insetBy(dx: -1, dy: -1)
        let workspaceFrame = workspace.convert(workspace.bounds, to: host)
        #expect(workspaceFrame.width > 0 && workspaceFrame.height > 0)
        #expect(usable.contains(workspaceFrame),
            "Workspace escaped the toolbar-safe content area: workspace=\(workspaceFrame), usable=\(usable)")
        for split in splits {
            let frame = split.convert(split.bounds, to: host)
            #expect(outer.contains(frame), "Navigation split escaped the host: split=\(frame), host=\(host.bounds)")
        }
        let viewport = scroll.convert(scroll.bounds, to: host)
        #expect(viewport.width > 0 && viewport.height > 0)
        #expect(usable.contains(viewport), "Loaded image viewport must remain below the titlebar and within the window")
    }
    #expect(!window.isVisible, "Layout verification must not present or steal focus from the user's window")
}
#endif
