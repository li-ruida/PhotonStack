import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct ProcessingWorkspaceSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var selectedTab: WorkspaceTab = .layers

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Picker("", selection: $selectedTab) {
                ForEach(WorkspaceTab.allCases) { tab in
                    Text(model.localized(tab.titleKey))
                        .tag(tab)
                }
            }
            .labelsHidden()
            .pickerStyle(.segmented)

            selectedContent
        }
    }

    @ViewBuilder
    private var selectedContent: some View {
        switch selectedTab {
        case .assets:
            SourceAssetsWorkspaceView(model: model)
        case .objects:
            SmartObjectsWorkspaceView(model: model, artifactIcon: artifactIcon)
        case .layers:
            LayersWorkspaceView(model: model, artifactIcon: artifactIcon)
        }
    }

    private func artifactIcon(for kind: ProcessingArtifactKind) -> String {
        switch kind {
        case .rawSequence, .decodedSequence, .calibratedSequence, .registeredSequence, .cleanedTimelapseSequence:
            return "rectangle.stack"
        case .stackMaster:
            return "square.stack.3d.up"
        case .editedImage:
            return "photo"
        case .mask:
            return "circle.dashed"
        case .meteorLayer:
            return "sparkles"
        case .starLayer:
            return "star"
        }
    }
}

private enum WorkspaceTab: String, CaseIterable, Identifiable {
    case assets
    case objects
    case layers

    var id: String {
        rawValue
    }

    var titleKey: LocalizedTextKey {
        switch self {
        case .assets:
            return .workspaceAssetsTab
        case .objects:
            return .workspaceObjectsTab
        case .layers:
            return .workspaceLayersTab
        }
    }
}

private struct SourceAssetsWorkspaceView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if model.project.assets.isEmpty {
                Text(model.localized(.noSourceAssets))
                    .foregroundStyle(.secondary)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 8) {
                        ForEach(model.project.assets) { asset in
                            SourceAssetWorkspaceRow(model: model, asset: asset)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 220)
            }
        }
    }
}

private struct SourceAssetWorkspaceRow: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let asset: PhotonStackAsset

    var body: some View {
        HStack(spacing: 8) {
            Image(systemName: model.isAssetMissing(asset) ? "exclamationmark.triangle.fill" : "photo")
                .frame(width: 18)
                .foregroundStyle(
                    model.isAssetMissing(asset)
                        ? Color.orange
                        : (model.selectedAssetID == asset.id ? Color.accentColor : Color.secondary)
                )

            VStack(alignment: .leading, spacing: 2) {
                Text(asset.displayName)
                    .lineLimit(1)
                Text("\(model.roleName(asset.role)) / \(asset.kind.rawValue.uppercased())")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                if model.isAssetMissing(asset) {
                    Text(model.localized(.missingAssetStatus))
                        .font(.caption2.weight(.semibold))
                        .foregroundStyle(.orange)
                }
            }

            Spacer()

            Button {
                model.select(asset)
            } label: {
                Label(model.localized(.previewProduct), systemImage: "eye")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(model.localized(.previewProduct))
            .accessibilityLabel("\(model.localized(.previewProduct)): \(asset.displayName)")
            .disabled(model.isProcessing || model.isAssetMissing(asset))

            Button {
                model.createLayer(from: asset)
            } label: {
                Label(model.localized(.addLayerFromSource), systemImage: "square.stack.3d.up")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(model.localized(.addLayerFromSource))
            .disabled(model.canModifyWorkspaceProducts == false || model.isAssetMissing(asset))

            Button {
                presentRelinkPanel(for: asset, model: model)
            } label: {
                Label(model.localized(.relinkAssetAction), systemImage: "link")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(model.localized(.relinkAssetHelp))
            .disabled(model.isProcessing)

            Button {
                model.requestAssetRemoval(asset)
            } label: {
                Label(model.localized(.removeAssetAction), systemImage: "trash")
                    .labelStyle(.iconOnly)
            }
            .buttonStyle(.borderless)
            .help(model.localized(.removeAssetHelp))
            .disabled(model.isProcessing)
        }
        .padding(.vertical, 3)
        .contextMenu {
            Button {
                presentRelinkPanel(for: asset, model: model)
            } label: {
                Label(model.localized(.relinkAssetAction), systemImage: "link")
            }
            .disabled(model.isProcessing)

            Divider()

            Button(role: .destructive) {
                model.requestAssetRemoval(asset)
            } label: {
                Label(model.localized(.removeAssetAction), systemImage: "trash")
            }
            .disabled(model.isProcessing)
        }
    }
}

private struct SmartObjectsWorkspaceView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let artifactIcon: (ProcessingArtifactKind) -> String

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            if model.project.artifacts.isEmpty {
                Text(model.localized(.noArtifacts))
                    .foregroundStyle(.secondary)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 8) {
                        ForEach(model.project.artifacts.reversed()) { artifact in
                            SmartObjectWorkspaceRow(model: model, artifact: artifact, iconName: artifactIcon(artifact.kind))
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 240)
            }
        }
        .disabled(model.canModifyWorkspaceProducts == false)
    }
}

