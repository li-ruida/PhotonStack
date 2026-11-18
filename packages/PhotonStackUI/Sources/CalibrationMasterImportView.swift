import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct CalibrationMasterImportView: View {
    @ObservedObject var session: CalibrationLibrarySession
    let chinese: Bool
    let close: () -> Void
    @State private var source: URL?
    @State private var name = ""
    @State private var role = "dark"
    @State private var biasState = "unknown"
    @State private var notes = ""
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(t("添加校准母片", "Add calibration master")).font(.headline)
            HStack {
                Button(t("选择 FITS…", "Choose FITS…")) {
                    let panel = NativeFilePanels.makeRelinkAssetPanel(message: t("选择尚未去马赛克的单通道母片。", "Choose a single-plane master before debayering."))
                    panel.allowedContentTypes = ["fit", "fits", "fts"].compactMap { UTType(filenameExtension: $0) }
                    if panel.runModal() == .OK, let url = panel.url { source = url; name = url.deletingPathExtension().lastPathComponent }
                }.accessibilityIdentifier("calibration.import-file")
                Text(source?.lastPathComponent ?? t("未选择", "No file chosen")).lineLimit(1)
            }.disabled(session.busy)
            Form {
                TextField(t("名称", "Name"), text: $name).accessibilityIdentifier("calibration.import-name")
                Picker(t("用途", "Role"), selection: $role) {
                    ForEach(CalibrationPlanReport.roles, id: \.self) { Text(CalibrationPresentation.role($0, chinese: chinese)).tag($0) }
                }.accessibilityIdentifier("calibration.import-role")
                if role != "bias" {
                    Picker(t("偏置状态", "Bias state"), selection: $biasState) {
                        Text(t("尚未确认", "Not confirmed")).tag("unknown")
                        Text(t("仍含偏置", "Bias included")).tag("included")
                        Text(t("已扣除偏置", "Bias removed")).tag("removed")
                    }.accessibilityIdentifier("calibration.import-bias")
                }
                TextField(t("制作说明（可选）", "Preparation notes (optional)"), text: $notes, axis: .vertical).lineLimit(2...4)
                    .accessibilityIdentifier("calibration.import-notes")
            }.formStyle(.grouped).disabled(session.busy)
            Text(t("母片会保存独立副本，重复文件沿用已有记录。用途和偏置状态是你的制作记录，不会根据图像亮度猜测；文件中的明确偏置标记必须相符；未标记时可先记录未知状态，使用前仍需核对。", "Stores an independent copy; duplicate files retain their existing record. Role and bias state are your preparation record, never inferred from brightness. Explicit bias tags in the file must agree; untagged files can retain an unknown state for later review."))
                .font(.caption).foregroundStyle(.secondary)
            if let error = session.error { Text(error).font(.caption).foregroundStyle(.red).textSelection(.enabled) }
            HStack {
                if session.busy { ProgressView().controlSize(.small) }
                Spacer()
                Button(t("取消", "Cancel")) { session.cancel(); close() }.keyboardShortcut(.cancelAction)
                Button(t("保存到母片库", "Save to library")) {
                    guard let source else { return }
                    let state = role == "bias" ? "included" : biasState
                    session.perform {
                        let id = try await session.library.importMaster(source, label: name, role: role, biasState: state, notes: notes)
                        try await session.reload(); session.selection = id; close()
                    }
                }.buttonStyle(.borderedProminent).disabled(session.busy || source == nil || !CalibrationLibrary.validLabel(name) || notes.utf8.count > 4096)
                    .accessibilityIdentifier("calibration.import-save")
            }
        }.padding(20).frame(width: 540).interactiveDismissDisabled(session.busy)
    }
}
