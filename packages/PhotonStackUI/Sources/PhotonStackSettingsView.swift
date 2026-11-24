import PhotonStackAppCore
import SwiftUI

public struct PhotonStackSettingsView: View {
    @ObservedObject private var commandState: PhotonStackApplicationCommandState

    @ObservedObject private var updateChecker: PhotonStackUpdateChecker
    @State private var expandedStages: Set<String> = []

    public init(commandState: PhotonStackApplicationCommandState, updateChecker: PhotonStackUpdateChecker = PhotonStackUpdateChecker()) {
        self.updateChecker = updateChecker
        self.commandState = commandState
    }

    private var language: AppLanguage {
        commandState.snapshot.language
    }

    private var selectedLanguage: Binding<AppLanguage> {
        Binding(
            get: { commandState.snapshot.language },
            set: { commandState.requestLanguage($0) }
        )
    }

    private var analysisWorkerCount: Binding<Int> {
        Binding(
            get: { commandState.snapshot.analysisWorkerCount },
            set: { commandState.requestAnalysisWorkerCount($0) }
        )
    }

    private var stackWorkerCount: Binding<Int> {
        Binding(
            get: { commandState.snapshot.stackWorkerCount },
            set: { commandState.requestStackWorkerCount($0) }
        )
    }

    private var backend: Binding<DeepSkyComputeBackend> {
        Binding(
            get: { commandState.snapshot.computeBackend },
            set: { commandState.requestComputeBackend($0) }
        )
    }

