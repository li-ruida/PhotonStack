import SwiftUI

struct DeepSkyToneReferenceControl: View {
    @Binding var value: String
    let title: String
    let chinese: Bool

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack {
                Text(title).frame(width: 310, alignment: .leading)
                Toggle(title, isOn: Binding(get: { value == "on" }, set: { value = $0 ? "on" : "off" }))
                    .labelsHidden().accessibilityIdentifier("develop.lock-denoise-stretch")
                Spacer(minLength: 0)
            }
            Text(chinese
                 ? "用本次背景校正后、降噪前的图像估计天空、白平衡和自动拉伸，避免调降噪时整幅图跟着变亮。修改背景或调色参数仍会生效；局部星点处理仍可能变化。"
                 : "Estimate sky, balance and automatic stretching before denoising, so changing noise reduction does not re-brighten the whole image. Background and color edits still apply; local star processing can still change.")
                .font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            Text(chinese ? "默认关闭 · 仅重新后期 · 额外占用一幅全尺寸浮点图的内存"
                 : "Off by default · Refinish only · Uses one additional full-size float image in memory")
                .font(.caption2).foregroundStyle(.secondary)
        }
    }
}
