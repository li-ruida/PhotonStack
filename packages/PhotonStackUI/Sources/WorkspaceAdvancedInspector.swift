import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

enum AdvancedTool: String, CaseIterable {
    case finishing, background, clouds, normalize, color, trails, stars, deconvolve, drizzle

    func title(chinese: Bool) -> String {
        switch self {
        case .finishing: chinese ? "色彩与显示修整" : "Display finishing"
        case .background: chinese ? "背景校正" : "Background"
        case .clouds: chinese ? "云雾" : "Clouds"
        case .normalize: chinese ? "信号归一化" : "Normalize"
        case .color: chinese ? "色彩校正" : "Color correction"
        case .trails: chinese ? "航迹与流星" : "Trails & meteors"
        case .stars: chinese ? "星点蒙版" : "Star masks"
        case .deconvolve: chinese ? "反卷积" : "Deconvolution"
        case .drizzle: chinese ? "Drizzle 堆栈" : "Drizzle stack"
        }
    }
}

@MainActor
final class AdvancedToolSettings: ObservableObject {
    @Published var tool: AdvancedTool = .finishing
    @Published var backgroundModel = "grid"
    @Published var backgroundMode = "subtract"
    @Published var backgroundStrength = 0.35
    @Published var preserveBackgroundBrightness = true
    @Published var protectBrightBackgroundTargets = true
    @Published var selectedCloudIndices: Set<Int> = []
    @Published var targetBackground = 0.25
    @Published var targetScale = 1.0
    @Published var neutralizeStrength = 1.0
    @Published var saturationAmount = 0.2
    @Published var starDetectionThreshold = 3.0
    @Published var starMinimumPeak = 0.05
    @Published var starMaskRadius = 2
    @Published var largeStarMaskRadius = 6
    @Published var layeredStarMask = true
    @Published var deconvolutionIterations = 8
    @Published var deconvolutionRadius = 2
    @Published var deconvolutionSigma = 1.2
    @Published var drizzleScale = 2
    @Published var drizzlePixfrac = 1.0
    @Published var drizzleAlignment: AlignmentMethod = .distortion
    @Published var selectedArtifactIndices: Set<Int> = []
    @Published var removeSelectedMeteors = false
}

