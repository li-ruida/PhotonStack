import SwiftUI

struct SpectralNarrowbandProfileView: View {
    let chinese: Bool
    let library: SpectralProfileLibrary
    let base: SpectralProfileLibrary.Entry
    let inspected: SpectralProfileLibrary.Selection
    let created: (String) -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var name = ""
    @State private var layout = "osc-dual"
    @State private var centerText = ["656.3", "500.7", "500.7"]
    @State private var widthText = ["7", "7", "7"]
    @State private var error: String?
    @State private var operation: Task<Void, Never>?
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    private var model: SpectralNarrowbandModel {
        .init(layout: layout, baseSHA: inspected.inspection.profile.sha, centers: centerText.compactMap { Double($0.trimmingCharacters(in: .whitespaces)) }, widths: widthText.compactMap { Double($0.trimmingCharacters(in: .whitespaces)) })
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(t("新建窄带响应", "Create narrowband response")).font(.headline)
            Text(t("基础响应：", "Base response: ") + base.label).font(.subheadline)
            Text(t("基础响应应包含传感器和其余光路，不应重复包含下面要建模的窄带滤镜。", "The base response should include the detector and other optics, without the narrowband filter modeled below.")).font(.caption).foregroundStyle(.secondary)
            TextField(t("配置名称", "Profile name"), text: $name).textFieldStyle(.roundedBorder).accessibilityIdentifier("spcc.narrowband.name").disabled(operation != nil)
            Picker(t("输入类型", "Input layout"), selection: $layout) {
                Text(t("单色滤镜合色", "Mono filter composite")).tag("mono")
                Text(t("OSC 双窄带 RGB", "OSC dual-band RGB")).tag("osc-dual")
                Text(t("提取后的 HOO", "Extracted HOO")).tag("extracted-hoo")
            }.accessibilityIdentifier("spcc.narrowband.layout").disabled(operation != nil)
            Text(layoutDescription).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Grid(alignment: .leading, horizontalSpacing: 16, verticalSpacing: 10) {
                GridRow {
                    Text(t("输出通道", "Output channel")); Text(t("名义中心（nm）", "Nominal center (nm)")); Text(t("名义宽度（nm）", "Nominal width (nm)"))
                }.font(.caption.bold())
                ForEach(0..<3) { c in
                    GridRow {
                        Text(["R", "G", "B"][c])
                        TextField("nm", text: $centerText[c])
                            .accessibilityIdentifier("spcc.narrowband.center.\(c)")
                        TextField("nm", text: $widthText[c])
                            .accessibilityIdentifier("spcc.narrowband.width.\(c)")
                    }.disabled(layout != "mono" && c == 2)
                }
            }.textFieldStyle(.roundedBorder).monospacedDigit().disabled(operation != nil)
            Text(t("宽度 3–100 nm；完整通带须在 336–1020 nm 及基础响应范围内。双窄带的 G / B 参数联动，两条通带必须分离。", "Widths: 3–100 nm; full passbands must lie within 336–1020 nm and the base response. Dual-band G / B settings are linked, and the two passbands must be separate.")).font(.caption).foregroundStyle(.secondary)
            Text(t("此配置使用单位透过率梯形近似，边缘占名义宽度的 0.01%，不代表实测滤镜。Gaia 光谱采样间隔不等于光谱分辨率；未分辨谱线及响应模型误差不包含在通量误差条中。", "This uses unit-transmission trapezoidal approximations with edges 0.01% of nominal width, not measured filters. Gaia sampling interval is not resolving power; unresolved lines and response-model errors are not included in flux error bars.")).font(.caption).foregroundStyle(.orange)
            Text(t("用于未拉伸线性图的恒星平衡，不恢复宽带色彩或绝对发射线通量。HOO / SHO 的艺术混色请在通道合成与后期调整中完成。", "Use for stellar balance in unstretched linear images, not recovery of broadband colors or absolute emission-line flux. Use channel composition and finishing for artistic HOO / SHO palettes.")).font(.caption).foregroundStyle(.secondary)
            if let error { Text(error).font(.caption).foregroundStyle(.red).textSelection(.enabled).lineLimit(4) }
            HStack {
                if operation != nil { ProgressView().controlSize(.small) }
                Spacer()
                Button(t("取消", "Cancel")) { operation?.cancel(); dismiss() }.keyboardShortcut(.cancelAction)
                Button(t("创建并检查", "Create & inspect")) { create() }.buttonStyle(.borderedProminent)
                    .disabled(operation != nil || !model.isValid || name.trimmingCharacters(in: .whitespaces).isEmpty)
                    .accessibilityIdentifier("spcc.narrowband.create")
            }
        }.padding(20).frame(width: 640)
            .onAppear { name = base.label + t(" · 双窄带", " · Narrowband") }
            .onChange(of: layout) { _, _ in synchronize() }
            .onChange(of: centerText[1]) { _, _ in synchronize() }
            .onChange(of: widthText[1]) { _, _ in synchronize() }
            .onDisappear { operation?.cancel() }
    }
    private var layoutDescription: String {
        switch layout {
        case "mono": t("每个输出通道采用各自的单色传感器响应和一条通带；重复 OⅢ 的 HOO 请给 G / B 相同参数。", "Each output channel uses its own mono detector response and one passband. For duplicated OIII in HOO, use identical G / B settings.")
        case "osc-dual": t("用于未经通道混合的线性 OSC RGB。两条通带同时乘入每个 RGB 响应，保留跨通道漏光。", "For unmixed linear OSC RGB. Both passbands multiply every RGB response, retaining cross-channel leakage.")
        default: t("仅用于 R=Hα、G=B=OⅢ 的直接合色，OⅢ 按 (G1+G2+B)/3 提取。基础响应第一组件须为传感器 QE，G / B 后续光路组件须相同。", "Only for direct R=Ha, G=B=OIII composition extracted as (G1+G2+B)/3. First base components must be detector QE; subsequent G / B optical components must be identical.")
        }
    }
    private func synchronize() {
        if layout != "mono" { centerText[2] = centerText[1]; widthText[2] = widthText[1] }
    }
    private func create() {
        guard operation == nil, model.isValid else { return }
        let requested = model
        error = nil
        operation = Task {
            defer { operation = nil }
            do {
                let id = try await library.createNarrowband(from: base, name: name, layout: layout, centers: requested.centers, widths: requested.widths)
                try Task.checkCancellation(); created(id); dismiss()
            } catch is CancellationError {} catch { self.error = error.localizedDescription }
        }
    }
}
