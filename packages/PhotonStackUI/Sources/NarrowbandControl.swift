import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct NarrowbandControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
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
                    model.startNarrowband(arguments: ["narrowband-compose", "--ha", hydrogen.path, "--oiii", oxygen.path,
                        "--green-ha", String(greenHydrogen), "--output-dir", dest.path], directory: dest, outputName: "HOO.fits")
                }.disabled(hydrogen == nil || oxygen == nil)
            } else {
                Text(zh ? "选中一张 Hα/OⅢ 双窄带拜耳 FITS。每个完整的 2×2 像素格生成一个 Hα 和一个 OⅢ 样本。"
                     : "Select an Hα/OⅢ dual-band Bayer FITS. Each complete 2×2 cell yields one sample per line.")
                    .font(.caption)
                Button(zh ? "提取并保存两张 FITS…" : "Extract and save both FITS…") {
                    guard let asset = model.selectedAsset, let dest = chooseOutput(asset.originalURL.deletingPathExtension().lastPathComponent) else { return }
                    model.startNarrowband(arguments: ["narrowband-extract", "--input", asset.originalURL.path, "--output-dir", dest.path],
                                          directory: dest, outputName: "Ha.fits")
                }.disabled(model.selectedAsset?.kind != .fits)
                Button(zh ? "选择多张原片并堆栈…" : "Stack dual-band exposures…") {
                    let panel = NSOpenPanel()
                    panel.allowsMultipleSelection = true
                    panel.allowedContentTypes = ["fit", "fits", "fts"].compactMap { UTType(filenameExtension: $0) }
                    guard panel.runModal() == .OK else { return }
                    let frames = panel.urls
                    guard let dest = chooseOutput("HOO-stack") else { return }
                    model.startNarrowband(arguments: ["narrowband-stack", "--output-dir", dest.path] + frames.map(\.path),
                                          directory: dest, outputName: "HOO.fits")
                }
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
