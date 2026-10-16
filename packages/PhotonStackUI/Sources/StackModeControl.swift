import PhotonStackAppCore
import SwiftUI

struct StackModeControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    private var chinese: Bool { model.language == .simplifiedChinese }

    var body: some View {
        if let schema = model.deepSkySchema,
           let field = schema.fields.first(where: { $0.key == "stack.method" }) {
            let selection = model.deepSkySettings.effectiveValue(field.key, schema: schema)
            VStack(alignment: .leading, spacing: 5) {
                HStack {
                    Picker(chinese ? "堆栈模式" : "Stack mode", selection: Binding(
                        get: { selection }, set: { model.setDeepSkyValue($0, for: field.key) }
                    )) {
                        ForEach(field.choices, id: \.self) { choice in
                            Text(StackMethod(rawValue: choice).map { model.language.stackMethodName($0) } ?? choice).tag(choice)
                        }
                    }.frame(width: 300).accessibilityIdentifier("stack-mode.quick-picker")
                    Spacer()
                    Button(chinese ? "应用星轨合成设置" : "Set up star-trail composition") {
                        model.configureStarTrailComposition()
                    }.accessibilityIdentifier("stack-mode.star-trails")
                        .help(chinese ? "最大值、不对齐、不归一化、不自动筛片。保留手动筛片与后期参数；不会自动去飞机线。" : "Maximum, no alignment, normalization or automatic selection. Manual decisions and finishing settings remain; does not remove aircraft lights.")
                }
                if let method = StackMethod(rawValue: selection) {
                    Text(method.explanation(chinese: chinese)).font(.caption).foregroundStyle(.secondary)
                }
                let alignment = model.deepSkySettings.effectiveValue("registration.mode", schema: schema)
                Text((chinese ? "对齐：" : "Alignment: ") +
                     (AlignmentMethod(rawValue: alignment).map { model.language.alignmentMethodName($0) } ?? alignment) +
                     (chinese ? " · 不兼容的 MAD、剔除图、权重和独立噪声估计会随模式关闭。" : " · Incompatible MAD, maps, weights and independent noise estimates are switched off."))
                    .font(.caption).foregroundStyle(.secondary)
            }.disabled(model.isProcessing)
        }
    }
}
