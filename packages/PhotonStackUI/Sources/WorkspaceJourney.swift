import PhotonStackAppCore
import SwiftUI

enum WorkspaceJourneyStep: Int, CaseIterable {
    case importPhotos, review, stack, finish, export

    func title(chinese: Bool) -> String {
        switch self {
        case .importPhotos: chinese ? "导入照片" : "Import"
        case .review: chinese ? "分析与筛片" : "Review"
        case .stack: chinese ? "堆栈" : "Stack"
        case .finish: chinese ? "后期调整" : "Finish"
        case .export: chinese ? "导出成片" : "Export"
        }
    }
}

/// Derived from real outputs, never persisted as a second source of workflow truth.
struct WorkspaceJourneyState {
    var lightCount: Int
    var analysisCurrent: Bool
    var resultCurrent: Bool
    var finishingRequested: Bool

    var step: WorkspaceJourneyStep {
        if finishingRequested || resultCurrent { return .finish }
        if lightCount < 3 { return .importPhotos }
        return analysisCurrent ? .stack : .review
    }

    var next: WorkspaceJourneyStep {
        step == .finish ? .export : (step == .stack ? .review : step)
    }

    static func lastImageExport(in jobs: [ProcessingJob]) -> URL? {
        jobs.last { $0.title == "Export Image" && $0.status == .succeeded && $0.outputURL != nil }?.outputURL
    }

    func completed(_ step: WorkspaceJourneyStep) -> Bool {
        switch step {
        case .importPhotos: lightCount > 0
        case .review: analysisCurrent
        case .stack: resultCurrent
        case .finish, .export: false // Editing and exporting are explicit user choices.
        }
    }
}

extension PhotonStackWorkspaceModel {
    var journeyAnalysisIsCurrent: Bool {
        DeepSkyReview.analysisIsCurrent(project.deepSkySettings?.lastReport, recipe: try? deepSkyRecipeText())
    }

    var journeyResultIsCurrent: Bool {
        guard let report = project.deepSkySettings?.lastReport,
              report.success, !report.analysisOnly, latestDeepSkyResultURL != nil else { return false }
        return DeepSkyReview.sameResultRecipe(try? deepSkyRecipeText(), report.recipe)
    }

    var journeyKeptCount: Int {
        guard journeyAnalysisIsCurrent, let report = project.deepSkySettings?.lastReport else { return lightAssets.count }
        return DeepSkyReview.keptCount(report.frames, settings: deepSkySettings)
    }
}

/// Both workflows use the same navigation, completion markers and keyboard-accessible actions.
struct JourneyStepStrip<Step: CaseIterable & Hashable & RawRepresentable>: View where Step.RawValue == Int {
    let current: Step
    let chinese: Bool
    let identifier: String
    let title: (Step) -> String
    let completed: (Step) -> Bool
    let unavailable: (Step) -> Bool
    let onSelect: (Step) -> Void

    var body: some View {
        ViewThatFits(in: .horizontal) {
            steps
            ScrollView(.horizontal) { steps }
        }
        .frame(maxWidth: .infinity, alignment: .leading)
    }

    private var steps: some View {
        HStack(spacing: 8) {
            ForEach(Array(Step.allCases), id: \.self) { step in
                if step.rawValue > 0 {
                    Image(systemName: "chevron.right").font(.caption2).foregroundStyle(.tertiary)
                        .accessibilityHidden(true)
                }
                Button { onSelect(step) } label: {
                    HStack(spacing: 6) {
                        Image(systemName: completed(step) ? "checkmark.circle.fill" : "\(step.rawValue + 1).circle")
                        Text(title(step)).fontWeight(current == step ? .semibold : .regular)
                    }
                    .padding(.vertical, 5).padding(.horizontal, 7)
                    .foregroundStyle(current == step ? Color.accentColor : Color.secondary)
                    .background(current == step ? Color.accentColor.opacity(0.1) : .clear, in: Capsule())
                }
                .buttonStyle(.plain)
                .accessibilityIdentifier("\(identifier).\(step.rawValue)")
                .accessibilityLabel(title(step))
                .accessibilityValue(current == step ? (chinese ? "当前阶段" : "Current step")
                    : completed(step) ? (chinese ? "已完成" : "Completed") : "")
                .disabled(unavailable(step))
            }
        }
        .fixedSize(horizontal: true, vertical: false)
    }
}

