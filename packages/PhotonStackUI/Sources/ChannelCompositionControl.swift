import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct ChannelCompositionControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @ObservedObject var session: ChannelCompositionSession
    @State private var preset: ChannelPreset = .hoo
    private var zh: Bool { model.language == .simplifiedChinese }
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Picker(zh ? "映射起点" : "Mapping preset", selection: $preset) {
                ForEach(ChannelPreset.allCases, id: \.self) { p in Text(p == .custom && zh ? "自定义" : p.rawValue).tag(p) }
            }.onChange(of: preset) { _, value in session.apply(value) }
                .accessibilityIdentifier("channels.preset")
            Text(zh ? "选择已校准、配准到同一像素网格的单色线性 FITS。合色不自动匹配背景、曝光或星点宽度。"
                 : "Choose calibrated, registered linear mono FITS. Composition does not match background, exposure or star widths.")
                .font(.caption).foregroundStyle(.secondary)
            ForEach(session.masters) { master in
                HStack {
                    TextField(zh ? "变量" : "Symbol", text: Binding(get: {
                        session.masters.first(where: { $0.id == master.id })?.name ?? master.name
                    }, set: { value in
                        if let index = session.masters.firstIndex(where: { $0.id == master.id }) { session.masters[index].name = value }
                    })).frame(width: 48).textFieldStyle(.roundedBorder)
                    Button(master.path.isEmpty ? (zh ? "选择母片…" : "Choose master…") : URL(fileURLWithPath: master.path).lastPathComponent) {
                        chooseMaster(master.id)
                    }.lineLimit(1).help(master.path).accessibilityIdentifier("channels.master." + master.name)
                    Spacer(minLength: 0)
                    Button { session.masters.removeAll { $0.id == master.id } } label: { Image(systemName: "minus.circle") }
                        .buttonStyle(.borderless).help(zh ? "移除母片" : "Remove master")
                }
            }
            Button(zh ? "添加母片…" : "Add master…") {
                let existing = Set(session.masters.map(\.name))
                let name = (1...17).map { "M\($0)" }.first { !existing.contains($0) } ?? "Master"
                let master = ChannelMaster(name: name); session.masters.append(master); chooseMaster(master.id)
            }.disabled(session.masters.count >= 16).accessibilityIdentifier("channels.add")
            MasterRegistrationControl(model: model, composition: session, session: session.registration)
            ForEach(0..<3, id: \.self) { channel in
                HStack {
                    Text(["R", "G", "B"][channel]).frame(width: 16)
                    TextField(zh ? "通道表达式" : "Channel expression", text: $session.expressions[channel])
                        .textFieldStyle(.roundedBorder).font(.system(.caption, design: .monospaced))
                        .accessibilityIdentifier("channels.expression." + ["R", "G", "B"][channel])
                }
            }
            Text(zh ? "支持 + − * /、括号、abs、sqrt、min、max、pow。变量区分大小写；保留负数和线性强度。"
                 : "Supports + − * /, parentheses, abs, sqrt, min, max, pow. Symbols are case-sensitive. Signed linear values are retained.")
                .font(.caption2).foregroundStyle(.secondary)
            if preset == .lrgb {
                Text(zh ? "LRGB 按 L / (0.2126R + 0.7152G + 0.0722B) 缩放 RGB，保留通道比；需自行匹配强度。分母为零处无效。"
                     : "LRGB scales RGB by L / (0.2126R + 0.7152G + 0.0722B), preserving ratios. Match intensities first; zero denominators are invalid.")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            Toggle(zh ? "确认母片已配准到相同网格" : "Masters are registered on the same grid", isOn: $session.aligned)
                .toggleStyle(.checkbox).accessibilityIdentifier("channels.aligned")
            Button(zh ? "合色并另存线性母片…" : "Compose & save linear master…") {
                guard let output = chooseOutput() else { return }
                model.startChannelCompositionJob { service in try await session.compose(output: output, using: service) }
            }.disabled(!session.canCompose).accessibilityIdentifier("channels.compose")
            Divider()
            HStack {
                Button(zh ? "打开合色配方…" : "Open composition…") {
                    let panel = NativeFilePanels.makeImportFolderPanel(message: zh ? "选择含 recipe.txt 的多滤镜合色目录。" : "Choose a channel composition folder containing recipe.txt.")
                    guard panel.runModal() == .OK, let directory = panel.url else { return }
                    model.startChannelCompositionJob { service in
                        let output = try await session.open(directory, using: service)
                        preset = .custom
                        return output
                    }
                }.accessibilityIdentifier("channels.open")
                if session.report != nil {
                    Button(zh ? "新配方" : "New recipe") { session.newComposition() }
                }
            }
            if session.report != nil {
                HStack {
                    Button(zh ? "查看合色" : "View RGB") { show("RGB.fits") }.accessibilityIdentifier("channels.view")
                    Button(zh ? "查看有效范围" : "View validity") { show("validity.fits") }
                }
                Text(zh ? "来源与产品均校验摘要。更改表达式后可另存，已保存母片不会被覆盖。" : "Sources and products are hash-checked. Save edited expressions as a new result.")
                    .font(.caption2).foregroundStyle(.secondary)
            }
            if let run = session.lastRun, let invalid = run.invalidPixels {
                Text(zh ? "无效像素：\(invalid) · \(run.wcsPreserved == true ? "已保留天球坐标" : "无共同天球坐标")"
                     : "Invalid pixels: \(invalid) · \(run.wcsPreserved == true ? "WCS retained" : "No common WCS")")
                    .font(.caption).foregroundStyle(.secondary)
            }
        }.disabled(model.hasActiveProcessing)
    }
    private func show(_ name: String) { model.startChannelCompositionJob { service in try await session.verifiedOutput(name, using: service) } }
    private func chooseMaster(_ id: UUID) {
        let panel = NativeFilePanels.makeRelinkAssetPanel(message: zh ? "选择已配准的单色线性 FITS 母片。" : "Choose a registered linear mono FITS master.")
        panel.allowedContentTypes = ["fit", "fits", "fts"].compactMap { UTType(filenameExtension: $0) }
        guard panel.runModal() == .OK, let url = panel.url, let index = session.masters.firstIndex(where: { $0.id == id }) else { return }
        session.masters[index].path = url.path
        session.aligned = false
    }
    private func chooseOutput() -> URL? {
        let panel = NativeFilePanels.makeBatchOutputDirectoryPanel(message: zh ? "将在所选目录中新建合色结果文件夹。" : "A new composition folder will be created here.")
        guard panel.runModal() == .OK, let parent = panel.url else { return nil }
        return parent.appendingPathComponent("channels-" + UUID().uuidString.prefix(8), isDirectory: true)
    }
}
