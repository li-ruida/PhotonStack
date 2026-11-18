import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct SpectralProfileLibraryView: View {
    let chinese: Bool
    let library: SpectralProfileLibrary
    let use: (SpectralProfileLibrary.Selection) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var entries: [SpectralProfileLibrary.Entry] = []
    @State private var selection: String?
    @State private var inspected: SpectralProfileLibrary.Selection?
    @State private var archived = false
    @State private var name = ""
    @State private var error: String?
    @State private var operation: Task<Void, Never>?
    private var selected: SpectralProfileLibrary.Entry? { entries.first { $0.id == selection } }
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    var body: some View {
        VStack(spacing: 12) {
            HStack {
                Text(t("光谱响应配置库", "Spectral response library")).font(.headline)
                Spacer()
                Button(t("导入配置…", "Import profile…")) { importProfile() }.disabled(operation != nil).accessibilityIdentifier("spcc.library.import")
                Button(t("关闭", "Close")) { dismiss() }.keyboardShortcut(.cancelAction)
            }
            HStack(alignment: .top, spacing: 14) {
                VStack(alignment: .leading, spacing: 8) {
                    Toggle(t("已归档", "Archived"), isOn: $archived).disabled(operation != nil).accessibilityIdentifier("spcc.library.archived")
                    List(entries, selection: $selection) { e in
                        VStack(alignment: .leading, spacing: 3) {
                            Text(e.label).lineLimit(2)
                            Text(e.issue == nil ? quality(e.profile?.quality ?? "") : t("配置记录损坏", "Damaged record"))
                                .font(.caption).foregroundStyle(e.issue == nil ? Color.secondary : .orange)
                        }.tag(e.id)
                    }.listStyle(.bordered).disabled(operation != nil).accessibilityIdentifier("spcc.library.list")
                    Text(t("导入后保存独立副本。相同内容自动合并；同名但不同的响应各自保留。", "Imports are independent copies. Identical content is deduplicated; different responses with the same name remain separate.")).font(.caption).foregroundStyle(.secondary)
                }.frame(width: 230)
                ScrollView {
                    VStack(alignment: .leading, spacing: 10) {
                        if let e = selected {
                            HStack {
                                TextField(t("配置名称", "Profile name"), text: $name).textFieldStyle(.roundedBorder).accessibilityIdentifier("spcc.library.name")
                                Button(t("改名", "Rename")) { perform { try await library.rename(e, label: name); try await reload(prefer: e.id) } }.disabled(operation != nil || name == e.label || e.issue != nil)
                            }
                            if let s = inspected { details(s) }
                            else if let issue = e.issue { Text(issue).foregroundStyle(.orange).textSelection(.enabled) }
                        } else {
                            ContentUnavailableView(t("选择相机与滤镜响应", "Choose camera and filter response"), systemImage: "waveform.path",
                                description: Text(t("导入已有 profile.txt，查看通带、白参考及来源后用于校准。", "Import a profile.txt, inspect its passbands, white reference and provenance, then use it for calibration.")))
                        }
                    }.frame(maxWidth: .infinity, alignment: .leading)
                }
            }
            if let error { Text(error).font(.caption).foregroundStyle(.red).textSelection(.enabled).lineLimit(3).frame(maxWidth: .infinity, alignment: .leading) }
            HStack {
                if operation != nil { ProgressView().controlSize(.small) }
                if let e = selected {
                    Button(archived ? t("恢复到配置库", "Restore") : t("归档", "Archive")) {
                        perform { try await library.archive(e); inspected = nil; try await reload() }
                    }.disabled(operation != nil).accessibilityIdentifier("spcc.library.archive")
                    Button(t("导出配置…", "Export profile…")) { export(e) }.disabled(operation != nil || inspected == nil).accessibilityIdentifier("spcc.library.export")
                }
                Spacer()
                Button(t("使用此配置", "Use profile")) {
                    guard let e = selected else { return }
                    perform { let verified = try await library.select(e); use(verified); dismiss() }
                }.buttonStyle(.borderedProminent).disabled(operation != nil || inspected == nil || archived).accessibilityIdentifier("spcc.library.use")
            }
        }.padding(18).frame(width: 900, height: 690)
            .task { perform { try await reload() } }
            .onChange(of: selection) { _, _ in if operation == nil { perform { try await inspectSelection() } } }
            .onChange(of: archived) { _, _ in perform { try await reload() } }
            .onDisappear { operation?.cancel() }
    }
    private func quality(_ value: String) -> String {
        switch value {
        case "measured": t("声明为实测响应", "Declared measured response")
        case "synthetic": t("模拟响应", "Synthetic response")
        default: t("近似响应", "Approximate response")
        }
    }
    private func details(_ s: SpectralProfileLibrary.Selection) -> some View {
        let r = s.inspection, low = r.bands.map { $0.support[0] }.min()!, high = r.bands.map { $0.support[1] }.max()!
        return VStack(alignment: .leading, spacing: 10) {
            Text(r.profile.name).font(.subheadline.bold()).textSelection(.enabled)
            Text(quality(r.profile.quality)).font(.caption).foregroundStyle(r.profile.quality == "measured" ? Color.secondary : .orange)
            Text(t("R / G / B 总响应（各组件透过率相乘）", "R / G / B combined response (product of components)")).font(.caption)
            SpectralResponsePlot(series: r.bands.map(\.samples), colors: [.red, .green, .blue], low: low, high: high, minimum: 0).frame(height: 135)
            Grid(alignment: .leading, horizontalSpacing: 15, verticalSpacing: 4) {
                GridRow { Text(t("通道", "Band")); Text(t("平均波长", "Mean λ")); Text(t("RMS 宽度", "RMS width")); Text(t("白参考 / G", "White / G")) }
                ForEach(0..<3) { c in
                    GridRow { Text(["R", "G", "B"][c]); Text(String(format: "%.2f nm", r.bands[c].meanWavelength)); Text(String(format: "%.2f nm", r.bands[c].rmsBandwidth)); Text(String(format: "%.5g", r.bands[c].whiteFlux / r.bands[1].whiteFlux)) }
                }
            }.font(.caption).monospacedDigit()
            Text(t("宽度为响应加权的标准差，不是半高全宽。曲线为显示采样，校准积分使用原始数据。", "Width is the response-weighted standard deviation, not FWHM. Plots are sampled for display; calibration integrates the original curves.")).font(.caption2).foregroundStyle(.secondary)
            Divider()
            Text(t("白参考：", "White reference: ") + r.profile.whiteName).font(.caption.bold())
            SpectralResponsePlot(series: [r.whiteSamples], colors: [.primary], low: r.whiteSamples.first![0], high: r.whiteSamples.last![0], minimum: min(0, r.whiteSamples.map { $0[1] }.min()!)).frame(height: 95)
            Text(t("Fλ 按绝对峰值归一，仅供检查光谱形状；校准保留原始通量。", "Fλ normalized by absolute peak for inspection only; calibration retains the original flux.")).font(.caption2).foregroundStyle(.secondary)
            Text(r.profile.provenance).font(.caption).textSelection(.enabled)
            Text(r.profile.whiteProvenance).font(.caption).textSelection(.enabled)
            DisclosureGroup(t("许可与内容校验", "Licenses & content identity")) {
                VStack(alignment: .leading, spacing: 5) {
                    Text(r.profile.license); Text(r.profile.whiteLicense)
                    Text(s.licenseIncluded ? t("已保留附带的响应许可文本；导出时一同复制。", "Included response license text is retained and exported.") : t("未附加单独许可文件；以上为配置中的许可声明。", "No separate license file supplied; license declarations are shown above."))
                    Text("SHA-256: " + r.profile.sha)
                }.font(.caption2).textSelection(.enabled)
            }.font(.caption)
            Text(t("配置不会按照片名称自动匹配。实测等级是来源声明，仍需确认相机、滤镜与整条光路一致。", "Profiles are not inferred from photo names. Measured quality is a provenance declaration; confirm the camera, filter and complete optical path.")).font(.caption2).foregroundStyle(.secondary)
        }.accessibilityElement(children: .contain).accessibilityIdentifier("spcc.library.details")
    }
    private func perform(_ action: @escaping @MainActor () async throws -> Void) {
        guard operation == nil else { return }; error = nil
        operation = Task { defer { operation = nil }; do { try await action() } catch is CancellationError {} catch { self.error = error.localizedDescription } }
    }
    private func reload(prefer: String? = nil) async throws {
        inspected = nil; entries = try await library.list(archived: archived)
        selection = entries.first { $0.id == prefer }?.id ?? entries.first?.id
        try await inspectSelection()
    }
    private func inspectSelection() async throws {
        inspected = nil; name = selected?.label ?? ""
        if let e = selected, e.issue == nil { inspected = try await library.select(e) }
    }
    private func importProfile() {
        let p = NSOpenPanel(); p.allowedContentTypes = [.plainText]
        p.message = t("选择 profile.txt；同目录的 response-source-license.txt 会一同保留。", "Choose profile.txt; response-source-license.txt from the same folder is retained when present.")
        if p.runModal() == .OK, let url = p.url {
            perform { let id = try await library.importProfile(url); archived = false; try await reload(prefer: id) }
        }
    }
    private func export(_ e: SpectralProfileLibrary.Entry) {
        let p = NSOpenPanel(); p.canChooseFiles = false; p.canChooseDirectories = true; p.canCreateDirectories = true
        p.message = t("选择存放位置，将创建独立配置文件夹。", "Choose a location for a new profile folder.")
        if p.runModal() == .OK, let parent = p.url {
            perform { let destination = parent.appendingPathComponent("SpectralProfile-" + UUID().uuidString.prefix(8)); try await library.export(e, to: destination); NSWorkspace.shared.activateFileViewerSelecting([destination]) }
        }
    }
}

