import PhotonStackAppCore
import SwiftUI

public struct PhotonStackSettingsView: View {
    @ObservedObject private var commandState: PhotonStackApplicationCommandState

    public init(commandState: PhotonStackApplicationCommandState) {
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
            } header: {
                Label(
                    language == .simplifiedChinese ? "通用" : "General",
                    systemImage: "globe"
                )
            } footer: {
                Text(language == .simplifiedChinese
                    ? "优先使用你在 PhotonStack 中选择的语言。首次启动会读取 macOS 偏好语言；无法识别时使用中文。"
                    : "PhotonStack uses your in-app choice first. On first launch it reads the macOS preferred languages and falls back to Chinese when none are supported.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }


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
                    ? "并发数受帧尺寸和可用内存限制。Metal 支持 RAW 解码、16 位 CFA 插值、全幅相似变换配准和 Winsorized 稳健堆栈。设置在下次运行时生效。"
                    : "Frame size and available memory limit concurrency. Metal supports RAW decoding, 16-bit CFA interpolation, full-frame similarity registration and Winsorized stacking. Changes apply to the next run.")
                    .foregroundStyle(.secondary)
                    .fixedSize(horizontal: false, vertical: true)
            }
        }
        .formStyle(.grouped)
        .frame(width: 560, height: 530)
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
        ForEach(run.stages.filter { $0.executed && ["raw-decode", "registration-resample", "demosaic", "robust-stack"].contains($0.name) }, id: \.name) { stage in
            let names = ["raw-decode": "RAW 解码", "registration-resample": "配准重采样",
                         "demosaic": "去马赛克", "robust-stack": "稳健堆栈"]
            let backends = ["metal+cpu-guard": zh ? "Metal 插值、CPU 星点保护" : "Metal interpolation, CPU star guard",
                            "metal+cpu-reduction": zh ? "Metal 中心估计、CPU 归约" : "Metal centers, CPU reduction"]
            LabeledContent(zh ? (names[stage.name] ?? stage.name) : stage.name,
                           value: backends[stage.backend] ?? stage.backend.uppercased())
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
            "requires native 16-bit Malvar CFA, unit gains and a full-size image": "此 CFA 格式、增益或插值模式使用 CPU",
            "Metal centers require an unweighted RGBA Winsor tile with 8–240 frames": "此帧数、权重或堆栈方法使用 CPU",
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
