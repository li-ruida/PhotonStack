import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct GeneralizedStretchControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var presented = false
    private var zh: Bool { model.language == .simplifiedChinese }
    var body: some View {
        Button(zh ? "GHS 拉伸…" : "GHS stretch & preview…") { presented = true }
            .disabled(model.currentInputURL == nil || model.hasActiveProcessing)
            .accessibilityIdentifier("ghs.open")
            .sheet(isPresented: $presented) {
                if let source = model.currentInputURL { GeneralizedStretchView(model: model, source: source) }
            }
    }
}
struct GeneralizedStretchView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @StateObject private var session: GeneralizedStretchSession
    @Environment(\.dismiss) private var dismiss
    @State private var saving = false
    @State private var operation: Task<Void, Never>?
    private var locked: Bool { session.busy || saving || operation != nil }
    private var zh: Bool { model.language == .simplifiedChinese }
    init(model: PhotonStackWorkspaceModel, source: URL) {
        self.model = model; _session = StateObject(wrappedValue: GeneralizedStretchSession(source: source))
    }
    var body: some View {
        VStack(spacing: 12) {
            HStack {
                Text(zh ? "GHS 广义双曲拉伸" : "Generalized hyperbolic stretch").font(.headline)
                Spacer()
                Button(zh ? "选择图像…" : "Choose image…") { chooseImage() }.disabled(locked)
                Button(zh ? "打开结果…" : "Open result…") { openResult() }.disabled(locked)
                ToolSheetCloseButton(chinese: zh, busy: locked) { dismiss() }
            }
            HStack(alignment: .top, spacing: 16) {
                VStack(spacing: 10) {
                    Picker(zh ? "对比" : "Compare", selection: $session.before) {
                        Text(zh ? "拉伸前" : "Before").tag(true)
                        Text(zh ? "拉伸后" : "After").tag(false)
                    }.pickerStyle(.segmented).accessibilityIdentifier("ghs.compare")
                    ZStack {
                        Color.black
                        if let image = session.image { Image(nsImage: image).resizable().scaledToFit() }
                        else { Text(zh ? "等待预览" : "Waiting for preview").foregroundStyle(.white.opacity(0.6)) }
                    }.frame(maxWidth: .infinity, maxHeight: .infinity)
                    if let r = session.report {
                        GeneralizedStretchPlot(report: r, zh: zh).frame(height: 130)
                        Text(zh ? "灰色：拉伸前；蓝色：拉伸后；白线：当前曲线。" : "Full-image luminance histogram (log counts): gray before, blue after; white line is the current curve.")
                            .font(.caption2).foregroundStyle(.secondary)
                    }
                }
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) {
                        Text(session.source.lastPathComponent).font(.caption).textSelection(.enabled)
                        if let r = session.report {
                            Text(r.sourceDomain == "linear" ? (zh ? "线性母片 → 显示成片" : "Linear master → display image") : (zh ? "已有显示图 → 再拉伸" : "Display image → further stretch"))
                                .font(.caption).foregroundStyle(.secondary)
                        }
                        control(zh ? "拉伸强度 ln(1+D)" : "Stretch ln(1+D)", \.logStretch, 0...10, "strength")
                        control(zh ? "局部强度 b" : "Local intensity b", \.localIntensity, -5...15, "local")
                        control(zh ? "中心点 SP" : "Symmetry SP", \.symmetry, 0...1, "symmetry")
                        control(zh ? "暗部保护 LP" : "Shadow protection LP", \.shadowProtection, 0...1, "shadow")
                        control(zh ? "亮部保护 HP" : "Highlight protection HP", \.highlightProtection, 0...1, "highlight")
                        Text(zh ? "保护区间使用平滑衔接的直线；需满足 LP ≤ SP ≤ HP。保护不等于保留原亮度。" : "Protection uses tangent linear segments. Require LP ≤ SP ≤ HP; protected regions still change brightness.")
                            .font(.caption2).foregroundStyle(.secondary)
                        Picker(zh ? "颜色" : "Color", selection: $session.settings.mode) {
                            Text(zh ? "按亮度缩放" : "Luminance scaling").tag("luminance")
                            Text(zh ? "RGB 独立" : "Independent RGB").tag("independent")
                            Text(zh ? "HSV 饱和度" : "HSV saturation").tag("saturation")
                        }
                        Toggle(zh ? "反向曲线" : "Inverse curve", isOn: $session.settings.inverse).accessibilityIdentifier("ghs.inverse")
                        if session.settings.inverse {
                            Text(zh ? "反向作用于本步曲线；无法恢复此前截断、量化或颜色越界混合丢失的信息，也不会把成片标记回科学线性数据。" : "Inverts this step's curve. Cannot recover clipping, quantization or gamut blending, and does not restore a scientific linear-data label.")
                                .font(.caption2).foregroundStyle(.secondary)
                        }
                        Text(zh ? "第 \(session.prefixSteps + 1) 步（最多 32 步）" : "Step \(session.prefixSteps + 1) of up to 32").font(.caption)
                        HStack {
                            Button(zh ? "追加下一步" : "Append next step") { session.appendStep() }
                                .disabled(!session.isCurrent || session.prefixSteps >= 31).accessibilityIdentifier("ghs.next-step")
                            Button(zh ? "从原图重来" : "Restart from source") { session.choose(session.source); session.begin() }
                                .disabled(session.prefixSteps == 0)
                        }
                        Toggle(zh ? "自动输入范围" : "Automatic input range", isOn: $session.settings.automaticRange)
                        HStack {
                            Text(zh ? "黑场" : "Black")
                            TextField(zh ? "黑场" : "Black", value: $session.settings.black, format: .number.precision(.fractionLength(0...6)))
                                .accessibilityIdentifier("ghs.black")
                            Text(zh ? "白场" : "White")
                            TextField(zh ? "白场" : "White", value: $session.settings.white, format: .number.precision(.fractionLength(0...6)))
                                .accessibilityIdentifier("ghs.white")
                        }.textFieldStyle(.roundedBorder).disabled(session.settings.automaticRange)
                        Text(zh ? "自动设置黑白点，也可手动调整。" : "Auto: covered min/max for linear data; 0–1 for display images. Preview resolves and fixes the range.")
                            .font(.caption2).foregroundStyle(.secondary)
                        HStack {
                            Button(zh ? "重置曲线" : "Reset curve") {
                                let old = session.settings
                                session.settings = .init(black: old.black, white: old.white, automaticRange: old.automaticRange)
                            }
                            Button(zh ? "更新预览" : "Update preview") { session.begin() }
                                .buttonStyle(.bordered).disabled(!session.settings.isValid)
                                .accessibilityIdentifier("ghs.preview")
                        }
                        if !session.settings.isValid {
                            Text(zh ? "请检查参数：暗部保护 ≤ 对称点 ≤ 高光保护，且黑点须小于白点。" : "Check settings: shadow protection ≤ symmetry ≤ highlight protection; black must be below white.")
                                .foregroundStyle(.orange).font(.caption)
                        } else if !session.isCurrent {
                            Text(zh ? "参数已更改，预览待更新。" : "Settings changed; update the preview.").foregroundStyle(.orange).font(.caption)
                        }
                        if let r = session.report {
                            Text(zh ? "截断像素：黑场 \(r.belowBlack)，白场 \(r.aboveWhite)\n颜色越界混合：\(r.gamutBlendedPixels) / \(r.coveredPixels)"
                                 : "Clipped: black \(r.belowBlack), white \(r.aboveWhite)\nGamut blended: \(r.gamutBlendedPixels) / \(r.coveredPixels)")
                                .font(.caption).monospacedDigit()
                        }
                        Text(zh ? "保存 TIFF、拉伸后的 FITS 和处理参数。测光等操作请使用原始线性图。" : "Saves full-size 16-bit TIFF, nonlinear float FITS and a replayable recipe. Keep the linear master for scientific processing.")
                            .font(.caption).foregroundStyle(.secondary)
                    }.disabled(locked)
                }.frame(width: 310)
            }
            if let error = session.error { ProcessingErrorNotice(message:error,chinese:zh) }
            ToolActionBar {
                if locked {
                    ToolTaskStatus(title: saving ? (zh ? "正在保存…" : "Saving…") : (zh ? "正在处理…" : "Processing…"), chinese: zh,
                                   cancel: saving ? nil : { operation?.cancel(); session.cancel() })
                }
                Spacer()
                Button(zh ? "保存并使用…" : "Save & use result…") { save() }
                    .disabled(!session.isCurrent || locked).toolPrimaryAction().accessibilityIdentifier("ghs.save")
            }
        }.padding(18).frame(minWidth: 940, idealWidth: 1100, minHeight: 700, idealHeight: 780)
            .interactiveDismissDisabled(locked).task { session.begin() }.onDisappear { operation?.cancel(); session.close(after: operation) }
    }
    private func control(_ title: String, _ key: WritableKeyPath<GeneralizedStretchSettings, Double>, _ range: ClosedRange<Double>, _ id: String) -> some View {
        VStack(alignment: .leading, spacing: 3) {
            HStack {
                Text(title).font(.caption)
                Spacer()
                TextField(title, value: Binding(get: { session.settings[keyPath: key] }, set: { session.settings[keyPath: key] = $0 }), format: .number.precision(.fractionLength(0...5)))
                    .textFieldStyle(.roundedBorder).frame(width: 75).accessibilityIdentifier("ghs."+id)
            }
            Slider(value: Binding(get: { min(range.upperBound, max(range.lowerBound, session.settings[keyPath: key])) }, set: { session.settings[keyPath: key] = $0 }), in: range)
                .accessibilityLabel(title)
        }
    }
    private func chooseImage() {
        let panel = NSOpenPanel(); panel.canChooseFiles = true; panel.canChooseDirectories = false; panel.allowsMultipleSelection = false
        panel.allowedContentTypes = [.tiff, .png, .jpeg, UTType(filenameExtension: "fits") ?? .data, UTType(filenameExtension: "fit") ?? .data]
        guard panel.runModal() == .OK, let file = panel.url else { return }
        session.choose(file); session.begin()
    }
    private func folder() -> URL? {
        let panel = NSOpenPanel(); panel.canChooseFiles = false; panel.canChooseDirectories = true; panel.allowsMultipleSelection = false; panel.canCreateDirectories = true
        return panel.runModal() == .OK ? panel.url : nil
    }
    private func openResult() {
        guard let folder = folder() else { return }
        operation = Task {
            defer { operation = nil }
            do { try await session.open(folder); session.error = nil }
            catch is CancellationError { }
            catch { if !Task.isCancelled { session.error = error.localizedDescription } }
        }
    }
    private func save() {
        guard let folder = folder() else { return }; saving = true
        let projectID = model.project.id
        operation = Task {
            defer { saving = false; operation = nil }
            do {
                let image = try await session.saveResult(to: folder.appendingPathComponent("GHS-"+UUID().uuidString.prefix(8)))
                try Task.checkCancellation()
                guard model.project.id == projectID else { return }
                model.showDerivedImage(image); dismiss()
            } catch is CancellationError { } catch { if !Task.isCancelled { session.error = error.localizedDescription } }
        }
    }
}
private struct GeneralizedStretchPlot: View {
    let report: GeneralizedStretchReport
    let zh: Bool
    var body: some View {
        Canvas { context, size in
            let peak = log1p(Double(max(report.beforeHistogram.max() ?? 1, report.afterHistogram.max() ?? 1)))
            for (values, color) in [(report.beforeHistogram, Color.gray.opacity(0.5)), (report.afterHistogram, Color.blue.opacity(0.7))] {
                var path = Path(); path.move(to: CGPoint(x: 0, y: size.height))
                for (i, n) in values.enumerated() { path.addLine(to: CGPoint(x: Double(i)*size.width/255, y: size.height*(1-log1p(Double(n))/peak))) }
                path.addLine(to: CGPoint(x: size.width, y: size.height)); path.closeSubpath(); context.fill(path, with: .color(color))
            }
            var curve = Path()
            for (i, y) in report.curve.enumerated() {
                let point = CGPoint(x: Double(i)*size.width/1024, y: (1-y)*size.height)
                if i == 0 { curve.move(to: point) } else { curve.addLine(to: point) }
            }
            context.stroke(curve, with: .color(.primary), lineWidth: 1.5)
        }.accessibilityLabel(zh ? "拉伸曲线与前后亮度直方图" : "Stretch curve and luminance histograms")
    }
}
