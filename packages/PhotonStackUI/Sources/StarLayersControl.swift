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
    @State private var source: URL?
    private var zh: Bool { model.language == .simplifiedChinese }
    private var input: URL? {
        guard let url = source ?? model.currentInputURL,
              ["tif", "tiff", "fit", "fits", "fts"].contains(url.pathExtension.lowercased()) else { return nil }
        return url
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(zh ? "将线性 FITS 或 TIFF 分成星云和星点，分别调整后合成。请检查去星结果是否丢失细节。"
                 : "Use a linear FITS master or developed TIFF. Linear layers retain signed residuals and validity; identity recombination preserves the original FITS exactly. Review neural starless estimates.")
                .font(.caption).foregroundStyle(.secondary).help(zh ? "支持线性 FITS 母片和已显影 TIFF。线性分层保留负残差与有效掩膜；默认重组精确保留原始 FITS。神经网络无星图仍需人工复查。" : "Linear FITS retains signed residuals and validity. Identity recombination preserves original samples; previews are developed separately.")
            HStack {
                Button(zh ? "选择图像…" : "Choose image…") { if let selected = chooseFile() { source = selected } }
                    .accessibilityIdentifier("star-layers.source")
                if let master = model.project.deepSkySettings?.lastReport?.master, !master.isEmpty {
                    Button(zh ? "使用线性母片" : "Use linear master") { source = URL(fileURLWithPath: master) }
                }
            }
            if let input { Text(input.lastPathComponent).font(.caption).lineLimit(1).help(input.path) }
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
                            Button("StarNet 2.6+ Linear FITS…") { configureLinear() }
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
                if !session.linear {
                    ParameterSlider(title: zh ? "星云拉伸" : "Nebula stretch", value: $session.settings.nebulaGamma, range: 0.25...4)
                } else {
                    Text(zh ? "当前为线性图。可调整曝光和色彩，合成后再用 GHS 拉伸。" : "Linear mode applies exposure and linear color mixing. Use GHS after recombination for nonlinear stretching. Previews are developed; FITS retains linear values.")
                        .font(.caption2).foregroundStyle(.secondary)
                }
                ParameterSlider(title: zh ? "星云饱和度" : "Nebula saturation", value: $session.settings.nebulaSaturation, range: 0...3)
                ParameterSlider(title: zh ? "恒星曝光" : "Star exposure", value: $session.settings.starsEV, range: -8...4)
                ParameterSlider(title: zh ? "恒星饱和度" : "Star saturation", value: $session.settings.starsSaturation, range: 0...3)
                HStack {
                    Button(zh ? "重置" : "Reset") { session.settings = StarLayerRecipe() }
                    Spacer()
                    Button(zh ? "应用并保存" : "Apply & save") {
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
                Text(zh ? "保存 TIFF、FITS 和调整参数。下次打开分层文件夹即可继续编辑。"
                     : "Each apply saves TIFF and unclipped FITS. Reopen this folder to restore the recipe. Previews are developed separately; science layers retain negative residuals.")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            if let error { ProcessingErrorNotice(message:error,chinese:zh) }
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
    private func configureLinear() {
        guard let executable = chooseFile(message: zh ? "选择支持 --linear 和 FITS 的新版 starnet2；保留其同目录模型和运行库" : "Choose starnet2 with --linear and FITS support; keep its model and runtime folder intact") else { return }
        attempt {
            var tool = ExternalImageTool(name: "StarNet Linear FITS", executable: executable,
                arguments: ["--input", "{input}", "--output", "{output}", "--linear"], linearFITS: true)
            let bundled = executable.resolvingSymlinksInPath().deletingLastPathComponent().appendingPathComponent("LICENSE.txt")
            let licenseURL: URL
            if FileManager.default.fileExists(atPath: bundled.path) { licenseURL = bundled }
            else {
                guard let selected = chooseFile(message: zh ? "选择这个 StarNet 版本随附的完整 LICENSE.txt" : "Choose the full LICENSE.txt supplied with this StarNet version") else { return }
                licenseURL = selected
            }
            let license = try ExternalImageTool.license(at: licenseURL)
            let alert = NSAlert()
            alert.messageText = zh ? "StarNet 使用许可" : "StarNet license"
            alert.informativeText = zh ? "去星由外部 StarNet 执行。请阅读以下完整条款；明确同意后才会启用此工具。" : "Star removal runs in external StarNet. Read the complete terms below; this tool is enabled only after acceptance."
            alert.addButton(withTitle: zh ? "我已阅读并同意" : "I have read and agree")
            alert.addButton(withTitle: zh ? "取消" : "Cancel")
            let scroll = NSScrollView(frame: NSRect(x: 0, y: 0, width: 620, height: 320))
            let text = NSTextView(frame: scroll.bounds)
            text.isEditable = false; text.isSelectable = true; text.string = license.text
            text.font = .systemFont(ofSize: 12); text.isVerticallyResizable = true; text.isHorizontallyResizable = false
            text.textContainer?.widthTracksTextView = true
            scroll.documentView = text; scroll.hasVerticalScroller = true; alert.accessoryView = scroll
            guard alert.runModal() == .alertFirstButtonReturn else { return }
            tool.licenseFile = licenseURL; tool.acceptedLicenseSHA256 = license.sha256
            try installTool(tool)
        }
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
            let linear = ["fit", "fits", "fts"].contains(input.pathExtension.lowercased())
            guard let file = chooseFile(types: linear ? nil : [.tiff], message: linear
                ? (zh ? "选择相同尺寸、位置及物理数值单位的线性无星 FITS，不能用已拉伸图" : "Choose a same-grid linear starless FITS in identical physical units, not a stretched image")
                : (zh ? "选择与当前成片同尺寸、同位置的无星 TIFF" : "Choose a starless TIFF with the same dimensions and alignment")) else { return }
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
