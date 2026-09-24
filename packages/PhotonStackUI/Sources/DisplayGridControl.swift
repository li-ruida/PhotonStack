import PhotonStackAppCore
import SwiftUI

struct DisplayGridControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var presented = false
    @State private var settings = DisplayGridSettings()
    private var chinese: Bool { model.language == .simplifiedChinese }
    var body: some View {
        Button { presented = true } label: {
            Label(chinese ? "细纹抑制" : "Reduce grid", systemImage: "square.grid.3x3")
        }
        .disabled(model.currentInputURL.map(DisplayGridSettings.supports) != true)
        .help(chinese ? "减轻横纵细纹；请在 1:1 下比较星点和尘带。" : "Reduce fine horizontal and vertical patterns. Compare stars and dust lanes at 1:1.")
        .popover(isPresented: $presented) {
            VStack(alignment: .leading, spacing: 12) {
                Text(chinese ? "细纹抑制" : "Reduce fine grid patterns").font(.headline)
                Text(chinese ? "用于已显影的 TIFF 或 PNG。会减轻细纹，也可能减弱最细的真实细节。可在处理历史中调节或撤销。" : "For developed TIFF or PNG images. Reduces grid patterns but may soften the finest real detail. Adjust or undo it in processing history.")
                    .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
                HStack {
                    Text(chinese ? "强度" : "Amount")
                    Spacer()
                    TextField(chinese ? "细纹抑制强度" : "Grid reduction amount", value: $settings.amount,
                        format: .number.precision(.fractionLength(2...3)))
                        .labelsHidden().textFieldStyle(.roundedBorder)
                        .multilineTextAlignment(.trailing).frame(width: 70)
                }
                Slider(value: $settings.amount, in: 0...1)
                HStack {
                    Button(chinese ? "重置" : "Reset") { settings = DisplayGridSettings() }
                    Spacer()
                    Button(chinese ? "应用" : "Apply") {
                        presented = false
                        model.startDisplayGridPreview(settings: settings)
                    }.buttonStyle(.borderedProminent).disabled(!settings.isValid)
                }
            }.padding(18).frame(width: 330)
        }
    }
}
