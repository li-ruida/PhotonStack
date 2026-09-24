import PhotonStackAppCore
import SwiftUI

struct GreenCastControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var presented = false
    @State private var settings = GreenCastSettings()
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var scientificInput: Bool {
        ["fit", "fits", "fts"].contains(model.currentInputURL?.pathExtension.lowercased() ?? "")
    }
    var body: some View {
        Button { presented = true } label: {
            Label(chinese ? "色偏调整" : "Color cast", systemImage: "eyedropper.halffull")
        }
        .disabled(scientificInput)
        .help(chinese ? "用于已显影图像，减轻绿色偏色并可保留亮度。请先显影线性 FITS。" : "Reduce green casts in developed images, optionally preserving lightness. Develop linear FITS first.")
        .popover(isPresented: $presented) {
            VStack(alignment: .leading, spacing: 12) {
                Text(chinese ? "绿色偏色调整" : "Green cast adjustment").font(.headline)
                Text(chinese ? "调整显示色彩，不是测光校准。可在处理历史中修改或撤销。" : "Display color styling, not photometric calibration. Edit or undo it in processing history.")
                    .font(.caption).foregroundStyle(.secondary)
                Picker(chinese ? "作用范围" : "Scope", selection: $settings.method) {
                    Text(chinese ? "全图中性化" : "Global neutralization").tag(GreenCastSettings.Method.averageNeutral)
                    Text(chinese ? "优先背景" : "Favor background").tag(GreenCastSettings.Method.background)
                }
                adjustment(chinese ? "强度" : "Amount", value: $settings.amount)
                adjustment(chinese ? "绿色余量" : "Green allowance", value: $settings.greenThreshold)
                if settings.method == .background {
                    adjustment(chinese ? "背景亮度上限" : "Background limit", value: $settings.backgroundLimit)
                }
                Toggle(chinese ? "保留亮度" : "Preserve lightness", isOn: $settings.preserveLightness)
                HStack {
                    Button(chinese ? "重置" : "Reset") { settings = GreenCastSettings() }
                    Spacer()
                    Button(chinese ? "应用" : "Apply") {
                        presented = false
                        model.startGreenCastPreview(settings: settings)
                    }.buttonStyle(.borderedProminent).disabled(!settings.isValid)
                }
            }.padding(18).frame(width: 330)
        }
    }
    private func adjustment(_ title: String, value: Binding<Double>) -> some View {
        VStack(spacing: 3) {
            HStack {
                Text(title)
                Spacer()
                TextField(title, value: value, format: .number.precision(.fractionLength(2...3)))
                    .labelsHidden().textFieldStyle(.roundedBorder)
                    .multilineTextAlignment(.trailing).frame(width: 70).accessibilityLabel(title)
            }
            Slider(value: value, in: 0...1)
        }.font(.caption)
    }
}
