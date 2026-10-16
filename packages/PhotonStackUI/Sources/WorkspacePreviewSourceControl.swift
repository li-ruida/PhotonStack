import SwiftUI

/// Keep the return path beside the canvas, independent of inspector navigation.
struct WorkspacePreviewSourceControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let imageURL: URL?
    private var chinese: Bool { model.language == .simplifiedChinese }

    var body: some View {
        HStack(spacing: 8) {
            ControlGroup {
                Toggle(isOn: Binding(
                    get: { model.isShowingLatestDeepSkyResult },
                    set: { if $0 { model.showLatestDeepSkyResult() } }
                )) {
                    Label(chinese ? "堆栈结果" : "Stack result", systemImage: "photo.stack")
                }
                .help(chinese ? "返回最近完成的堆栈成片" : "Return to the latest completed stack")
                .accessibilityIdentifier("workspace.preview-source.stack")

                Toggle(isOn: Binding(
                    get: { model.isShowingSelectedOriginal },
                    set: { if $0 { model.showSelectedOriginal() } }
                )) {
                    Label(chinese ? "单张原片" : "Single frame", systemImage: "photo")
                }
                .disabled(!model.canResetPreviewToOriginal)
                .help(model.selectedAsset?.displayName ?? (chinese ? "先选择一张素材" : "Select a frame first"))
                .accessibilityIdentifier("workspace.preview-source.original")
            }
            .toggleStyle(.button)
            .fixedSize()
            .disabled(!model.canModifyProcessingConfiguration)

            Spacer(minLength: 8)
            if let imageURL {
                Text(imageURL.lastPathComponent)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(imageURL.path)
                    .accessibilityLabel("\(model.localized(.currentImageLabel)): \(imageURL.lastPathComponent)")
                    .accessibilityIdentifier("workspace.current-image")
            }
        }
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.bar)
    }
}
