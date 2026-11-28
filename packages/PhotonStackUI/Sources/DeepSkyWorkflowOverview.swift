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

/// Workbench actions with warnings only when the current result needs attention.
struct DeepSkyWorkflowOverview: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var reportIsCurrent: Bool {
        model.journeyAnalysisIsCurrent
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            if let notice {
                Text(notice).font(.callout).foregroundStyle(.secondary)
            }
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

    private var notice: String? {
        if model.isProcessing { return nil }
        if let report = model.project.deepSkySettings?.lastReport, !report.success {
            return chinese ? "上次处理未完成，请查看错误后重试。" : "The previous run failed. Check the error and retry."
        }
        if let report = model.project.deepSkySettings?.lastReport,
           report.success, !report.analysisOnly, model.latestDeepSkyResultURL != nil,
           DeepSkyReview.sameResultRecipe(model.displayedDeepSkyRecipe(), report.recipe) {
            return chinese ? "已处理 \(report.selectedCount) 张亮场，请检查成片。" : "Result ready from \(report.selectedCount) lights. Review the image."
        }
        if reportIsCurrent, model.canRefinishDeepSkyMaster {
            return chinese ? "可使用已有母片重新显影。" : "Reprocess the existing master to update the result."
        }
        if !reportIsCurrent, model.project.deepSkySettings?.lastReport != nil {
            return chinese ? "素材或参数已变化，请重新分析。" : "Inputs or settings changed. Analyze again."
        }
        return nil
    }
}
