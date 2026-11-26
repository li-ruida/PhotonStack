import PhotonStackAppCore
import SwiftUI

private enum MosaicJourneyStep: Int, CaseIterable {
    case importPanels, preview, build, finish, export

    func title(chinese: Bool) -> String {
        switch self {
        case .importPanels: chinese ? "导入天区" : "Import panels"
        case .preview: chinese ? "配准预览" : "Align & preview"
        case .build: chinese ? "生成拼接" : "Build mosaic"
        case .finish: chinese ? "后期调整" : "Finish"
        case .export: chinese ? "导出成片" : "Export"
        }
    }
}

/// Recheck file signatures when the selection, settings or actual products change,
/// keeping file reads out of progress updates and ordinary SwiftUI body evaluations.
private struct MosaicJourneyInputs: Equatable {
    let projectID: UUID
    let panels: [PhotonStackAsset]
    let settings: [String: String]
    let previewWidth: Int
    let reportURL: URL?
    let resultID: UUID?
}

struct MosaicJourneyBar: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let finishingRequested: Bool
    let onImport: () -> Void
    let onInspect: () -> Void
    let onFinish: () -> Void
    let onExport: () -> Void
    @State private var products = (preview: false, result: false)
    private var chinese: Bool { model.language == .simplifiedChinese }

    private var step: MosaicJourneyStep {
        if model.hasActiveProcessing, let job = model.latestJob, job.kind == .mosaic {
            return job.title == "Mosaic Preview" ? .preview : .build
        }
        if products.preview && model.isShowingMosaicPreview { return .build }
        if products.result {
            return finishingRequested && model.isWorkingOnLatestMosaic ? .export : .finish
        }
        if !model.canRunMosaic { return .importPanels }
        return products.preview ? .build : .preview
    }

    var body: some View {
        let panels = model.mosaicAssets
        let inputs = MosaicJourneyInputs(projectID: model.project.id, panels: panels, settings: model.mosaicStitchParameters,
            previewWidth: model.parameters.mosaicPreviewWidth, reportURL: model.mosaicQualityReportSourceURL,
            resultID: model.latestMosaicOperation?.id)
        let frames = panels.reduce(0) { $0 + ($1.mosaicFolder?.frames.count ?? 1) }
        VStack(alignment: .leading, spacing: 10) {
            JourneyStepStrip(current: step, chinese: chinese, identifier: "journey.mosaic-step",
                title: { $0.title(chinese: chinese) }, completed: completed, unavailable: unavailable, onSelect: select)
            if !model.project.assets.isEmpty {
                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(chinese ? "\(panels.count) 个天区 · \(frames) 帧" : "\(panels.count) panels · \(frames) frames")
                            .font(.callout.weight(.semibold))
                        if let detail {
                            Text(detail).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    Spacer(minLength: 0)
                    if model.hasActiveProcessing {
                        ProgressView().controlSize(.small)
                        Button(chinese ? "取消" : "Cancel") { model.cancelCurrentTask() }.disabled(!model.canCancel)
                    } else {
                        Button(primaryTitle) { select(step) }
                            .buttonStyle(.borderedProminent)
                            .accessibilityIdentifier(primaryIdentifier)
                            .disabled(unavailable(step))
                    }
                }
            }
            if let error = model.errorMessage, !error.isEmpty {
                Label(model.localizedMessage(error), systemImage: "exclamationmark.triangle")
                    .font(.caption).foregroundStyle(.red).lineLimit(3).textSelection(.enabled)
            }
            if step == .export, let job = model.latestJob, job.title == "Export Image", job.status == .succeeded,
               let output = job.outputURL {
                Label(chinese ? "已导出 \(output.lastPathComponent)" : "Exported \(output.lastPathComponent)",
                      systemImage: "checkmark.circle.fill").font(.caption).foregroundStyle(.green)
                    .lineLimit(1).help(output.path)
            }
        }
        .padding(.horizontal, 16).padding(.vertical, 10).background(.bar)
        .onChange(of: inputs, initial: true) { _, value in products = model.mosaicJourneyProducts(for: value.panels) }
    }

    private func completed(_ candidate: MosaicJourneyStep) -> Bool {
        switch candidate {
        case .importPanels: model.canRunMosaic
        case .preview: products.preview || products.result
        case .build: products.result
        case .finish: step == .export
        case .export: false
        }
    }

    private func unavailable(_ candidate: MosaicJourneyStep) -> Bool {
        if model.hasActiveProcessing { return true }
        switch candidate {
        case .importPanels: return false
        case .preview, .build: return !model.canRunMosaic
        case .finish: return !products.result && !model.isShowingMosaicResult
        case .export: return (!products.result && !model.isShowingMosaicResult) || !model.canExportCurrentImage
        }
    }

    private func select(_ candidate: MosaicJourneyStep) {
        guard !unavailable(candidate) else { return }
        switch candidate {
        case .importPanels: onInspect(); onImport()
        case .preview: onInspect(); model.startRunMosaicPreview()
        case .build:
            onInspect()
            if products.result {
                model.showLatestMosaicResult()
            } else {
                model.startRunMosaic()
            }
        case .finish: onFinish()
        case .export: onExport()
        }
    }

    private var primaryTitle: String {
        switch step {
        case .importPanels: chinese ? "导入天区…" : "Add panels…"
        case .preview: chinese ? "预览拼接" : "Preview mosaic"
        case .build:
            if products.result { chinese ? "查看拼接成片" : "Show full mosaic" }
            else { chinese ? "生成拼接成片" : "Build full mosaic" }
        case .finish: chinese ? "进入后期调整" : "Edit mosaic"
        case .export: chinese ? "导出成片…" : "Export image…"
        }
    }

    private var primaryIdentifier: String {
        switch step {
        case .importPanels: "journey.mosaic-import"
        case .preview: "journey.mosaic-preview"
        case .build: "journey.mosaic-build"
        case .finish: "journey.mosaic-finish"
        case .export: "journey.mosaic-export"
        }
    }

    private var detail: String? {
        if model.hasActiveProcessing {
            let progress = [model.stackingMosaicPanel, model.progressMessage].compactMap { $0 }.filter { !$0.isEmpty }.joined(separator: " · ")
            return progress.isEmpty ? nil : progress
        }
        switch step {
        case .importPanels:
            return model.missingMosaicPanelHint ?? (chinese ? "至少需要两个相互重叠的天区。" : "At least two overlapping panels required.")
        case .preview:
            if model.latestMosaicOperation != nil {
                return chinese ? "天区或拼接设置已变化，请重新检查配准预览。" : "The panels or stitch settings have changed. Check the alignment again."
            }
            return nil
        case .build:
            if products.result {
                return nil
            }
            return chinese ? "请先检查重叠区域的星点是否对齐。" : "Check star alignment in the overlap before building."
        case .finish, .export:
            return nil
        }
    }
}
