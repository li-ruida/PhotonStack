import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct BackgroundReviewView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @StateObject var session: BackgroundReviewSession
    @Environment(\.dismiss) private var dismiss
    @State private var saving = false
    private var zh: Bool { model.language == .simplifiedChinese }
    init(model: PhotonStackWorkspaceModel, input: URL) {
        self.model = model
        _session = StateObject(wrappedValue: BackgroundReviewSession(source: input))
    }
    var body: some View {
        VStack(spacing: 12) {
            HStack {
                Text(zh ? "背景采样与模型" : "Background samples & model").font(.headline)
                Spacer()
                ToolSheetCloseButton(chinese: zh, busy: session.busy || saving) { dismiss() }
            }
            HStack(alignment: .top, spacing: 16) {
                VStack {
                    Picker(zh ? "预览" : "Preview", selection: $session.display) {
                        Text(zh ? "原图" : "Source").tag("source")
                        Text(zh ? "校正" : "Corrected").tag("corrected").disabled(session.renderedRecipe == nil)
                        Text(zh ? "背景模型" : "Model").tag("model").disabled(session.renderedRecipe == nil)
                        Text(zh ? "扣除量" : "Removed").tag("difference").disabled(session.renderedRecipe == nil)
                    }.pickerStyle(.segmented).accessibilityIdentifier("background.preview")
                    canvas
                    Text(zh ? "原图和校正使用相同预览拉伸；模型单独缩放。扣除量中灰为零，亮为减去、暗为加回。"
                         : "Source/corrected share a display stretch. Model scales separately. Removed: gray = zero, bright = subtract, dark = add.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) {
                        Text(session.source.lastPathComponent).font(.caption).lineLimit(2)
                        Picker(zh ? "采样操作" : "Sample action", selection: $session.editMode) {
                            Text(zh ? "添加 / 选择" : "Add / select").tag("add")
                            Text(zh ? "删除" : "Remove").tag("remove")
                        }.pickerStyle(.segmented)
                        Toggle(zh ? "显示采样点" : "Show samples", isOn: $session.showSamples)
                        HStack {
                            Button(zh ? "生成网格" : "Reset grid") { session.resetGrid() }
                            Button(zh ? "清空" : "Clear") { session.recipe?.samples = []; session.selected = nil }
                        }
                        Text(zh ? "点击空白天空添加采样点，避开星云和亮星。绿点有效，橙红点需检查，灰点未使用。"
                             : "Click to add points away from nebulosity, halos and bright stars. Green: used; orange: partial; red: outlier; gray: disabled or uncovered.")
                            .font(.caption).foregroundStyle(.secondary)
                        selectedSample
                        Divider()
                        if session.recipe != nil {
                            Picker(zh ? "模型" : "Model", selection: field(\.model, fallback: "polynomial")) {
                                Text(zh ? "多项式" : "Polynomial").tag("polynomial")
                                Text("RBF").tag("rbf")
                            }.accessibilityIdentifier("background.model")
                            if session.recipe?.model == "rbf" {
                                HStack {
                                    Text(zh ? "平滑 λ" : "Smoothing λ").font(.caption)
                                    Slider(value: Binding(
                                        get: { log10(max(1e-6, session.recipe?.rbfSmoothing ?? 0.001)) },
                                        set: { session.recipe?.rbfSmoothing = pow(10, $0) }), in: -6...0)
                                        .accessibilityLabel(zh ? "RBF 平滑（对数）" : "RBF smoothing (logarithmic)")
                                    TextField("λ", value: field(\.rbfSmoothing, fallback: 0.001),
                                              format: .number.precision(.fractionLength(0...6)))
                                        .frame(width: 76).textFieldStyle(.roundedBorder)
                                        .accessibilityIdentifier("background.rbf-smoothing")
                                }
                                Toggle(zh ? "剔除留出检查异常点" : "Reject leave-one-out outliers",
                                       isOn: field(\.rbfRejectOutliers, fallback: true))
                                Text(zh ? "平滑值越大，背景变化越缓和。采样点请避开星云，最多 512 个。"
                                     : "More smoothing approaches a plane; 0 interpolates retained points. Low smoothing may overfit. Avoid targets. Maximum 512 valid samples.")
                                    .font(.caption).foregroundStyle(.secondary)
                            } else {
                                Picker(zh ? "多项式阶数" : "Polynomial degree", selection: field(\.degree, fallback: 2)) {
                                    ForEach(1...4, id: \.self) { Text("\($0)").tag($0) }
                                }
                            }
                            ParameterSlider(title: zh ? "校正强度" : "Strength",
                                            value: field(\.strength, fallback: 0.35), range: 0...1)
                            Toggle(zh ? "保留背景中值" : "Preserve background median",
                                   isOn: field(\.preserveBrightness, fallback: true))
                            Text(zh ? "\(session.recipe?.samples.filter(\.enabled).count ?? 0) 个启用采样点；至少 12 个。多项式还需不少于系数数量的两倍。"
                                 : "\(session.recipe?.samples.filter(\.enabled).count ?? 0) enabled samples; at least 12 and twice the number of coefficients.")
                                .font(.caption)
                        }
                        Button(zh ? "更新预览" : "Update preview") { session.begin() }
                            .buttonStyle(.bordered)
                            .disabled(session.recipe == nil)
                            .accessibilityIdentifier("background.evaluate")
                        if let report = session.report, session.renderedRecipe != nil {
                            Text(zh ? "拟合残差 RMS（线性 R/G/B）" : "Fit residual RMS (linear R/G/B)").font(.caption)
                            Text(report.residualRMS.map { String(format: "%.4g", $0) }.joined(separator: " / "))
                                .font(.system(.caption, design: .monospaced))
                            if session.renderedRecipe?.model == "rbf", let values = report.validationRMS {
                                Text(zh ? "留出预测 RMS（R/G/B）" : "Leave-one-out RMS (R/G/B)").font(.caption)
                                Text(values.map { String(format: "%.4g", $0) }.joined(separator: " / "))
                                    .font(.system(.caption, design: .monospaced))
                            }
                            if !session.isCurrent {
                                Text(zh ? "参数或采样已更改，预览待更新。" : "Settings or samples changed; update the preview.")
                                    .font(.caption).foregroundStyle(.orange)
                            }
                        }
                        HStack {
                            Button(zh ? "保存参数…" : "Save settings…") { saveRecipe() }.disabled(session.recipe == nil)
                            Button(zh ? "打开参数…" : "Open settings…") { openRecipe() }
                        }
                        Text(zh ? "另存为 FITS，保留原坐标和有效范围。"
                             : "Correction keeps pixel positions. Float FITS retains negative values, coverage and recognized TAN/SIP coordinates.")
                            .font(.caption).foregroundStyle(.secondary)
                    }.disabled(session.busy || saving)
                }.frame(width: 290)
            }
            if let error = session.error { ProcessingErrorNotice(message:error,chinese:zh) }
            ToolActionBar {
                if session.busy || saving {
                    ToolTaskStatus(title: saving ? (zh ? "正在保存…" : "Saving…") : (zh ? "正在更新预览…" : "Updating preview…"), chinese: zh,
                                   cancel: saving ? nil : { session.cancel() })
                }
                Spacer()
                Button(zh ? "保存并使用…" : "Save & use result…") { saveResult() }
                    .disabled(!session.isCurrent || session.busy || saving)
                    .toolPrimaryAction().accessibilityIdentifier("background.save-result")
            }
        }
        .padding(18).frame(minWidth: 920, idealWidth: 1100, minHeight: 640, idealHeight: 760)
        .interactiveDismissDisabled(session.busy || saving)
        .task { session.begin(inspect: true) }
        .onDisappear { session.close() }
    }
    private func field<Value>(_ key: WritableKeyPath<BackgroundReviewRecipe, Value>, fallback: Value) -> Binding<Value> {
        Binding(get: { session.recipe?[keyPath: key] ?? fallback },
                set: { session.recipe?[keyPath: key] = $0 })
    }
    private var canvas: some View {
        GeometryReader { geometry in
            if let image = session.image, let recipe = session.recipe {
                let ratio = Double(recipe.width)/Double(recipe.height)
                let width = min(geometry.size.width, geometry.size.height*ratio)
                let height = width/ratio
                ZStack {
                    Image(nsImage: image).resizable()
                    if session.showSamples {
                        Canvas { context, size in
                            for i in recipe.samples.indices {
                                let p = recipe.samples[i]
                                let center = CGPoint(x: (p.x+0.5)/Double(recipe.width)*size.width,
                                                     y: (p.y+0.5)/Double(recipe.height)*size.height)
                                let r = max(5,Double(p.radius)*size.width/Double(recipe.width))
                                let box = CGRect(x: center.x-r, y: center.y-r, width: 2*r, height: 2*r)
                                context.stroke(Path(ellipseIn: box), with: .color(color(i)), lineWidth: i == session.selected ? 3 : 1.5)
                            }
                        }.allowsHitTesting(false)
                    }
                }
                .frame(width: width, height: height)
                .contentShape(Rectangle())
                .onTapGesture { point in
                    session.tap(x: point.x/width*Double(recipe.width)-0.5,
                                y: point.y/height*Double(recipe.height)-0.5, tolerance: 12/width*Double(recipe.width))
                }
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            } else { ContentUnavailableView(zh ? "等待预览" : "Waiting for preview", systemImage: "photo") }
        }.background(.black).accessibilityIdentifier("background.canvas")
    }
    private func color(_ index: Int) -> Color {
        if session.selected == index { return .cyan }
        guard let points = session.recipe?.samples, points.indices.contains(index), points[index].enabled else { return .gray }
        guard session.isCurrent, let samples = session.report?.samples, samples.indices.contains(index) else { return .yellow }
        switch samples[index].status {
        case "used": return .green
        case "partial": return .orange
        case "outlier": return .red
        default: return .gray
        }
    }
    @ViewBuilder private var selectedSample: some View {
        if let i = session.selected, let recipe = session.recipe, recipe.samples.indices.contains(i) {
            let point = recipe.samples[i]
            VStack(alignment: .leading) {
                Text(zh ? "采样点 \(i+1)" : "Sample \(i+1)").font(.subheadline)
                Toggle(zh ? "参与拟合" : "Enabled", isOn: backgroundPointBinding(session, point: point, key: \.enabled))
                HStack {
                    Text("X")
                    TextField("X", value: backgroundPointBinding(session, point: point, key: \.x), format: .number)
                    Text("Y")
                    TextField("Y", value: backgroundPointBinding(session, point: point, key: \.y), format: .number)
                }.textFieldStyle(.roundedBorder)
                Stepper(zh ? "半径 \(recipe.samples[i].radius) 像素" : "Radius \(recipe.samples[i].radius) px",
                        value: backgroundPointBinding(session, point: point, key: \.radius), in: 1...64)
                if session.isCurrent, let points = session.report?.samples, points.indices.contains(i) {
                    let point = points[i]
                    Text((zh ? "测量 " : "Measured ") + point.measured.map { String(format: "%.4g", $0) }.joined(separator: " / "))
                        .font(.caption2).monospacedDigit()
                    Text((zh ? "拟合 " : "Model ") + point.fitted.map { String(format: "%.4g", $0) }.joined(separator: " / "))
                        .font(.caption2).monospacedDigit()
                    if session.renderedRecipe?.model == "rbf", let errors = point.validationResidual {
                        Text((zh ? "留出 / 剔除后残差 " : "Held-out residual ") + errors.map { String(format: "%.4g", $0) }.joined(separator: " / "))
                            .font(.caption2).monospacedDigit()
                    }
                    Text((zh ? "采用通道 " : "Used channels ") + zip(["R", "G", "B"], point.used).filter { $0.1 }.map { $0.0 }.joined(separator: ", "))
                        .font(.caption2)
                }
                Button(zh ? "删除此点" : "Delete sample") {
                    session.removePoint(id: point.id)
                }
            }
        }
    }
    private func saveRecipe() {
        let panel = NSSavePanel(); panel.allowedContentTypes = [.json]; panel.nameFieldStringValue = "background-recipe.json"
        guard panel.runModal() == .OK, let file = panel.url else { return }
        do { try session.saveRecipe(to: file); session.error = nil }
        catch { session.error = error.localizedDescription }
    }
    private func openRecipe() {
        let panel = NSOpenPanel(); panel.allowedContentTypes = [.json]; panel.allowsMultipleSelection = false
        guard panel.runModal() == .OK, let file = panel.url else { return }
        do { try session.loadRecipe(file); session.begin(inspect: true); session.error = nil }
        catch { session.error = error.localizedDescription }
    }
    private func saveResult() {
        let panel = NSOpenPanel(); panel.canChooseDirectories = true; panel.canChooseFiles = false; panel.canCreateDirectories = true
        panel.message = zh ? "选择校正结果的保存目录" : "Choose where to save the correction"
        guard panel.runModal() == .OK, let folder = panel.url else { return }
        saving = true
        Task {
            defer { saving = false }
            do {
                let output = try await session.saveResult(to: folder.appendingPathComponent("Background-\(UUID().uuidString.prefix(8))"))
                model.showDerivedImage(output)
                dismiss()
            } catch { session.error = error.localizedDescription }
        }
    }
}

// SwiftUI may read an old field binding while a selection is being removed.
// Stable identities keep those reads safe and prevent writes to a different point.
@MainActor
func backgroundPointBinding<Value>(_ session: BackgroundReviewSession, point: BackgroundReviewPoint,
                                   key: WritableKeyPath<BackgroundReviewPoint, Value>) -> Binding<Value> {
    Binding(get: { session.point(id: point.id)?[keyPath: key] ?? point[keyPath: key] },
            set: { session.updatePoint(id: point.id, key: key, value: $0) })
}
