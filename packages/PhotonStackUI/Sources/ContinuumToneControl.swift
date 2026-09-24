import PhotonStackAppCore
import SwiftUI

struct ContinuumToneControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var presented = false
    @State private var settings = ContinuumToneSettings()
    @State private var curveStrength = 1.0
    private var chinese: Bool { model.language == .simplifiedChinese }
    private var scientificInput: Bool {
        model.currentInputURL.map { AssetKind.detect(from: $0) == .fits } ?? false
    }
    var body: some View {
        Button { presented = true } label: {
            Label(chinese ? "星系层次" : "Diffuse tone", systemImage: "circle.lefthalf.filled")
        }
        .disabled(scientificInput)
        .help(chinese ? "先显影，再调整星系的明暗层次并减少星点膨胀。" : "Develop first, then adjust diffuse tone while limiting stellar expansion.")
        .popover(isPresented: $presented) {
            VStack(alignment: .leading, spacing: 12) {
                Text(chinese ? "星系层次" : "Diffuse tone").font(.headline)
                Text(chinese ? "分开调整平滑背景与细节。应用后可在处理历史中修改或撤销。" : "Adjust diffuse brightness and detail separately. Edit or undo the result in processing history.")
                    .font(.caption).foregroundStyle(.secondary)
                adjustment(chinese ? "提亮强度" : "Tone strength", value: $curveStrength, range: 0...1)
                adjustment(chinese ? "细节对比" : "Detail contrast", value: $settings.amount, range: 0...2)
                Group {
                    Divider()
                    Text(chinese ? "高级参数" : "Advanced").font(.caption).foregroundStyle(.secondary)
                    VStack(alignment: .leading, spacing: 10) {
                        Stepper(chinese ? "细节半径：\(settings.fineRadius) 像素" : "Fine radius: \(settings.fineRadius) px",
                                value: $settings.fineRadius, in: 1...max(1, settings.radius - 1))
                        Stepper(chinese ? "背景半径：\(settings.radius) 像素" : "Continuum radius: \(settings.radius) px",
                                value: $settings.radius, in: min(64, settings.fineRadius + 1)...64)
                        adjustment(chinese ? "星点色彩保留" : "Star color retention", value: $settings.starChroma, range: 0...1)
                    }.font(.caption).padding(.top, 8)
                }
                HStack {
                    Button(chinese ? "重置" : "Reset") { settings = ContinuumToneSettings(); curveStrength = 1 }
                    Spacer()
                    Button(chinese ? "应用" : "Apply") {
                        presented = false
                        model.startContinuumTonePreview(settings: settings)
                    }.buttonStyle(.borderedProminent).disabled(!settings.isValid)
                }
            }.padding(18).frame(width: 340)
                .fixedSize(horizontal: false, vertical: true)
        }
        .onChange(of: curveStrength) { _, value in settings.setCurveStrength(value) }
    }
    private func adjustment(_ title: String, value: Binding<Double>, range: ClosedRange<Double>) -> some View {
        VStack(spacing: 3) {
            HStack {
                Text(title)
                Spacer()
                TextField(title, value: value, format: .number.precision(.fractionLength(2...3)))
                    .labelsHidden().textFieldStyle(.roundedBorder).multilineTextAlignment(.trailing)
                    .frame(width: 70).accessibilityLabel(title)
            }
            Slider(value: value, in: range)
        }.font(.caption)
    }
}
