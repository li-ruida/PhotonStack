import AppKit
import SwiftUI

struct PsfRestorationControl:View {
    @ObservedObject var model:PhotonStackWorkspaceModel
    @ObservedObject var session:PsfMeasurementSession
    private var zh:Bool {model.language == .simplifiedChinese}
    var body:some View {
        DisclosureGroup(zh ? "使用实测 PSF 恢复通道" : "Restore measured channel") {
            VStack(alignment:.leading,spacing:8) {
                Text(zh ? "恢复测量时选择的亮度或单个颜色通道，另存线性单色结果。RGB 可分别测量和恢复后重新合色。" : "Restore the measured luminance or color channel to a new linear monochrome image. Measure and restore RGB separately before recombining.").font(.caption2).foregroundStyle(.secondary)
                HStack {Text(zh ? "迭代次数" : "Iterations");TextField("Iterations",value:$session.restoreIterations,format:.number).accessibilityIdentifier("psf.restore.iterations")}
                number(zh ? "混合强度 0–1" : "Amount 0–1",value:$session.restoreAmount,id:"amount")
                number(zh ? "负值补偿" : "Pedestal",value:$session.restorePedestal,id:"pedestal")
                number(zh ? "噪声抑制 σ" : "Noise damping σ",value:$session.restoreNoise,id:"noise")
                number(zh ? "亮星保护阈值" : "Protect above",value:$session.restoreProtect,id:"protect")
                Text(zh ? "数值使用母片像素单位。补偿会先加入、恢复后减回；不足以覆盖负值时会停止。噪声和保护阈值为 0 时关闭；保护范围可在结果中查看。" : "Use source pixel units. Pedestal is added before restoration and removed afterwards; insufficient pedestal is rejected. Zero disables noise damping or bright protection. Inspect the saved protection map.").font(.caption2).foregroundStyle(.secondary)
                if session.report != nil && !session.canRestore {Text(zh ? "请先保存当前 PSF 测量与排除项，并检查恢复参数。" : "Save the current PSF measurement/exclusions and check restoration settings.").font(.caption2).foregroundStyle(.secondary)}
                Button(zh ? "恢复并另存…" : "Restore & save…") {
                    let panel=NativeFilePanels.makeBatchOutputDirectoryPanel(message:zh ? "选择保存恢复结果的目录。" : "Choose a parent folder for restoration products.")
                    guard panel.runModal() == .OK,let parent=panel.url else {return}
                    let output=parent.appendingPathComponent("psf-restoration-"+UUID().uuidString.prefix(8),isDirectory:true)
                    model.startChannelCompositionJob {service in try await session.restore(output:output,using:service)}
                }.disabled(!session.canRestore).accessibilityIdentifier("psf.restore.run")
                Button(zh ? "打开恢复结果…" : "Open restoration…") {
                    let panel=NativeFilePanels.makeImportFolderPanel(message:zh ? "选择含恢复 recipe.txt 的结果目录。" : "Choose a saved restoration folder.")
                    guard panel.runModal() == .OK,let url=panel.url else {return}
                    model.startChannelCompositionJob {service in try await session.openRestoration(url,using:service)}
                }.accessibilityIdentifier("psf.restore.open")
                if let r=session.restoration {
                    Text(zh ? "已保存：\(r.channel) · \(r.iterations) 次迭代 · 保护 \(r.protectedPixels) / \(r.coveredPixels) 像素" : "Saved: \(r.channel) · \(r.iterations) iterations · \(r.protectedPixels) / \(r.coveredPixels) protected pixels").font(.caption2)
                    Text(String(format:zh ? "再卷积残差 RMS %.5g → %.5g\n覆盖加权总量 %.6g → %.6g" : "Reblur residual RMS %.5g → %.5g\nCoverage-weighted flux %.6g → %.6g",r.beforeRms,r.afterRms,r.beforeFlux,r.afterFlux)).font(.caption2).monospacedDigit()
                    HStack {button("恢复图","Restored","restored.fits");button("原通道","Input channel","channel.fits")}
                    HStack {button("变化差图","Change","difference.fits");button("再卷积残差","Residual","residual.fits");button("保护范围","Protection","protection.fits")}
                    Text(zh ? "差图保留正负值，预览会自动拉伸。单一高斯 PSF、噪声与欠采样都会影响结果；残差变小不代表细节真实恢复。" : "Signed differences are auto-stretched for display. A single Gaussian PSF, noise and undersampling limit restoration; lower residuals do not establish recovered detail.").font(.caption2).foregroundStyle(.secondary)
                }
            }.textFieldStyle(.roundedBorder).padding(.top,4)
        }
    }
    private func number(_ label:String,value:Binding<Double>,id:String)->some View {
        HStack {Text(label);TextField(label,value:value,format:.number).accessibilityIdentifier("psf.restore."+id)}
    }
    private func button(_ chinese:String,_ english:String,_ name:String)->some View {
        Button(zh ? chinese:english) {model.startChannelCompositionJob {service in try await session.restoredOutput(name,using:service)}}.accessibilityIdentifier("psf.restore."+name)
    }
}
