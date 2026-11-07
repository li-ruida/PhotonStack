import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct MosaicSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var parameters: ProcessingParameters
    @State private var draggedMosaicPanelID: PhotonStackAsset.ID?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            let plan = model.mosaicPlan

            Text(model.localized(.mosaicSection))
                .font(.headline)

            HStack {
                Text(model.localized(.mosaicPanels))
                Spacer()
                Text("\(model.mosaicAssets.count)")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
            }

            Stepper(
                "\(model.localized(.overlapPixels)) \(parameters.mosaicOverlapPixels)",
                value: $parameters.mosaicOverlapPixels,
                in: 0...4096,
                step: 8
            )

            Picker(model.localized(.mosaicProjection), selection: $parameters.mosaicProjection) {
                ForEach(MosaicProjection.allCases) { projection in
                    Text(model.mosaicProjectionName(projection))
                        .tag(projection)
                }
            }
            .pickerStyle(.segmented)

            Picker(model.localized(.mosaicLayout), selection: $parameters.mosaicLayout) {
                ForEach(MosaicLayoutMode.allCases) { layout in
                    Text(model.mosaicLayoutName(layout))
                        .tag(layout)
                }
            }
            .pickerStyle(.segmented)

            if parameters.mosaicLayout == .grid {
                Stepper(
                    "\(model.localized(.mosaicColumns)) \(parameters.mosaicColumns)",
                    value: $parameters.mosaicColumns,
                    in: 1...12
                )
            }

            Picker(model.localized(.mosaicAlignment), selection: $parameters.mosaicAlignment) {
                ForEach(MosaicAlignmentMode.allCases) { alignment in
                    Text(model.mosaicAlignmentName(alignment))
                        .tag(alignment)
                }
            }
            .pickerStyle(.segmented)

            Picker(model.localized(.mosaicBlend), selection: $parameters.mosaicBlendMode) {
                ForEach(MosaicBlendMode.allCases) { blendMode in
                    Text(model.mosaicBlendModeName(blendMode))
                        .tag(blendMode)
                }
            }
            .pickerStyle(.segmented)

            Toggle(model.localized(.mosaicExposureMatch), isOn: $parameters.mosaicExposureMatching)

            Stepper(
                "\(model.localized(.mosaicPreviewWidth)) \(parameters.mosaicPreviewWidth)",
                value: $parameters.mosaicPreviewWidth,
                in: 320...4096,
                step: 80
            )

            if model.mosaicAssets.isEmpty {
                Text(model.localized(.mosaicNoPanels))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                Button {
                    if let suggestedOverlap = plan.suggestedOverlapPixels {
                        parameters.mosaicOverlapPixels = suggestedOverlap
                    }
                } label: {
                    Label(model.localized(.estimateOverlap), systemImage: "wand.and.stars")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(plan.suggestedOverlapPixels == nil || model.isProcessing)

                VStack(alignment: .leading, spacing: 4) {
                    HStack {
                        Text(model.localized(.mosaicEstimatedSize))
                        Spacer()
                        if let width = plan.estimatedWidth, let height = plan.estimatedHeight {
                            Text("\(width) x \(height)")
                                .font(.system(.caption, design: .monospaced))
                        } else {
                            Text(model.localized(.mosaicMissingMetadata))
                                .foregroundStyle(.secondary)
                        }
                    }

                    if plan.missingMetadataCount > 0 {
                        Text("\(model.localized(.mosaicMissingMetadata)): \(plan.missingMetadataCount)")
                            .foregroundStyle(.secondary)
                    }

                    if plan.hasHeightMismatch {
                        Text(model.localized(.mosaicHeightMismatch))
                            .foregroundStyle(.orange)
                    }
                }
                .font(.caption)

                MosaicLayoutPreviewView(plan: plan)
                    .frame(height: 72)

                ForEach(Array(model.mosaicAssets.enumerated()), id: \.element.id) { index, asset in
                    HStack(spacing: 8) {
                        Text("\(index + 1)")
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .frame(width: 18)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(asset.displayName)
                                .font(.caption)
                                .lineLimit(1)
                            if let width = asset.metadata?.width, let height = asset.metadata?.height {
                                Text("\(width) x \(height)")
                                    .font(.caption2.monospacedDigit())
                                    .foregroundStyle(.secondary)
                            }
                        }
                        Spacer()
                        HStack(spacing: 4) {
                            Button {
                                model.moveMosaicPanel(asset.id, direction: .up)
                            } label: {
                                Label(model.localized(.moveOperationUp), systemImage: "chevron.up")
                                    .labelStyle(.iconOnly)
                            }
                            .disabled(model.isProcessing || index == 0)

                            Button {
                                model.moveMosaicPanel(asset.id, direction: .down)
                            } label: {
                                Label(model.localized(.moveOperationDown), systemImage: "chevron.down")
                                    .labelStyle(.iconOnly)
                            }
                            .disabled(model.isProcessing || index == model.mosaicAssets.count - 1)
                        }
                        .buttonStyle(.borderless)
                    }
                    .onDrag {
                        draggedMosaicPanelID = asset.id
                        return NSItemProvider(object: asset.id.uuidString as NSString)
                    }
                    .onDrop(of: [.text], delegate: MosaicPanelDropDelegate(
                        targetID: asset.id,
                        draggedID: $draggedMosaicPanelID,
                        model: model
                    ))
                    .padding(.vertical, 2)
                }

                if let report = model.mosaicQualityReport {
                    MosaicQualityReportView(model: model, report: report)
                }
            }

            HStack {
                Button {
                    model.startRunMosaicPreview()
                } label: {
                    Label(model.localized(.previewMosaic), systemImage: "eye")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canRunMosaic == false || model.isProcessing)

                Button {
                    model.startRunMosaic()
                } label: {
                    Label(model.localized(.runMosaic), systemImage: "rectangle.3.group")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.borderedProminent)
                .disabled(model.canRunMosaic == false || model.isProcessing)
            }
        }
        .disabled(model.canModifyProcessingConfiguration == false)
    }
}

