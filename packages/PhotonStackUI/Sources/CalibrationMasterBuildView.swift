import AppKit
import SwiftUI

struct CalibrationMasterBuildView: View {
    @StateObject private var model: CalibrationMasterBuildSession
    let chinese: Bool
    let saved: @MainActor (String) async -> Void
    @Environment(\.dismiss) private var dismiss
    init(library: CalibrationLibrary, entries: [CalibrationLibrary.Entry], chinese: Bool, saved: @escaping @MainActor (String) async -> Void) {
        _model = StateObject(wrappedValue: CalibrationMasterBuildSession(library: library, entries: entries))
        self.chinese = chinese; self.saved = saved
    }
    private func t(_ zh: String, _ en: String) -> String { chinese ? zh : en }
    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            HStack {
                Text(t("制作校准母片", "Build calibration master")).font(.headline)
                Spacer()
                if model.busy {
                    ProgressView().controlSize(.small)
                    Button(t("停止", "Stop")) { model.cancel() }.accessibilityIdentifier("calibration.build-stop")
                }
                Button(t("关闭", "Close")) { model.close(); dismiss() }.keyboardShortcut(.cancelAction)
            }
            HStack(spacing: 18) {
                step("1", t("选择原片", "Choose frames"), complete: !model.frames.isEmpty)
                Image(systemName: "chevron.right").foregroundStyle(.tertiary)
                step("2", t("制作与检查", "Build and review"), complete: model.currentDraft != nil)
                Image(systemName: "chevron.right").foregroundStyle(.tertiary)
                step("3", t("保存入库", "Save to library"), complete: false)
            }.font(.callout)
            HSplitView {
                settings.frame(minWidth: 290, idealWidth: 310, maxWidth: 330)
                ScrollView {
                    VStack(alignment: .leading, spacing: 12) {
                        if let draft = model.currentDraft { CalibrationMasterReportView(report: draft.report, chinese: chinese) }
                        else {
                            Text(t("先选择同一拍摄参数的校准原片。", "Choose calibration exposures with matching acquisition settings.")).font(.headline)
                            Text(t("支持偏置、暗场、暗平场和平场，保留原始 CFA / 单色像素。平场必须先校正偏置或暗平场。", "Supports bias, dark, dark-flat and flat frames, preserving raw CFA / mono samples. Flats require bias or dark-flat correction."))
                                .font(.callout).foregroundStyle(.secondary)
                            Text(t("\(model.frames.count) / 256 帧", "\(model.frames.count) / 256 frames")).font(.callout)
                            LazyVStack(alignment: .leading, spacing: 6) {
                                ForEach(model.frames, id: \.self) { url in
                                    HStack {
                                        Text(url.lastPathComponent).lineLimit(1).help(url.path)
                                        Spacer()
                                        Button { model.frames.removeAll { $0 == url } } label: { Image(systemName: "minus.circle") }
                                            .buttonStyle(.plain).disabled(model.busy).help(t("移除原片", "Remove frame"))
                                    }.font(.caption)
                                }
                            }
                        }
                        if let error = model.error { ProcessingErrorNotice(message:error,chinese:chinese) }
                    }.frame(maxWidth: .infinity, alignment: .leading).padding(.leading, 14).padding(.trailing, 6)
                }
            }
            HStack {
                Text(t("原片只读。检查后保存母片、有效样本图和完整制作记录。", "Sources stay unchanged. Save the master, sample-count map and full construction record after review."))
                    .font(.caption).foregroundStyle(.secondary)
                Spacer()
                Button(t("制作并检查", "Build and review")) { model.build() }.disabled(model.busy || model.request == nil)
                    .accessibilityIdentifier("calibration.build-run")
                Button(t("保存到母片库", "Save to library")) {
                    model.save { id in await saved(id); dismiss() }
                }.buttonStyle(.borderedProminent).disabled(!model.canSave).accessibilityIdentifier("calibration.build-save")
            }
        }.padding(18).frame(width: 960, height: 710)
            .onChange(of: model.configuration) { _, _ in model.invalidate() }
            .onDisappear { model.close() }
            .interactiveDismissDisabled(model.busy)
    }
    private func step(_ number: String, _ label: String, complete: Bool) -> some View {
        Label(label, systemImage: complete ? "checkmark.circle.fill" : number + ".circle")
            .foregroundStyle(complete ? Color.accentColor : Color.secondary)
    }
    private var settings: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 12) {
                Button(t("选择原片或文件夹…", "Choose frames or folders…")) { chooseFrames() }
                    .accessibilityIdentifier("calibration.build-frames")
                Picker(t("母片类型", "Master type"), selection: $model.role) {
                    ForEach(CalibrationLibrary.roles, id: \.self) { Text(CalibrationPresentation.role($0, chinese: chinese)).tag($0) }
                }.accessibilityIdentifier("calibration.build-role")
                Picker(t("合成方法", "Combine"), selection: $model.method) {
                    Text(t("中位数", "Median")).tag("median"); Text(t("均值", "Mean")).tag("mean")
                }.accessibilityIdentifier("calibration.build-method")
                if model.role == "flat" {
                    Picker(t("平场校正", "Flat correction"), selection: $model.correction) {
                        Text(t("偏置", "Bias")).tag("bias"); Text(t("暗平场", "Dark-flat")).tag("dark-flat")
                    }.accessibilityIdentifier("calibration.build-correction")
                } else if model.role != "bias" {
                    Toggle(t("扣除偏置", "Subtract bias"), isOn: $model.subtractBias).accessibilityIdentifier("calibration.build-subtract-bias")
                }
                if model.usesDarkFlat { masterPicker(t("暗平场母片", "Dark-flat master"), selection: $model.darkFlatID, entries: model.darkFlatEntries, id: "calibration.build-dark-flat") }
                if model.needsBias { masterPicker(t("偏置母片", "Bias master"), selection: $model.biasID, entries: model.biasEntries, id: "calibration.build-bias") }
                if model.usesDarkFlat {
                    Text(t("仍含偏置的暗平场不再减偏置；已扣除偏置的暗平场需要额外偏置母片。", "A bias-included dark-flat needs no second bias subtraction. A bias-removed dark-flat requires a bias master."))
                        .font(.caption).foregroundStyle(.secondary)
                }
                field(t("温度容差 / °C", "Temperature tolerance / °C"), text: $model.temperature, id: "calibration.build-tolerance")
                field(t("最低有效帧数", "Minimum valid samples"), text: $model.minimum, id: "calibration.build-minimum")
                DisclosureGroup(t("资源设置", "Resource settings")) {
                    field(t("内存预算 / MiB", "Memory budget / MiB"), text: $model.memory, id: "calibration.build-memory")
                    field(t("CPU 线程", "CPU threads"), text: $model.workers, id: "calibration.build-workers")
                }.font(.callout)
                Divider()
                TextField(t("母片名称", "Master name"), text: $model.name).textFieldStyle(.roundedBorder).accessibilityIdentifier("calibration.build-name")
                TextField(t("制作说明（可选）", "Preparation notes (optional)"), text: $model.notes, axis: .vertical).lineLimit(2...4)
                    .textFieldStyle(.roundedBorder).accessibilityIdentifier("calibration.build-notes")
                if model.request == nil {
                    Text(t("请选择 2–256 张原片；确认校正母片，并填写有效参数：容差 0–10°C，最低帧数 2–原片数，内存 4–8192 MiB，线程 1–32。", "Choose 2–256 frames, required correction masters and valid settings: tolerance 0–10°C, minimum 2–frame count, memory 4–8192 MiB, threads 1–32."))
                        .font(.caption).foregroundStyle(.secondary)
                }
            }.padding(.trailing, 12)
        }.disabled(model.busy)
    }
    private func masterPicker(_ title: String, selection: Binding<String?>, entries: [CalibrationLibrary.Entry], id: String) -> some View {
        Picker(title, selection: selection) {
            Text(t("请选择", "Choose a master")).tag(nil as String?)
            ForEach(entries) { Text($0.label).tag(Optional($0.id)) }
        }.accessibilityIdentifier(id)
    }
    private func field(_ title: String, text: Binding<String>, id: String) -> some View {
        HStack { Text(title); Spacer(); TextField("", text: text).frame(width: 62).textFieldStyle(.roundedBorder).accessibilityIdentifier(id) }.font(.callout)
    }
    private func chooseFrames() {
        let panel = NSOpenPanel(); panel.canChooseFiles = true; panel.canChooseDirectories = true; panel.allowsMultipleSelection = true
        panel.message = t("选择一组同类型的原始校准 FITS。文件夹递归读取，最多 256 张。", "Choose raw calibration FITS of one type. Folders are scanned recursively, up to 256 frames.")
        if panel.runModal() == .OK { model.choose(panel.urls) }
    }
}