private struct SmartObjectWorkspaceRow: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let artifact: ProcessingArtifact
    let iconName: String

    var body: some View {
        VStack(alignment: .leading, spacing: 5) {
            HStack(spacing: 8) {
                Image(systemName: iconName)
                    .frame(width: 18)
                    .foregroundStyle(.blue)

                VStack(alignment: .leading, spacing: 1) {
                    Text(artifact.name)
                        .lineLimit(1)
                    Text(model.artifactKindName(artifact.kind))
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }

                Spacer()

                Button {
                    model.previewArtifact(artifact)
                } label: {
                    Label(model.localized(.previewProduct), systemImage: "eye")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .help(model.localized(.previewProduct))
                .accessibilityLabel("\(model.localized(.previewProduct)): \(artifact.name)")
                .disabled(model.isArtifactPreviewAvailable(artifact) == false || model.isProcessing)

                Button {
                    model.createLayer(from: artifact)
                } label: {
                    Label(model.localized(.addLayerFromObject), systemImage: "square.stack.3d.up")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .help(model.localized(.addLayerFromObject))
                .disabled(model.canCreateLayer(from: artifact) == false || model.isProcessing)

                Button(role: .destructive) {
                    model.requestArtifactRemoval(artifact)
                } label: {
                    Label(model.localized(.removeArtifactAction), systemImage: "trash")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .help(model.localized(.removeArtifactHelp))
                .accessibilityLabel("\(model.localized(.removeArtifactAction)): \(artifact.name)")
                .disabled(model.isProcessing)
            }

            if artifact.previewURL != nil && model.isArtifactPreviewAvailable(artifact) == false {
                Label(model.localized(.missingWorkspaceSourceError), systemImage: "exclamationmark.triangle.fill")
                    .font(.caption2)
                    .foregroundStyle(.orange)
                    .lineLimit(2)
            }

            HStack(spacing: 8) {
                Text("\(model.localized(.framesLabel)): \(artifact.frameCount)")
                if let output = artifact.outputDirectory ?? artifact.previewURL?.deletingLastPathComponent() {
                    Text(output.lastPathComponent)
                        .lineLimit(1)
                }
            }
            .font(.caption2)
            .foregroundStyle(.secondary)

            if let matches = artifact.frames.first(where: { $0.isReference == false })?.metrics["matches"] {
                Text("\(model.localized(.registrationMatches)): \(matches)")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }
        }
        .padding(.vertical, 3)
    }
}

private struct LayersWorkspaceView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let artifactIcon: (ProcessingArtifactKind) -> String

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(model.localized(.workspaceLayersTab))
                    .font(.headline)

                Text("\(model.project.layers.count)")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)

                Spacer()

                addLayerMenu

                Button {
                    model.startPreviewLayerStack()
                } label: {
                    Label(model.localized(.previewLayerStack), systemImage: "square.3.layers.3d")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .help(model.localized(.previewLayerStack))
                .disabled(model.canPreviewVisibleLayerStack == false || model.isProcessing)
            }

            if let activeLayer = model.activeLayer {
                Label("\(model.localized(.activeLayerLabel)): \(activeLayer.name)", systemImage: "target")
                    .font(.caption2)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            } else {
                Text(model.localized(.noActiveLayer))
                    .font(.caption2)
                    .foregroundStyle(.secondary)
            }

            if model.project.layers.isEmpty {
                Text(model.localized(.noLayers))
                    .foregroundStyle(.secondary)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 8) {
                        ForEach(model.project.layers.reversed()) { layer in
                            ProcessingLayerRow(model: model, layer: layer)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 260)
            }
        }
        .disabled(model.canModifyWorkspaceProducts == false)
    }

    private var addLayerMenu: some View {
        Menu {
            if model.project.assets.isEmpty == false {
                Section(model.localized(.workspaceAssetsTab)) {
                    ForEach(model.project.assets) { asset in
                        Button {
                            model.createLayer(from: asset)
                        } label: {
                            Label(asset.displayName, systemImage: asset.id == model.selectedAssetID ? "checkmark.circle" : "photo")
                        }
                        .disabled(model.isAssetMissing(asset))
                    }
                }
            }

            let artifacts = model.project.artifacts
                .filter { $0.previewURL != nil }
                .reversed()
            if artifacts.isEmpty == false {
                Section(model.localized(.workspaceObjectsTab)) {
                    ForEach(Array(artifacts)) { artifact in
                        Button {
                            model.createLayer(from: artifact)
                        } label: {
                            Label(artifact.name, systemImage: artifactIcon(artifact.kind))
                        }
                        .disabled(model.canCreateLayer(from: artifact) == false)
                    }
                }
            }
        } label: {
            Label(model.localized(.addLayerMenu), systemImage: "plus.square.on.square")
                .labelStyle(.iconOnly)
        }
        .menuStyle(.borderlessButton)
        .help(model.localized(.addLayerMenu))
        .disabled(model.isProcessing || model.hasAvailableLayerSource == false)
    }

}

