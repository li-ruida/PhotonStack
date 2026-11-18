import AppKit
import SwiftUI

struct MasterRegistrationControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @ObservedObject var composition: ChannelCompositionSession
    @ObservedObject var session: MasterRegistrationSession
    private var zh: Bool { model.language == .simplifiedChinese }
    var body: some View {
        DisclosureGroup(zh ? "母片配准与复核" : "Register & review masters") {
            VStack(alignment: .leading, spacing: 8) {
                Picker(zh ? "方式" : "Method", selection: $session.mode) {
                    ForEach(MasterRegistrationMode.allCases, id: \.self) { mode in Text(mode.title(chinese: zh)).tag(mode) }
                }.accessibilityIdentifier("masters.mode")
                Picker(zh ? "参考母片" : "Reference", selection: Binding(get: { session.referenceName(in: composition.masters) }, set: { session.reference = $0 })) {
                    ForEach(composition.masters) { master in Text(master.name).tag(master.name) }
                }.accessibilityIdentifier("masters.reference")
                Text(session.mode == .stars
                     ? (zh ? "按分布充分的共同星点估计平移、旋转与尺度；配准后至少需 12 颗星复核。" : "Fit translation, rotation and scale from distributed stars; require 12 post-warp verification stars.")
                     : (zh ? "所有母片须已有 WCS。以参考母片的画布重投影；共同星点不足时会标为未复核。" : "Every master needs WCS. Reproject to the reference canvas; sparse common stars are reported as unverified."))
                    .font(.caption2).foregroundStyle(.secondary)
                Button(zh ? "配准并载入母片…" : "Register & load masters…") {
                    guard let output = chooseOutput() else { return }
                    model.startChannelCompositionJob { service in try await session.register(composition, output: output, using: service) }
                }.disabled(!session.canRegister(composition.masters)).accessibilityIdentifier("masters.register")
                Button(zh ? "打开已配准母片…" : "Open registered masters…") {
                    let panel = NativeFilePanels.makeImportFolderPanel(message: zh ? "选择含配准 recipe.txt 的结果目录。" : "Choose a master registration result folder.")
                    guard panel.runModal() == .OK, let directory = panel.url else { return }
                    model.startChannelCompositionJob { service in try await session.open(directory, into: composition, using: service) }
                }.accessibilityIdentifier("masters.open")
                if let report = session.report {
                    Text(zh ? "上次配准结果" : "Last registration result").font(.caption).foregroundStyle(.secondary)
                    ForEach(report.masters, id: \.name) { master in
                        VStack(alignment: .leading, spacing: 3) {
                            HStack {
                                Button(master.name) { show(master.name) }.accessibilityIdentifier("masters.view." + master.name)
                                Button(zh ? "有效范围" : "Validity") { show(master.name, validity: true) }
                            }
                            Text(summary(master, reference: report.reference)).font(.caption2).foregroundStyle(.secondary)
                        }
                    }
                    Button(zh ? "按保存的变换重跑…" : "Replay saved transforms…") {
                        guard let output = chooseOutput() else { return }
                        model.startChannelCompositionJob { service in try await session.replay(output: output, into: composition, using: service) }
                    }.accessibilityIdentifier("masters.replay")
                }
                Text(zh ? "另存到参考网格，保留负信号与缺失范围；不会自动匹配曝光或背景。成功后载入下方合色，不修改原母片。"
                     : "Save on the reference grid, retaining negative values and missing support. No exposure/background matching. Load the saved masters for composition; originals stay unchanged.")
                    .font(.caption2).foregroundStyle(.secondary)
            }.padding(.top, 4)
        }
    }
    private func summary(_ master: MasterRegistrationReport.Master, reference: String) -> String {
        let coverage = String(format: "%.1f%%", 100 * master.validFraction)
        if master.name == reference { return (zh ? "参考母片 · 有效 " : "Reference · Valid ") + coverage }
        if master.checkStars < 12 { return (zh ? "未通过共同星点复核 · 有效 " : "Common stars unverified · Valid ") + coverage }
        return zh ? String(format: "复核 %d 星 · 中位 %.3f / P95 %.3f px · 有效 %@", master.checkStars, master.median, master.p95, coverage)
                  : String(format: "%d check stars · Median %.3f / P95 %.3f px · Valid %@", master.checkStars, master.median, master.p95, coverage)
    }
    private func show(_ name: String, validity: Bool = false) {
        model.startChannelCompositionJob { service in try await session.verifiedMaster(name, validity: validity, using: service) }
    }
    private func chooseOutput() -> URL? {
        let panel = NativeFilePanels.makeBatchOutputDirectoryPanel(message: zh ? "将在所选目录中新建已配准母片文件夹。" : "A new registered-masters folder will be created here.")
        guard panel.runModal() == .OK, let parent = panel.url else { return nil }
        return parent.appendingPathComponent("registered-masters-" + UUID().uuidString.prefix(8), isDirectory: true)
    }
}
