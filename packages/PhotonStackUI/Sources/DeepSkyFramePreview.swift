import AppKit
import PhotonStackAppCore
import SwiftUI

@MainActor
final class DeepSkyReviewPreviewStore: ObservableObject {
    private let directory = FileManager.default.temporaryDirectory.appendingPathComponent("PhotonStackFrameReview-\(UUID().uuidString)")
    private var cache: [String: URL] = [:]
    private var order: [String] = []

    private func key(_ input: String, model: PhotonStackWorkspaceModel) -> String {
        let attributes = try? FileManager.default.attributesOfItem(atPath: input)
        return "\(input)|\((attributes?[.modificationDate] as? Date ?? .distantPast).timeIntervalSince1970)|\(attributes?[.size] ?? 0)|\(model.deepSkyReviewDecodeKey)"
    }

    func cachedURL(_ input: String, model: PhotonStackWorkspaceModel) -> URL? {
        let key = key(input, model: model)
        guard let url = cache[key], FileManager.default.fileExists(atPath: url.path) else { return nil }
        order.removeAll { $0 == key }
        order.append(key)
        return url
    }

    func load(_ input: String, model: PhotonStackWorkspaceModel) async throws -> URL {
        try Task.checkCancellation()
        if let url = cachedURL(input, model: model) { return url }
        let key = key(input, model: model)
        try FileManager.default.createDirectory(at: directory, withIntermediateDirectories: true)
        let output = directory.appendingPathComponent(UUID().uuidString + ".tiff")
        do {
            try await model.makeDeepSkyReviewPreview(input: URL(fileURLWithPath: input), output: output)
            try Task.checkCancellation()
            cache[key] = output
            order.append(key)
            while order.count > 4 {
                if let expired = cache.removeValue(forKey: order.removeFirst()) { try? FileManager.default.removeItem(at: expired) }
            }
            return output
        } catch {
            try? FileManager.default.removeItem(at: output)
            throw error
        }
    }
    deinit { try? FileManager.default.removeItem(at: directory) }
}

struct DeepSkyFramePreview: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let frame: DeepSkyReport.Frame
    let reference: DeepSkyReport.Frame?
    @StateObject private var previews = DeepSkyReviewPreviewStore()
    @State private var currentURL: URL?
    @State private var referenceURL: URL?
    @State private var showingReference = false
    @State private var loading = false
    @State private var failure: String?
    @State private var magnification = 1.0
    @State private var fit = true
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var displayedURL: URL? { showingReference ? referenceURL : currentURL }

    var body: some View {
        VStack(spacing: 6) {
            HStack(spacing: 8) {
                Label(showingReference ? (chinese ? "参考帧 #\((reference?.index ?? 0) + 1)" : "Reference #\((reference?.index ?? 0) + 1)")
                      : (chinese ? "原片 #\(frame.index + 1)" : "Original #\(frame.index + 1)"), systemImage: showingReference ? "star" : "photo")
                    .font(.callout.weight(.medium))
                Spacer()
                Button(chinese ? "适配" : "Fit") { fit = true }.help(chinese ? "查看整张原片" : "Fit the full frame")
                Button("1:1") { fit = false; magnification = 1 }.help(chinese ? "一个原片像素对应一个屏幕像素" : "One source pixel per screen pixel")
                Button("2×") { fit = false; magnification = 2 }
                Button {
                    showingReference.toggle()
                } label: {
                    Label(showingReference ? (chinese ? "返回当前" : "Current frame") : (chinese ? "对照参考帧" : "Reference"), systemImage: "arrow.left.arrow.right")
                }
                .disabled(referenceURL == nil || frame.reference || loading)
                .help(chinese ? "切换原片和参考帧，保持缩放；预览未配准" : "Switch at the same zoom; previews are not registered")
                .accessibilityIdentifier("review.compare-reference")
            }.buttonStyle(.bordered).controlSize(.small)
            ZStack {
                Color.black
                if let url = displayedURL {
                    NativeImagePreview(url: url, magnification: $magnification, fitToWindow: $fit)
                        .opacity(loading || failure != nil ? 0 : 1)
                }
                if loading {
                    ProgressView(chinese ? "读取原片…" : "Loading original…")
                        .padding(14).background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
                } else if let failure {
                    VStack(spacing: 8) {
                        Image(systemName: "exclamationmark.triangle")
                        Text(failure).font(.caption).multilineTextAlignment(.center)
                        Button(chinese ? "在访达中显示原片" : "Show original in Finder") {
                            NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: frame.input)])
                        }
                    }.padding().background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
                }
            }.frame(minHeight: 190).clipShape(RoundedRectangle(cornerRadius: 6))
            Text(chinese ? "单张拉伸预览 · 未对齐 · 不改原片；用于检查星形与轨迹，不直接比较亮度。" : "Stretched original, unregistered. Inspect star shape and trails; brightness is not directly comparable.")
                .font(.caption).foregroundStyle(.secondary).frame(maxWidth: .infinity, alignment: .leading)
        }
        .task(id: frame.input + "|" + model.deepSkyReviewDecodeKey) {
            referenceURL = nil; showingReference = false; failure = nil; loading = true
            do {
                if previews.cachedURL(frame.input, model: model) == nil {
                    try await Task.sleep(for: .milliseconds(100))
                }
                let current = try await previews.load(frame.input, model: model)
                try Task.checkCancellation()
                currentURL = current; loading = false
                if let reference, reference.input != frame.input {
                    // A failed reference preview must not hide the current frame.
                    let other = try? await previews.load(reference.input, model: model)
                    try Task.checkCancellation()
                    referenceURL = other
                }
            } catch is CancellationError { }
            catch { if !Task.isCancelled { failure = error.localizedDescription; loading = false } }
        }
    }
}
