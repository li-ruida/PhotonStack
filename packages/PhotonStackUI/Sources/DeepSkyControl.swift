import AppKit
import PhotonStackAppCore
import SwiftUI
import UniformTypeIdentifiers

struct DeepSkyControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    var context = "inspector"
    private var chinese: Bool { model.language == .simplifiedChinese }
    var body: some View {
        Button {
            NotificationCenter.default.post(name: .photonStackOpenDeepSky, object: nil)
        } label: {
            Label(chinese ? "深空筛片与堆栈" : "Deep Sky Assessment & Stack", systemImage: "line.3.horizontal.decrease.circle")
        }
        .accessibilityIdentifier("deep-sky.open.\(context)")

    }
}

struct DeepSkyRecipeSheet: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var presented: Bool
    @Binding var reviewFilter: DeepSkyReviewFilter?
    var onImport: () -> Void
    @State private var expandedSettings: Set<String> = ["selection", "stack"]
    @State private var tab = 1
    @State private var reviewQuery = ""
    @State private var openingScope: DeepSkyReviewOpeningScope
    @State private var checkingSchema = true
    @State private var protectionPresented = false
    @State private var stackSetupExpanded = false
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var report: DeepSkyReport? { model.project.deepSkySettings?.lastReport }
    init(model: PhotonStackWorkspaceModel, presented: Binding<Bool>, reviewFilter: Binding<DeepSkyReviewFilter?>,
         onImport: @escaping () -> Void) {
        self.model = model
        _presented = presented
        _reviewFilter = reviewFilter
        self.onImport = onImport
        _openingScope = State(initialValue: DeepSkyReviewOpeningScope(settings: model.deepSkySettings))
    }

    private var processingPresentation: DeepSkyProcessingPresentation {
        DeepSkyProcessingPresentation(job: model.jobs.last(where: { $0.status == .running }),
            message: model.progressMessage, fraction: model.progressFraction, chinese: chinese)
    }
    var body: some View {
        let lightCount = model.lightAssets.count
        let settings = model.project.deepSkySettings ?? DeepSkySettings()
        let reportIsCurrent = model.journeyAnalysisIsCurrent
        let resultIsCurrent = model.journeyResultIsCurrent
        let canReview = reportIsCurrent || DeepSkyReview.canRecoverSelection(report, recipe: try? model.deepSkyRecipeText())
        let plannedCount = canReview ? DeepSkyReview.keptCount(report?.frames ?? [], settings: settings) : lightCount
        let minimumKept = DeepSkyReview.minimumKept(settings: settings, count: lightCount)
        let tooFew = plannedCount < minimumKept
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(chinese ? "深空筛片与堆栈" : "Deep Sky Assessment & Stack").font(.title2)
                Spacer()
                Button(chinese ? "关闭" : "Close") { presented = false }.keyboardShortcut(.cancelAction)
            }
            Text(chinese ? "2 分析与筛片 → 3 堆栈 → 4 后期调整。排除只影响这次堆栈，不会删除原片。" : "2 Review → 3 Stack → 4 Finish. Excluding frames never deletes originals.")
                .font(.callout).foregroundStyle(.secondary)
            if resultIsCurrent && !model.isProcessing, let report {
                DeepSkyResultReviewView(report: report, chinese: chinese, viewImage: {
                    model.showLatestDeepSkyResult()
                    presented = false
                }, editFinishing: {
                    expandedSettings = ["background", "denoise", "develop"]
                    tab = 0
                }, reviewFrames: {
                    reviewQuery = ""
                    reviewFilter = report.frames.contains(where: DeepSkyReview.attention) ? .attention : .all
                    tab = 1
                })
                .accessibilityIdentifier("deep-sky.stack-complete")
            }
            Picker(chinese ? "工作区" : "Workspace", selection: $tab) {
                Text(chinese ? "筛片工作台" : "Frame review").tag(1)
                Text(chinese ? "处理参数" : "Processing settings").tag(0)
            }.pickerStyle(.segmented).labelsHidden()
            if report == nil || tab == 0 {
                StackModeControl(model: model)
            } else {
                DisclosureGroup(chinese ? "堆栈模式与对齐设置" : "Stack mode and alignment", isExpanded: $stackSetupExpanded) {
                    StackModeControl(model: model).padding(.top, 6)
                }.font(.callout)
            }
            ZStack {
                reviewContent(reportIsCurrent: reportIsCurrent, canReview: canReview, settings: settings)
                    .opacity(tab == 1 ? 1 : 0)
                    .allowsHitTesting(tab == 1)
                    .disabled(tab != 1)
                    .accessibilityHidden(tab != 1)
                if tab == 0 { settingsContent }
            }
            if model.isProcessing && (report != nil || tab == 0) {
                DeepSkyProcessingView(presentation: processingPresentation, compact: true)
            }
            if !model.isProcessing && !model.lightAssets.isEmpty {
                DeepSkyPreflightView(model: model)
            }
            if let error = model.errorMessage {
                Text(model.localizedMessage(error)).font(.caption).foregroundStyle(.red).lineLimit(3)
            }
            ViewThatFits(in: .horizontal) {
                HStack {
                    recipeControls(plannedCount: plannedCount, minimumKept: minimumKept)
                    Spacer(minLength: 8)
                    previousResultControls(resultIsCurrent: resultIsCurrent)
                    runControls(reportIsCurrent: reportIsCurrent, resultIsCurrent: resultIsCurrent,
                                plannedCount: plannedCount, tooFew: tooFew)
                }
                VStack(alignment: .trailing, spacing: 8) {
                    HStack {
                        recipeControls(plannedCount: plannedCount, minimumKept: minimumKept)
                        Spacer(minLength: 8)
                        previousResultControls(resultIsCurrent: resultIsCurrent)
                    }
                    HStack {
                        Spacer(minLength: 0)
                        runControls(reportIsCurrent: reportIsCurrent, resultIsCurrent: resultIsCurrent,
                                    plannedCount: plannedCount, tooFew: tooFew)
                    }
                }
            }
        }.padding(16)
            .frame(minWidth: 700, idealWidth: 1180, maxWidth: .infinity,
                   minHeight: 680, idealHeight: 820, maxHeight: .infinity)
            .task {
                await model.loadDeepSkySchema()
                if !Task.isCancelled { checkingSchema = false }
            }
            .sheet(isPresented: $protectionPresented) {
                if let report { BackgroundProtectionEditor(model: model, report: report) }
            }
    }

    @ViewBuilder private func recipeControls(plannedCount: Int, minimumKept: Int) -> some View {
        Menu(chinese ? "配方" : "Recipe") {
            Button(chinese ? "导入配方" : "Import recipe") { importRecipe() }
            Button(chinese ? "导出配方" : "Export recipe") { exportRecipe() }
            Divider()
            Button(chinese ? "恢复默认处理参数" : "Reset processing settings") { model.resetDeepSkySettings() }
        }.disabled(model.isProcessing)
        .fixedSize()
        if plannedCount < minimumKept {
            Text(chinese ? "当前保留 \(plannedCount) 张，至少需要 \(minimumKept) 张。请增加保留的亮场。" : "\(plannedCount) kept; at least \(minimumKept) required. Add or restore light frames.")
                .font(.caption).foregroundStyle(.orange)
        }
    }

    @ViewBuilder private func previousResultControls(resultIsCurrent: Bool) -> some View {
        if model.latestDeepSkyResultURL != nil && !resultIsCurrent {
            ResultComparisonControl(model: model, context: "deep-sky")
            Button(chinese ? "查看堆栈结果" : "View stack result") {
                model.showLatestDeepSkyResult()
                presented = false
            }
            .accessibilityIdentifier("deep-sky.show-result")
            .disabled(!model.canModifyProcessingConfiguration)
        }
    }

    @ViewBuilder private func runControls(reportIsCurrent: Bool, resultIsCurrent: Bool,
                                          plannedCount: Int, tooFew: Bool) -> some View {
        if model.isProcessing {
            Button(chinese ? "取消处理" : "Cancel") { model.cancelCurrentTask() }
        } else {
            if model.canRefinishDeepSkyMaster {
                Button(chinese ? "从母版重新显影" : "Reprocess master") { model.startRefinishDeepSkyMaster() }
                    .accessibilityIdentifier("deep-sky.refinish")
                    .help(chinese ? "复用线性母片，只更新裁边、背景、降噪和显影；素材和堆栈参数必须保持一致。" : "Reuse the linear master for crop, background, denoising and development. Inputs and stack settings must match.")
            }
            if reportIsCurrent {
                Button(chinese ? "重新分析" : "Analyze again") { analyze() }
                    .disabled(model.lightAssets.count < 3 || model.deepSkySchema == nil)
                if resultIsCurrent {
                    Button(chinese ? "下一步：后期调整" : "Next: finish image") {
                        model.showLatestDeepSkyResult()
                        presented = false
                        NotificationCenter.default.post(name: .photonStackOpenFinishing, object: nil)
                    }.buttonStyle(.borderedProminent)
                        .accessibilityIdentifier("deep-sky.continue-finishing")
                } else {
                    Button(chinese ? "确认并堆栈 \(plannedCount) 张" : "Confirm & stack \(plannedCount) frames") {
                        tab = 1; model.startDeepSkyWorkflow(analyzeOnly: false)
                    }
                    .buttonStyle(.borderedProminent).disabled(model.deepSkySchema == nil || tooFew)
                    .accessibilityIdentifier("deep-sky.confirm-stack")
                    .help(chinese ? "按当前标记运行；引擎会重新核验质量。" : "Run with current decisions; the engine rechecks quality.")
                }
            } else {
                Button(chinese ? "开始分析照片" : "Analyze photos") { analyze() }
                    .buttonStyle(.borderedProminent)
                    .disabled(model.lightAssets.count < 3 || model.deepSkySchema == nil)
                    .accessibilityIdentifier("deep-sky.analyze")
            }
        }
    }

    private func analyze() {
        tab = 1
        reviewQuery = ""
        openingScope = DeepSkyReviewOpeningScope(settings: model.deepSkySettings)
        reviewFilter = nil
        model.startDeepSkyWorkflow(analyzeOnly: true)
    }

    private func selectedReviewFilter(report: DeepSkyReport, current: Bool) -> Binding<DeepSkyReviewFilter> {
        let filter = openingScope.filter(report: report, isCurrent: current, requested: reviewFilter)
        return Binding(get: { filter }, set: { reviewFilter = $0 })
    }

    private var settingsContent: some View {
        DeepSkySettingsView(model: model, expandedGroups: $expandedSettings,
                            onEditProtection: { protectionPresented = true })
    }

    @ViewBuilder private func reviewContent(reportIsCurrent: Bool, canReview: Bool, settings: DeepSkySettings) -> some View {
        VStack(alignment: .leading, spacing: 10) {
                if report != nil && model.deepSkySchema == nil && checkingSchema {
                    ProgressView().accessibilityLabel(chinese ? "读取筛片设置" : "Loading selection settings")
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                } else if let report {
                    if report.frames.contains(where: { $0.starsMeasured && $0.fwhmEstimator != "native-gaussian-v1" }) {
                        Label(chinese ? "星点测量已升级；重新分析可更新星宽、模糊程度和参考帧建议。" : "Star measurements have improved. Analyze again to update widths, blur checks and reference-frame recommendations.", systemImage: "viewfinder")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    if !reportIsCurrent {
                        let status: String = if canReview {
                            chinese ? "保留帧数未达最低要求。可调整标记后重新分析。" : "Too few frames were kept. Adjust the decisions and analyze again."
                        } else if !report.success {
                            chinese ? "上次处理未完成。下方保留了当时的报告，请修正问题后重新分析。" : "The previous run did not finish. Review its report, correct the problem and analyze again."
                        } else {
                            chinese ? "素材或分析参数已变化。下方显示上次筛片记录，重新分析后可修改标记。" : "Inputs or analysis settings changed. This is the previous selection. Analyze again to edit decisions."
                        }
                        Label(status, systemImage: "arrow.clockwise.circle")
                            .font(.caption).foregroundStyle(.orange)
                    } else if report.success, !report.analysisOnly,
                              !DeepSkyReview.sameResultRecipe(try? model.deepSkyRecipeText(), report.recipe) {
                        Label(model.canRefinishDeepSkyMaster
                            ? (chinese ? "修整参数已变化；筛片仍有效。点击“从母版重新显影”生成新成片，当前显示仍是上次结果。" : "Finishing settings changed; frame review is still valid. Reprocess the master to update the displayed result.")
                            : (chinese ? "当前选择或参数尚未生成成片，显示的仍是上次结果。" : "Current decisions have not been rendered; the displayed image is the previous result."), systemImage: "photo.badge.arrow.down")
                            .font(.caption).foregroundStyle(.orange)
                            .accessibilityIdentifier("deep-sky.result-outdated")
                    }
                    if !report.success { Text(report.message).font(.caption).foregroundStyle(.red).lineLimit(2) }
                    if DeepSkyReview.hasRecordedSelection(report) {
                        DeepSkyReviewView(model: model, report: report, settings: settings, reportIsCurrent: canReview,
                                          filter: selectedReviewFilter(report: report, current: canReview), query: $reviewQuery)
                    } else {
                        Text(chinese ? "没有可核实的逐帧筛片决定。" : "No frame selection decisions were recorded.")
                            .font(.callout)
                        List(report.frames.filter { !$0.input.isEmpty }) { frame in
                            HStack {
                                Text(URL(fileURLWithPath: frame.input).lastPathComponent)
                                Spacer()
                                if !frame.errorCode.isEmpty {
                                    Text(DeepSkyReview.reason(frame.errorCode, chinese: chinese)).foregroundStyle(.secondary)
                                }
                            }
                        }.listStyle(.inset)
                    }
                } else if model.isProcessing {
                    VStack {
                        Spacer()
                        DeepSkyProcessingView(presentation: processingPresentation)
                        Spacer()
                    }.frame(maxWidth: .infinity)
                } else {
                    VStack(spacing: 16) {
                        Spacer()
                        Image(systemName: "photo.on.rectangle.angled").font(.system(size: 42)).foregroundStyle(.secondary)
                        Text(chinese ? "先了解这组照片，再决定用哪些" : "Inspect the sequence before choosing frames").font(.title3.weight(.semibold))
                        Text(chinese ? "已导入 \(model.lightAssets.count) 张亮场。分析会检查星点、噪声、对齐和局部异常。\n完成后可直接看原片、多选标记，并随时撤销。" : "\(model.lightAssets.count) lights imported. Analyze stars, noise, registration and local anomalies.\nThen inspect originals, mark multiple frames and undo decisions.")
                            .multilineTextAlignment(.center).foregroundStyle(.secondary)
                        if model.lightAssets.count < 3 {
                            Button(chinese ? "导入亮场文件夹…" : "Import light folder…", action: onImport)
                                .buttonStyle(.borderedProminent)
                                .accessibilityIdentifier("deep-sky.import-lights")
                                .disabled(model.isProcessing)
                            Text(chinese ? "至少需要 3 张亮场。" : "At least 3 light frames are required.").font(.caption)
                        }
                        Spacer()
                    }.frame(maxWidth: .infinity)
                }
        }
    }

    private func importRecipe() {
        guard !model.isProcessing else { return }
        let panel = NSOpenPanel(); panel.allowedContentTypes = [.plainText]; panel.allowsMultipleSelection = false
        if panel.runModal() == .OK, let url = panel.url { Task { await model.importDeepSkyRecipe(from: url) } }
    }
    private func exportRecipe() {
        guard !model.isProcessing else { return }
        let panel = NSSavePanel(); panel.nameFieldStringValue = "deep-sky-recipe.txt"; panel.allowedContentTypes = [.plainText]
        if panel.runModal() == .OK, let url = panel.url {
            model.exportDeepSkyRecipe(to: url)
        }
    }
}
