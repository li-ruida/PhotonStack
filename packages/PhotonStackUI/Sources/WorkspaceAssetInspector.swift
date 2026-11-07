import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct SelectedAssetSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Text(model.localized(.assetSection))
                .font(.headline)

            if let asset = model.selectedAsset {
                LabeledContent(model.localized(.nameLabel), value: asset.displayName)
                LabeledContent(model.localized(.kindLabel), value: asset.kind.rawValue.uppercased())
                LabeledContent(model.localized(.pathLabel), value: asset.originalURL.deletingLastPathComponent().path)
                    .font(.caption)

                if model.isAssetMissing(asset) {
                    Label(model.localized(.missingAssetStatus), systemImage: "exclamationmark.triangle.fill")
                        .font(.caption.weight(.semibold))
                        .foregroundStyle(.orange)
                }

                Button {
                    presentRelinkPanel(for: asset, model: model)
                } label: {
                    Label(model.localized(.relinkAssetAction), systemImage: "link")
                }
                .help(model.localized(.relinkAssetHelp))
                .disabled(model.isProcessing)

                Picker(
                    model.localized(.roleLabel),
                    selection: Binding(
                        get: { model.selectedAsset?.role ?? .light },
                        set: { role in
                            if let selected = model.selectedAsset {
                                model.setRole(role, for: selected)
                            }
                        }
                    )
                ) {
                    ForEach(CalibrationFrameRole.allCases, id: \.self) { role in
                        Text(model.roleName(role))
                            .tag(role)
                    }
                }
                .pickerStyle(.menu)
                .disabled(model.canModifyProcessingConfiguration == false)

                if asset.kind == .raw {
                    RawPipelineSection(model: model, asset: asset)
                        .disabled(
                            model.isAssetMissing(asset) ||
                                model.canModifyProcessingConfiguration == false
                        )
                }
            } else {
                Text(model.localized(.noAssetSelected))
                    .foregroundStyle(.secondary)
            }
        }
    }
}