struct WorkspaceJourneyBar: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let finishingRequested: Bool
    let onSelect: (WorkspaceJourneyStep) -> Void
    private var chinese: Bool { model.language == .simplifiedChinese }
    var body: some View {
        let state = WorkspaceJourneyState(lightCount: model.lightAssets.count,
            analysisCurrent: model.journeyAnalysisIsCurrent, resultCurrent: model.journeyResultIsCurrent,
            finishingRequested: finishingRequested)
        VStack(alignment: .leading, spacing: 10) {
            JourneyStepStrip(current: state.step, chinese: chinese, identifier: "journey.step",
                title: { $0.title(chinese: chinese) }, completed: state.completed,
                unavailable: { unavailable($0, lightCount: state.lightCount) }, onSelect: onSelect)
            if !model.project.assets.isEmpty {
                HStack(spacing: 12) {
                    VStack(alignment: .leading, spacing: 3) {
                        if let title = statusTitle(state) {
                            Text(title).font(.callout.weight(.semibold))
                        }
                        if state.step == .stack, !model.hasActiveProcessing, let report = model.deepSkySettings.lastReport {
                            let kept = DeepSkyReview.keptCount(report.frames, settings: model.deepSkySettings)
                            let excluded = report.frames.count - kept
                            HStack(spacing: 8) {
                                Text(chinese ? "保留 \(kept) 张" : "\(kept) kept")
                                    .foregroundStyle(.secondary)
                                if excluded > 0 {
                                    Button(chinese ? "查看排除的 \(excluded) 张" : "View \(excluded) excluded") {
                                        NotificationCenter.default.post(name: .photonStackOpenDeepSky, object: DeepSkyReviewFilter.excluded)
                                    }
                                    .buttonStyle(.link)
                                    .accessibilityIdentifier("journey.review-excluded")
                                }
                            }.font(.caption)
                        } else if let detail = statusDetail(state) {
                            Text(detail).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                        }
                    }
                    Spacer(minLength: 0)
                    if model.hasActiveProcessing {
                        ProgressView().controlSize(.small)
                        Button(chinese ? "取消" : "Cancel") { model.cancelCurrentTask() }
                            .disabled(!model.canCancel)
                    } else {
                        if state.lightCount >= 3, !state.resultCurrent, !finishingRequested {
                            Button(chinese ? "一键成片" : "Stack & finish") { model.startDeepSkyWorkflow(analyzeOnly: false) }
                                .accessibilityIdentifier("journey.stack-and-finish")
                        }
                        if state.step != .finish, model.canProcessCurrentInput {
                            Button(chinese ? "直接后期" : "Edit current image") { onSelect(.finish) }
                                .help(chinese ? "跳过筛片与堆栈，调整当前图像；适合已堆好的成片。" : "Skip stacking and edit the current image, for existing masters or single photos.")
                                .accessibilityIdentifier("journey.edit-current")
                        }
                        Button(primaryTitle(state)) { onSelect(state.next) }
                            .buttonStyle(.borderedProminent)
                            .accessibilityIdentifier("journey.next")
                            .disabled(unavailable(state.next, lightCount: state.lightCount))
                    }
                }
            }
            if let error = model.errorMessage, !error.isEmpty {
                ProcessingErrorNotice(message:model.localizedMessage(error),chinese:chinese)
                    .accessibilityIdentifier("journey.error")
            }
            if let output = WorkspaceJourneyState.lastImageExport(in: model.jobs) {
                Label(chinese ? "上次已导出：\(output.lastPathComponent)" : "Last export: \(output.lastPathComponent)", systemImage: "checkmark.circle.fill")
                    .font(.caption).foregroundStyle(.green)
                    .lineLimit(1).truncationMode(.middle)
                    .help(output.path)
                    .accessibilityLabel(chinese ? "上次已导出：\(output.path)" : "Last export: \(output.path)")
                    .accessibilityIdentifier("journey.export-success")
            }
        }
        .padding(.horizontal, 16).padding(.vertical, 10)
        .background(.bar)
        .task { await model.loadDeepSkySchema() }
    }

    private func unavailable(_ step: WorkspaceJourneyStep, lightCount: Int) -> Bool {
        if model.hasActiveProcessing { return true }
        switch step {
        case .importPhotos: return false
        case .review, .stack: return lightCount < 3
        case .finish: return !model.canProcessCurrentInput
        case .export: return !model.canExportCurrentImage
        }
    }

    private func primaryTitle(_ state: WorkspaceJourneyState) -> String {
        switch state.step {
        case .importPhotos: chinese ? "继续导入照片…" : "Add photos…"
        case .review: chinese ? "下一步：分析与筛片" : "Next: analyze & review"
        case .stack: chinese ? "确认筛片并堆栈…" : "Review & stack…"
        case .finish, .export: chinese ? "导出成片…" : "Export image…"
        }
    }

    private func statusTitle(_ state: WorkspaceJourneyState) -> String? {
        if model.hasActiveProcessing { return chinese ? "正在处理…" : "Processing…" }
        if state.step == .finish { return nil }
        let lights = chinese ? "\(state.lightCount) 张亮场" : "\(state.lightCount) lights"
        guard calibrationCount > 0 else { return lights }
        return lights + (chinese ? " · \(calibrationCount) 张校准帧" : " · \(calibrationCount) calibration frames")
    }

    private var calibrationCount: Int { model.darkAssets.count + model.flatAssets.count + model.biasAssets.count }

    private func statusDetail(_ state: WorkspaceJourneyState) -> String? {
        if model.hasActiveProcessing { return model.progressMessage }
        if state.step == .finish {
            if model.editGraphNeedsReplay { return model.localized(.editGraphNeedsReplay) }
            if model.latestDeepSkyResultURL != nil && !state.resultCurrent {
                return chinese ? "当前显示上次成片。素材或参数已变更，请重新筛片并堆栈。" : "Inputs or settings changed; this is the previous result. Review and stack again to update it."
            }
            return nil
        }
        if state.lightCount < 3 { return chinese ? "堆栈至少需要 3 张同一天体的亮场。" : "Stacking needs at least 3 lights of one target." }
        return nil
    }
}
