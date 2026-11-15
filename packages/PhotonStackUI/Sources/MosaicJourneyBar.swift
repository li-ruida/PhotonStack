import SwiftUI

struct MosaicJourneyBar: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let onImportFolders: () -> Void
    let onImport: () -> Void
    let onFinish: () -> Void
    let onExport: () -> Void
    private var chinese: Bool { model.language == .simplifiedChinese }

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack {
                Text(chinese ? "面板 \(model.mosaicAssets.count) 张" : "\(model.mosaicAssets.count) panels")
                    .font(.callout.weight(.medium))
                Button(chinese ? "导入天区文件夹…" : "Add panel folders…", action: onImportFolders)
                    .accessibilityIdentifier("journey.mosaic-folders")
                Button(chinese ? "导入母片…" : "Add masters…", action: onImport)
                Button(chinese ? "预览拼接" : "Preview mosaic") { model.startRunMosaicPreview() }
                    .accessibilityIdentifier("journey.mosaic-preview")
                    .disabled(!model.canRunMosaic)
                Button(chinese ? "生成拼接成片" : "Build full mosaic") { model.startRunMosaic() }
                    .buttonStyle(.borderedProminent)
                    .accessibilityIdentifier("journey.mosaic-build")
                    .disabled(!model.canRunMosaic)
                if model.isShowingMosaicResult {
                    Button(chinese ? "后期调整" : "Finish", action: onFinish)
                    Button(chinese ? "导出成片…" : "Export…", action: onExport)
                        .disabled(!model.canExportCurrentImage)
                }
                Spacer()
            }
            .disabled(model.hasActiveProcessing)
            if model.hasActiveProcessing {
                HStack {
                    ProgressView().controlSize(.small)
                    if let panel = model.stackingMosaicPanel { Text(panel) }
                    Text(model.progressMessage ?? (chinese ? "正在拼接…" : "Stitching…"))
                    Button(chinese ? "取消" : "Cancel") { model.cancelCurrentTask() }.disabled(!model.canCancel)
                }.font(.caption)
            } else if let error = model.errorMessage, !error.isEmpty {
                Text(model.localizedMessage(error)).font(.caption).foregroundStyle(.red)
            }
        }
        .padding(.horizontal, 14).padding(.vertical, 6).background(.bar)
    }
}
