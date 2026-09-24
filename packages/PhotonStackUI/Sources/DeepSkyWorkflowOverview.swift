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
        guard let report = model.deepSkySettings.lastReport else { return false }
        return DeepSkyReview.sameAnalysisRecipe(try? model.deepSkyRecipeText(), report.recipe)
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
                .disabled(!model.canModifyWorkspaceProducts)
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
        if let report = model.deepSkySettings.lastReport,
           DeepSkyReview.sameResultRecipe(try? model.deepSkyRecipeText(), report.recipe),
           report.success, !report.analysisOnly, model.latestDeepSkyResultURL != nil {
            return chinese ? "已用 \(report.selectedCount) 张亮场完成堆栈。查看结果后，可继续修整或导出。" : "Stack completed with \(report.selectedCount) lights. View the result, then finish or export."
        }
        if reportIsCurrent, model.canRefinishDeepSkyMaster {
            return chinese ? "修整参数已变化，当前显示的仍是上次成片。在工作台更新背景与降噪即可，无需重新筛片和堆栈。" : "Finishing settings changed; the displayed image is the previous result. Reprocess the master in the workbench without analyzing or stacking again."
        }
        if reportIsCurrent, let report = model.deepSkySettings.lastReport {
            let kept = report.frames.filter { DeepSkyReview.kept($0, settings: model.deepSkySettings) }.count
            return chinese ? "已分析 \(report.inputCount) 张，当前保留 \(kept) 张。可检查异常原片或开始堆栈。" : "\(report.inputCount) analyzed, \(kept) kept. Review flagged frames or start stacking."
        }
        return chinese ? "素材已就绪。先分析星点、噪声和对齐质量，再决定保留哪些照片。" : "Inputs ready. Analyze stars, noise and registration before choosing frames."
    }
}
