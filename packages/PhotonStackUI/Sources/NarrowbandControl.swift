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
                Text(zh ? "选中一张 Hα/OⅢ 双窄带拜耳 FITS。每个完整的 2×2 像素格生成一个 Hα 和一个 OⅢ 样本。"
                     : "Select an Hα/OⅢ dual-band Bayer FITS. Each complete 2×2 cell yields one sample per line.")
                    .font(.caption)
                Button(zh ? "提取并保存两张 FITS…" : "Extract and save both FITS…") {
                    guard let asset = model.selectedAsset, let dest = chooseOutput(asset.originalURL.deletingPathExtension().lastPathComponent) else { return }
                    model.startNarrowbandJob { service in
                        try await session.run(["narrowband-extract", "--input", asset.originalURL.path, "--output-dir", dest.path],
                                              output: dest, using: service)
                    }
                }.disabled(model.selectedAsset?.kind != .fits).accessibilityIdentifier("narrowband.extract")
                Button(zh ? "选择多张原片并堆栈…" : "Stack dual-band exposures…") {
                    let panel = NSOpenPanel()
                    panel.allowsMultipleSelection = true
                    panel.allowedContentTypes = ["fit", "fits", "fts"].compactMap { UTType(filenameExtension: $0) }
                    guard panel.runModal() == .OK else { return }
                    let frames = panel.urls
                    guard let dest = chooseOutput("HOO-stack") else { return }
                    model.startNarrowbandJob { service in
                        try await session.run(["narrowband-stack", "--output-dir", dest.path] + frames.map(\.path),
                                              output: dest, using: service)
                    }
                }
            }
            Divider()
            Button(zh ? "打开双窄带结果…" : "Open narrowband result…") {
                let panel = NSOpenPanel()
                panel.canChooseDirectories = true; panel.canChooseFiles = false
                guard panel.runModal() == .OK, let directory = panel.url else { return }
                model.startNarrowbandJob { service in try await session.open(directory, using: service) }
            }.accessibilityIdentifier("narrowband.open")
            if let report = session.report {
                Text(zh ? "已保存配方 · \(report.frames) 个输入" : "Recipe saved · \(report.frames) inputs")
                    .font(.caption).foregroundStyle(.secondary)
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
                Button(zh ? "按配方重新运行…" : "Run saved recipe…") {
                    guard let dest = chooseOutput("narrowband-replay") else { return }
                    model.startNarrowbandJob { service in try await session.replay(output: dest, using: service) }
                }.accessibilityIdentifier("narrowband.replay")
                Text(zh ? "输入与结果均校验摘要。采用权重是 Sigma 剔除后有效覆盖权重之和；几何覆盖率为剔除前的帧覆盖比例。双窄带堆栈只使用两条通带都有效的像素格，尚未进行暗偏平校准。"
                     : "Inputs and results are hash-checked. Accepted weights sum post-Sigma coverage; geometric coverage is measured before clipping. Stacking uses cells valid in both bands. No bias/dark/flat calibration is applied.")
                    .font(.caption2).foregroundStyle(.secondary)
            }
        }.disabled(model.hasActiveProcessing)
    }

    private func linePicker(_ name: String, selection: Binding<URL?>) -> some View {
        HStack {
            Button(name + "…") {
                let panel = NSOpenPanel()
                panel.allowedContentTypes = ["fit", "fits", "fts"].compactMap { UTType(filenameExtension: $0) }
                if panel.runModal() == .OK { selection.wrappedValue = panel.url }
            }
            if let url = selection.wrappedValue { Text(url.lastPathComponent).lineLimit(1).help(url.path) }
        }
    }

    private func chooseOutput(_ prefix: String) -> URL? {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.canCreateDirectories = true
        panel.prompt = zh ? "保存到此处" : "Save here"
        guard panel.runModal() == .OK, let parent = panel.url else { return nil }
        return parent.appendingPathComponent(prefix + "-" + UUID().uuidString.prefix(8), isDirectory: true)
    }
}
