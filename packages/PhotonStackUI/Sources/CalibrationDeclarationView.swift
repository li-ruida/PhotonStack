import AppKit
import SwiftUI

struct CalibrationDeclarationView: View {
    let library: CalibrationLibrary
    let chinese: Bool
    @Environment(\.dismiss) private var dismiss
    @State private var inputs: [URL] = []
    @State private var key = "CAM-SER"
    @State private var value = ""
    @State private var declarations: [String: String] = [:]
    @State private var reason = ""
    @State private var busy = false
    @State private var error: String?
    @State private var saved: URL?
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(t("补充或修正拍摄参数", "Declare acquisition metadata")).font(.headline)
            HStack {
                Button(t("选择原片…", "Choose source frames…")) {
                    let panel = NSOpenPanel(); panel.canChooseDirectories = false; panel.allowsMultipleSelection = true
                    if panel.runModal() == .OK { inputs = panel.urls }
                }
                Text("\(inputs.count) " + t("张", "frames"))
            }.disabled(busy)
            HStack {
                Picker(t("字段", "Field"), selection: $key) {
                    ForEach(CalibrationDeclaration.fields, id: \.key) { field in Text(chinese ? field.zh : field.en).tag(field.key) }
                }.frame(width: 230)
                TextField(t("新值", "New value"), text: $value).accessibilityIdentifier("calibration.declaration-value")
                Button(declarations[key] == nil ? t("添加", "Add") : t("更新", "Update")) { declarations[key] = value; value = "" }.disabled(value.isEmpty)
            }.disabled(busy)
            List(declarations.keys.sorted(), id: \.self) { field in
                HStack {
                    Text(CalibrationDeclaration.fields.first(where: { $0.key == field }).map { chinese ? $0.zh : $0.en } ?? field)
                    Spacer(); Text(declarations[field] ?? "").textSelection(.enabled)
                    Button { declarations.removeValue(forKey: field) } label: { Image(systemName: "minus.circle") }.buttonStyle(.borderless).disabled(busy)
                        .help(t("移除这项修正", "Remove this correction"))
                        .accessibilityLabel(t("移除这项修正", "Remove this correction"))
                }
            }.frame(height: 160)
            TextField(t("修正原因（必填）", "Reason (required)"), text: $reason).disabled(busy)
            Text(t("修改会应用到所有选中照片，并保存为副本。只改拍摄信息，不改像素。", "Declarations apply to every selected frame. Choose matching acquisition settings. Saves copies and an audit record; original files and pixel data remain unchanged. Declarations do not change actual exposure or binning."))
                .font(.caption).foregroundStyle(.secondary)
            if let saved {
                HStack {
                    Label(t("副本已保存", "Declared copies saved"), systemImage: "checkmark.circle")
                    Button(t("在访达中显示", "Show in Finder")) { NSWorkspace.shared.activateFileViewerSelecting([saved]) }
                }
                Text(saved.path).font(.caption).textSelection(.enabled)
            }
            if let error { ProcessingErrorNotice(message:error,chinese:chinese) }
            ToolActionBar {
                if busy { ProgressView().controlSize(.small) }
                Spacer(); Button(t("关闭", "Close")) { dismiss() }.disabled(busy).keyboardShortcut(.cancelAction)
                Button(t("保存修正副本…", "Save declared copies…")) { save() }
                    .disabled(busy || inputs.isEmpty || declarations.isEmpty || !CalibrationLibrary.validLabel(reason))
                    .toolPrimaryAction().accessibilityIdentifier("calibration.declaration-save")
            }
        }.padding(20).frame(width: 720).interactiveDismissDisabled(busy)
    }
    private func save() {
        let panel = NSSavePanel(); panel.nameFieldStringValue = "Declared acquisition"
        if panel.runModal() == .OK, let target = panel.url {
            busy = true; error = nil; saved = nil
            Task {
                do { _ = try await library.declareAcquisition(inputs, fields: declarations, reason: reason, to: target); saved = target }
                catch { self.error = error.localizedDescription }
                busy = false
            }
        }
    }
}
