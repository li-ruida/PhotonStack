import SwiftUI

struct CalibrationMasterReviewView: View {
    let library: CalibrationLibrary
    let entry: CalibrationLibrary.Entry
    let chinese: Bool
    @Environment(\.dismiss) private var dismiss
    @State private var label: String
    @State private var notes: String
    @State private var bias: String
    @State private var quality: String
    @State private var reason = ""
    @State private var busy = false
    @State private var error: String?
    init(library: CalibrationLibrary, entry: CalibrationLibrary.Entry, chinese: Bool) {
        self.library = library; self.entry = entry; self.chinese = chinese
        _label = State(initialValue: entry.label); _notes = State(initialValue: entry.receipt?.notes ?? "")
        _bias = State(initialValue: entry.receipt?.biasState ?? "unknown")
        _quality = State(initialValue: entry.receipt?.reviewHistory?.last?.quality ?? "unreviewed")
    }
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    private func qualityLabel(_ value:String)->String {
        switch value {
        case "keep": return t("保留", "Keep")
        case "reject": return t("不用于校准", "Excluded")
        case "unreviewed": return t("未检查", "Unreviewed")
        default: return value
        }
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(t("检查校准母片", "Review calibration master")).font(.headline)
            Form {
                TextField(t("名称", "Name"), text: $label).accessibilityIdentifier("calibration.review-name")
                Picker(t("偏置状态", "Bias state"), selection: $bias) {
                    Text(t("未确认", "Unknown")).tag("unknown")
                    Text(t("仍含偏置", "Included")).tag("included")
                    Text(t("已扣除", "Removed")).tag("removed")
                }.disabled(entry.receipt?.construction != nil || entry.receipt?.acquisition.text["biasState"] != nil || entry.receipt?.role == "bias")
                Picker(t("是否使用", "Quality decision"), selection: $quality) {
                    Text(t("未检查", "Unreviewed")).tag("unreviewed")
                    Text(t("保留", "Keep")).tag("keep")
                    Text(t("不用于校准", "Reject for calibration")).tag("reject")
                }.accessibilityIdentifier("calibration.review-quality")
                TextField(t("备注", "Notes"), text: $notes, axis: .vertical).lineLimit(2...4)
                TextField(t("修改原因（必填）", "Reason (required)"), text: $reason).accessibilityIdentifier("calibration.review-reason")
            }.disabled(busy)
            Text(t("选择“不用于校准”后，这张母片将不再参与匹配。原图和修改记录会保留。", "The reviewer supplies this decision; it is not automatic scientific validation. Rejected masters are excluded from matching. History is retained and image data stays unchanged."))
                .font(.caption).foregroundStyle(.secondary)
            if let history = entry.receipt?.reviewHistory {
                ScrollView {
                    VStack(alignment: .leading, spacing: 8) {
                        ForEach(Array(history.enumerated().reversed()), id: \.offset) { _, item in
                            Text(item.date.formatted() + " · " + qualityLabel(item.quality) + " · " + item.reason).font(.caption)
                        }
                    }.frame(maxWidth: .infinity, alignment: .leading)
                }.frame(maxHeight: 100)
            }
            if let error { ProcessingErrorNotice(message:error,chinese:chinese) }
            ToolActionBar {
                if busy { ProgressView().controlSize(.small) }
                Spacer()
                Button(t("取消", "Cancel")) { dismiss() }.disabled(busy).keyboardShortcut(.cancelAction)
                Button(t("保存", "Save review")) {
                    busy = true; error = nil
                    Task {
                        do { try await library.revise(entry, label: label, notes: notes, biasState: bias, quality: quality, reason: reason); dismiss() }
                        catch { self.error = error.localizedDescription }
                        busy = false
                    }
                }.disabled(busy || !CalibrationLibrary.validLabel(reason) || !CalibrationLibrary.validLabel(label))
                    .toolPrimaryAction().accessibilityIdentifier("calibration.review-save")
            }
        }.padding(20).frame(width: 610).interactiveDismissDisabled(busy)
    }
}
