import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct AdvancedProcessingSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var parameters: ProcessingParameters
    @State private var backgroundModel = "grid"
    @State private var backgroundMode = "subtract"
    @State private var backgroundStrength = 0.35
    @State private var preserveBackgroundBrightness = true
    @State private var protectBrightBackgroundTargets = true
    @State private var selectedCloudIndices: Set<Int> = []
    @State private var targetBackground = 0.25
    @State private var targetScale = 1.0
    @State private var neutralizeStrength = 1.0
    @State private var saturationAmount = 0.2
    @State private var starDetectionThreshold = 3.0
    @State private var starMinimumPeak = 0.05
    @State private var starMaskRadius = 2
    @State private var largeStarMaskRadius = 6
    @State private var layeredStarMask = true
    @State private var deconvolutionIterations = 8
    @State private var deconvolutionRadius = 2
    @State private var deconvolutionSigma = 1.2
    @State private var drizzleScale = 2
    @State private var drizzlePixfrac = 1.0
    @State private var drizzleAlignment: AlignmentMethod = .distortion
    @State private var selectedArtifactIndices: Set<Int> = []
    @State private var removeSelectedMeteors = false

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text(model.localized(.advancedProcessingSection))
                .font(.headline)

            VStack(alignment: .leading, spacing: 8) {
                Picker(model.localized(.backgroundModel), selection: $backgroundModel) {
                    Text(model.localized(.globalModel)).tag("global")
                    Text(model.localized(.gridModel)).tag("grid")
                }
                .pickerStyle(.segmented)

                Picker(model.localized(.backgroundMode), selection: $backgroundMode) {
                    Text(model.localized(.subtractMode)).tag("subtract")
                    Text(model.localized(.divideMode)).tag("divide")
                }
                .pickerStyle(.segmented)

                ParameterSlider(title: model.localized(.backgroundStrength), value: $backgroundStrength, range: 0.0...1.0)

                if backgroundMode == "divide" {
                    Toggle(
                        model.localized(.preserveBackgroundBrightness),
                        isOn: $preserveBackgroundBrightness
                    )
                    .toggleStyle(.checkbox)
                    .help(model.localized(.preserveBackgroundBrightnessHelp))
                }

                if backgroundModel == "grid" {
                    Toggle(
                        model.localized(.protectBrightBackgroundTargets),
                        isOn: $protectBrightBackgroundTargets
                    )
                    .toggleStyle(.checkbox)
                    .help(model.localized(.protectBrightBackgroundTargetsHelp))
                }

                Button {
                    model.startBackgroundPreview(
                        model: backgroundModel,
                        mode: backgroundMode,
                        strength: backgroundStrength,
                        preserveBrightness: preserveBackgroundBrightness,
                        protectBrightTargets: protectBrightBackgroundTargets
                    )
                } label: {
                    Label(model.localized(.applyBackground), systemImage: "circle.lefthalf.filled")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

            }

            VStack(alignment: .leading, spacing: 8) {
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
                .disabled(model.temporalCloudReferenceCount < 2 || model.canProcessCurrentInput == false || model.isProcessing)

                if model.temporalCloudReferenceCount < 2 {
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
                            selectedIndices: Array(selectedCloudIndices).sorted(),
                            strength: model.parameters.cloudRemovalStrength
                        )
                    } label: {
                        Label(model.localized(.removeSelectedClouds), systemImage: "cloud.sun")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(selectedCloudIndices.isEmpty || model.canProcessCurrentInput == false || model.isProcessing)
                    if report.temporalFramesUsed > 0 {
                        Text("去除会复用这次多帧确认的掩膜；需要至少保留 3 张同组帧。")
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                    }
                }
            }

            VStack(alignment: .leading, spacing: 8) {
                ParameterSlider(title: model.localized(.targetBackground), value: $targetBackground, range: 0.02...0.6)
                ParameterSlider(title: model.localized(.targetScale), value: $targetScale, range: 0.5...2.0)

                Button {
                    model.startNormalizePreview(targetBackground: targetBackground, targetScale: targetScale)
                } label: {
                    Label(model.localized(.normalizeAction), systemImage: "equal.circle")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)
            }

            VStack(alignment: .leading, spacing: 8) {
                Text(model.localized(.colorSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                ParameterSlider(title: model.localized(.neutralizeStrength), value: $neutralizeStrength, range: 0.0...1.0)
                ParameterSlider(title: model.localized(.saturationAmount), value: $saturationAmount, range: -0.5...1.5)

                HStack {
                    Button {
                        model.startColorNeutralizePreview(strength: neutralizeStrength)
                    } label: {
                        Label(model.localized(.neutralizeAction), systemImage: "eyedropper.halffull")
                            .frame(maxWidth: .infinity)
                    }

                    Button {
                        model.startColorSaturatePreview(amount: saturationAmount)
                    } label: {
                        Label(model.localized(.saturationAction), systemImage: "paintpalette")
                            .frame(maxWidth: .infinity)
                    }
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)
            }

            VStack(alignment: .leading, spacing: 8) {
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

                    Toggle(model.localized(.allowMeteorRemoval), isOn: $removeSelectedMeteors)
                        .toggleStyle(.checkbox)

                    Button {
                        model.startRemoveArtifactTrails(
                            selectedIndices: Array(selectedArtifactIndices).sorted(),
                            removeMeteors: removeSelectedMeteors
                        )
                    } label: {
                        Label(model.localized(.removeSelectedTrails), systemImage: "checklist.checked")
                            .frame(maxWidth: .infinity)
                    }
                    .buttonStyle(.borderedProminent)
                    .disabled(selectedArtifactIndices.isEmpty || model.canProcessCurrentInput == false || model.isProcessing)
                }
            }

            VStack(alignment: .leading, spacing: 8) {
                Text(model.localized(.starToolsSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                ParameterSlider(
                    title: model.localized(.starDetectionThreshold),
                    value: $starDetectionThreshold,
                    range: 1.0...8.0
                )
                ParameterSlider(
                    title: model.localized(.starMinimumPeak),
                    value: $starMinimumPeak,
                    range: 0.0...0.5
                )
                Stepper(
                    "\(model.localized(.starMaskRadius)) \(starMaskRadius)",
                    value: $starMaskRadius,
                    in: 1...16
                )
                Stepper(
                    "\(model.localized(.largeStarMaskRadius)) \(largeStarMaskRadius)",
                    value: $largeStarMaskRadius,
                    in: max(starMaskRadius, 2)...32
                )
                Toggle(model.localized(.layeredStarMask), isOn: $layeredStarMask)

                HStack {
                    Button {
                        model.startDetectStars(
                            sigmaThreshold: starDetectionThreshold,
                            minPeak: starMinimumPeak
                        )
                    } label: {
                        Label(model.localized(.detectStarsAction), systemImage: "sparkle.magnifyingglass")
                            .frame(maxWidth: .infinity)
                    }

                    Button {
                        model.startCreateStarMask(
                            radius: starMaskRadius,
                            largeRadius: largeStarMaskRadius,
                            layered: layeredStarMask,
                            sigmaThreshold: starDetectionThreshold,
                            minPeak: starMinimumPeak
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
            }

            VStack(alignment: .leading, spacing: 8) {
                Text(model.localized(.deconvolutionSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                Stepper(
                    "\(model.localized(.deconvolutionIterations)) \(deconvolutionIterations)",
                    value: $deconvolutionIterations,
                    in: 1...32
                )
                Stepper(
                    "\(model.localized(.deconvolutionRadius)) \(deconvolutionRadius)",
                    value: $deconvolutionRadius,
                    in: 1...8
                )
                ParameterSlider(title: model.localized(.deconvolutionSigma), value: $deconvolutionSigma, range: 0.5...3.5)

                Button {
                    model.startDeconvolvePreview(
                        iterations: deconvolutionIterations,
                        radius: deconvolutionRadius,
                        sigma: deconvolutionSigma
                    )
                } label: {
                    Label(model.localized(.deconvolveAction), systemImage: "scope")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)
            }

            VStack(alignment: .leading, spacing: 8) {
                Text(model.localized(.drizzleSection))
                    .font(.subheadline)
                    .foregroundStyle(.secondary)

                Stepper("\(model.localized(.drizzleScale)) \(drizzleScale)x", value: $drizzleScale, in: 1...4)
                ParameterSlider(
                    title: model.localized(.drizzlePixfrac),
                    value: $drizzlePixfrac,
                    range: 0.1...1.0
                )
                Picker(model.localized(.drizzleAlign), selection: $drizzleAlignment) {
                    ForEach(AlignmentMethod.allCases, id: \.self) { method in
                        Text(model.alignmentMethodName(method)).tag(method)
                    }
                }
                .pickerStyle(.menu)

                Button {
                    model.startRunDrizzle(
                        scale: drizzleScale,
                        pixfrac: drizzlePixfrac,
                        alignment: drizzleAlignment
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
            selectedArtifactIndices = Set(
                report?.topItems
                    .filter { $0.kind != "meteor" }
                    .prefix(8)
                    .map(\.index) ?? []
            )
            removeSelectedMeteors = false
            model.refreshArtifactMarkedPreview(selectedIndices: selectedArtifactIndices)
        }
        .onChange(of: model.cloudRegionReport) { _, report in
            selectedCloudIndices.removeAll()
            model.refreshCloudMarkedPreview(selectedIndices: selectedCloudIndices)
        }
        .onChange(of: starMaskRadius) { _, radius in
            largeStarMaskRadius = max(largeStarMaskRadius, radius)
        }
    }

    private func cloudSelectionBinding(for index: Int) -> Binding<Bool> {
        Binding {
            selectedCloudIndices.contains(index)
        } set: { isSelected in
            if isSelected {
                selectedCloudIndices.insert(index)
            } else {
                selectedCloudIndices.remove(index)
            }
            model.refreshCloudMarkedPreview(selectedIndices: selectedCloudIndices)
        }
    }

    private func selectVisibleClouds(_ report: CloudRegionReport) {
        selectedCloudIndices = Set(report.topItems.map(\.index))
        model.refreshCloudMarkedPreview(selectedIndices: selectedCloudIndices)
    }

    private func clearCloudSelection() {
        selectedCloudIndices.removeAll()
        model.refreshCloudMarkedPreview(selectedIndices: selectedCloudIndices)
    }

    private func artifactSelectionBinding(for index: Int) -> Binding<Bool> {
        Binding {
            selectedArtifactIndices.contains(index)
        } set: { isSelected in
            if isSelected {
                selectedArtifactIndices.insert(index)
            } else {
                selectedArtifactIndices.remove(index)
            }
            model.refreshArtifactMarkedPreview(selectedIndices: selectedArtifactIndices)
        }
    }

    private func selectVisibleArtificialArtifacts(_ report: ArtifactTrailReport) {
        selectedArtifactIndices = Set(
            report.topItems
                .filter { $0.kind != "meteor" }
                .map(\.index)
        )
        model.refreshArtifactMarkedPreview(selectedIndices: selectedArtifactIndices)
    }

    private func clearArtifactSelection() {
        selectedArtifactIndices.removeAll()
        model.refreshArtifactMarkedPreview(selectedIndices: selectedArtifactIndices)
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
