import AppKit
import SwiftUI
import UniformTypeIdentifiers

struct NarrowbandCalibrationControl: View {
    @ObservedObject var session: NarrowbandSession
    let chinese: Bool
    var body: some View {
        Toggle(chinese ? "提取前校准传感器" : "Calibrate sensor before extraction", isOn: $session.useCalibration)
            .accessibilityIdentifier("narrowband.calibrate")
        if session.useCalibration {
            VStack(alignment: .leading, spacing: 6) {
                master(chinese ? "偏置母片" : "Master bias", path: $session.calibration.bias, role: "bias")
                master(chinese ? "暗场母片" : "Master dark", path: $session.calibration.dark, role: "dark")
                if !session.calibration.dark.isEmpty {
                    biasState(chinese ? "暗场偏置" : "Dark bias", state: $session.calibration.darkBiasState)
                }
                master(chinese ? "平场母片" : "Master flat", path: $session.calibration.flat, role: "flat")
                if !session.calibration.flat.isEmpty {
                    biasState(chinese ? "平场偏置" : "Flat bias", state: $session.calibration.flatBiasState)
                }
                Text(chinese
                     ? "使用未去马赛克、带拜耳信息的线性 FITS 母片。平场按四个拜耳位置分别归一化，负信号保留。已去偏置暗场或仍含偏置平场需提供偏置母片。"
                     : "Use linear CFA FITS masters with Bayer metadata. Flat normalization is per Bayer phase; negative signals are retained. A bias-removed dark or bias-included flat requires a bias master.")
                    .font(.caption2).foregroundStyle(.secondary)
                Text(chinese
                     ? "请确认相机、增益、binning、温度、平场光路及滤镜匹配，暗场曝光与亮场一致。程序检查尺寸、拜耳排列及已提供的相机/增益/binning/曝光字段；缺失字段和温度不作自动匹配。"
                     : "Confirm camera, gain, binning, temperature, flat optics/filter and matching dark exposure. Dimensions, Bayer phase and supplied camera/gain/binning/exposure fields are checked; missing fields and temperature are not matched automatically.")
                    .font(.caption2).foregroundStyle(.secondary)
                if !session.canCalibrate {
                    Text(chinese ? "选择母片并明确偏置状态后可运行。" : "Choose masters and specify their bias state to continue.")
                        .font(.caption).foregroundStyle(.orange)
                }
            }
        }
    }
    private func biasState(_ title: String, state: Binding<String>) -> some View {
        Picker(title, selection: state) {
            Text(chinese ? "请选择…" : "Choose…").tag("unknown")
            Text(chinese ? "仍含偏置" : "Bias included").tag("included")
            Text(chinese ? "已去偏置" : "Bias removed").tag("removed")
        }.pickerStyle(.menu)
    }
    private func master(_ title: String, path: Binding<String>, role: String) -> some View {
        HStack {
            Button(title + "…") {
                let panel = NativeFilePanels.makeRelinkAssetPanel(message: chinese ? "选择未去马赛克的校准母片。" : "Choose an un-debayered calibration master.")
                panel.allowedContentTypes = ["fit", "fits", "fts"].compactMap { UTType(filenameExtension: $0) }
                if panel.runModal() == .OK, let url = panel.url { path.wrappedValue = url.path }
            }.accessibilityIdentifier("narrowband.master." + role)
            if !path.wrappedValue.isEmpty {
                Text(URL(fileURLWithPath: path.wrappedValue).lastPathComponent).lineLimit(1).help(path.wrappedValue)
                Button {
                    path.wrappedValue = ""
                    if role == "dark" { session.calibration.darkBiasState = "unknown" }
                    if role == "flat" { session.calibration.flatBiasState = "unknown" }
                } label: { Image(systemName: "xmark.circle") }
                .buttonStyle(.plain).accessibilityLabel(chinese ? "移除" + title : "Remove " + title)
            }
        }
    }
}