private struct RawPipelineSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    var asset: PhotonStackAsset

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Divider()

            Text(model.localized(.rawPipelineSection))
                .font(.subheadline.weight(.semibold))

            Picker(model.localized(.rawWhiteBalance), selection: rawWhiteBalanceBinding) {
                ForEach(RawWhiteBalanceMode.allCases) { mode in
                    Text(model.rawWhiteBalanceModeName(mode))
                        .tag(mode)
                }
            }
            .pickerStyle(.menu)

            if model.parameters.rawProcessingOptions.whiteBalanceMode == .manual {
                ParameterSlider(
                    title: model.localized(.rawTemperature),
                    value: rawTemperatureBinding,
                    range: 2000...50000
                )
                ParameterSlider(
                    title: model.localized(.rawTint),
                    value: rawTintBinding,
                    range: -150...150
                )
            }

            HStack(spacing: 6) {
                Picker(
                    model.localized(.fitsValueMode),
                    selection: Binding(
                        get: { model.exportOptions.fitsValueMode },
                        set: { model.setFITSValueMode($0) }
                    )
                ) {
                    ForEach(FITSValueMode.allCases) { mode in
                        Text(model.fitsValueModeName(mode))
                            .tag(mode)
                    }
                }
                .pickerStyle(.menu)

                Image(systemName: "info.circle")
                    .foregroundStyle(.secondary)
                    .help(model.localized(.fitsValueModeHelp))
            }

            ParameterSlider(
                title: model.localized(.rawExposureBias),
                value: rawExposureBiasBinding,
                range: -2.0...2.0
            )

            Picker(model.localized(.rawBlackLevel), selection: rawBlackLevelBinding) {
                ForEach(RawBlackLevelMode.allCases) { mode in
                    Text(model.rawBlackLevelModeName(mode))
                        .tag(mode)
                }
            }
            .pickerStyle(.segmented)

            if model.parameters.rawProcessingOptions.blackLevelMode == .manual {
                ParameterSlider(
                    title: model.localized(.rawManualBlackLevel),
                    value: rawManualBlackLevelBinding,
                    range: 0...1
                )
            }

            Picker(model.localized(.rawDemosaicQuality), selection: rawDemosaicBinding) {
                ForEach(RawDemosaicQuality.allCases) { quality in
                    Text(model.rawDemosaicQualityName(quality))
                        .tag(quality)
                }
            }
            .pickerStyle(.segmented)

            Toggle(model.localized(.rawLinearOutput), isOn: rawLinearOutputBinding)

            if model.isRawPreviewUpdating {
                ProgressView()
                    .controlSize(.small)
            }

            if let status = model.rawDecodeStatus {
                Label(
                    model.localized(
                        status.backend == .appleRaw ? .rawDecoderAppleActive : .rawDecoderImageIOFallback
                    ),
                    systemImage: status.usedFallback ? "exclamationmark.triangle.fill" : "checkmark.circle"
                )
                .font(.caption)
                .foregroundStyle(status.usedFallback ? .orange : .secondary)
                .help(
                    status.usedFallback
                        ? "\(status.fallbackErrorCode): \(status.fallbackMessage)"
                        : model.localized(.rawDecoderAppleActive)
                )
            }

            VStack(alignment: .leading, spacing: 4) {
                Text(model.localized(.rawAutoPreview))
                Text(model.localized(.rawAppliedParameters))
                Text(model.localized(.rawDeferredParameters))
            }
            .font(.caption)
            .foregroundStyle(.secondary)

            Text(model.localized(.rawProfessionalNote))
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private var rawWhiteBalanceBinding: Binding<RawWhiteBalanceMode> {
        Binding(
            get: { model.parameters.rawProcessingOptions.whiteBalanceMode },
            set: { model.setRawWhiteBalanceMode($0) }
        )
    }

    private var rawExposureBiasBinding: Binding<Double> {
        Binding(
            get: { model.parameters.rawProcessingOptions.exposureBias },
            set: { model.setRawExposureBias($0) }
        )
    }

    private var rawTemperatureBinding: Binding<Double> {
        Binding(
            get: { model.parameters.rawProcessingOptions.manualWhiteBalanceTemperature },
            set: { model.setRawManualWhiteBalanceTemperature($0) }
        )
    }

    private var rawTintBinding: Binding<Double> {
        Binding(
            get: { model.parameters.rawProcessingOptions.manualWhiteBalanceTint },
            set: { model.setRawManualWhiteBalanceTint($0) }
        )
    }

    private var rawBlackLevelBinding: Binding<RawBlackLevelMode> {
        Binding(
            get: { model.parameters.rawProcessingOptions.blackLevelMode },
            set: { model.setRawBlackLevelMode($0) }
        )
    }

    private var rawManualBlackLevelBinding: Binding<Double> {
        Binding(
            get: { model.parameters.rawProcessingOptions.manualBlackLevel },
            set: { model.setRawManualBlackLevel($0) }
        )
    }

    private var rawDemosaicBinding: Binding<RawDemosaicQuality> {
        Binding(
            get: { model.parameters.rawProcessingOptions.demosaicQuality },
            set: { model.setRawDemosaicQuality($0) }
        )
    }

    private var rawLinearOutputBinding: Binding<Bool> {
        Binding(
            get: { model.parameters.rawProcessingOptions.linearOutput },
            set: { model.setRawLinearOutput($0) }
        )
    }
}