private struct MosaicQualityReportView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let report: MosaicQualityReport

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Text(model.localized(.mosaicQualityReport))
                .font(.caption)
                .fontWeight(.semibold)

            HStack {
                Label("\(report.matches)", systemImage: "sparkles")
                Spacer()
                Text("\(model.localized(.mosaicFallbackPanels)): \(report.fallbackPanels)")
            }

            HStack {
                Text(report.autoAligned ? model.mosaicAlignmentName(.auto) : model.mosaicAlignmentName(.manual))
                Spacer()
                Text(report.exposureMatched ? model.localized(.mosaicExposureMatch) : report.blend)
            }

            ForEach(report.placements.prefix(4)) { placement in
                HStack(alignment: .top) {
                    Text("#\(placement.index + 1)")
                    Spacer()
                    VStack(alignment: .trailing, spacing: 1) {
                        Text("dx \(placement.dx, specifier: "%.1f") dy \(placement.dy, specifier: "%.1f")")
                        Text("\(transformName(placement.model))  \(placement.scale, specifier: "%.3f")x  \(placement.rotationDegrees, specifier: "%.2f")°\(placement.coarseAlignment ? "  2K" : "")")
                    }
                    VStack(alignment: .trailing, spacing: 1) {
                        Text("\(placement.matches)")
                        if placement.references > 0 {
                            Label("\(placement.references)", systemImage: "link")
                        }
                    }
                }
            }
        }
        .font(.caption2.monospacedDigit())
        .foregroundStyle(.secondary)
        .padding(.top, 4)
    }

    private func transformName(_ value: String) -> String {
        switch value {
        case "translation":
            return model.alignmentMethodName(.translation)
        case "similarity":
            return model.alignmentMethodName(.similarity)
        case "affine":
            return model.alignmentMethodName(.affine)
        default:
            return model.mosaicAlignmentName(.manual)
        }
    }
}

private struct MosaicPanelDropDelegate: DropDelegate {
    let targetID: PhotonStackAsset.ID
    @Binding var draggedID: PhotonStackAsset.ID?
    @ObservedObject var model: PhotonStackWorkspaceModel

    func dropEntered(info: DropInfo) {
        guard let draggedID, draggedID != targetID else {
            return
        }
        model.moveMosaicPanel(draggedID, before: targetID)
    }

