import PhotonStackAppCore
import SwiftUI

struct AstroDevelopControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var presented = false
    @State private var settings = AstroDevelopSettings()
    private var chinese: Bool { model.language == .simplifiedChinese }

    var body: some View {
        Button {
            presented = true
        } label: {
            Label(chinese ? "深空显影" : "Develop", systemImage: "sparkles.rectangle.stack")
        }
        .help(chinese ? "对线性 RGB 堆栈进行恒星白平衡与高亮压缩" : "Stellar balance and highlight compression for a linear RGB stack")
        .popover(isPresented: $presented) {
            VStack(alignment: .leading, spacing: 12) {
                Text(chinese ? "深空显影" : "Deep Sky Development").font(.headline)
                Text(chinese ? "从线性 RGB 图像开始。先去除背景梯度，再显影。" : "Start with linear RGB data after background gradient removal.")
                    .font(.caption).foregroundStyle(.secondary)
                Picker(chinese ? "拉伸方式" : "Tone curve", selection: $settings.toneCurve) {
                    Text(chinese ? "保留高光层次" : "Preserve highlight detail").tag("asinh")
                    Text(chinese ? "增强中间调" : "Emphasize midtones").tag("rational")
                }
                Toggle(chinese ? "恒星统计白平衡" : "Statistical stellar balance", isOn: $settings.stellarBalance)
                adjustment(chinese ? "外盘亮度" : "Brightness", value: $settings.brightness, range: 0.3...4)
                adjustment(chinese ? "背景亮度" : "Background", value: $settings.background, range: 0.01...0.12)
                adjustment(chinese ? "恒星曝光" : "Star exposure", value: $settings.starExposure, range: 0.1...1)
                adjustment(chinese ? "亮星门槛" : "Bright-star threshold", value: $settings.starPeakThreshold, range: 0...1)
                    .help(chinese ? "0 包括全部星点；越高越只调整亮星。1 不调整星点。" : "Zero includes all stars; higher values select brighter stars. One leaves stars unchanged.")
                adjustment(chinese ? "色彩浓度" : "Saturation", value: $settings.saturation, range: 0...1.5)
                adjustment(chinese ? "背景彩噪抑制" : "Background color noise", value: $settings.shadowNeutralization, range: 0...1)
                    .help(chinese ? "降低低信噪比暗部的色彩，不改变亮度；0 关闭。" : "Reduce low-SNR shadow color without changing luminance; zero disables.")
                adjustment(chinese ? "红色微调" : "Red gain", value: $settings.redGain, range: 0.6...1.4)
                adjustment(chinese ? "蓝色微调" : "Blue gain", value: $settings.blueGain, range: 0.6...1.4)
                DisclosureGroup(chinese ? "高级拉伸" : "Advanced tone settings") {
                    Text(chinese ? "0 表示自动；数值使用输入图像的线性单位。" : "Zero selects automatic values, in the input image's linear units.")
                        .font(.caption).foregroundStyle(.secondary)
                    TextField(chinese ? "拉伸尺度" : "Tone scale", value: $settings.toneScale, format: .number)
                    TextField(chinese ? "高光参考值" : "Highlight reference", value: $settings.whitePoint, format: .number)
                }.font(.caption)
                HStack {
                    Button(chinese ? "重置" : "Reset") { settings = AstroDevelopSettings() }
                    Spacer()
                    Button(chinese ? "应用" : "Apply") {
                        presented = false
                        model.startAstroDevelop(settings: settings)
                    }.buttonStyle(.borderedProminent)
                }
            }.padding(18).frame(width: 330)
        }
    }

    private func adjustment(_ title: String, value: Binding<Double>, range: ClosedRange<Double>) -> some View {
        VStack(spacing: 3) {
            HStack {
                Text(title)
                Spacer()
                TextField(title, value: value, format: .number.precision(.fractionLength(2...3)))
                    .labelsHidden().textFieldStyle(.roundedBorder)
                    .multilineTextAlignment(.trailing).frame(width: 70)
                    .accessibilityLabel(title)
            }
            Slider(value: value, in: range)
        }.font(.caption)
    }
}
