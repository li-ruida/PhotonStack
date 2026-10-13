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
    var onImport: () -> Void
    @State private var expandedSettings: Set<String> = ["selection", "stack"]
    @State private var tab = 1
    @State private var protectionPresented = false
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var report: DeepSkyReport? { model.deepSkySettings.lastReport }
    private var groups: [String] {
        var seen = Set<String>()
        return (model.deepSkySchema?.fields ?? []).map(\.group).filter { seen.insert($0).inserted }
    }
    private var reportIsCurrent: Bool {
        report.map { DeepSkyReview.sameAnalysisRecipe(try? model.deepSkyRecipeText(), $0.recipe) } ?? false
    }
    private var plannedCount: Int {
        guard reportIsCurrent else { return model.lightAssets.count }
        return report?.frames.filter { DeepSkyReview.kept($0, settings: model.deepSkySettings) }.count ?? model.lightAssets.count
    }
    private var minimumKept: Int { DeepSkyReview.minimumKept(settings: model.deepSkySettings, count: model.lightAssets.count) }
    private var tooFew: Bool { plannedCount < minimumKept }
    private var resultIsCurrent: Bool { model.journeyResultIsCurrent }
    private var processingPresentation: DeepSkyProcessingPresentation {
        DeepSkyProcessingPresentation(job: model.jobs.last(where: { $0.status == .running }),
            message: model.progressMessage, fraction: model.progressFraction, chinese: chinese)
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(chinese ? "深空筛片与堆栈" : "Deep Sky Assessment & Stack").font(.title2)
                Spacer()
                Button(chinese ? "关闭" : "Close") { presented = false }.keyboardShortcut(.cancelAction)
            }
            Text(chinese ? "2 分析与筛片 → 3 堆栈 → 4 后期调整。排除只影响这次堆栈，不会删除原片。" : "2 Review → 3 Stack → 4 Finish. Excluding frames never deletes originals.")
                .font(.callout).foregroundStyle(.secondary)
            if resultIsCurrent && !model.isProcessing {
                Label(chinese ? "堆栈完成 · 使用 \(report?.selectedCount ?? 0) 张亮场。下一步进入后期调整。" : "Stack complete · \(report?.selectedCount ?? 0) lights. Continue to finishing.", systemImage: "checkmark.circle.fill")
                    .foregroundStyle(.green).accessibilityIdentifier("deep-sky.stack-complete")
            }
            Picker(chinese ? "工作区" : "Workspace", selection: $tab) {
                Text(chinese ? "筛片工作台" : "Frame review").tag(1)
                Text(chinese ? "处理参数" : "Processing settings").tag(0)
            }.pickerStyle(.segmented).labelsHidden()
            StackModeControl(model: model)
            ZStack {
                reviewContent
                    .opacity(tab == 1 ? 1 : 0)
                    .allowsHitTesting(tab == 1)
                    .disabled(tab != 1)
                    .accessibilityHidden(tab != 1)
                if tab == 0 { settingsContent }
            }
            if model.isProcessing && (report != nil || tab == 0) {
                DeepSkyProcessingView(presentation: processingPresentation, compact: true)
            }
            if let error = model.errorMessage { Text(error).font(.caption).foregroundStyle(.red).lineLimit(3) }
            HStack {
                Menu(chinese ? "配方" : "Recipe") {
                    Button(chinese ? "导入配方" : "Import recipe") { importRecipe() }
                    Button(chinese ? "导出配方" : "Export recipe") { exportRecipe() }
                    Divider()
                    Button(chinese ? "恢复默认处理参数" : "Reset processing settings") { model.resetDeepSkySettings() }
                }.disabled(model.isProcessing)
                .fixedSize()
                if tooFew {
                    Text(chinese ? "当前保留 \(plannedCount) 张，至少需要 \(minimumKept) 张。请增加保留的亮场。" : "\(plannedCount) kept; at least \(minimumKept) required. Add or restore light frames.")
                        .font(.caption).foregroundStyle(.orange)
                }
                Spacer()
                if model.latestDeepSkyResultURL != nil && !resultIsCurrent {
                    ResultComparisonControl(model: model, context: "deep-sky")
                    Button(chinese ? "查看堆栈结果" : "View stack result") {
                        model.showLatestDeepSkyResult()
                        presented = false
                    }
                    .accessibilityIdentifier("deep-sky.show-result")
                    .disabled(!model.canModifyProcessingConfiguration)
                }
                if model.isProcessing {
                    Button(chinese ? "取消处理" : "Cancel") { model.cancelCurrentTask() }
                } else {
                    if model.canRefinishDeepSkyMaster {
                        Button(chinese ? "更新背景与降噪" : "Reprocess master") { model.startRefinishDeepSkyMaster() }
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
        }.padding(16).frame(width: 1120, height: 700)
            .task { await model.loadDeepSkySchema() }
            .sheet(isPresented: $protectionPresented) {
                if let report { BackgroundProtectionEditor(model: model, report: report) }
            }
    }

    private func analyze() {
        tab = 1
        model.startDeepSkyWorkflow(analyzeOnly: true)
    }

    @ViewBuilder private var settingsContent: some View {
                ScrollView {
                    if let schema = model.deepSkySchema {
                        VStack(alignment: .leading, spacing: 14) {
                            ForEach(groups, id: \.self) { group in
                                DisclosureGroup(groupName(group), isExpanded: Binding(
                                    get: { expandedSettings.contains(group) },
                                    set: { if $0 { expandedSettings.insert(group) } else { expandedSettings.remove(group) } }
                                )) {
                                    ForEach(schema.fields.filter { $0.group == group && $0.key != "selection.overrides" }) { field in
                                        fieldControl(field)
                                        if field.key == "background.exclusions" {
                                            Button(chinese ? "在图像上编辑保护区…" : "Edit regions on image…") { protectionPresented = true }
                                                .disabled(!model.canRefinishDeepSkyMaster || report?.crop == nil)
                                                .accessibilityIdentifier("background-protection.open")
                                                .help(chinese ? "在上次成片上画出目标及外围光晕，保存后更新背景与降噪。" : "Outline the target and outskirts on the previous result, then reprocess the master.")
                                        }
                                    }
                                }
                            }
                        }.padding(8)
                    } else { ProgressView().padding() }
                }
                .disabled(model.isProcessing)
    }

    @ViewBuilder private var reviewContent: some View {
        VStack(alignment: .leading, spacing: 10) {
                if let report {
                    if report.frames.contains(where: { $0.starsMeasured && $0.fwhmEstimator != "native-gaussian-v1" }) {
                        Label(chinese ? "星点测量已升级；重新分析可更新星宽、模糊程度和参考帧建议。" : "Star measurements have improved. Analyze again to update widths, blur checks and reference-frame recommendations.", systemImage: "viewfinder")
                            .font(.caption).foregroundStyle(.secondary)
                    }
                    if !reportIsCurrent {
                        Label(chinese ? "处理参数或素材已变化，下方是上次分析的测量值；请重新分析。" : "Settings or inputs changed. Measurements below are from the previous analysis; analyze again.", systemImage: "arrow.clockwise.circle")
                            .font(.caption).foregroundStyle(.orange)
                    } else if report.success, !report.analysisOnly,
                              !DeepSkyReview.sameResultRecipe(try? model.deepSkyRecipeText(), report.recipe) {
                        Label(model.canRefinishDeepSkyMaster
                            ? (chinese ? "修整参数已变化；筛片仍有效。点击“更新背景与降噪”生成新成片，当前显示仍是上次结果。" : "Finishing settings changed; frame review is still valid. Reprocess the master to update the displayed result.")
                            : (chinese ? "当前选择或参数尚未生成成片，显示的仍是上次结果。" : "Current decisions have not been rendered; the displayed image is the previous result."), systemImage: "photo.badge.arrow.down")
                            .font(.caption).foregroundStyle(.orange)
                            .accessibilityIdentifier("deep-sky.result-outdated")
                    }
                    if !report.success { Text(report.message).font(.caption).foregroundStyle(.red).lineLimit(2) }
                    DeepSkyReviewView(model: model, report: report)
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

    private func binding(_ field: DeepSkySchema.Field) -> Binding<String> {
        Binding(get: { model.deepSkySettings.values[field.key] ?? field.defaultValue }, set: { model.setDeepSkyValue($0, for: field.key) })
    }
    @ViewBuilder private func fieldControl(_ field: DeepSkySchema.Field) -> some View {
        let title = chinese ? field.chineseLabel : field.label
        HStack {
            Text(title).frame(width: 310, alignment: .leading)
            if field.type == "bool" {
                Toggle(title, isOn: Binding(get: { binding(field).wrappedValue == "on" }, set: { binding(field).wrappedValue = $0 ? "on" : "off" })).labelsHidden()
            } else if field.type == "choice" {
                Picker(title, selection: binding(field)) {
                    ForEach(field.choices, id: \.self) { choice in
                        Text(field.key == "stack.method"
                            ? (StackMethod(rawValue: choice).map { model.language.stackMethodName($0) } ?? choice)
                            : field.key == "denoise.noise-model"
                            ? (choice == "scene" ? (chinese ? "从图像估计" : "From image") : (chinese ? "从独立子堆栈估计（实验）" : "Independent sub-stacks (experimental)"))
                            : field.key == "decode.demosaic" && choice == "malvar-guarded"
                            ? (chinese ? "Malvar · 亮星暗环保护" : "Malvar · bright-star ringing guard")
                            : choice).tag(choice)
                            .disabled(field.key == "denoise.noise-model" && choice == "independent-luminance" &&
                                (model.deepSkySettings.values["stack.method"] ?? "sigma") != "sigma")
                    }
                }.labelsHidden().frame(width: 260)
            } else {
                TextField(title, text: binding(field)).textFieldStyle(.roundedBorder).frame(width: field.type == "text" ? 540 : 190)
                if field.type == "number" || field.type == "integer" {
                    Text("\(field.minimum.formatted()) … \(field.maximum.formatted())").font(.caption).foregroundStyle(.secondary)
                }
            }
            Spacer(minLength: 0)
        }.accessibilityIdentifier(field.key)
            .disabled(model.deepSkySchema.map { !model.deepSkySettings.fieldIsApplicable(field.key, schema: $0) } ?? false)
            .help(field.key == "output.save-denoise-stages"
                ? (chinese ? "默认关闭，节省磁盘空间。开启后额外保存各个已启用降噪阶段的全尺寸 FITS，供排查涂抹和噪点使用；不改变处理效果。线性母片、背景校正图和最终成片始终保留。"
                    : "Off by default to save disk space. Saves full-size FITS for each enabled denoising stage for diagnosis; does not change processing. The linear master, background-corrected image and final result are always retained.")
                : field.key == "decode.demosaic"
                ? (chinese ? "亮星暗环保护会约束去马赛克产生的异常暗凹，保留原始采样和单星 RGB 总通量。改变此项需要重新堆栈，无法通过更新显影修复已有母片。"
                    : "The bright-star guard constrains demosaicing undershoot while retaining native samples and each star's integrated RGB. Rebuild the stack after changing this setting; finishing an existing master cannot apply it.")
                : field.key == "calibration.sensor-pattern"
                ? (chinese ? "在去马赛克前减去传感器固定细纹。需至少 36 张同设置的亮场及足够视场位移，暂不能与暗场、偏置、平场一起使用。默认关闭；请对比检查微弱结构。"
                    : "Subtract sensor-fixed fine texture before debayering. Requires at least 36 matching lights and sufficient field movement; cannot combine with calibration frames. Off by default; compare faint structures before adoption.")
                : field.key == "denoise.noise-model"
                ? (chinese ? "独立估计可减少把微弱纹理误当噪声的情况。首次启用需重新堆栈；至少 6 张有效亮场、Sigma 堆栈，并将非局部降噪强度设为 0。仅改变亮度降噪，请用同位置对比确认效果。"
                    : "Independent estimates can protect faint texture from over-smoothing. Rebuild the stack on first use; requires at least six usable lights, Sigma stacking and NLM strength 0. Affects luminance only; compare the same region before adoption.")
                : field.key == "denoise.background-strength"
                ? (chinese ? "减轻暗背景的斑驳纹理。0 为关闭；自动保护星点，并保留背景校正中排除的目标区域。增强前请检查微弱弥散细节。"
                    : "Reduce mottled background texture. Zero disables it. Stars and excluded target regions are protected; inspect faint diffuse detail before increasing strength.")
                : title)
    }
    private func groupName(_ group: String) -> String {
        guard chinese else { return group.capitalized }
        return ["calibration": "暗场、偏置与平场校准", "decode": "科学解码", "registration": "星点对齐", "stack": "堆栈与像素剔除", "selection": "整帧质量筛选", "background": "背景校正", "crop": "覆盖率裁边", "denoise": "结构保护降噪", "develop": "色彩与显影", "output": "输出文件"][group] ?? group
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