    func performDrop(info: DropInfo) -> Bool {
        draggedID = nil
        return true
    }

    func dropUpdated(info: DropInfo) -> DropProposal? {
        DropProposal(operation: .move)
    }
}

private struct MosaicLayoutPreviewView: View {
    let plan: MosaicPlanSummary

    var body: some View {
        GeometryReader { proxy in
            let layout = panelFrames(in: proxy.size)

            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 6)
                    .fill(.quaternary)
                    .overlay {
                        RoundedRectangle(cornerRadius: 6)
                            .stroke(.secondary.opacity(0.22), lineWidth: 1)
                    }

                ForEach(layout) { panel in
                    RoundedRectangle(cornerRadius: 4)
                        .fill(panel.color.opacity(0.38))
                        .overlay {
                            RoundedRectangle(cornerRadius: 4)
                                .stroke(panel.color.opacity(0.75), lineWidth: 1)
                        }
                        .frame(width: panel.frame.width, height: panel.frame.height)
                        .position(x: panel.frame.midX, y: panel.frame.midY)

                    Text("\(panel.index)")
                        .font(.caption2.monospacedDigit())
                        .foregroundStyle(.primary)
                        .position(x: panel.frame.midX, y: panel.frame.midY)
                }
            }
        }
    }

    private func panelFrames(in size: CGSize) -> [LayoutPanel] {
        guard plan.panels.isEmpty == false else {
            return []
        }

        let widths = plan.panels.map { CGFloat($0.width ?? 1) }
        let heights = plan.panels.map { CGFloat($0.height ?? 1) }
        let effectiveOverlap = min(CGFloat(plan.overlapPixels), max((widths.min() ?? 1) - 1, 0))
        let rows = layoutRows(widths: widths, heights: heights, overlap: effectiveOverlap)
        let rawWidth = max(rows.map(\.width).max() ?? 1, 1)
        let rawHeight = max(rows.map(\.height).reduce(0, +) - effectiveOverlap * CGFloat(max(rows.count - 1, 0)), 1)
        let scale = min(size.width / rawWidth, size.height / rawHeight)
        let previewWidth = rawWidth * scale
        let previewHeight = rawHeight * scale
        let xInset = max((size.width - previewWidth) / 2.0, 0)
        let yInset = max((size.height - previewHeight) / 2.0, 0)

        var result: [LayoutPanel] = []
        var panelOffset = 0
        var y = yInset
        for row in rows {
            var x = xInset
            for column in 0..<row.count {
                let panel = plan.panels[panelOffset + column]
                let panelWidth = max(CGFloat(panel.width ?? 1) * scale, 8)
                let panelHeight = max(CGFloat(panel.height ?? 1) * scale, 12)
                let frame = CGRect(x: x, y: y + (row.height * scale - panelHeight) / 2.0, width: panelWidth, height: panelHeight)
                result.append(LayoutPanel(index: panel.index, frame: frame, color: palette[(panelOffset + column) % palette.count]))
                x += panelWidth - effectiveOverlap * scale
            }
            panelOffset += row.count
            y += row.height * scale - effectiveOverlap * scale
        }
        return result
    }

    private func layoutRows(widths: [CGFloat], heights: [CGFloat], overlap: CGFloat) -> [(width: CGFloat, height: CGFloat, count: Int)] {
        let columns = plan.layout == .horizontal ? max(plan.panels.count, 1) : max(plan.columns, 1)
        var rows: [(width: CGFloat, height: CGFloat, count: Int)] = []
        for start in stride(from: 0, to: plan.panels.count, by: columns) {
            let end = min(start + columns, plan.panels.count)
            let rowWidths = widths[start..<end]
            let rowHeights = heights[start..<end]
            let width = rowWidths.reduce(0, +) - overlap * CGFloat(max(rowWidths.count - 1, 0))
            rows.append((width: width, height: rowHeights.max() ?? 1, count: rowWidths.count))
        }
        return rows
    }

    private var palette: [Color] {
        [.blue, .green, .orange, .purple, .pink, .teal]
    }

    private struct LayoutPanel: Identifiable {
        var index: Int
        var frame: CGRect
        var color: Color

        var id: Int {
            index
        }
    }
}
