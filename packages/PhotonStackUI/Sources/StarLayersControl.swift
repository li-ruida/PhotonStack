import AppKit
import SwiftUI
import UniformTypeIdentifiers
import PhotonStackAppCore
import PhotonStackProcessing

struct StarLayersControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @ObservedObject var session: StarLayerSession
    @AppStorage("starLayers.externalTool") private var toolData = Data()
    @State private var error: String?
    private var zh: Bool { model.language == .simplifiedChinese }
    private var input: URL? {
        guard let url = model.currentInputURL,
              ["tif", "tiff"].contains(url.pathExtension.lowercased()) else { return nil }
        return url
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(zh ? "先把成片显影成 TIFF，再分离星云和恒星。两层保留在独立文件夹，可重新打开继续调整。"
                 : "Start with a developed TIFF. Keep the nebula and stars in a layer folder that can be reopened.")
                .font(.caption).foregroundStyle(.secondary)
            HStack {
                Button(zh ? "导入无星图…" : "Import starless…") { separate(imported: true) }
                    .disabled(input == nil)
                Button(zh ? "打开分层…" : "Open layers…") { openLayers() }
            }
            DisclosureGroup(zh ? "外部去星工具" : "External star removal") {
                VStack(alignment: .leading, spacing: 8) {
                    Text(session.tool?.name ?? (zh ? "尚未配置" : "Not configured")).font(.caption)
                    HStack {
                        Menu(zh ? "配置工具" : "Configure") {
                            Button("StarNet 2 Torch…") { configureTorch() }
                            Button(zh ? "导入工具定义…" : "Import tool definition…") {
                                guard let url = chooseFile(types: [.json]) else { return }
                                attempt { try installTool(ExternalImageTool.load(from: url)) }
                            }
                            Button(zh ? "移除配置" : "Remove configuration") { toolData = Data(); session.tool = nil }
                        }
                        Button(zh ? "去星并分层…" : "Remove & split…") { separate(imported: false) }
                            .disabled(input == nil || session.tool == nil)
                    }
                    Text(zh ? "选择已安装的命令行工具。PhotonStack 不附带模型；工具及其输出按提供方的许可使用。"
                         : "Choose an installed command-line tool. Models are supplied separately under their provider’s terms.")
                        .font(.caption2).foregroundStyle(.secondary)
                }.padding(.top, 6)
            }
            if let directory = session.directory {
                Divider()
                Text(directory.lastPathComponent).font(.caption).lineLimit(1).help(directory.path)
                Picker(zh ? "查看" : "View", selection: $session.display) {
                    Text(zh ? "合成" : "Combined").tag("combined.tiff")
                    Text(zh ? "星云" : "Nebula").tag("starless-preview.tiff")
                    Text(zh ? "恒星" : "Stars").tag("stars-preview.tiff")
                }.pickerStyle(.segmented).accessibilityIdentifier("star-layers.view")
                    .onChange(of: session.display) { _, _ in
                        if let preview = session.preview { model.showDerivedImage(preview) }
                    }
                ParameterSlider(title: zh ? "星云曝光" : "Nebula exposure", value: $session.settings.nebulaEV, range: -4...4)
                ParameterSlider(title: zh ? "星云拉伸" : "Nebula stretch", value: $session.settings.nebulaGamma, range: 0.25...4)
                ParameterSlider(title: zh ? "星云饱和度" : "Nebula saturation", value: $session.settings.nebulaSaturation, range: 0...3)
                ParameterSlider(title: zh ? "恒星曝光" : "Star exposure", value: $session.settings.starsEV, range: -8...4)
                ParameterSlider(title: zh ? "恒星饱和度" : "Star saturation", value: $session.settings.starsSaturation, range: 0...3)
                HStack {
                    Button(zh ? "重置" : "Reset") { session.settings = StarLayerRecipe() }
                    Spacer()
                    Button(zh ? "应用并保存配方" : "Apply & save recipe") {
                        model.startStarLayerJob { service in try await session.render(using: service) }
                    }.buttonStyle(.borderedProminent).disabled(!session.settings.isValid)
                        .accessibilityIdentifier("star-layers.apply")
                }
                if session.clippedSamples > 0 {
                    Text(zh ? "有 \(session.clippedSamples) 个通道值超出 TIFF 显示范围，FITS 中保留原值。"
                         : "\(session.clippedSamples) channel values exceed the TIFF range; FITS preserves them.")
                        .font(.caption2).foregroundStyle(.orange)
                }
                Button(zh ? "在访达中查看文件" : "Show files in Finder") {
                    NSWorkspace.shared.activateFileViewerSelecting([session.rendered ?? directory])
                }
                Text(zh ? "每次应用生成 TIFF 和保留亮度范围的 FITS。打开此分层文件夹可恢复配方；查看恒星图时负残差显示为黑色，合成仍保留它。"
                     : "Each apply saves TIFF and unclipped FITS. Reopen this folder to restore the recipe. Negative residuals appear black in the star preview but remain in the blend.")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            if let error { Text(error).font(.caption).foregroundStyle(.red).textSelection(.enabled) }
        }
        .disabled(model.hasActiveProcessing)
        .onAppear {
            if session.tool == nil, !toolData.isEmpty {
                attempt {
                    let tool = try JSONDecoder().decode(ExternalImageTool.self, from: toolData)
                    try tool.validate(); session.tool = tool
                }
            }
        }
    }

    private func chooseFile(types: [UTType]? = nil, message: String? = nil) -> URL? {
        let panel = NSOpenPanel()
        panel.allowsMultipleSelection = false
        if let types { panel.allowedContentTypes = types }
        panel.message = message ?? ""
        return panel.runModal() == .OK ? panel.url : nil
    }
    private func chooseFolder(_ message: String) -> URL? {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true; panel.canChooseFiles = false
        panel.canCreateDirectories = true; panel.message = message
        return panel.runModal() == .OK ? panel.url : nil
    }
    private func attempt(_ work: () throws -> Void) {
        do { try work(); error = nil } catch { self.error = error.localizedDescription }
    }
    private func installTool(_ tool: ExternalImageTool) throws {
        try tool.validate()
        toolData = try JSONEncoder().encode(tool)
        session.tool = tool
    }
    private func configureTorch() {
        guard let executable = chooseFile(message: zh ? "选择已安装的 starnet2 命令行程序" : "Choose the installed starnet2 executable"),
              let weights = chooseFile(message: zh ? "选择 StarNet2_weights.pt 模型文件" : "Choose StarNet2_weights.pt") else { return }
        attempt {
            try installTool(ExternalImageTool(name: "StarNet 2 Torch", executable: executable,
                arguments: ["--input", "{input}", "--output", "{output}", "--weights", weights.path,
                            "--mask", "unused-mask.tiff"]))
        }
    }
    private func separate(imported: Bool) {
        guard let input else { return }
        let starless: URL?
        if imported {
            guard let file = chooseFile(types: [.tiff], message: zh ? "选择与当前成片同尺寸、同位置的无星 TIFF" : "Choose a starless TIFF with the same dimensions and alignment") else { return }
            starless = file
        } else { starless = nil }
        guard let parent = chooseFolder(zh ? "选择保存分层的目录" : "Choose where to save the layers") else { return }
        let output = parent.appendingPathComponent("StarLayers-\(UUID().uuidString.prefix(8))")
        model.startStarLayerJob { service in
            try await session.separate(input: input, starless: starless, output: output, using: service)
        }
    }
    private func openLayers() {
        guard let directory = chooseFolder(zh ? "选择包含 layers.txt 的分层文件夹" : "Choose the layer folder containing layers.txt") else { return }
        attempt {
            try session.open(directory)
            model.startStarLayerJob { service in try await session.render(using: service) }
        }
    }
}
