import PhotonStackAppCore
import SwiftUI

/// Presents engine-owned settings without creating a second source of defaults.
struct DeepSkySettingsView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var expandedGroups: Set<String>
    var onEditProtection: () -> Void
    @State private var query = ""
    @State private var showAdvanced = false
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var searching: Bool { !query.trimmingCharacters(in: .whitespacesAndNewlines).isEmpty }

    private var fields: [DeepSkySchema.Field] {
        (model.deepSkySchema?.fields ?? []).filter { field in
            guard field.key != "selection.overrides" else { return false }
            if searching {
                return [field.label, field.chineseLabel, field.key, groupName(field.group)]
                    .contains { $0.localizedStandardContains(query.trimmingCharacters(in: .whitespacesAndNewlines)) }
            }
            // A non-default value always stays discoverable, even in the concise view.
            let modified = model.deepSkySettings.values[field.key].map { $0 != field.defaultValue } ?? false
            return showAdvanced || modified || Self.commonKeys.contains(field.key)
        }
    }

    private var groups: [String] {
        var seen = Set<String>()
        return fields.map(\.group).filter { seen.insert($0).inserted }
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Image(systemName: "magnifyingglass").foregroundStyle(.secondary)
                TextField(chinese ? "查找参数，如：背景、星点、降噪" : "Find a setting: background, stars, denoise", text: $query)
                    .textFieldStyle(.roundedBorder)
                    .accessibilityIdentifier("deep-sky.settings.search")
                if searching {
                    Button { query = "" } label: { Image(systemName: "xmark.circle.fill") }
                        .buttonStyle(.plain)
                        .accessibilityLabel(chinese ? "清除搜索" : "Clear search")
                }
                Toggle(chinese ? "全部参数" : "All settings", isOn: $showAdvanced)
                    .toggleStyle(.switch).controlSize(.small)
                    .accessibilityIdentifier("deep-sky.settings.advanced")
            }
            Text(searching
                 ? (chinese ? "搜索包含全部参数；调整后需更新相应处理阶段。" : "Search includes all settings. Rerun the affected stage after changes.")
                 : (chinese ? "先使用默认值。这里显示常用项及已修改项；展开分组逐步调整。" : "Start with the defaults. Common and modified settings appear here; expand a section to adjust."))
                .font(.caption).foregroundStyle(.secondary)
            if model.deepSkySchema == nil {
                ProgressView().frame(maxWidth: .infinity, maxHeight: .infinity)
            } else if fields.isEmpty {
                ContentUnavailableView.search(text: query)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 16) {
                        ForEach(groups, id: \.self) { group in
                            DisclosureGroup(isExpanded: Binding(
                                get: { searching || expandedGroups.contains(group) },
                                set: { if $0 { expandedGroups.insert(group) } else { expandedGroups.remove(group) } }
                            )) {
                                VStack(alignment: .leading, spacing: 12) {
                                    Text(groupGuidance(group)).font(.caption).foregroundStyle(.secondary)
                                        .fixedSize(horizontal: false, vertical: true)
                                    ForEach(fields.filter { $0.group == group }) { field in
                                        fieldControl(field)
                                        if field.key == "background.exclusions" {
                                            Button(chinese ? "在图像上编辑保护区…" : "Edit regions on image…", action: onEditProtection)
                                                .disabled(!model.canRefinishDeepSkyMaster || model.deepSkySettings.lastReport?.crop == nil)
                                                .accessibilityIdentifier("background-protection.open")
                                        }
                                    }
                                }.padding(.top, 8)
                            } label: {
                                Text(groupName(group)).font(.body.weight(.medium))
                            }
                            Divider()
                        }
                    }.padding(8)
                }
            }
        }.disabled(model.isProcessing)
    }

    private func groupGuidance(_ group: String) -> String {
        switch group {
        case "calibration": chinese ? "校准帧需与拍摄条件匹配。也可在素材栏指定暗场、偏置和平场。" : "Use calibration frames matched to acquisition conditions. Assign darks, biases and flats in the asset list."
        case "decode": chinese ? "彩色 FITS 根据 CFA 信息去马赛克。改变解码设置需要重新堆栈。" : "Color FITS uses CFA metadata for debayering. Decode changes require a new stack."
        case "selection": chinese ? "先分析，再复查异常帧。相对质量筛选无法保证所有保留帧都清晰。" : "Analyze first, then inspect flagged frames. Relative screening cannot guarantee sharpness."
        case "registration": chinese ? "深空照片需要星点对齐；星轨合成应保留相机坐标。" : "Align stars for deep sky; preserve camera coordinates for star trails."
        case "stack": chinese ? "合成方法决定如何保留信号、排除异常值；更改后需要重新堆栈。" : "The method controls signal combination and outlier rejection. Changes require a new stack."
        case "background": chinese ? "先保护星云、星系和外围光晕，再消除背景渐变，避免扣除真实信号。" : "Protect nebulosity, galaxies and their outskirts before removing gradients."
        case "crop": chinese ? "根据有效覆盖范围裁掉对齐后的边缘；不会裁切原文件。" : "Crop alignment edges using valid coverage; source files are unchanged."
        case "denoise": chinese ? "从较弱强度开始，用同位置 100% 细节对比检查微弱结构。" : "Start gently and compare the same area at 100% to check faint structure."
        case "develop": chinese ? "调整用于观看的亮度与色彩；线性母片会单独保留。" : "Adjust brightness and color for viewing. The linear master is retained separately."
        default: chinese ? "诊断文件会额外占用磁盘空间；按需要开启。" : "Diagnostic outputs use additional disk space; enable as needed."
        }
    }

    private static let commonKeys: Set<String> = [
        "calibration.darks", "calibration.biases", "calibration.flats",
        "decode.debayer", "decode.demosaic", "registration.mode",
        "selection.apply", "selection.maximum-fwhm-ratio", "selection.maximum-eccentricity",
        "selection.maximum-noise-ratio", "selection.minimum-kept",
        "stack.method", "stack.normalize-background", "stack.sigma-low", "stack.sigma-high",
        "background.model", "background.exclusions", "background.polynomial-strength",
        "background.grid-strength", "crop.coverage",
        "denoise.multiscale-luminance", "denoise.multiscale-chroma", "denoise.background-strength",
        "develop.curve", "develop.background", "develop.brightness", "develop.saturation",
        "develop.lock-denoise-stretch", "develop.color-transfer", "output.save-denoise-stages"
    ]

    private func binding(_ field: DeepSkySchema.Field) -> Binding<String> {
        Binding(get: { model.deepSkySettings.values[field.key] ?? field.defaultValue }, set: { model.setDeepSkyValue($0, for: field.key) })
    }
    @ViewBuilder private func fieldControl(_ field: DeepSkySchema.Field) -> some View {
        let title = chinese ? field.chineseLabel : field.label
        if field.key == "develop.lock-denoise-stretch" {
            DeepSkyToneReferenceControl(value: binding(field), title: title, chinese: chinese)
        } else {
        HStack {
            Text(title).frame(minWidth: 220, idealWidth: 290, maxWidth: 310, alignment: .leading)
            if field.type == "bool" {
                Toggle(title, isOn: Binding(get: { binding(field).wrappedValue == "on" }, set: { binding(field).wrappedValue = $0 ? "on" : "off" })).labelsHidden()
            } else if field.type == "choice" {
                Picker(title, selection: binding(field)) {
                    ForEach(field.choices, id: \.self) { choice in
                        Text(choiceTitle(choice, field: field)).tag(choice)
                            .disabled(field.key == "denoise.noise-model" && choice == "independent-luminance" &&
                                model.deepSkySchema.map { model.deepSkySettings.effectiveValue("stack.method", schema: $0) } != "sigma")
                    }
                }.labelsHidden().frame(width: 260)
            } else {
                TextField(title, text: binding(field)).textFieldStyle(.roundedBorder).frame(minWidth: 120, idealWidth: field.type == "text" ? 360 : 150, maxWidth: field.type == "text" ? .infinity : 190)
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
                : field.key == "develop.color-transfer"
                ? (chinese ? "线性光模式在线性 RGB 中分配颜色，再编码为显示色彩。旧版模式用于重现此前的显影效果。切换后可从现有母版重新显影。"
                    : "Linear light applies color in linear RGB before display encoding. Legacy rendering replays earlier results. Reprocess the existing master after switching.")
                : field.key == "calibration.sensor-pattern"
                ? (chinese ? "在去马赛克前减去传感器固定细纹。需至少 36 张同设置的亮场及足够视场位移，暂不能与暗场、偏置、平场一起使用。默认关闭；请对比检查微弱结构。"
                    : "Subtract sensor-fixed fine texture before debayering. Requires at least 36 matching lights and sufficient field movement; cannot combine with calibration frames. Off by default; compare faint structures before adoption.")
                : field.key == "denoise.noise-model"
                ? (chinese ? "独立估计可减少把微弱纹理误当噪声的情况。首次启用需重新堆栈；至少 6 张有效亮场、Sigma 堆栈，并将非局部降噪强度设为 0。仅改变亮度降噪，请用同位置对比确认效果。"
                    : "Independent estimates can protect faint texture from over-smoothing. Rebuild the stack on first use; requires at least six usable lights, Sigma stacking and NLM strength 0. Affects luminance only; compare the same region before adoption.")
                : field.key == "selection.maximum-eccentricity"
                ? (chinese ? "超过此阈值会提示复查。只有同时比序列中位数高 0.15 的帧才被自动排除；整组星点都拉长时仍可能保留，请检查原片 1:1。"
                    : "Above this threshold, inspect the frame. Automatic rejection also requires exceeding the sequence median by 0.15. A uniformly elongated sequence may remain selected; inspect originals at 1:1.")
                : field.key == "develop.sky-percentile"
                ? (chinese ? "0.5 保持原来的全图中位数参考。大面积星云被扣暗时可尝试 0.15–0.3；更低会同时抬高噪声和残余渐变。先校正背景，再比较微弱结构；不是光度标定。"
                    : "0.5 retains the original whole-frame median reference. Try 0.15–0.3 for extended nebulosity; lower values also lift noise and residual gradients. Correct gradients first and compare faint structure; not photometric calibration.")
                : field.key == "denoise.background-strength"
                ? (chinese ? "减轻暗背景的斑驳纹理。0 为关闭；自动保护星点，并保留背景校正中排除的目标区域。增强前请检查微弱弥散细节。"
                    : "Reduce mottled background texture. Zero disables it. Stars and excluded target regions are protected; inspect faint diffuse detail before increasing strength.")
                : title)
        }
    }
    private func choiceTitle(_ choice: String, field: DeepSkySchema.Field) -> String {
        if field.key == "stack.method", let method = StackMethod(rawValue: choice) {
            return model.language.stackMethodName(method)
        }
        if field.key == "registration.mode", let method = AlignmentMethod(rawValue: choice) {
            return model.language.alignmentMethodName(method)
        }
        if field.key == "denoise.noise-model" {
            return choice == "scene" ? (chinese ? "从图像估计" : "From image")
                : (chinese ? "从独立子堆栈估计（实验）" : "Independent sub-stacks (experimental)")
        }
        let names: [String: (String, String)] = [
            "none": ("关闭", "Off"), "polynomial": ("多项式", "Polynomial"),
            "grid": ("网格", "Grid"), "polynomial-grid": ("多项式 + 网格", "Polynomial + grid"),
            "asinh": ("反双曲正弦", "Asinh"), "rational": ("有理曲线", "Rational"),
            "bilinear": ("双线性", "Bilinear"), "bicubic": ("双三次", "Bicubic"),
            "malvar-guarded": ("Malvar · 亮星暗环保护", "Malvar · bright-star ringing guard"),
            "linear-light": ("线性光（新默认）", "Linear light (default)"),
            "legacy-code": ("旧版显影（兼容重放）", "Legacy rendering (replay)"),
            "malvar": ("Malvar", "Malvar"), "menon": ("Menon", "Menon"),
            "ratio": ("通道比例", "Channel ratio"),
            "median": ("中位数", "Median"), "average": ("平均值", "Average"),
            "unknown": ("未知", "Unknown"), "included": ("包含偏置", "Bias included"),
            "removed": ("已减去偏置", "Bias removed"),
            "camera": ("相机设置", "Camera"), "auto": ("自动", "Automatic"),
            "manual": ("手动", "Manual"), "daylight": ("日光", "Daylight"),
            "fast": ("快速", "Fast"), "balanced": ("均衡", "Balanced"), "high": ("高质量", "High quality")
        ]
        guard let name = names[choice] else { return choice }
        return chinese ? name.0 : name.1
    }

    private func groupName(_ group: String) -> String {
        guard chinese else { return group.capitalized }
        return ["calibration": "暗场、偏置与平场校准", "decode": "科学解码", "registration": "星点对齐", "stack": "堆栈与像素剔除", "selection": "整帧质量筛选", "background": "背景校正", "crop": "覆盖率裁边", "denoise": "结构保护降噪", "develop": "色彩与显影", "output": "输出文件"][group] ?? group
    }
}