struct PhotoInfoSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.photoInfoSection))
                .font(.headline)

            if let asset = model.selectedAsset {
                VStack(alignment: .leading, spacing: 7) {
                    LabeledContent(model.localized(.nameLabel), value: asset.displayName)
                    LabeledContent(model.localized(.kindLabel), value: asset.kind.rawValue.uppercased())

                    if let metadata = asset.metadata {
                        if let dimensions = dimensions(metadata) {
                            LabeledContent(model.localized(.imageSizeLabel), value: dimensions)
                        }
                        if let format = metadata.formatDescription {
                            LabeledContent(model.localized(.formatLabel), value: format)
                        }
                        if let channels = metadata.channels {
                            LabeledContent(model.localized(.channelsLabel), value: "\(channels)")
                        }
                        if let bits = metadata.bitsPerChannel {
                            LabeledContent(model.localized(.bitDepthLabel), value: "\(bits)-bit")
                        }
                        if let camera = cameraName(metadata) {
                            LabeledContent(model.localized(.cameraLabel), value: camera)
                        }
                        if let lens = metadata.lensModel {
                            LabeledContent(model.localized(.lensLabel), value: lens)
                        }
                        if let captureDate = metadata.captureDate {
                            LabeledContent(model.localized(.captureDateLabel), value: captureDate)
                        }
                        if let whiteBalance = formattedWhiteBalance(metadata.whiteBalance) {
                            LabeledContent(model.localized(.rawWhiteBalance), value: whiteBalance)
                        }
                        if let exposure = metadata.exposureTimeSeconds {
                            LabeledContent(model.localized(.exposureLabel), value: formatExposure(exposure))
                        }
                        if let exposureBias = metadata.exposureBias {
                            LabeledContent(model.localized(.exposureBiasLabel), value: exposureBias)
                        }
                        if let fNumber = metadata.fNumber {
                            LabeledContent(model.localized(.fNumberLabel), value: "f/\(formatNumber(fNumber, fractionDigits: 1))")
                        }
                        if let iso = metadata.iso {
                            LabeledContent(model.localized(.isoLabel), value: "\(iso)")
                        }
                        if let focalLength = metadata.focalLengthMM {
                            LabeledContent(model.localized(.focalLengthLabel), value: "\(formatNumber(focalLength, fractionDigits: 1)) mm")
                        }
                        if let orientation = metadata.orientation {
                            LabeledContent(model.localized(.orientationLabel), value: "\(orientation)")
                        }
                        if let profile = metadata.colorProfile ?? metadata.colorModel {
                            LabeledContent(model.localized(.colorProfileLabel), value: profile)
                        }
                        if let decoder = metadata.rawDecoder {
                            LabeledContent(model.localized(.rawDecoderLabel), value: decoder)
                        }
                    } else {
                        Text(model.localized(.metadataUnavailable))
                            .foregroundStyle(.secondary)
                    }
                }
                .font(.caption)
            } else {
                Text(model.localized(.noAssetSelected))
                    .foregroundStyle(.secondary)
            }
        }
    }

    private func dimensions(_ metadata: AssetMetadata) -> String? {
        guard let width = metadata.width, let height = metadata.height else {
            return nil
        }
        return "\(width) x \(height)"
    }

    private func cameraName(_ metadata: AssetMetadata) -> String? {
        let name = [metadata.cameraMake, metadata.cameraModel]
            .compactMap { value in
                let trimmed = value?.trimmingCharacters(in: .whitespacesAndNewlines)
                return trimmed?.isEmpty == false ? trimmed : nil
            }
            .joined(separator: " ")
        return name.isEmpty ? nil : name
    }

    private func formatExposure(_ seconds: Double) -> String {
        guard seconds.isFinite, seconds > 0 else {
            return "-"
        }
        if seconds < 1 {
            let denominator = (1.0 / seconds).rounded()
            if denominator.isFinite, denominator <= Double(Int.max) {
                return "1/\(Int(denominator)) s"
            }
        }
        return seconds < 0.01 ? String(format: "%.3g s", seconds) : "\(formatNumber(seconds, fractionDigits: 2)) s"
    }

    private func formattedWhiteBalance(_ value: String?) -> String? {
        guard let value else {
            return nil
        }
        switch value.lowercased() {
        case "auto":
            return model.rawWhiteBalanceModeName(.auto)
        case "manual":
            return model.rawWhiteBalanceModeName(.manual)
        default:
            return value
        }
    }

    private func formatNumber(_ value: Double, fractionDigits: Int) -> String {
        let formatter = NumberFormatter()
        formatter.minimumFractionDigits = 0
        formatter.maximumFractionDigits = fractionDigits
        return formatter.string(from: NSNumber(value: value)) ?? "\(value)"
    }
}
