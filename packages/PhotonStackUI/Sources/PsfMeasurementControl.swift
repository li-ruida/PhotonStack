import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct PsfMeasurementControl:View {
    @ObservedObject var model:PhotonStackWorkspaceModel
    @ObservedObject var session:PsfMeasurementSession
    @State private var showRejected=false
    private var zh:Bool {model.language == .simplifiedChinese}
    var body:some View {
        DisclosureGroup(zh ? "从恒星测量 PSF" : "Measure PSF from stars") {
            VStack(alignment:.leading,spacing:8) {
                Button(session.source.isEmpty ? (zh ? "选择线性母片…" : "Choose linear master…") : URL(fileURLWithPath:session.source).lastPathComponent) {
                    let panel=NativeFilePanels.makeRelinkAssetPanel(message:zh ? "选择单色或 RGB 线性 FITS 母片。" : "Choose a mono or RGB linear FITS master.")
                    panel.allowedContentTypes=["fit","fits","fts"].compactMap {UTType(filenameExtension:$0)}
                    guard panel.runModal() == .OK,let url=panel.url else {return};session.choose(url)
                }.help(session.source).accessibilityIdentifier("psf.source")
                Picker(zh ? "测量通道" : "Channel",selection:$session.channel) {
                    Text(zh ? "亮度 / 单色" : "Luminance / mono").tag("luminance")
                    Text("R").tag("red");Text("G").tag("green");Text("B").tag("blue")
                }.accessibilityIdentifier("psf.channel")
                HStack {
                    Text(zh ? "检测阈值 σ" : "Detection σ")
                    TextField("Sigma",value:$session.sigma,format:.number).accessibilityIdentifier("psf.sigma")
                }.textFieldStyle(.roundedBorder)
                HStack {
                    Text(zh ? "饱和值" : "Saturation")
                    TextField("Saturation",value:$session.saturation,format:.number).accessibilityIdentifier("psf.saturation")
                }.textFieldStyle(.roundedBorder)
                Text(zh ? "饱和值使用母片物理计数；0 表示未知，只排除明显平台饱和。测量使用原始分辨率的椭圆高斯与局部背景模型。"
                     : "Saturation uses master pixel units; 0 means unknown, with only obvious plateaus rejected. Fit elliptical Gaussians and local sky at native resolution.")
                    .font(.caption2).foregroundStyle(.secondary)
                Button(zh ? "测量并保存 PSF…" : "Measure & save PSF…") {
                    guard let output=chooseOutput() else {return}
                    model.startChannelCompositionJob {service in try await session.measure(output:output,using:service)}
                }.disabled(!session.canMeasure).accessibilityIdentifier("psf.measure")
                Button(zh ? "打开 PSF 测量…" : "Open PSF measurement…") {
                    let panel=NativeFilePanels.makeImportFolderPanel(message:zh ? "选择含 PSF recipe.txt 的结果目录。" : "Choose a saved PSF measurement folder.")
                    guard panel.runModal() == .OK,let url=panel.url else {return}
                    model.startChannelCompositionJob {service in try await session.open(url,using:service)}
                }.accessibilityIdentifier("psf.open")
                if let report=session.report { summary(report) }
                Text(zh ? "本入口保存测量、星点审查图和总和为 1 的 PSF 核。下方仍为手动高斯反卷积，尚不自动采用此核。" : "Save measurements, star review and a unit-sum PSF. Manual Gaussian deconvolution below does not yet use this kernel.")
                    .font(.caption2).foregroundStyle(.secondary)
            }.padding(.top,4)
        }.disabled(model.hasActiveProcessing)
    }
    @ViewBuilder private func summary(_ r:PsfMeasurementReport) -> some View {
        Text(zh ? "采用 \(r.accepted) / 审查 \(r.stars.count) / 检出 \(r.detected)" : "Accepted \(r.accepted) / Reviewed \(r.stars.count) / Detected \(r.detected)").font(.caption)
        Text(String(format:zh ? "长 / 短轴 FWHM %.3f / %.3f px\n方向 %.1f° · 星宽 P10–P90 %.3f–%.3f px" : "Major / minor FWHM %.3f / %.3f px\nAngle %.1f° · Width P10–P90 %.3f–%.3f px",r.majorFwhm,r.minorFwhm,r.angle,r.fwhmP10,r.fwhmP90)).font(.caption2).monospacedDigit()
        if r.minorFwhm<2 {Text(zh ? "星点不足 2 像素，测量可能受采样相位影响；不是光学分辨率。" : "Stars are narrower than 2 pixels; sampling phase may bias the fit. This is not optical resolution.").font(.caption2).foregroundStyle(.orange)}
        if r.spatiallyVariable {Text(zh ? "星宽或形状随位置明显变化，单一 PSF 可能不适合全图。" : "Stellar widths or shapes vary substantially; a single PSF may not suit the full field.").font(.caption2).foregroundStyle(.orange)}
        HStack {
            Button(zh ? "星点审查" : "Review stars") {show("stars-preview.tiff")}.accessibilityIdentifier("psf.review")
            Button(zh ? "PSF 核" : "PSF kernel") {show("psf.fits")}.accessibilityIdentifier("psf.kernel")
            Button(zh ? "原母片" : "Original") {show("original")}
        }
        Text(zh ? "绿：采用 · 灰：手动排除 · 橙：达到饱和值 · 红：其他剔除。审查图已拉伸，仅用于显示。" : "Green: accepted; gray: excluded; orange: saturated; red: other rejection. Stretched review is display-only.").font(.caption2).foregroundStyle(.secondary)
        DisclosureGroup(zh ? "4 × 4 星宽分布" : "4 × 4 field widths") {
            ForEach(0..<16,id:\.self) {i in
                Text(r.regions[i].count==0 ? "\(i/4+1),\(i%4+1) · —" : String(format:"%d,%d · n=%d · FWHM %.3f px",i/4+1,i%4+1,r.regions[i].count,r.regions[i].fwhm)).font(.caption2).monospacedDigit()
            }
        }
        DisclosureGroup(zh ? "审查星点与排除项" : "Review & exclude stars") {
            Text(zh ? "勾选后另存重测；更改通道或阈值会清除手动排除项。" : "Check stars to exclude, then measure again. Changing channel or thresholds clears exclusions.").font(.caption2).foregroundStyle(.secondary)
            if !session.canEditExclusions {Text(zh ? "参数已改变，请先重测再编辑排除项。" : "Settings changed. Measure again before editing exclusions.").font(.caption2).foregroundStyle(.secondary)}
            Toggle(zh ? "显示自动剔除的星点" : "Include automatically rejected stars",isOn:$showRejected).toggleStyle(.checkbox)
            Text(zh ? "手动排除：\(session.excluded.count)" : "Manual exclusions: \(session.excluded.count)").font(.caption2)
            ScrollView {
                LazyVStack(alignment:.leading,spacing:4) {
                    ForEach(r.stars.filter {showRejected || $0.status=="accepted" || $0.status=="excluded"}) {star in
                        Toggle(isOn:Binding(get:{session.excluded.contains(star.id)},set:{value in if value {session.excluded.insert(star.id)} else {session.excluded.remove(star.id)}})) {
                            Text(String(format:"#%d · %.1f, %.1f · %@ · %@",star.id,star.x,star.y,star.fwhm>0 ? String(format:"%.2f px",star.fwhm) : "—",status(star.status))).font(.caption2).monospacedDigit()
                        }.toggleStyle(.checkbox).disabled(star.status != "accepted" && star.status != "excluded")
                            .accessibilityIdentifier("psf.star.\(star.id)")
                    }
                }
            }.frame(height:180).disabled(!session.canEditExclusions)
        }
    }
    private func status(_ s:String)->String {
        guard zh else {return s}
        return ["accepted":"采用","excluded":"手动排除","edge":"边缘","masked":"覆盖不足","saturated":"饱和","crowded":"拥挤","fit-failed":"拟合不符","shape-outlier":"星宽异常"][s] ?? s
    }
    private func show(_ name:String) {model.startChannelCompositionJob {service in try await session.verifiedOutput(name,using:service)}}
    private func chooseOutput()->URL? {
        let panel=NativeFilePanels.makeBatchOutputDirectoryPanel(message:zh ? "将在所选目录中新建 PSF 测量文件夹。" : "A new PSF measurement folder will be created here.")
        guard panel.runModal() == .OK,let parent=panel.url else {return nil}
        return parent.appendingPathComponent("psf-measurement-"+UUID().uuidString.prefix(8),isDirectory:true)
    }
}
