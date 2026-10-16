import SwiftUI

/// Render scientific frames through the same CFA-aware path as frame review.
/// The display TIFF stays in a bounded temporary cache, never in the edit graph.
struct WorkspaceFITSPreview: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @ObservedObject var previews: DeepSkyReviewPreviewStore
    let input: URL
    @Binding var magnification: Double
    @Binding var fitToWindow: Bool
    @State private var loadedKey: String?
    @State private var preview: URL?
    @State private var failure: String?
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var requestKey: String { input.path + "|" + model.deepSkyReviewDecodeKey }

    var body: some View {
        VStack(spacing: 6) {
            ZStack {
                Color.black
                if loadedKey == requestKey, let preview {
                    NativeImagePreview(url: preview, magnification: $magnification, fitToWindow: $fitToWindow)
                } else if loadedKey == requestKey, let failure {
                    Text(failure).font(.callout).multilineTextAlignment(.center)
                        .padding().background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
                } else {
                    ProgressView(chinese ? "读取原片…" : "Loading original…")
                        .padding().background(.regularMaterial, in: RoundedRectangle(cornerRadius: 8))
                }
            }
            Text(chinese ? "单张拉伸预览 · 未对齐 · 不改原片" : "Stretched original · Unregistered · Source unchanged")
                .font(.caption).foregroundStyle(.secondary)
                .accessibilityIdentifier("workspace.single-frame-preview")
        }
        .task(id: requestKey) {
            do {
                let output = try await previews.load(input.path, model: model)
                try Task.checkCancellation()
                preview = output
                failure = nil
                loadedKey = requestKey
            } catch is CancellationError { }
            catch {
                guard !Task.isCancelled else { return }
                preview = nil
                failure = error.localizedDescription
                loadedKey = requestKey
            }
        }
    }
}
