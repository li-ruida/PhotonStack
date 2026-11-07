import PhotonStackAppCore
import SwiftUI

enum WorkspaceInspectorPage: String, CaseIterable {
    case processing, assets, tools, project

    func title(chinese: Bool) -> String {
        switch self {
        case .processing: chinese ? "处理" : "Process"
        case .assets: chinese ? "素材" : "Assets"
        case .tools: chinese ? "工具" : "Tools"
        case .project: chinese ? "项目" : "Project"
        }
    }
}

/// A short next-action summary; the workbench owns all pipeline configuration.
struct DeepSkyWorkflowOverview: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var reportIsCurrent: Bool {
        model.journeyAnalysisIsCurrent
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(chinese ? "导入 → 筛片 → 堆栈 → 修整与导出" : "Import → Review → Stack → Finish & export")
                .font(.caption).foregroundStyle(.secondary)
            HStack {
                Label("\(model.lightAssets.count) \(chinese ? "张亮场" : "lights")", systemImage: "photo.stack")
                Spacer()
                Text(chinese ? "校准帧 \(calibrationCount)" : "\(calibrationCount) calibration frames")
                    .font(.caption).foregroundStyle(.secondary)
            }
            Text(guidance).font(.callout)
            DeepSkyControl(model: model)
                .buttonStyle(.borderedProminent)
            if model.latestDeepSkyResultURL != nil {
                Button {
                    model.showLatestDeepSkyResult()
                } label: {
                    Label(chinese ? "查看堆栈结果" : "View stack result", systemImage: "photo")
                }
                .accessibilityIdentifier("workspace.show-stack-result")
                .disabled(!model.canModifyProcessingConfiguration)
            }
        }
        .task { await model.loadDeepSkySchema() }
    }

    private var calibrationCount: Int {
        model.darkAssets.count + model.biasAssets.count + model.flatAssets.count
    }

    private var guidance: String {
        if model.isProcessing {
            return chinese ? "正在处理，可在工作台查看进度或取消。" : "Processing. Open the workbench for progress or cancellation."
        }
        if model.lightAssets.count < 3 {
            return chinese ? "先导入至少 3 张亮场，工作台会引导你分析和筛片。" : "Import at least 3 lights, then analyze and review them in the workbench."
        }
        if let report = model.project.deepSkySettings?.lastReport, !report.success {
            return chinese ? "上次处理未完成。打开工作台查看失败原因，修正后重新分析；已有测量可供参考。"
                : "The previous run did not complete. Open the workbench for the failure details, then analyze again after correcting the issue. Existing measurements remain available for reference."
        }
        if let report = model.project.deepSkySettings?.lastReport,
           report.success, !report.analysisOnly, model.latestDeepSkyResultURL != nil,
           DeepSkyReview.sameResultRecipe(try? model.deepSkyRecipeText(), report.recipe) {
            return chinese ? "\(report.selectedCount) 张亮场的处理结果已生成，尚需画质复查。先检查全图和 100% 细节，再决定修整或导出。" : "Result available from \(report.selectedCount) lights; visual review is still needed. Inspect the full image and 100% detail before finishing or exporting."
        }
        let current = reportIsCurrent
        if current, model.canRefinishDeepSkyMaster {
            return chinese ? "修整参数已变化，当前显示的仍是上次成片。在工作台点击“从母版重新显影”即可，无需重新筛片和堆栈。" : "Finishing settings changed; the displayed image is the previous result. Reprocess the master in the workbench without analyzing or stacking again."
        }
        if current, let report = model.project.deepSkySettings?.lastReport {
            let kept = DeepSkyReview.keptCount(report.frames, settings: model.deepSkySettings)
            return chinese ? "已分析 \(report.frames.count) 张，当前保留 \(kept) 张、排除 \(report.frames.count - kept) 张。" : "\(report.frames.count) analyzed, \(kept) kept and \(report.frames.count - kept) excluded."
        }
        return chinese ? "素材已就绪。先分析星点、噪声和对齐质量，再决定保留哪些照片。" : "Inputs ready. Analyze stars, noise and registration before choosing frames."
    }
}
