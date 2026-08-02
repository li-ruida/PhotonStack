import CoreGraphics
import Foundation
import ImageIO
import PhotonStackAppCore
import SwiftUI

struct MaskBrushCanvasGeometry {
    let imageRect: CGRect
    let canvasSize: CGSize

    init(sourceSize: CGSize, viewportSize: CGSize, zoomScale: Double) {
        guard sourceSize.width.isFinite,
              sourceSize.height.isFinite,
              viewportSize.width.isFinite,
              viewportSize.height.isFinite,
              sourceSize.width > 0,
              sourceSize.height > 0,
              viewportSize.width > 0,
              viewportSize.height > 0
        else {
            imageRect = .zero
            canvasSize = CGSize(
                width: max(0, viewportSize.width.isFinite ? viewportSize.width : 0),
                height: max(0, viewportSize.height.isFinite ? viewportSize.height : 0)
            )
            return
        }

        let fitScale = min(
            viewportSize.width / sourceSize.width,
            viewportSize.height / sourceSize.height
        )
        let safeZoom = zoomScale.isFinite ? min(max(zoomScale, 1), 8) : 1
        let imageSize = CGSize(
            width: sourceSize.width * fitScale * safeZoom,
            height: sourceSize.height * fitScale * safeZoom
        )
        canvasSize = CGSize(
            width: max(viewportSize.width, imageSize.width),
            height: max(viewportSize.height, imageSize.height)
        )
        imageRect = CGRect(
            x: (canvasSize.width - imageSize.width) * 0.5,
            y: (canvasSize.height - imageSize.height) * 0.5,
            width: imageSize.width,
            height: imageSize.height
        )
    }

    func normalizedPoint(at canvasPoint: CGPoint) -> MaskBrushPoint? {
        guard imageRect.width > 0,
              imageRect.height > 0,
              canvasPoint.x.isFinite,
              canvasPoint.y.isFinite,
              canvasPoint.x >= imageRect.minX,
              canvasPoint.x <= imageRect.maxX,
              canvasPoint.y >= imageRect.minY,
              canvasPoint.y <= imageRect.maxY
        else {
            return nil
        }
        return MaskBrushPoint(
            x: (canvasPoint.x - imageRect.minX) / imageRect.width,
            y: (canvasPoint.y - imageRect.minY) / imageRect.height
        )
    }

    func canvasPoint(for point: MaskBrushPoint) -> CGPoint {
        CGPoint(
            x: imageRect.minX + CGFloat(point.x) * imageRect.width,
            y: imageRect.minY + CGFloat(point.y) * imageRect.height
        )
    }

    func displayDiameter(for diameterFraction: Double) -> CGFloat {
        guard diameterFraction.isFinite else {
            return 1
        }
        return max(1, CGFloat(diameterFraction) * min(imageRect.width, imageRect.height))
    }
}

struct MaskBrushStrokeSampler: Equatable {
    private(set) var completedSegments: [[MaskBrushPoint]] = []
    private(set) var activePoints: [MaskBrushPoint] = []

    var visibleSegments: [[MaskBrushPoint]] {
        activePoints.isEmpty ? completedSegments : completedSegments + [activePoints]
    }

    mutating func sample(_ point: MaskBrushPoint?, minimumSpacing: Double) {
        guard let point else {
            finishActiveSegment()
            return
        }
        if let previous = activePoints.last {
            let spacing = minimumSpacing.isFinite ? max(0, minimumSpacing) : 0
            let dx = point.x - previous.x
            let dy = point.y - previous.y
            guard hypot(dx, dy) >= spacing else {
                return
            }
        }
        activePoints.append(point)
    }

    mutating func finish() -> [[MaskBrushPoint]] {
        finishActiveSegment()
        let result = completedSegments
        clear()
        return result
    }

    mutating func clear() {
        completedSegments.removeAll(keepingCapacity: true)
        activePoints.removeAll(keepingCapacity: true)
    }

    private mutating func finishActiveSegment() {
        guard activePoints.isEmpty == false else {
            return
        }
        completedSegments.append(activePoints)
        activePoints.removeAll(keepingCapacity: true)
    }
}