private struct SpectralResponsePlot: View {
    let series: [[[Double]]]
    let colors: [Color]
    let low, high, minimum: Double
    var body: some View {
        VStack(spacing: 3) {
            Canvas { context, size in
                let rect = CGRect(origin: .zero, size: size).insetBy(dx: 1, dy: 2)
                for fraction in [0.0, 0.5, 1.0] {
                    var line = Path(); let y = rect.minY + fraction * rect.height
                    line.move(to: CGPoint(x: rect.minX, y: y)); line.addLine(to: CGPoint(x: rect.maxX, y: y))
                    context.stroke(line, with: .color(.secondary.opacity(0.2)), lineWidth: 1)
                }
                for (i, samples) in series.enumerated() {
                    var path = Path()
                    for (j, p) in samples.enumerated() {
                        let point = CGPoint(x: rect.minX + (p[0] - low) / (high - low) * rect.width,
                                            y: rect.maxY - (p[1] - minimum) / (1 - minimum) * rect.height)
                        if j == 0 { path.move(to: point) } else { path.addLine(to: point) }
                    }
                    context.stroke(path, with: .color(colors[i]), lineWidth: 1.5)
                }
            }
            HStack { Text(String(format: "%.1f nm", low)); Spacer(); Text(String(format: "%.1f nm", high)) }.font(.caption2).foregroundStyle(.secondary).monospacedDigit()
        }.accessibilityLabel("Spectral response plot, \(low) to \(high) nanometers")
    }
}
