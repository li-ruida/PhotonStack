import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct SpectralCalibrationControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var presented = false
    var body: some View {
        Button(model.language == .simplifiedChinese ? "光谱星色校准（SPCC）…" : "Spectral color calibration…") { presented = true }
            .disabled(model.currentInputURL == nil || model.hasActiveProcessing).accessibilityIdentifier("spcc.open")
            .sheet(isPresented: $presented) { if let source = model.currentInputURL { SpectralCalibrationView(model: model, source: source) } }
    }
}
struct SpectralCalibrationView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @StateObject private var session: SpectralCalibrationSession
    @Environment(\.dismiss) private var dismiss
    @State private var operation: Task<Void, Never>?
    @State private var selected: String?
    @State private var before = false
    @State private var showStars = true
    @State private var filter = "all"
    @State private var query = ""
    @State private var saved: URL?
    @State private var libraryPresented = false
    @State private var gaiaPresented = false
    private var zh: Bool { model.language == .simplifiedChinese }
    private func text(_ zh: String, _ en: String) -> String { self.zh ? zh : en }
    private var locked: Bool { session.busy || operation != nil }
    init(model: PhotonStackWorkspaceModel, source: URL) {
        self.model = model; _session = StateObject(wrappedValue: SpectralCalibrationSession(source: source))
    }
    var body: some View {
        VStack(spacing: 12) {
            HStack {
                Text(text("光谱星色校准 · SPCC", "Spectral color calibration · SPCC")).font(.headline)
                Spacer()
                Button(text("选择线性母片…", "Choose linear master…")) { chooseSource() }.disabled(locked).accessibilityIdentifier("spcc.source")
                Button(text("打开审查结果…", "Open review…")) { open() }.disabled(locked).accessibilityIdentifier("spcc.reopen")
                Button(text("关闭", "Close")) { dismiss() }.keyboardShortcut(.cancelAction)
            }
            HStack(alignment: .top, spacing: 14) {
                preview
                ScrollView { VStack(alignment: .leading, spacing: 10) {
                    inputs.disabled(locked)
                    if let r = session.report {
                        summary(r)
                        starList(r).frame(height: 220)
                        if let star = r.stars.first(where: { $0.id == selected }) { detail(star) }
                    } else {
                        Text(text("使用已解算的宽带线性 RGB FITS。响应应包含相机、滤镜等光路；星表坐标须对应拍摄日期。", "Use a solved broadband linear RGB FITS. Supply the camera/filter response and catalog positions at the observation epoch.")).font(.caption).foregroundStyle(.secondary)
                        Spacer()
                    }
                }.frame(maxWidth: .infinity, alignment: .leading) }.frame(width: 360)
            }
            if let error = session.error { Text(error).font(.caption).foregroundStyle(.red).lineLimit(3).textSelection(.enabled).frame(maxWidth: .infinity, alignment: .leading) }
            HStack {
                if locked { ProgressView().controlSize(.small); Button(text("取消", "Cancel")) { session.cancel(); operation?.cancel() } }
                if !session.isCurrent, session.report != nil { Text(text("设置已变更，请重新审查。", "Settings changed; review again.")).font(.caption).foregroundStyle(.orange) }
                if let saved { Button(text("查看保存位置", "Show saved review")) { NSWorkspace.shared.activateFileViewerSelecting([saved]) } }
                Spacer()
                Button(text("保存审查与配方…", "Save review & recipe…")) { save(use: false) }.disabled(locked || !session.isCurrent).accessibilityIdentifier("spcc.save")
                Button(text("另存并使用校准母片…", "Save & use calibrated master…")) { save(use: true) }
                    .buttonStyle(.borderedProminent).disabled(locked || !session.isCurrent || session.report?.fit.valid != true).accessibilityIdentifier("spcc.use")
            }
        }.padding(18).frame(width: 1140, height: 800)
            .sheet(isPresented: $libraryPresented) {
                SpectralProfileLibraryView(chinese: zh, library: SpectralProfileLibrary()) { session.chooseProfile($0.url, name: $0.inspection.profile.name) }
            }
            .sheet(isPresented: $gaiaPresented) {
                GaiaCatalogDownloadView(source: session.source, chinese: zh) { session.chooseCatalog($0); selected = nil }
            }
            .onDisappear { let pending = operation; pending?.cancel(); Task { await pending?.value; session.close() } }
    }
    private var preview: some View {
        VStack(spacing: 8) {
            HStack {
                Picker(text("对比", "Compare"), selection: $before) {
                    Text(text("校准前", "Before")).tag(true); Text(text("校准后", "After")).tag(false)
                }.pickerStyle(.segmented).disabled(session.afterImage == nil).accessibilityIdentifier("spcc.compare")
                Toggle(text("星点", "Stars"), isOn: $showStars).toggleStyle(.button).accessibilityIdentifier("spcc.markers")
            }
            if let report = session.report, let image = before ? session.beforeImage : (session.afterImage ?? session.beforeImage) {
                SpectralCalibrationCanvas(report: report, image: image, showStars: showStars, selected: $selected)
                Text(text("黄色：拟合　青色：独立留出　橙色：剔除 · 点击星点可查看原因", "Yellow: fit · Cyan: held out · Orange: rejected · Click a star for details")).font(.caption2).foregroundStyle(.secondary)
                if report.fit.valid {
                    SpectralResidualPlot(report: report, before: before, selected: selected).frame(height: 115)
                    Text(text("相对白参考的对数颜色残差。留出星不参与增益拟合。", "Log color residuals relative to the white reference. Held-out stars do not fit the gains.")).font(.caption2).foregroundStyle(.secondary)
                }
            } else {
                ContentUnavailableView(text("审查恒星颜色与仪器响应", "Review star colors and system response"), systemImage: "star.leadinghalf.filled",
                    description: Text(text("选择离线响应配置和星表后，点击“校准并审查”。", "Choose an offline response profile and catalog, then calibrate and review."))).frame(maxHeight: .infinity)
            }
            Text(session.source.lastPathComponent).font(.caption).lineLimit(1).textSelection(.enabled)
            Text(text("预览使用共同显示尺度；保存的科学母片保留线性值、负值和覆盖，不做拉伸。", "Previews share a display scale. Scientific masters preserve linear values, negative pixels and coverage without stretching.")).font(.caption2).foregroundStyle(.secondary)
        }.frame(maxWidth: .infinity, maxHeight: .infinity)
    }
    private var inputs: some View {
        VStack(alignment: .leading, spacing: 8) {
            Button(text("响应配置库…", "Response library…")) { libraryPresented = true }.accessibilityIdentifier("spcc.library.open")
            HStack { Button(text("响应配置…", "Response profile…")) { chooseData(profile: true) }.accessibilityIdentifier("spcc.profile"); Text(session.profileName ?? session.profile?.lastPathComponent ?? text("未选择", "Not selected")).lineLimit(2).font(.caption).help(session.profile?.path ?? "") }
            HStack { Button(text("光谱星表…", "Spectral catalog…")) { chooseData(profile: false) }.accessibilityIdentifier("spcc.catalog"); Text(session.catalog?.lastPathComponent ?? text("未选择", "Not selected")).lineLimit(1).font(.caption) }
            Button(text("获取 Gaia 光谱星表…", "Get Gaia spectral catalog…")) { gaiaPresented = true }.accessibilityIdentifier("spcc.gaia.open")
            DisclosureGroup(text("孔径与质量设置", "Aperture & quality")) {
                VStack(spacing: 5) {
                    number(text("孔径半径 (px)", "Aperture radius (px)"), \.radius, "radius")
                    number(text("背景内半径", "Sky inner radius"), \.skyInner, "sky-inner")
                    number(text("背景外半径", "Sky outer radius"), \.skyOuter, "sky-outer")
                    number(text("最低单通道信噪比", "Minimum channel SNR"), \.minSNR, "snr")
                    number(text("饱和值（0 = 未知）", "Saturation (0 = unknown)"), \.saturation, "saturation")
                }.padding(.top, 5)
            }.font(.caption)
            HStack {
                Button(text("校准并审查", "Calibrate & review")) { session.begin() }.buttonStyle(.borderedProminent).disabled(!session.canReview).accessibilityIdentifier("spcc.run")
                if !session.settings.excluded.isEmpty { Button(text("清除排除", "Clear exclusions")) { session.settings.excluded = [] } }
            }
        }
    }
    private func number(_ title: String, _ key: WritableKeyPath<SpectralCalibrationSettings, Double>, _ id: String) -> some View {
        HStack { Text(title); Spacer(); TextField(title, value: Binding(get: { session.settings[keyPath: key] }, set: { session.settings[keyPath: key] = $0 }), format: .number.precision(.fractionLength(0...3))).frame(width: 75).textFieldStyle(.roundedBorder).accessibilityIdentifier("spcc."+id) }
    }
    private func summary(_ r: SpectralCalibrationReport) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(r.fit.valid ? text("通过当前质量门限", "Passed current quality limits") : text("未通过校准验收", "Calibration did not pass")).font(.subheadline.bold()).foregroundStyle(r.fit.valid ? Color.primary : .orange)
            Text(text("拟合 \(r.fit.training) · 留出 \(r.fit.validation) · 覆盖 \(r.fit.regions)/16 区", "Fit \(r.fit.training) · Held out \(r.fit.validation) · Regions \(r.fit.regions)/16")).font(.caption)
            if r.fit.valid {
                Text("RGB " + triplet(r.fit.gains)).font(.caption).monospacedDigit()
                Text("P90 ln(R/G), ln(B/G): " + [r.fit.validationP90[0],r.fit.validationP90[2]].map { $0.map { String(format:"%.4f",$0) } ?? "—" }.joined(separator: " / ")).font(.caption2)
            } else { Text(r.fit.reason).font(.caption).foregroundStyle(.orange).textSelection(.enabled) }
            DisclosureGroup(text("响应与来源", "Response & provenance")) {
                VStack(alignment: .leading, spacing: 4) {
                    Text(r.profile.name + " · " + r.profile.quality)
                    Text(text("白参考：", "White: ")+r.profile.whiteName)
                    Text(r.catalog.name + String(format:" · J%.5f", r.catalog.epoch))
                    Text(r.profile.provenance); Text(r.profile.whiteProvenance); Text(r.catalog.provenance)
                    Text(r.profile.license); Text(r.profile.whiteLicense); Text(r.catalog.license)
                    Text(String(format: "P90 ≤ %.3f · Δ aperture ≤ %.3f · spectral error ≤ %.3f", r.options.maxValidationLogError, r.options.maxApertureColorChange, r.options.maxRelativeSpectralError))
                }.font(.caption2).textSelection(.enabled)
            }.font(.caption)
            if r.profile.quality != "measured" { Text(text("当前响应为近似或模拟配置，不等同于整机实测响应。", "Approximate or synthetic response; not a measured full optical system.")).font(.caption2).foregroundStyle(.orange) }
            Text(text("通过拟合不代表已验证仪器响应或绝对星色精度。", "Passing the fit does not verify the response or absolute color accuracy.")).font(.caption2).foregroundStyle(.secondary)
        }
    }
    private func starList(_ r: SpectralCalibrationReport) -> some View {
        let rows = r.stars.filter { (query.isEmpty || $0.id.localizedCaseInsensitiveContains(query)) && (filter == "all" || (filter == "used" ? $0.used : !$0.used)) }
        return VStack(spacing: 5) {
            HStack {
                TextField(text("搜索星表 ID", "Search catalog ID"), text: $query).textFieldStyle(.roundedBorder).accessibilityIdentifier("spcc.search")
                Picker("", selection: $filter) { Text(text("全部", "All")).tag("all"); Text(text("采用", "Used")).tag("used"); Text(text("剔除", "Rejected")).tag("rejected") }.frame(width: 90)
            }
            List(rows, selection: $selected) { star in
                HStack {
                    Circle().fill(spectralStarColor(star)).frame(width: 6, height: 6)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(star.id).font(.caption).lineLimit(1)
                        Text(spectralStarStatus(star.status, chinese: zh)).font(.caption2).foregroundStyle(.secondary)
                    }
                    Spacer()
                    if session.settings.excluded.contains(star.id) { Image(systemName: "minus.circle").foregroundStyle(.orange) }
                }.tag(star.id)
            }.listStyle(.bordered).frame(minHeight: 110).accessibilityIdentifier("spcc.stars")
        }
    }
    private func triplet(_ values: [Double?]) -> String { values.map { $0.map { String(format:"%.4g",$0) } ?? "—" }.joined(separator: " / ") }
    private func detail(_ s: SpectralCalibrationReport.Star) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(s.id).font(.caption.bold()).textSelection(.enabled)
            Text(spectralStarStatus(s.status, chinese: zh)).font(.caption)
            if s.photometry.valid {
                Text("SNR RGB " + triplet(s.photometry.snr)).font(.caption2).monospacedDigit()
                if let residual = s.residual { Text(text("对数残差 ", "Log residual ")+triplet(residual)).font(.caption2).monospacedDigit() }
                Text(text("扩大孔径颜色差 ", "Expanded-aperture color change ")+triplet(s.apertureColorChange)).font(.caption2)
            }
            Toggle(text("手动排除此星", "Exclude this star"), isOn: Binding(get: { session.settings.excluded.contains(s.id) }, set: { session.exclude(s.id, $0) })).disabled(locked || !session.canExclude).font(.caption).accessibilityIdentifier("spcc.exclude")
        }
    }
    private func chooseSource() {
        let p = NSOpenPanel(); p.allowedContentTypes = [UTType(filenameExtension:"fits") ?? .data, UTType(filenameExtension:"fit") ?? .data]
        if p.runModal() == .OK, let url = p.url { session.chooseSource(url); selected = nil; saved = nil }
    }
    private func chooseData(profile: Bool) {
        let p = NSOpenPanel(); p.allowedContentTypes = [.plainText]; p.message = profile ? text("选择 PhotonStack 光谱响应配置 profile.txt", "Choose a PhotonStack response profile.txt") : text("选择传播至拍摄日期的光谱星表 catalog.txt", "Choose a spectral catalog.txt propagated to the observation epoch")
        if p.runModal() == .OK, let url = p.url { if profile { session.chooseProfile(url) } else { session.chooseCatalog(url); selected = nil } }
    }
    private func folder() -> URL? { let p = NSOpenPanel(); p.canChooseFiles = false; p.canChooseDirectories = true; p.canCreateDirectories = true; return p.runModal() == .OK ? p.url : nil }
    private func open() {
        guard let folder = folder() else { return }; operation = Task { defer { operation = nil }; do { try await session.open(folder); session.error = nil; selected = nil; saved = nil } catch { session.error = error.localizedDescription } }
    }
    private func save(use: Bool) {
        guard let parent = folder() else { return }; let destination = parent.appendingPathComponent("SPCC-"+UUID().uuidString.prefix(8))
        operation = Task {
            defer { operation = nil }
            do {
                let image = try await session.save(to: destination); saved = destination; session.error = nil
                if use, let image { model.showDerivedImage(image); dismiss() }
            } catch { session.error = error.localizedDescription }
        }
    }
}