    public var body: some View {
        Form {
            Section {
                Picker(
                    language == .simplifiedChinese ? "界面语言" : "Interface language",
                    selection: selectedLanguage
                ) {
                    ForEach(AppLanguage.allCases, id: \.self) { option in
                        Text(option.displayName).tag(option)
                    }
                }
                .pickerStyle(.segmented)
                .accessibilityIdentifier("settings.language")
                Toggle(language == .simplifiedChinese ? "退出时提醒保存项目" : "Ask to save projects on exit", isOn: Binding(
                    get: { commandState.snapshot.promptToSaveOnExit },
                    set: { commandState.requestPromptToSaveOnExit($0) }
                ))
                .accessibilityIdentifier("settings.promptToSaveOnExit")
                Text(language == .simplifiedChinese
                    ? "关闭窗口或退出时，提醒保存未保存的修改。默认关闭。"
                    : "Ask about unsaved changes when closing a window or quitting. Off by default.")
                    .font(.caption).foregroundStyle(.secondary)
            } header: {
                Label(
                    language == .simplifiedChinese ? "通用" : "General",
                    systemImage: "globe"
                )
            } footer: {
                Text(language == .simplifiedChinese
                    ? "首次启动时按 macOS 偏好选择中英文，其他语言默认使用中文。"
                    : "On first launch, PhotonStack uses English or Chinese from your macOS preferences, with Chinese as the fallback.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }

            updateSection

            Section {
                Picker(
                    language == .simplifiedChinese ? "分析与筛片并发" : "Analysis concurrency",
                    selection: analysisWorkerCount
                ) {
                    workerChoices(maximum: AppSettings.maximumAnalysisWorkerCount)
                }
                .accessibilityIdentifier("settings.analysisWorkers")

                Picker(
                    language == .simplifiedChinese ? "堆栈合成线程" : "Stack combine workers",
                    selection: stackWorkerCount
                ) {
                    workerChoices(maximum: AppSettings.maximumStackWorkerCount)
                }
                .accessibilityIdentifier("settings.stackWorkers")

                Picker(language == .simplifiedChinese ? "深空计算后端" : "Deep-sky compute backend", selection: backend) {
                    Text(language == .simplifiedChinese ? "自动（推荐）" : "Automatic (recommended)").tag(DeepSkyComputeBackend.automatic)
                    Text(language == .simplifiedChinese ? "优先 GPU" : "Prefer GPU").tag(DeepSkyComputeBackend.metal)
                    Text(language == .simplifiedChinese ? "仅 CPU" : "CPU only").tag(DeepSkyComputeBackend.cpu)
                }
                .accessibilityIdentifier("settings.computeBackend")
                if let run = commandState.snapshot.performance {
                    performanceDetails(run)
                }
            } header: {
                Label(language == .simplifiedChinese ? "性能" : "Performance", systemImage: "speedometer")
            } footer: {
                Text(language == .simplifiedChinese
                    ? "设置从下次处理起生效。实际并发数取决于图像尺寸和可用内存；运行记录会标明哪些步骤使用了 GPU。"
                    : "Changes apply to the next run. Image size and available memory limit concurrency. Run details show which stages used the GPU.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .formStyle(.grouped)
        .frame(width: 560, height: 530)
    }

    private var updateSection: some View {
        let zh = language == .simplifiedChinese
        return Section {
            Toggle(zh ? "启动时检查更新" : "Check for updates at launch", isOn: $updateChecker.automatic)
                .accessibilityIdentifier("settings.updates.automatic")
            LabeledContent(zh ? "当前版本" : "Installed version") {
                Text(updateChecker.installedVersion)
                Button(zh ? "检查更新" : "Check for updates") {
                    Task { await updateChecker.check() }
                }
                .disabled(updateChecker.status == .checking)
                .accessibilityIdentifier("settings.updates.check")
            }
            switch updateChecker.status {
            case .idle:
                EmptyView()
            case .checking:
                Text(zh ? "正在检查…" : "Checking…")
            case .current:
                Text(zh ? "已是最新版本" : "You're up to date")
            case .noRelease:
                Text(zh ? "GitHub 暂无正式版本" : "No published release on GitHub yet")
            case .available(let tag):
                LabeledContent(zh ? "发现新版 \(tag)" : "Version \(tag) available") {
                    Link(zh ? "查看新版" : "View release", destination: PhotonStackUpdateChecker.releasePage(tag: tag))
                }
            case .failed:
                LabeledContent(zh ? "检查失败，请稍后重试" : "Update check failed. Try again later.") {
                    Link(zh ? "查看发布页" : "View releases", destination: PhotonStackUpdateChecker.releasesPage)
                }
            }
        } header: {
            Label(zh ? "软件更新" : "Software updates", systemImage: "arrow.triangle.2.circlepath")
        }
    }

    @ViewBuilder
    private func performanceDetails(_ run: DeepSkyRunPerformance) -> some View {
        let zh = language == .simplifiedChinese
        let auto = zh ? "自动" : "auto"
        let analysis = run.requestedAnalysisWorkers == 0 ? auto : String(run.requestedAnalysisWorkers)
        let stack = run.requestedStackWorkers == 0 ? auto : String(run.requestedStackWorkers)
        let combine = run.stages.first { $0.name == "robust-stack" }?.peakWorkers ?? 0
        LabeledContent(zh ? "上次分析并发（请求 → 实际）" : "Last analysis (requested → actual)",
                       value: "\(analysis) → \(run.peakAnalysisFrames)")
        LabeledContent(zh ? "上次堆栈线程（请求 → 实际）" : "Last combine (requested → actual)",
                       value: "\(stack) → \(combine)")
        if !run.admissionReason.isEmpty { Text(reason(run.admissionReason)).font(.caption) }
        ForEach(run.stages.filter { $0.executed && ["raw-decode", "registration-resample", "calibration", "star-detection", "demosaic", "robust-stack"].contains($0.name) }, id: \.name) { stage in
            let names = ["raw-decode": "RAW 解码", "registration-resample": "配准重采样",
                         "calibration": "暗场／偏置／平场校准", "star-detection": "星点检测", "demosaic": "去马赛克", "robust-stack": "稳健堆栈"]
            let backends = ["mixed": zh ? "混合计算" : "Mixed",
                            "metal+cpu-cfa": zh ? "Metal 插值、CPU 边界处理" : "Metal interpolation, CPU boundaries",
                            "metal+cpu-photometry": zh ? "Metal 筛选、CPU 测光" : "Metal screening, CPU photometry",
                            "metal+cpu-guard": zh ? "Metal 插值与星点分析、CPU 星点修复" : "Metal interpolation and star analysis, CPU aperture repair",
                            "metal+cpu-reduction": zh ? "Metal 中心估计、CPU 归约" : "Metal centers, CPU reduction"]
            LabeledContent(zh ? (names[stage.name] ?? stage.name) : stage.name,
                           value: backends[stage.backend] ?? stage.backend.uppercased())
            if let operations = stage.metalOperations, !operations.isEmpty {
                let expanded = expandedStages.contains(stage.name)
                Button {
                    if expanded { expandedStages.remove(stage.name) } else { expandedStages.insert(stage.name) }
                } label: {
                    HStack {
                        Image(systemName: expanded ? "chevron.down" : "chevron.right")
                        Text(zh ? "GPU 调用明细" : "GPU dispatches")
                        Spacer()
                    }
                    .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .font(.caption)
                .accessibilityIdentifier("settings.gpuOperations." + stage.name)
                .accessibilityValue(expanded ? (zh ? "已展开" : "Expanded") : (zh ? "已折叠" : "Collapsed"))
                if expanded {
                    ForEach(operations.keys.sorted(), id: \.self) { key in
                        let labels = ["warp": "配准重采样", "demosaic": "CFA 插值", "peaks": "星点候选",
                                      "scientificCFA": "浮点 CFA 插值", "guardLuminance": "星点亮度", "guardSky": "局部天空背景",
                                      "calibration": "校准", "winsor": "中位数与 MAD",
                                      "winsorWide": "加权中位数与 MAD", "winsorStream": "大序列中位数与 MAD",
                                      "reduceWinsor": "剔除统计与合成"]
                        LabeledContent(zh ? (labels[key] ?? key) : key, value: String(operations[key] ?? 0))
                            .font(.caption)
                    }
                }
            }
            if !stage.fallback.isEmpty { Text(reason(stage.fallback)).font(.caption) }
        }
    }

    private func reason(_ message: String) -> String {
        guard language == .simplifiedChinese else { return message }
        let messages = [
            "automatic uses validated CPU registration": "此规模或算法使用 CPU 配准",
            "frame size, CPU and memory admission": "根据帧尺寸、核心数和可用内存限制并发",
            "system memory pressure": "系统内存紧张，已使用 CPU",
            "memory budget unavailable": "无法获取可用内存，已串行处理",
            "CPU limit": "受可用 CPU 核心数限制",
            "serial decoder or analysis mode": "此解码或分析模式需串行处理",
            "unknown FITS dimensions": "无法确定 FITS 尺寸，已串行处理",
            "small or narrow frame": "帧尺寸过小，已串行处理",
            "requires a full-size finite RGBA warp of at least 65536 pixels": "此帧尺寸或采样网格使用 CPU 配准",
            "Metal CFA requires full-size scientific Malvar interpolation": "此采样尺寸或插值模式使用 CPU",
            "requires native 16-bit Malvar CFA, unit gains and a full-size image": "此 CFA 格式、增益或插值模式使用 CPU",
            "Metal centers require an unweighted RGBA Winsor tile with 8–240 frames": "此帧数、权重或堆栈方法使用 CPU",
            "Metal reduction requires an RGBA Winsor tile with at least 8 frames and no independent noise groups": "此图块、帧数或诊断模式使用 CPU 归约",
            "transform model requires CPU": "此变换模型使用 CPU",
            "integer translation sampler": "整数平移采样使用 CPU",
            "Metal working-set budget": "超过 GPU 内存预算，已使用 CPU",
            "Metal buffer allocation failed": "GPU 缓冲区分配失败，已使用 CPU",
            "Metal device or pipeline unavailable": "Metal 设备或计算管线不可用，已使用 CPU",
            "Metal command failed": "GPU 计算失败，已重新使用 CPU 计算"
        ]
        return messages[message] ?? message
    }

    @ViewBuilder
    private func workerChoices(maximum: Int) -> some View {
        Text(language == .simplifiedChinese ? "自动（推荐）" : "Automatic (Recommended)").tag(0)
        ForEach(1...maximum, id: \.self) { count in
            Text(language == .simplifiedChinese ? "\(count) 路" : "\(count) worker\(count == 1 ? "" : "s")")
                .tag(count)
        }
    }
}