struct MaskBrushEditorSheet: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let layerID: ProcessingLayer.ID
    let maskURL: URL

    @Environment(\.dismiss) private var dismiss
    @State private var mode: MaskBrushMode = .hide
    @State private var brushSize: Double
    @State private var brushOpacity = 1.0
    @State private var strokeGroups: [[MaskBrushStroke]] = []
    @State private var activeSampler = MaskBrushStrokeSampler()
    @State private var zoomScale = 1.0

    private let maskImage: CGImage?
    private let sourceSize: CGSize

    init(model: PhotonStackWorkspaceModel, layerID: ProcessingLayer.ID, maskURL: URL) {
        self.model = model
        self.layerID = layerID
        self.maskURL = maskURL
        let image = Self.loadImage(maskURL)
        maskImage = image
        sourceSize = CGSize(width: image?.width ?? 1, height: image?.height ?? 1)
        let initialSize = min(max(min(sourceSize.width, sourceSize.height) * 0.025, 8), 160)
        _brushSize = State(initialValue: initialSize)
    }

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 12) {
                Text(model.localized(.maskEditorTitle))
                    .font(.headline)

                Spacer()

                Picker(model.localized(.editLayerMask), selection: $mode) {
                    Label(model.localized(.maskBrushHide), systemImage: "paintbrush.pointed.fill")
                        .tag(MaskBrushMode.hide)
                    Label(model.localized(.maskBrushReveal), systemImage: "paintbrush.pointed")
                        .tag(MaskBrushMode.reveal)
                }
                .labelsHidden()
                .pickerStyle(.segmented)
                .frame(width: 230)

                Divider()
                    .frame(height: 20)

                Button {
                    zoomScale = max(1, zoomScale / 1.25)
                } label: {
                    Label(model.localized(.zoomOut), systemImage: "minus.magnifyingglass")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.zoomOut))
                .disabled(zoomScale <= 1)

                Slider(value: $zoomScale, in: 1...8)
                    .frame(width: 100)

                Button {
                    zoomScale = min(8, zoomScale * 1.25)
                } label: {
                    Label(model.localized(.zoomIn), systemImage: "plus.magnifyingglass")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.zoomIn))
                .disabled(zoomScale >= 8)

                Text("\(Int((zoomScale * 100).rounded()))%")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
                    .frame(width: 48, alignment: .trailing)

                Button {
                    _ = strokeGroups.popLast()
                } label: {
                    Label(model.localized(.undoMaskStroke), systemImage: "arrow.uturn.backward")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.undoMaskStroke))
                .disabled(strokeGroups.isEmpty)

                Button {
                    strokeGroups.removeAll()
                    activeSampler.clear()
                } label: {
                    Label(model.localized(.resetMaskStrokes), systemImage: "trash")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.resetMaskStrokes))
                .disabled(strokeGroups.isEmpty && activeSampler.visibleSegments.isEmpty)
            }
            .buttonStyle(.bordered)
            .padding(14)
            .background(.bar)

            Divider()

            GeometryReader { proxy in
                let geometry = MaskBrushCanvasGeometry(
                    sourceSize: sourceSize,
                    viewportSize: proxy.size,
                    zoomScale: zoomScale
                )
                ScrollView([.horizontal, .vertical]) {
                    Canvas { context, _ in
                        context.fill(
                            Path(CGRect(origin: .zero, size: geometry.canvasSize)),
                            with: .color(.black)
                        )
                        if let maskImage {
                            context.draw(Image(decorative: maskImage, scale: 1), in: geometry.imageRect)
                        }
                        draw(
                            strokes: strokeGroups.flatMap { $0 },
                            activeSegments: activeSampler.visibleSegments,
                            geometry: geometry,
                            context: &context
                        )
                    }
                    .frame(width: geometry.canvasSize.width, height: geometry.canvasSize.height)
                    .contentShape(Rectangle())
                    .gesture(brushGesture(in: geometry))
                }
                .background(.black)
                .overlay {
                    if maskImage == nil {
                        ContentUnavailableView(maskURL.lastPathComponent, systemImage: "exclamationmark.triangle")
                    }
                }
            }
            .frame(minHeight: 440)

            Divider()

            HStack(spacing: 14) {
                Text(model.localized(.maskBrushSize))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Slider(value: $brushSize, in: brushSizeRange)
                    .frame(width: 210)
                Text("\(Int(brushSize.rounded())) px")
                    .font(.caption.monospacedDigit())
                    .frame(width: 60, alignment: .trailing)

                Divider()
                    .frame(height: 20)

                Text(model.localized(.maskBrushOpacity))
                    .font(.caption)
                    .foregroundStyle(.secondary)
                Slider(value: $brushOpacity, in: 0.05...1)
                    .frame(width: 150)
                Text("\(Int((brushOpacity * 100).rounded()))%")
                    .font(.caption.monospacedDigit())
                    .frame(width: 42, alignment: .trailing)

                Spacer()

                Button(model.localized(.cancelAction)) {
                    dismiss()
                }

                Button(model.localized(.saveAction)) {
                    if model.applyMaskBrushEdits(strokeGroups.flatMap { $0 }, to: layerID) {
                        dismiss()
                    }
                }
                .buttonStyle(.borderedProminent)
                .disabled(
                        strokeGroups.isEmpty ||
                        maskImage == nil ||
                        model.canModifyWorkspaceProducts == false
                )
            }
            .padding(14)
            .background(.bar)
        }
        .frame(minWidth: 820, minHeight: 620)
    }

    private var brushSizeRange: ClosedRange<Double> {
        let maximum = max(16, min(sourceSize.width, sourceSize.height) * 0.25)
        return 1...maximum
    }

    private var diameterFraction: Double {
        brushSize / max(1, min(sourceSize.width, sourceSize.height))
    }

    private func brushGesture(in geometry: MaskBrushCanvasGeometry) -> some Gesture {
        DragGesture(minimumDistance: 0)
            .onChanged { value in
                activeSampler.sample(
                    geometry.normalizedPoint(at: value.location),
                    minimumSpacing: max(0.0005, diameterFraction * 0.035)
                )
            }
            .onEnded { _ in
                let segments = activeSampler.finish()
                guard segments.isEmpty == false else {
                    return
                }
                strokeGroups.append(segments.map { points in
                    MaskBrushStroke(
                        mode: mode,
                        points: points,
                        diameterFraction: diameterFraction,
                        opacity: brushOpacity
                    )
                })
            }
    }

    private func draw(
        strokes: [MaskBrushStroke],
        activeSegments: [[MaskBrushPoint]],
        geometry: MaskBrushCanvasGeometry,
        context: inout GraphicsContext
    ) {
        for stroke in strokes {
            draw(stroke: stroke, geometry: geometry, context: &context)
        }
        for activePoints in activeSegments {
            draw(
                stroke: MaskBrushStroke(
                    mode: mode,
                    points: activePoints,
                    diameterFraction: diameterFraction,
                    opacity: brushOpacity
                ),
                geometry: geometry,
                context: &context
            )
        }
    }

    private func draw(
        stroke: MaskBrushStroke,
        geometry: MaskBrushCanvasGeometry,
        context: inout GraphicsContext
    ) {
        guard stroke.points.isEmpty == false else {
            return
        }
        let points = stroke.points.map(geometry.canvasPoint(for:))
        let diameter = geometry.displayDiameter(for: stroke.diameterFraction)
        let color = stroke.mode == .reveal ? Color.white : Color.black
        if points.count == 1, let point = points.first {
            context.fill(
                Path(ellipseIn: CGRect(
                    x: point.x - diameter * 0.5,
                    y: point.y - diameter * 0.5,
                    width: diameter,
                    height: diameter
                )),
                with: .color(color.opacity(stroke.opacity))
            )
            return
        }

        var path = Path()
        path.move(to: points[0])
        for point in points.dropFirst() {
            path.addLine(to: point)
        }
        context.stroke(
            path,
            with: .color(color.opacity(stroke.opacity)),
            style: StrokeStyle(lineWidth: diameter, lineCap: .round, lineJoin: .round)
        )
    }

    private static func loadImage(_ url: URL) -> CGImage? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else {
            return nil
        }
        return CGImageSourceCreateImageAtIndex(source, 0, [
            kCGImageSourceShouldCache: true,
            kCGImageSourceShouldCacheImmediately: true,
        ] as CFDictionary)
    }
}
