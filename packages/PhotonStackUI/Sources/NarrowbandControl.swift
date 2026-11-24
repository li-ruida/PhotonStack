import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct NarrowbandControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @ObservedObject var session: NarrowbandSession
    @State private var hydrogen: URL?
    @State private var oxygen: URL?
    @State private var greenHydrogen: Double = 0
    @State private var compose = false
    private var zh: Bool { model.language == .simplifiedChinese }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Picker(zh ? "操作" : "Operation", selection: $compose) {
                Text(zh ? "原片提取" : "Extract lines").tag(false)
                Text(zh ? "HOO 合色" : "Compose HOO").tag(true)
            }.pickerStyle(.segmented)
            if compose {
                Text(zh ? "选择已对齐到相同像素网格的两张单色线性 FITS。" : "Choose two registered mono FITS on the same pixel grid.")
                    .font(.caption)
                linePicker("Hα", selection: $hydrogen)
                linePicker("OⅢ", selection: $oxygen)
                ParameterSlider(title: zh ? "绿色中 Hα 比例" : "Hα fraction in green",
                                value: $greenHydrogen, range: 0...1)
                Text(zh ? "红色使用 Hα，蓝色使用 OⅢ。" : "Red uses Hα and blue uses OⅢ.").font(.caption)
                Button(zh ? "保存 HOO 母片…" : "Save HOO master…") {
                    guard let hydrogen, let oxygen, let dest = chooseOutput("HOO") else { return }
                    model.startNarrowbandJob { service in
                        try await session.run(["narrowband-compose", "--ha", hydrogen.path, "--oiii", oxygen.path,
                            "--green-ha", String(greenHydrogen), "--output-dir", dest.path], output: dest, using: service)
                    }
                }.disabled(hydrogen == nil || oxygen == nil)
            } else {
                Text(zh ? "选中一张 Hα/OⅢ 双窄带拜耳 FITS。每个完整的 2×2 像素格生成一个 Hα 和一个 OⅢ 样本，分别保存为两张 FITS。"
                     : "Select an Hα/OⅢ dual-band Bayer FITS. Each complete 2×2 cell yields one sample per line.")
                    .font(.caption)
                Picker(zh ? "通带采样" : "Line sampling", selection: $session.sampling) {
                    ForEach(NarrowbandSampling.allCases, id: \.self) { mode in Text(mode.title(chinese: zh)).tag(mode) }
                }.accessibilityIdentifier("narrowband.sampling")
                Text(session.sampling == .redGrid
                     ? (zh ? "Hα 保留红像素值，OⅢ 对称插值到同一位置。边缘不足处标为无效，星点宽度可能变化。"
                        : "Hα retains red samples; OⅢ is interpolated symmetrically to the same sites. Unsupported edges are masked; star width may change.")
                     : (zh ? "保留旧式像素格均值。OⅢ 的采样中心与 Hα 不同。"
                        : "Preserve the historical cell average. Its OⅢ sampling center differs from Hα."))
                    .font(.caption2).foregroundStyle(.secondary)
                NarrowbandCalibrationControl(session: session, chinese: zh)
                Button(zh ? "提取并保存…" : "Extract and save both FITS…") {
                    guard let asset = model.selectedAsset, let dest = chooseOutput(asset.originalURL.deletingPathExtension().lastPathComponent) else { return }
                    model.startNarrowbandJob { service in
                        try await session.run(["narrowband-extract", "--input", asset.originalURL.path, "--output-dir", dest.path] + session.extractionArguments,
                                              output: dest, using: service)
                    }
                }.disabled(model.selectedAsset?.kind != .fits || !session.canCalibrate).accessibilityIdentifier("narrowband.extract")
                Button(zh ? "选择原片并堆栈…" : "Stack dual-band exposures…") {
                    let panel = NativeFilePanels.makeImportFilesPanel(message: zh ? "选择至少 3 张同一目标的双窄带亮场。" : "Select at least 3 dual-band exposures of the same target.")
                    panel.title = zh ? "双窄带亮场" : "Dual-band exposures"
                    panel.prompt = zh ? "选择亮场" : "Choose exposures"
                    panel.allowedContentTypes = ["fit", "fits", "fts"].compactMap { UTType(filenameExtension: $0) }
                    guard panel.runModal() == .OK else { return }
                    let frames = panel.urls
                    guard frames.count >= 3 else {
                        let alert = NSAlert()
                        alert.messageText = zh ? "至少选择 3 张亮场" : "Select at least 3 exposures"
                        alert.informativeText = zh ? "当前选择了 \(frames.count) 张。单张原片可使用“提取并保存”。" : "You selected \(frames.count). Use line extraction for a single exposure."
                        alert.runModal()
                        return
                    }
                    guard let dest = chooseOutput("HOO-stack") else { return }
                    model.startNarrowbandJob { service in
                        try await session.run(["narrowband-stack", "--output-dir", dest.path] + session.extractionArguments + frames.map(\.path),
                                              output: dest, using: service)
                    }
                }.disabled(!session.canCalibrate).accessibilityIdentifier("narrowband.stack")
            }
            Divider()
            Button(zh ? "打开双窄带结果…" : "Open narrowband result…") {
                let panel = NativeFilePanels.makeImportFolderPanel(message: zh ? "选择含 recipe.txt 的双窄带结果目录。" : "Choose a narrowband result folder containing recipe.txt.")
                guard panel.runModal() == .OK, let directory = panel.url else { return }
                model.startNarrowbandJob { service in try await session.open(directory, using: service) }
            }.accessibilityIdentifier("narrowband.open")
            if let report = session.report {
                Text(zh ? "已保存参数 · \(report.frames) 个输入" : "Recipe saved · \(report.frames) inputs")
                    .font(.caption).foregroundStyle(.secondary)
                if let mode = report.extractionSampling {
                    Text((zh ? "结果采样：" : "Result sampling: ") + mode.title(chinese: zh))
                        .font(.caption).foregroundStyle(.secondary)
                }
                HStack {
                    ForEach(report.outputs.filter { ["Ha.fits", "OIII.fits", "HOO.fits"].contains($0) }, id: \.self) { name in
                        Button(name.replacingOccurrences(of: ".fits", with: "")) {
                            model.startNarrowbandJob { service in try await session.verifiedOutput(name, using: service) }
                        }.accessibilityIdentifier("narrowband.view." + name)
                    }
                }
                Menu(zh ? "查看覆盖与采用权重" : "View coverage & accepted weights") {
                    ForEach(report.outputs.filter { !["Ha.fits", "OIII.fits", "HOO.fits"].contains($0) }, id: \.self) { name in
                        Button(name) { model.startNarrowbandJob { service in try await session.verifiedOutput(name, using: service) } }
                    }
                }
                if report.operation == "compose" || report.outputs.contains("Ha.fits") {
                    ParameterSlider(title: zh ? "重配绿色中 Hα 比例" : "Remix Hα in green",
                                    value: $session.greenHydrogen, range: 0...1)
                    Button(zh ? "重配并另存 HOO…" : "Remix & save HOO…") {
                        guard let dest = chooseOutput("HOO-remix") else { return }
                        model.startNarrowbandJob { service in
                            if report.operation == "compose" { return try await session.replay(output: dest, using: service) }
                            return try await session.remix(output: dest, using: service)
                        }
                    }.accessibilityIdentifier("narrowband.remix")
                }
                Button(zh ? "按原参数重跑…" : "Run saved recipe…") {
                    guard let dest = chooseOutput("narrowband-replay") else { return }
                    model.startNarrowbandJob { service in try await session.replay(output: dest, using: service) }
                }.accessibilityIdentifier("narrowband.replay")
                Text(report.operation == "compose"
                     ? (zh ? "合色阶段不重复应用传感器校准。" : "Sensor calibration is not repeated during composition.")
                     : report.calibration == nil
                        ? (zh ? "此结果未应用传感器校准。" : "This recipe applies no sensor calibration.")
                        : (zh ? "此结果已在通带提取前应用传感器校准。" : "Sensor calibration was applied before line extraction."))
                    .font(.caption).foregroundStyle(.secondary)
                Text(zh ? "输入与结果均校验摘要。采用权重是 Sigma 剔除后有效覆盖权重之和；几何覆盖率为剔除前的帧覆盖比例。双窄带堆栈只使用两条通带都有效的像素格。"
                     : "Inputs and results are hash-checked. Accepted weights sum post-Sigma coverage; geometric coverage is measured before clipping. Stacking uses cells valid in both bands.")
                    .font(.caption2).foregroundStyle(.secondary)
            }
        }.disabled(model.hasActiveProcessing)
    }

    private func linePicker(_ name: String, selection: Binding<URL?>) -> some View {
        HStack {
            Button(name + "…") {
                let panel = NativeFilePanels.makeRelinkAssetPanel(message: zh ? "选择已配准的单色线性母片。" : "Choose a registered linear mono master.")
                panel.allowedContentTypes = ["fit", "fits", "fts"].compactMap { UTType(filenameExtension: $0) }
                if panel.runModal() == .OK { selection.wrappedValue = panel.url }
            }
            if let url = selection.wrappedValue { Text(url.lastPathComponent).lineLimit(1).help(url.path) }
        }
    }

    private func chooseOutput(_ prefix: String) -> URL? {
        let panel = NativeFilePanels.makeBatchOutputDirectoryPanel(message: zh ? "将在所选目录中新建结果文件夹。" : "A new result folder will be created here.")
        panel.prompt = zh ? "保存到此处" : "Save here"
        guard panel.runModal() == .OK, let parent = panel.url else { return nil }
        return parent.appendingPathComponent(prefix + "-" + UUID().uuidString.prefix(8), isDirectory: true)
    }
}
