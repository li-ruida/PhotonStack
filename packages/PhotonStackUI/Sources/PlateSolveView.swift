import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct PlateSolveControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var presented = false
    private var zh: Bool { model.language == .simplifiedChinese }
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(zh ? "从星表识别当前母片的天区，供天球配准、拼接及坐标查看使用。" : "Identify this master against a local star catalog for sky registration and mosaics.")
                .font(.caption).foregroundStyle(.secondary)
            Button(zh ? "星图定位…" : "Plate solve & review…") { presented = true }
                .disabled(model.currentInputURL == nil || model.hasActiveProcessing).accessibilityIdentifier("plate-solve.open")
                .sheet(isPresented: $presented) { if let source = model.currentInputURL { PlateSolveView(model: model, source: source) } }
        }
    }
}
struct PlateSolveView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @StateObject private var session: PlateSolveSession
    @Environment(\.dismiss) private var dismiss
    @State private var saving = false
    @State private var marks = true
    @State private var operation: Task<Void, Never>?
    @AppStorage("plateSolve.solver") private var solverPath = ""
    @AppStorage("plateSolve.indices") private var indexPaths = "[]"
    private var zh: Bool { model.language == .simplifiedChinese }
    private var locked: Bool { session.busy || saving || operation != nil }
    init(model: PhotonStackWorkspaceModel, source: URL) { self.model = model; _session = StateObject(wrappedValue: PlateSolveSession(source: source)) }
    var body: some View {
        VStack(spacing: 12) {
            HStack {
                Text(zh ? "星图定位" : "Plate solving").font(.headline)
                Spacer()
                Button(zh ? "选择母片…" : "Choose master…") { chooseMaster() }.disabled(locked)
                Button(zh ? "打开结果…" : "Open result…") { openResult() }.disabled(locked)
                ToolSheetCloseButton(chinese: zh, busy: locked) { dismiss() }
            }
            HStack(alignment: .top, spacing: 16) {
                VStack {
                    ZStack {
                        Color.black
                        if let image = session.image { Image(nsImage: image).resizable().scaledToFit() }
                        else { Text(zh ? "定位后显示母片与复核星点" : "Solve to review the master and validation stars").foregroundStyle(.secondary) }
                        if marks, let report = session.report { PlateSolveMarks(report: report) }
                    }
                    Toggle(zh ? "标出复核恒星：青色为留出星，黄色为拟合星" : "Show stars: cyan held out, yellow fitted", isOn: $marks).font(.caption)
                }.frame(maxWidth: .infinity, maxHeight: .infinity)
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) {
                        Text(session.source.lastPathComponent).font(.caption).textSelection(.enabled)
                        configuration
                        Divider()
                        if let r = session.report { result(r) }
                        if !session.isCurrent, session.report != nil { Text(zh ? "设置已更改，请重新定位后再保存。" : "Settings changed; solve again before saving.").font(.caption).foregroundStyle(.orange) }
                        if let error = session.error { ProcessingErrorNotice(message:error,chinese:zh) }
                    }
                }.frame(width: 330)
            }
            ToolActionBar {
                if locked {
                    ToolTaskStatus(title: saving ? (zh ? "正在保存…" : "Saving…") : (zh ? "正在处理…" : "Processing…"), chinese: zh,
                                   cancel: saving ? nil : { session.cancel(); operation?.cancel() })
                }
                Spacer()
                Button(zh ? "保存带坐标母片…" : "Save solved master…") { save() }
                    .disabled(locked || !session.isCurrent).toolPrimaryAction().accessibilityIdentifier("plate-solve.save")
            }
        }.padding(18).frame(width: 1000, height: 680)
        .interactiveDismissDisabled(locked)
        .onAppear {
            session.settings.solver = solverPath.isEmpty ? (["/opt/homebrew/bin/solve-field", "/usr/local/bin/solve-field"].first { FileManager.default.isExecutableFile(atPath: $0) } ?? "") : solverPath
            session.settings.indices = (try? JSONDecoder().decode([String].self, from: Data(indexPaths.utf8))) ?? []
        }.onDisappear {
            let pending = operation; pending?.cancel()
            Task { await pending?.value; session.close() }
        }
    }
    private var configuration: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(zh ? "本地求解器" : "Local solver").font(.subheadline.bold())
            HStack { Text(session.settings.solver.isEmpty ? (zh ? "尚未选择" : "Not selected") : URL(fileURLWithPath: session.settings.solver).lastPathComponent).font(.caption); Spacer(); Button(zh ? "选择…" : "Choose…") { chooseSolver() }.accessibilityIdentifier("plate-solve.solver") }
            Text(zh ? "需要安装 Astrometry.net。照片只在本机处理。" : "Uses an installed Astrometry.net solve-field; images and stars stay on this Mac.").font(.caption2).foregroundStyle(.secondary)
            HStack { Text(zh ? "星表索引 \(session.settings.indices.count) 个" : "\(session.settings.indices.count) catalog indices").font(.caption); Spacer(); Button(zh ? "选择索引…" : "Select indices…") { chooseIndices() }.accessibilityIdentifier("plate-solve.indices") }
            ForEach(session.settings.indices, id: \.self) { Text(URL(fileURLWithPath: $0).lastPathComponent).font(.caption2).lineLimit(1) }
            Toggle(zh ? "已确认索引使用 ICRS 坐标" : "Indices use ICRS coordinates", isOn: $session.settings.icrsConfirmed).font(.caption).accessibilityIdentifier("plate-solve.icrs")
            Text(zh ? "索引应覆盖目标和视场尺度。缺少相应索引时无法求解；不读取原图 WCS 作为位置提示。" : "Indices must cover your sky and field scale. Existing image WCS is ignored.").font(.caption2).foregroundStyle(.secondary)
            Toggle(zh ? "提供像素尺度范围以加快搜索" : "Use a pixel-scale hint", isOn: $session.settings.useScaleHint).font(.caption)
            if session.settings.useScaleHint {
                HStack { TextField(zh ? "最小值" : "Min", value: $session.settings.scaleLow, format: .number); Text("–"); TextField(zh ? "最大值" : "Max", value: $session.settings.scaleHigh, format: .number); Text("″/px") }.textFieldStyle(.roundedBorder)
            }
            Stepper(value: $session.settings.timeout, in: 10...600, step: 10) { Text(zh ? "时间上限 \(session.settings.timeout) 秒" : "Time limit \(session.settings.timeout) s").font(.caption) }
            Button(zh ? "开始定位" : "Solve field") { session.begin() }.disabled(!session.settings.isValid).accessibilityIdentifier("plate-solve.run")
        }.disabled(locked)
    }
    private func result(_ r: PlateSolveReport) -> some View {
        VStack(alignment: .leading, spacing: 8) {
            Label(zh ? "定位与留出复核通过" : "Solved and independently checked", systemImage: "checkmark.circle.fill").foregroundStyle(.green)
            Text(String(format: "RA %.6f° · Dec %+.6f°\n%.4f ″/px", r.centerRA, r.centerDec, r.scaleArcsecPerPixel)).font(.caption).monospacedDigit()
            Text(zh ? "拟合 \(r.fitted.count) 星 · 留出复核 \(r.validation.count) 星\n留出覆盖 \(r.validation.regions) / 16 区" : "\(r.fitted.count) fitted · \(r.validation.count) held-out matches\nHeld-out coverage: \(r.validation.regions) / 16 regions").font(.caption)
            Text(String(format: zh ? "留出残差：中位 %.3f / P95 %.3f 像素" : "Held-out residual: median %.3f / P95 %.3f px", r.validation.medianPixels, r.validation.p95Pixels)).font(.caption).monospacedDigit()
            Text(zh ? "验证星未参与定位，用于检查定位结果。" : "Held-out stars were excluded from solving. This checks consistency against the selected index, not independent Gaia accuracy or photometric calibration.").font(.caption2).foregroundStyle(.secondary)
            Text(zh ? "另存带坐标的 FITS、残差报告和参数。保留原像素、拍摄信息和扩展数据。" : "Saves a new WCS FITS, residual report and recipe. Pixel bytes, acquisition metadata and extensions are retained.").font(.caption).foregroundStyle(.secondary)
        }
    }
    private func chooseMaster() { let p = NSOpenPanel(); p.allowedContentTypes = [UTType(filenameExtension: "fits") ?? .data, UTType(filenameExtension: "fit") ?? .data]; if p.runModal() == .OK, let u = p.url { session.choose(u) } }
    private func chooseSolver() { let p = NSOpenPanel(); p.message = zh ? "选择 Astrometry.net 的 solve-field 可执行文件" : "Select the Astrometry.net solve-field executable"; if p.runModal() == .OK, let u = p.url { session.settings.solver = u.path; solverPath = u.path } }
    private func chooseIndices() {
        let p = NSOpenPanel(); p.allowsMultipleSelection = true; p.allowedContentTypes = [UTType(filenameExtension: "fits") ?? .data]
        if p.runModal() == .OK { session.settings.indices = p.urls.map(\.path).sorted(); session.settings.icrsConfirmed = false; if let data = try? JSONEncoder().encode(session.settings.indices), let value = String(data: data, encoding: .utf8) { indexPaths = value } }
    }
    private func folder() -> URL? { let p = NSOpenPanel(); p.canChooseFiles = false; p.canChooseDirectories = true; p.canCreateDirectories = true; return p.runModal() == .OK ? p.url : nil }
    private func openResult() { guard let url = folder() else { return }; operation = Task { defer { operation = nil }; do { try await session.open(url); session.error = nil } catch is CancellationError { } catch { if !Task.isCancelled { session.error = error.localizedDescription } } } }
    private func save() {
        guard let parent = folder() else { return }; saving = true
        let projectID = model.project.id
        operation = Task {
            defer { saving = false; operation = nil }
            do {
                let file = try await session.save(to: parent.appendingPathComponent("PlateSolve-"+UUID().uuidString.prefix(8)))
                try Task.checkCancellation()
                guard model.project.id == projectID else { return }
                model.showDerivedImage(file); dismiss()
            } catch is CancellationError { } catch { if !Task.isCancelled { session.error = error.localizedDescription } }
        }
    }
}
private struct PlateSolveMarks: View {
    let report: PlateSolveReport
    var body: some View {
        Canvas { context, size in
            let scale = min(size.width/Double(report.width), size.height/Double(report.height))
            let x0 = (size.width-Double(report.width)*scale)/2, y0 = (size.height-Double(report.height)*scale)/2
            for r in report.residuals {
                let s = report.stars[r.source], p = CGPoint(x: x0+(s.x+0.5)*scale, y: y0+(s.y+0.5)*scale)
                context.stroke(Path(ellipseIn: CGRect(x: p.x-4, y: p.y-4, width: 8, height: 8)), with: .color(s.heldOut ? .cyan : .yellow.opacity(0.7)), lineWidth: 1)
            }
        }.allowsHitTesting(false)
    }
}
