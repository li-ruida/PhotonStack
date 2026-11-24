import AppKit
import SwiftUI

struct LinearMatchControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @ObservedObject var composition: ChannelCompositionSession
    @ObservedObject var session: LinearMatchSession
    private var zh: Bool { model.language == .simplifiedChinese }
    var body: some View {
        DisclosureGroup(zh ? "母片强度匹配（可选）" : "Match master intensities (optional)") {
            VStack(alignment:.leading,spacing:8) {
                Picker(zh ? "参考母片" : "Reference",selection:Binding(get:{session.referenceName(composition.masters)},set:{session.reference=$0})) {
                    ForEach(composition.masters) { Text($0.name).tag($0.name) }
                }.accessibilityIdentifier("intensity.reference")
                Picker(zh ? "待匹配母片" : "Target",selection:Binding(get:{session.targetName(composition.masters)},set:{session.target=$0})) {
                    ForEach(composition.masters.filter { $0.name != session.referenceName(composition.masters) }) { Text($0.name).tag($0.name) }
                }.accessibilityIdentifier("intensity.target")
                HStack {
                    Text(zh ? "采样分位" : "Quantiles")
                    TextField(zh ? "下限" : "Low",value:$session.low,format:.number).accessibilityIdentifier("intensity.low")
                    Text("–")
                    TextField(zh ? "上限" : "High",value:$session.high,format:.number).accessibilityIdentifier("intensity.high")
                }.textFieldStyle(.roundedBorder)
                Text(zh ? "匹配两张已对齐母片的亮度和背景。不同滤镜的星云结构可能无法完全匹配。"
                     : "Fit gain and offset from common pixels after registration. Different filters may show different structures; this is not catalog color calibration.")
                    .font(.caption2).foregroundStyle(.secondary)
                Button(zh ? "匹配并使用…" : "Match & load target…") {
                    guard let output=chooseOutput() else {return}
                    model.startChannelCompositionJob { service in try await session.match(composition,output:output,using:service) }
                }.disabled(!session.canMatch(composition)).accessibilityIdentifier("intensity.match")
                Button(zh ? "打开匹配结果…" : "Open intensity result…") {
                    let panel=NativeFilePanels.makeImportFolderPanel(message:zh ? "选择含强度匹配 recipe.txt 的目录。" : "Choose an intensity matching result folder.")
                    guard panel.runModal() == .OK,let directory=panel.url else {return}
                    model.startChannelCompositionJob { service in try await session.open(directory,using:service) }
                }.accessibilityIdentifier("intensity.open")
                if let r=session.report {
                    Text("\(r.target.name) → \(r.reference.name)").font(.caption)
                    Text(String(format:zh ? "增益 %.6g · 偏移 %.6g" : "Gain %.6g · Offset %.6g",r.gain,r.offset)).font(.caption)
                    Text(String(format:zh ? "拟合 %d · 留出 %d\n留出 RMS %.4g → %.4g\n残差中位 %.4g · |残差| P95 %.4g" : "Fit %d · Held out %d\nHeld-out RMS %.4g → %.4g\nResidual median %.4g · |Residual| P95 %.4g",r.fitSamples,r.checkSamples,r.beforeRms,r.afterRms,r.medianResidual,r.p95AbsoluteResidual))
                        .font(.caption2).foregroundStyle(.secondary)
                    if r.afterRms > r.beforeRms || r.afterRms > 0.1*r.referenceSpan {
                        Text(zh ? "仍有明显残差，请检查分区与差图，再决定是否用于合色。" : "Residuals remain substantial. Review regions and residual image before composing.")
                            .font(.caption2).foregroundStyle(.orange)
                    }
                    HStack {
                        Button(zh ? "原母片" : "Original") {show("original")}
                        Button(zh ? "匹配后" : "Matched") {show("matched.fits")}.accessibilityIdentifier("intensity.view")
                        Button(zh ? "差图" : "Residual") {show("residual.fits")}.accessibilityIdentifier("intensity.residual")
                    }
                    Button(zh ? "有效范围" : "Validity") {show("validity.fits")}
                    DisclosureGroup(zh ? "4 × 4 留出区域" : "4 × 4 held-out regions") {
                        ForEach(0..<16,id:\.self) { i in
                            let q=r.regions[i]
                            Text(q.count == 0 ? "\(i/4+1),\(i%4+1) · —" : String(format:"%d,%d · n=%d · Δ %.4g → %.4g · RMS %.4g",i/4+1,i%4+1,q.count,q.beforeMean,q.afterMean,q.afterRms))
                                .font(.caption2).monospacedDigit()
                        }
                    }
                    Button(zh ? "载入母片并重设通道" : "Load this pair and reset R / G / B mapping") {
                        model.startChannelCompositionJob { service in try await session.restorePair(into:composition,using:service) }
                    }.accessibilityIdentifier("intensity.restore")
                    Button(zh ? "按原参数重跑…" : "Replay saved coefficients…") {
                        guard let output=chooseOutput() else {return}
                        model.startChannelCompositionJob { service in try await session.replay(output:output,using:service) }
                    }.accessibilityIdentifier("intensity.replay")
                }
            }.padding(.top,4)
        }
    }
    private func show(_ name:String) { model.startChannelCompositionJob { service in try await session.verifiedOutput(name,using:service) } }
    private func chooseOutput() -> URL? {
        let panel=NativeFilePanels.makeBatchOutputDirectoryPanel(message:zh ? "将在所选目录中新建强度匹配结果文件夹。" : "A new intensity-match folder will be created here.")
        guard panel.runModal() == .OK,let parent=panel.url else {return nil}
        return parent.appendingPathComponent("intensity-match-"+UUID().uuidString.prefix(8),isDirectory:true)
    }
}