struct AdvancedProcessingSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var parameters: ProcessingParameters
    @ObservedObject var settings: AdvancedToolSettings

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Picker(model.language == .simplifiedChinese ? "工具" : "Tool", selection: $settings.tool) {
                ForEach(AdvancedTool.allCases, id: \.self) { tool in
                    Text(tool.title(chinese: model.language == .simplifiedChinese)).tag(tool)
                }
            }
            switch settings.tool {
            case .finishing:
                VStack(alignment: .leading, spacing: 10) {
                    AstroDevelopControl(model: model)
                    GreenCastControl(model: model)
                    ContinuumToneControl(model: model)
                    DisplayGridControl(model: model)
                }
                .buttonStyle(.bordered)

            case .background:
                Picker(model.localized(.backgroundModel), selection: $settings.backgroundModel) {
                    Text(model.localized(.globalModel)).tag("global")
                    Text(model.localized(.gridModel)).tag("grid")
                }
                .pickerStyle(.segmented)

                Picker(model.localized(.backgroundMode), selection: $settings.backgroundMode) {
                    Text(model.localized(.subtractMode)).tag("subtract")
                    Text(model.localized(.divideMode)).tag("divide")
                }
                .pickerStyle(.segmented)

                ParameterSlider(title: model.localized(.backgroundStrength), value: $settings.backgroundStrength, range: 0.0...1.0)

                if settings.backgroundMode == "divide" {
                    Toggle(
                        model.localized(.preserveBackgroundBrightness),
                        isOn: $settings.preserveBackgroundBrightness
                    )
                    .toggleStyle(.checkbox)
                    .help(model.localized(.preserveBackgroundBrightnessHelp))
                }

                if settings.backgroundModel == "grid" {
                    Toggle(
                        model.localized(.protectBrightBackgroundTargets),
                        isOn: $settings.protectBrightBackgroundTargets
                    )
                    .toggleStyle(.checkbox)
                    .help(model.localized(.protectBrightBackgroundTargetsHelp))
                }

                Button {
                    model.startBackgroundPreview(
                        model: settings.backgroundModel,
                        mode: settings.backgroundMode,
                        strength: settings.backgroundStrength,
                        preserveBrightness: settings.preserveBackgroundBrightness,
                        protectBrightTargets: settings.protectBrightBackgroundTargets
                    )
                } label: {
                    Label(model.localized(.applyBackground), systemImage: "circle.lefthalf.filled")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

            case .clouds:
                let hasReferences = model.temporalCloudReferenceCount >= 2
                Text(model.localized(.cloudsSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                ParameterSlider(title: model.localized(.cloudRemovalStrength), value: $parameters.cloudRemovalStrength, range: 0.0...1.0)

                Button {
                    model.startDetectClouds()
                } label: {
                    Label(model.localized(.detectClouds), systemImage: "cloud")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

                Button {
                    model.startDetectCloudsTemporally()
                } label: {
                    Label("多帧辅助识别", systemImage: "rectangle.3.group.bubble")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(!hasReferences || model.canProcessCurrentInput == false || model.isProcessing)

                if !hasReferences {
                    Text("至少载入 3 张同尺寸的光照/参考帧后可启用弱云辅助识别。")
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }

                if let report = model.cloudRegionReport {
                    Text("\(model.localized(.cloudSummary)): \(report.clouds)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                    if report.temporalFramesUsed > 0 {
                        Text("多帧：\(report.temporalFramesUsed) 帧，确认 \(report.temporallyConfirmedClouds) 个暗云候选")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }

                    if report.topItems.isEmpty == false {
                        VStack(alignment: .leading, spacing: 6) {
                            Text(model.localized(.topCloudRegions))
                                .font(.caption)
                                .foregroundStyle(.secondary)

                            HStack(spacing: 8) {
                                Button {
                                    selectVisibleClouds(report)
                                } label: {
                                    Label(model.localized(.selectAllClouds), systemImage: "checklist.checked")
                                        .frame(maxWidth: .infinity)
                                }

                                Button {
                                    clearCloudSelection()
                                } label: {
                                    Label(model.localized(.clearCloudSelection), systemImage: "xmark.circle")
                                        .frame(maxWidth: .infinity)
                                }
                            }
                            .buttonStyle(.bordered)
                            .controlSize(.small)

                            Text(model.localized(.selectedCloudPreviewHint))
                                .font(.caption2)
                                .foregroundStyle(.secondary)

                            ForEach(report.topItems) { item in
                                Toggle(isOn: cloudSelectionBinding(for: item.index)) {
                                    HStack {
                                        Image(systemName: "cloud")
                                            .foregroundStyle(.cyan)
                                        VStack(alignment: .leading, spacing: 2) {
                            Text(item.displayLabel)
                                                .font(.caption)
                                            Text(item.temporalSupport > 0
                                                ? "A \(item.coverage * 100, format: .number.precision(.fractionLength(1)))%  C \(item.confidence, format: .number.precision(.fractionLength(2)))  T \(item.temporalSupport)"
                                                : "A \(item.coverage * 100, format: .number.precision(.fractionLength(1)))%  C \(item.confidence, format: .number.precision(.fractionLength(2)))")
                                                .font(.caption2)
                                                .foregroundStyle(.secondary)
                                        }
                                        Spacer()
                                    }
                                }
                                .toggleStyle(.checkbox)
                            }

                            if report.items.count > report.topItems.count {
                                Text(model.localized(.cloudTop20Hint))
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }

                    Button {
                        model.startRemoveClouds(
                            selectedIndices: Array(settings.selectedCloudIndices).sorted(),
                            strength: model.parameters.cloudRemovalStrength
                        )
                    } label: {
                        Label(model.localized(.removeSelectedClouds), systemImage: "cloud.sun")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(settings.selectedCloudIndices.isEmpty || model.canProcessCurrentInput == false || model.isProcessing)
                    if report.temporalFramesUsed > 0 {
                        Text("去除会复用这次多帧确认的掩膜；需要至少保留 3 张同组帧。")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                }
            case .normalize:
                ParameterSlider(title: model.localized(.targetBackground), value: $settings.targetBackground, range: 0.02...0.6)
                ParameterSlider(title: model.localized(.targetScale), value: $settings.targetScale, range: 0.5...2.0)

                Button {
                    model.startNormalizePreview(targetBackground: settings.targetBackground, targetScale: settings.targetScale)
                } label: {
                    Label(model.localized(.normalizeAction), systemImage: "equal.circle")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)
            case .color:
                Text(model.localized(.colorSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                ParameterSlider(title: model.localized(.neutralizeStrength), value: $settings.neutralizeStrength, range: 0.0...1.0)
                ParameterSlider(title: model.localized(.saturationAmount), value: $settings.saturationAmount, range: -0.5...1.5)

                HStack {
                    Button {
                        model.startColorNeutralizePreview(strength: settings.neutralizeStrength)
                    } label: {
                        Label(model.localized(.neutralizeAction), systemImage: "eyedropper.halffull")
                            .frame(maxWidth: .infinity)
                    }

                    Button {
                        model.startColorSaturatePreview(amount: settings.saturationAmount)
                    } label: {
                        Label(model.localized(.saturationAction), systemImage: "paintpalette")
                            .frame(maxWidth: .infinity)
                    }
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)
            case .trails:
                Text(model.localized(.artifactTrailsSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                HStack {
                    Button {
                        model.startDetectArtifactTrails()
                    } label: {
                        Label(model.localized(.detectArtifactTrails), systemImage: "scope")
                            .frame(maxWidth: .infinity)
                    }

                    Button {
                        model.startRemoveArtifactTrails(selectedIndices: nil, removeMeteors: false)
                    } label: {
                        Label(model.localized(.removeArtificialTrails), systemImage: "wand.and.stars")
                            .frame(maxWidth: .infinity)
                    }
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

                Button {
                    model.startCleanTimelapseArtifactSequence()
                } label: {
                    Label(model.localized(.cleanTimelapseTrails), systemImage: "film.stack")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canCleanTimelapseArtifactSequence == false || model.isProcessing)

                HStack {
                    Button {
                        model.startExtractMeteorLayer()
                    } label: {
                        Label(model.localized(.extractMeteorLayer), systemImage: "sparkles")
                            .frame(maxWidth: .infinity)
                    }

                    Button {
                        model.startRestoreMeteorsFromSelectedAsset()
                    } label: {
                        Label(model.localized(.restoreMeteorsAction), systemImage: "rectangle.stack.badge.plus")
                            .frame(maxWidth: .infinity)
                    }
                }
                .buttonStyle(.bordered)
                .disabled(model.canRestoreMeteorsFromSelectedAsset == false || model.isProcessing)

                if let report = model.artifactTrailReport {
                    Text("\(model.localized(.artifactTrailSummary)): \(report.trails)   \(model.localized(.protectedMeteors)): \(report.protectedMeteors)")
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    if report.topItems.isEmpty == false {
                        VStack(alignment: .leading, spacing: 6) {
                            Text(model.localized(.topArtifactTrails))
                                .font(.caption)
                                .foregroundStyle(.secondary)

                            HStack(spacing: 8) {
                                Button {
                                    selectVisibleArtificialArtifacts(report)
                                } label: {
                                    Label(model.localized(.selectAllArtificialTrails), systemImage: "checklist.checked")
                                        .frame(maxWidth: .infinity)
                                }

                                Button {
                                    clearArtifactSelection()
                                } label: {
                                    Label(model.localized(.clearArtifactSelection), systemImage: "xmark.circle")
                                        .frame(maxWidth: .infinity)
                                }
                            }
                            .buttonStyle(.bordered)
                            .controlSize(.small)

                            Text(model.localized(.selectedArtifactPreviewHint))
                                .font(.caption2)
                                .foregroundStyle(.secondary)

                            ForEach(report.topItems) { item in
                                Toggle(isOn: artifactSelectionBinding(for: item.index)) {
                                    HStack {
                                        Image(systemName: artifactIcon(for: item))
                                            .foregroundStyle(artifactColor(for: item))
                                        VStack(alignment: .leading, spacing: 2) {
                                            Text(artifactKindName(item.kind))
                                                .font(.caption)
                                            Text("\(model.localized(.artifactTrailWeight)) \(item.weight, format: .number.precision(.fractionLength(2)))  L \(item.length, format: .number.precision(.fractionLength(0)))  B \(item.meanBrightness, format: .number.precision(.fractionLength(2)))  C \(item.confidence, format: .number.precision(.fractionLength(2)))")
                                                .font(.caption2)
                                                .foregroundStyle(.secondary)
                                        }
                                        Spacer()
                        Text(item.displayLabel)
                                            .font(.caption2.monospaced())
                                            .foregroundStyle(.secondary)
                                    }
                                }
                                .toggleStyle(.checkbox)
                            }

                            if report.items.count > report.topItems.count {
                                Text(model.localized(.artifactTrailTop20Hint))
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                            }
                        }
                    }

                    Toggle(model.localized(.allowMeteorRemoval), isOn: $settings.removeSelectedMeteors)
                        .toggleStyle(.checkbox)

                    Button {
                        model.startRemoveArtifactTrails(
                            selectedIndices: Array(settings.selectedArtifactIndices).sorted(),
                            removeMeteors: settings.removeSelectedMeteors
                        )
                    } label: {
                        Label(model.localized(.removeSelectedTrails), systemImage: "checklist.checked")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(settings.selectedArtifactIndices.isEmpty || model.canProcessCurrentInput == false || model.isProcessing)
                }
            case .stars:
                Text(model.localized(.starToolsSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                ParameterSlider(
                    title: model.localized(.starDetectionThreshold),
                    value: $settings.starDetectionThreshold,
                    range: 1.0...8.0
                )
                ParameterSlider(
                    title: model.localized(.starMinimumPeak),
                    value: $settings.starMinimumPeak,
                    range: 0.0...0.5
                )
                Stepper(
                    "\(model.localized(.starMaskRadius)) \(settings.starMaskRadius)",
                    value: $settings.starMaskRadius,
                    in: 1...16
                )
                Stepper(
                    "\(model.localized(.largeStarMaskRadius)) \(settings.largeStarMaskRadius)",
                    value: $settings.largeStarMaskRadius,
                    in: max(settings.starMaskRadius, 2)...32
                )
                Toggle(model.localized(.layeredStarMask), isOn: $settings.layeredStarMask)

                HStack {
                    Button {
                        model.startDetectStars(
                            sigmaThreshold: settings.starDetectionThreshold,
                            minPeak: settings.starMinimumPeak
                        )
                    } label: {
                        Label(model.localized(.detectStarsAction), systemImage: "sparkle.magnifyingglass")
                            .frame(maxWidth: .infinity)
                    }

                    Button {
                        model.startCreateStarMask(
                            radius: settings.starMaskRadius,
                            largeRadius: settings.largeStarMaskRadius,
                            layered: settings.layeredStarMask,
                            sigmaThreshold: settings.starDetectionThreshold,
                            minPeak: settings.starMinimumPeak
                        )
                    } label: {
                        Label(model.localized(.createStarMaskAction), systemImage: "circle.grid.cross")
                            .frame(maxWidth: .infinity)
                    }
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

                if let count = model.detectedStarCount {
                    Text("\(model.localized(.detectedStarsLabel)): \(count)")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }
            case .deconvolve:
                Text(model.localized(.deconvolutionSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                Stepper(
                    "\(model.localized(.deconvolutionIterations)) \(settings.deconvolutionIterations)",
                    value: $settings.deconvolutionIterations,
                    in: 1...32
                )
                Stepper(
                    "\(model.localized(.deconvolutionRadius)) \(settings.deconvolutionRadius)",
                    value: $settings.deconvolutionRadius,
                    in: 1...8
                )
                ParameterSlider(title: model.localized(.deconvolutionSigma), value: $settings.deconvolutionSigma, range: 0.5...3.5)

                Button {
                    model.startDeconvolvePreview(
                        iterations: settings.deconvolutionIterations,
                        radius: settings.deconvolutionRadius,
                        sigma: settings.deconvolutionSigma
                    )
                } label: {
                    Label(model.localized(.deconvolveAction), systemImage: "scope")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)
            case .drizzle:
                Text(model.localized(.drizzleSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                Stepper("\(model.localized(.drizzleScale)) \(settings.drizzleScale)x", value: $settings.drizzleScale, in: 1...4)
                ParameterSlider(
                    title: model.localized(.drizzlePixfrac),
                    value: $settings.drizzlePixfrac,
                    range: 0.1...1.0
                )
                Picker(model.localized(.drizzleAlign), selection: $settings.drizzleAlignment) {
                    ForEach(AlignmentMethod.allCases, id: \.self) { method in
                        Text(model.alignmentMethodName(method)).tag(method)
                    }
                }
                .pickerStyle(.menu)

                Button {
                    model.startRunDrizzle(
                        scale: settings.drizzleScale,
                        pixfrac: settings.drizzlePixfrac,
                        alignment: settings.drizzleAlignment
                    )
                } label: {
                    Label(model.localized(.drizzleAction), systemImage: "rectangle.grid.2x2")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .disabled(model.canRunDrizzle == false || model.isProcessing)
            }
        }
        .onChange(of: model.artifactTrailReport) { _, report in
            settings.selectedArtifactIndices = Set(
                report?.topItems
                    .filter { $0.kind != "meteor" }
                    .prefix(8)
                    .map(\.index) ?? []
            )
            settings.removeSelectedMeteors = false
            model.refreshArtifactMarkedPreview(selectedIndices: settings.selectedArtifactIndices)
        }
        .onChange(of: model.cloudRegionReport) { _, report in
            settings.selectedCloudIndices.removeAll()
            model.refreshCloudMarkedPreview(selectedIndices: settings.selectedCloudIndices)
        }
        .onChange(of: settings.starMaskRadius) { _, radius in
            settings.largeStarMaskRadius = max(settings.largeStarMaskRadius, radius)
        }
    }

    private func cloudSelectionBinding(for index: Int) -> Binding<Bool> {
        Binding {
            settings.selectedCloudIndices.contains(index)
        } set: { isSelected in
            if isSelected {
                settings.selectedCloudIndices.insert(index)
            } else {
                settings.selectedCloudIndices.remove(index)
            }
            model.refreshCloudMarkedPreview(selectedIndices: settings.selectedCloudIndices)
        }
    }

    private func selectVisibleClouds(_ report: CloudRegionReport) {
        settings.selectedCloudIndices = Set(report.topItems.map(\.index))
        model.refreshCloudMarkedPreview(selectedIndices: settings.selectedCloudIndices)
    }

    private func clearCloudSelection() {
        settings.selectedCloudIndices.removeAll()
        model.refreshCloudMarkedPreview(selectedIndices: settings.selectedCloudIndices)
    }

    private func artifactSelectionBinding(for index: Int) -> Binding<Bool> {
        Binding {
            settings.selectedArtifactIndices.contains(index)
        } set: { isSelected in
            if isSelected {
                settings.selectedArtifactIndices.insert(index)
            } else {
                settings.selectedArtifactIndices.remove(index)
            }
            model.refreshArtifactMarkedPreview(selectedIndices: settings.selectedArtifactIndices)
        }
    }

    private func selectVisibleArtificialArtifacts(_ report: ArtifactTrailReport) {
        settings.selectedArtifactIndices = Set(
            report.topItems
                .filter { $0.kind != "meteor" }
                .map(\.index)
        )
        model.refreshArtifactMarkedPreview(selectedIndices: settings.selectedArtifactIndices)
    }

    private func clearArtifactSelection() {
        settings.selectedArtifactIndices.removeAll()
        model.refreshArtifactMarkedPreview(selectedIndices: settings.selectedArtifactIndices)
    }

    private func artifactKindName(_ kind: String) -> String {
        switch kind {
        case "airplane":
            return model.localized(.airplaneTrail)
        case "drone":
            return model.localized(.droneTrail)
        case "satellite":
            return model.localized(.satelliteTrail)
        case "meteor":
            return model.localized(.meteorTrail)
        default:
            return kind
        }
    }

    private func artifactIcon(for item: ArtifactTrailItem) -> String {
        switch item.kind {
        case "airplane":
            return "airplane"
        case "drone":
            return "dot.radiowaves.left.and.right"
        case "satellite":
            return "dot.circle"
        case "meteor":
            return "sparkles"
        default:
            return "line.diagonal"
        }
    }

    private func artifactColor(for item: ArtifactTrailItem) -> Color {
        switch item.kind {
        case "meteor":
            return .green
        case "drone":
            return .orange
        case "satellite":
            return .cyan
        default:
            return .blue
        }
    }
}