private struct ProcessingLayerRow: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let layer: ProcessingLayer
    @State private var showingMaskEditor = false

    private var isActive: Bool {
        model.activeLayerID == layer.id
    }

    private var isInputAvailable: Bool {
        model.isLayerInputAvailable(layer)
    }

    private var isMaskAvailable: Bool {
        model.isLayerMaskInputAvailable(layer)
    }

    private var isWorkspaceSourceAvailable: Bool {
        isInputAvailable && isMaskAvailable
    }

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 8) {
                if layer.kind == .mask {
                    Image(systemName: "circle.lefthalf.filled")
                        .foregroundStyle(.secondary)
                        .accessibilityHidden(true)
                        .frame(width: 20)
                } else {
                    Button {
                        model.setLayerVisibility(layer.id, isVisible: model.layerIsVisible(layer.id) == false)
                    } label: {
                        Label(
                            model.localized(model.layerIsVisible(layer.id) ? .hideLayer : .showLayer),
                            systemImage: model.layerIsVisible(layer.id) ? "eye" : "eye.slash"
                        )
                        .labelStyle(.iconOnly)
                        .frame(width: 20)
                    }
                    .buttonStyle(.borderless)
                    .help(model.localized(
                        model.layerIsVisible(layer.id)
                            ? .hideLayer
                            : (model.canShowLayer(layer.id) ? .showLayer : .disabledOperationLayer)
                    ))
                    .disabled(
                        model.isProcessing ||
                            (model.layerIsVisible(layer.id) == false && model.canShowLayer(layer.id) == false)
                    )
                }

                VStack(alignment: .leading, spacing: 2) {
                    HStack(spacing: 4) {
                        Text(layer.name)
                            .lineLimit(1)
                        if isActive {
                            Image(systemName: "target")
                                .font(.caption2)
                                .foregroundStyle(Color.accentColor)
                        }
                    }
                    Text(
                        layer.kind == .mask
                            ? model.layerKindName(layer.kind)
                            : "\(model.layerKindName(layer.kind)) · \(model.layerBlendModeName(model.layerBlendMode(layer.id)))"
                    )
                        .font(.caption2)
                        .foregroundStyle(.secondary)
                }

                if isWorkspaceSourceAvailable == false {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.orange)
                        .help(model.localized(.missingWorkspaceSourceError))
                }

                Spacer()

                if layer.kind != .mask {
                    Button {
                        model.moveProcessingLayer(layer.id, direction: .up)
                    } label: {
                        Label(model.localized(.moveOperationUp), systemImage: "chevron.up")
                            .labelStyle(.iconOnly)
                    }
                    .buttonStyle(.borderless)
                    .disabled(model.isProcessing || model.canMoveProcessingLayer(layer.id, direction: .up) == false)

                    Button {
                        model.moveProcessingLayer(layer.id, direction: .down)
                    } label: {
                        Label(model.localized(.moveOperationDown), systemImage: "chevron.down")
                            .labelStyle(.iconOnly)
                    }
                    .buttonStyle(.borderless)
                    .disabled(model.isProcessing || model.canMoveProcessingLayer(layer.id, direction: .down) == false)
                }

                Button {
                    model.selectLayer(layer)
                } label: {
                    Label(model.localized(.previewProduct), systemImage: "eye")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .accessibilityLabel("\(model.localized(.previewProduct)): \(layer.name)")
                .disabled(isInputAvailable == false || model.isProcessing)

                Button {
                    model.deleteProcessingLayer(layer.id)
                } label: {
                    Label(model.localized(.deleteOperation), systemImage: "trash")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .help(model.localized(.deleteOperation))
                .disabled(model.isProcessing)
            }

            if layer.kind != .mask {
                HStack(spacing: 8) {
                    Picker(model.localized(.blendModeLabel), selection: blendModeBinding) {
                        ForEach(ProcessingLayerBlendMode.allCases) { mode in
                            Text(model.layerBlendModeName(mode)).tag(mode)
                        }
                    }
                    .labelsHidden()
                    .pickerStyle(.menu)
                    .frame(width: 94)
                    .disabled(model.isProcessing || isWorkspaceSourceAvailable == false)

                    Slider(value: opacityBinding, in: 0.0...1.0) { editing in
                        if editing {
                            model.beginLayerOpacityAdjustment(layer.id)
                        } else {
                            model.endLayerOpacityAdjustment(layer.id)
                        }
                    }
                        .disabled(model.isProcessing || isWorkspaceSourceAvailable == false)

                    Text(formatPercent(model.layerOpacity(layer.id)))
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary)
                        .frame(width: 36, alignment: .trailing)
                }
                .padding(.leading, 54)
            }

            if layer.kind != .mask {
                VStack(alignment: .leading, spacing: 6) {
                    HStack(spacing: 8) {
                        Menu {
                            if model.layerMaskArtifact(layer.id) != nil {
                                Button {
                                    model.setLayerMask(layer.id, artifactID: nil)
                                } label: {
                                    Label(model.localized(.detachLayerMask), systemImage: "xmark.circle")
                                }
                                Divider()
                            }

                            ForEach(model.availableMaskArtifacts) { artifact in
                                Button {
                                    model.setLayerMask(layer.id, artifactID: artifact.id)
                                } label: {
                                    Label(
                                        artifact.name,
                                        systemImage: model.layerMaskArtifact(layer.id)?.id == artifact.id
                                            ? "checkmark.circle.fill"
                                            : "circle.lefthalf.filled"
                                    )
                                }
                            }
                        } label: {
                            Label(model.localized(.layerMaskLabel), systemImage: "circle.lefthalf.filled")
                                .labelStyle(.iconOnly)
                        }
                        .menuStyle(.borderlessButton)
                        .help(model.localized(.layerMaskLabel))
                        .disabled(
                            model.isProcessing ||
                                (model.layerMaskArtifact(layer.id) == nil && model.availableMaskArtifacts.isEmpty)
                        )

                        Text(model.layerMaskArtifact(layer.id)?.name ?? model.localized(.noLayerMask))
                            .font(.caption2)
                            .foregroundStyle(.secondary)
                            .lineLimit(1)

                        Spacer()

                        if model.layerMaskArtifact(layer.id) != nil && isMaskAvailable {
                            Button {
                                showingMaskEditor = true
                            } label: {
                                Label(model.localized(.editLayerMask), systemImage: "paintbrush.pointed")
                                    .labelStyle(.iconOnly)
                            }
                            .buttonStyle(.borderless)
                            .help(model.localized(.editLayerMask))
                            .disabled(model.isProcessing)

                            Button {
                                model.setLayerMaskInverted(
                                    layer.id,
                                    inverted: model.layerMaskIsInverted(layer.id) == false
                                )
                            } label: {
                                Label(model.localized(.invertLayerMask), systemImage: "circle.lefthalf.filled")
                                    .labelStyle(.iconOnly)
                                    .foregroundStyle(
                                        model.layerMaskIsInverted(layer.id) ? Color.accentColor : Color.secondary
                                    )
                            }
                            .buttonStyle(.borderless)
                            .help(model.localized(.invertLayerMask))
                            .disabled(model.isProcessing)
                        }
                    }

                    if model.layerMaskArtifact(layer.id) != nil && isMaskAvailable {
                        HStack(spacing: 8) {
                            Text(model.localized(.maskDensity))
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .frame(width: 48, alignment: .leading)
                            Slider(value: maskDensityBinding, in: 0...1) { editing in
                                if editing {
                                    model.beginLayerMaskAdjustment(layer.id)
                                } else {
                                    model.endLayerMaskAdjustment(layer.id)
                                }
                            }
                                .disabled(model.isProcessing)
                            Text(formatPercent(model.layerMaskDensity(layer.id)))
                                .font(.caption2.monospacedDigit())
                                .foregroundStyle(.secondary)
                                .frame(width: 36, alignment: .trailing)
                        }

                        HStack(spacing: 8) {
                            Text(model.localized(.maskFeather))
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                                .frame(width: 48, alignment: .leading)
                            Slider(value: maskFeatherBinding, in: maskFeatherRange) { editing in
                                if editing {
                                    model.beginLayerMaskAdjustment(layer.id)
                                } else {
                                    model.endLayerMaskAdjustment(layer.id)
                                }
                            }
                                .disabled(model.isProcessing)
                            Text("\(Int(model.layerMaskFeatherRadius(layer.id).rounded())) px")
                                .font(.caption2.monospacedDigit())
                                .foregroundStyle(.secondary)
                                .frame(width: 48, alignment: .trailing)
                        }
                    }
                }
                .padding(.leading, 54)
            }
        }
        .padding(.vertical, 4)
        .padding(.horizontal, 6)
        .opacity(layer.kind == .mask || model.layerIsVisible(layer.id) ? 1.0 : 0.55)
        .contentShape(Rectangle())
        .background(isActive ? Color.accentColor.opacity(0.14) : Color.clear, in: RoundedRectangle(cornerRadius: 6))
        .onTapGesture {
            guard model.canModifyWorkspaceProducts else {
                return
            }
            model.selectLayer(layer)
        }
        .sheet(isPresented: $showingMaskEditor) {
            if let maskURL = model.layerMaskArtifact(layer.id)?.previewURL {
                MaskBrushEditorSheet(model: model, layerID: layer.id, maskURL: maskURL)
            }
        }
    }

    private var opacityBinding: Binding<Double> {
        Binding(
            get: { model.layerOpacity(layer.id) },
            set: { model.setLayerOpacity(layer.id, opacity: $0) }
        )
    }

    private var blendModeBinding: Binding<ProcessingLayerBlendMode> {
        Binding(
            get: { model.layerBlendMode(layer.id) },
            set: { model.setLayerBlendMode(layer.id, blendMode: $0) }
        )
    }

    private var maskDensityBinding: Binding<Double> {
        Binding(
            get: { model.layerMaskDensity(layer.id) },
            set: { model.setLayerMaskDensity(layer.id, density: $0) }
        )
    }

    private var maskFeatherBinding: Binding<Double> {
        Binding(
            get: { model.layerMaskFeatherRadius(layer.id) },
            set: { model.setLayerMaskFeatherRadius(layer.id, radius: $0) }
        )
    }

    private var maskFeatherRange: ClosedRange<Double> {
        guard let size = model.layerMaskPixelSize(layer.id) else {
            return 0...1
        }
        return 0...max(1, min(size.width, size.height) * 0.1)
    }

    private func formatPercent(_ value: Double) -> String {
        "\(Int((value * 100).rounded()))%"
    }
}
