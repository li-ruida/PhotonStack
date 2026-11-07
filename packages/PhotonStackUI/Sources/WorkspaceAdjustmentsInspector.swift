import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct AdjustmentsInspectorSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var parameters: ProcessingParameters
    @State private var selectedTab: AdjustmentInspectorTab = .curves

    private let columns = [
        GridItem(.flexible(), spacing: 6),
        GridItem(.flexible(), spacing: 6)
    ]

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            DisclosureGroup(model.language == .simplifiedChinese ? "色彩与显示修整" : "Color and display finishing") {
                VStack(alignment: .leading, spacing: 8) {
                    GreenCastControl(model: model)
                    ContinuumToneControl(model: model)
                    DisplayGridControl(model: model)
                }
                .labelStyle(.titleAndIcon)
                .disabled(!model.canProcessCurrentInput || model.isProcessing || model.editGraphNeedsReplay)
                .padding(.vertical, 6)
            }

            LazyVGrid(columns: columns, spacing: 6) {
                ForEach(AdjustmentInspectorTab.allCases) { tab in
                    Button {
                        selectedTab = tab
                    } label: {
                        Label(model.localized(tab.titleKey), systemImage: tab.iconName)
                            .font(.caption)
                            .lineLimit(1)
                            .frame(maxWidth: .infinity, minHeight: 28)
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(selectedTab == tab ? Color.accentColor : Color.primary)
                    .background(selectedTab == tab ? Color.accentColor.opacity(0.14) : Color.clear, in: RoundedRectangle(cornerRadius: 6))
                    .overlay {
                        RoundedRectangle(cornerRadius: 6)
                            .stroke(selectedTab == tab ? Color.accentColor.opacity(0.28) : Color.secondary.opacity(0.16), lineWidth: 1)
                    }
                }
            }

            Divider()

            selectedContent
        }
        .disabled(model.canModifyProcessingConfiguration == false)
    }

    @ViewBuilder
    private var selectedContent: some View {
        switch selectedTab {
        case .curves:
            HistogramCurvesSection(model: model, parameters: $parameters)
        case .parameters:
            ParametersSection(parameters: $parameters, model: model)
        case .export:
            ExportSettingsSection(model: model)
        case .advanced:
            AdvancedProcessingSection(model: model, parameters: $parameters)
        }
    }
}

private enum AdjustmentInspectorTab: String, CaseIterable, Identifiable {
    case curves
    case parameters
    case export
    case advanced

    var id: String {
        rawValue
    }

    var titleKey: LocalizedTextKey {
        switch self {
        case .curves:
            return .histogramCurvesSection
        case .parameters:
            return .parametersSection
        case .export:
            return .exportSettingsSection
        case .advanced:
            return .advancedProcessingSection
        }
    }

    var iconName: String {
        switch self {
        case .curves:
            return "point.topleft.down.curvedto.point.bottomright.up"
        case .parameters:
            return "slider.horizontal.3"
        case .export:
            return "square.and.arrow.up"
        case .advanced:
            return "wand.and.stars"
        }
    }
}

private struct ParametersSection: View {
    @Binding var parameters: ProcessingParameters
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text(model.localized(.parametersSection))
                .font(.headline)

            Stepper("\(model.localized(.previewWidth)) \(parameters.previewWidth) px", value: $parameters.previewWidth, in: 512...4096, step: 256)

            ParameterSlider(
                title: model.language == .simplifiedChinese ? "自动拉伸 · 目标背景" : "Auto Stretch · Target background",
                value: $parameters.stretchTargetBackground,
                range: 0.05...0.65
            ).accessibilityIdentifier("parameters.stretch-target")

            ParameterSlider(
                title: model.localized(.starReduction),
                value: $parameters.starReductionAmount,
                range: 0.05...0.9
            )

            Toggle(model.localized(.profileAwareStars), isOn: $parameters.profileAwareStarReduction)

            Toggle(model.localized(.edgeAwareStars), isOn: $parameters.edgeAwareStarReduction)

            ParameterSlider(
                title: model.localized(.comaReduction),
                value: $parameters.comaReductionAmount,
                range: 0.0...1.0
            )

            Stepper("\(model.localized(.comaRadius)) \(parameters.comaReductionRadius)", value: $parameters.comaReductionRadius, in: 2...16)

            ParameterSlider(
                title: model.localized(.comaEccentricity),
                value: $parameters.comaReductionEccentricity,
                range: 0.1...0.9
            )

            Toggle(model.localized(.edgeAwareComa), isOn: $parameters.edgeAwareComaReduction)

            ParameterSlider(
                title: model.localized(.localContrast),
                value: $parameters.localContrastAmount,
                range: 0.0...1.0
            )

            Stepper("\(model.localized(.contrastRadius)) \(parameters.localContrastRadius)", value: $parameters.localContrastRadius, in: 1...64)

            ParameterSlider(
                title: model.localized(.denoiseParameter),
                value: $parameters.denoiseAmount,
                range: 0.0...1.0
            )

            ParameterSlider(
                title: model.localized(.chromaDenoiseParameter),
                value: $parameters.denoiseChromaAmount,
                range: 0.0...1.0
            )

            Stepper("\(model.localized(.denoiseRadius)) \(parameters.denoiseRadius)", value: $parameters.denoiseRadius, in: 1...8)

            ParameterSlider(
                title: model.localized(.sharpenParameter),
                value: $parameters.sharpenAmount,
                range: 0.0...1.0
            )

            Stepper("\(model.localized(.sharpenRadius)) \(parameters.sharpenRadius)", value: $parameters.sharpenRadius, in: 1...8)
        }
    }
}

private struct ExportSettingsSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.exportSettingsSection))
                .font(.headline)

            Picker(
                model.localized(.exportBitDepth),
                selection: Binding(
                    get: { model.exportOptions.bitDepth },
                    set: { model.setExportBitDepth($0) }
                )
            ) {
                ForEach(ExportBitDepth.allCases) { bitDepth in
                    Text(model.exportBitDepthName(bitDepth))
                        .tag(bitDepth)
                }
            }
            .pickerStyle(.segmented)

            Picker(
                model.localized(.exportColorSpace),
                selection: Binding(
                    get: { model.exportOptions.colorSpace },
                    set: { model.setExportColorSpace($0) }
                )
            ) {
                ForEach(ExportColorSpace.allCases) { colorSpace in
                    Text(model.exportColorSpaceName(colorSpace))
                        .tag(colorSpace)
                }
            }
            .pickerStyle(.menu)

            ParameterSlider(
                title: model.localized(.jpegQuality),
                value: Binding(
                    get: { model.exportOptions.jpegQuality },
                    set: { model.setExportJPEGQuality($0) }
                ),
                range: 0.1...1.0
            )
        }
    }
}

private struct HistogramCurvesSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var parameters: ProcessingParameters
    @State private var selectedCurvePointID: CurveEditorPoint.ID?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.histogramCurvesSection))
                .font(.headline)

            Picker(
                model.localized(.curveChannel),
                selection: $parameters.curveChannel
            ) {
                ForEach(CurveChannel.allCases) { channel in
                    Text(model.curveChannelName(channel))
                        .tag(channel)
                }
            }
            .pickerStyle(.segmented)
            .onChange(of: parameters.curveChannel) { _, _ in
                model.startCurvePreview()
            }

            CurveEditorView(
                model: model,
                snapshot: model.curveReferenceHistogram ?? model.histogram,
                parameters: $parameters,
                selectedPointID: $selectedCurvePointID
            )
                .frame(height: 156)

            HStack(spacing: 8) {
                if let selectedPoint = parameters.curveEditorPoints.first(where: { $0.id == selectedCurvePointID }) {
                    Text("x \(selectedPoint.input, format: .number.precision(.fractionLength(3)))")
                    Text("y \(selectedPoint.output, format: .number.precision(.fractionLength(3)))")
                } else {
                    Text(model.localized(.addCurvePoint))
                }

                Spacer()

                Button {
                    parameters.addCurvePoint(input: 0.5, output: 0.5)
                    selectedCurvePointID = parameters.curveEditorPoints.min { left, right in
                        abs(left.input - 0.5) < abs(right.input - 0.5)
                    }?.id
                    model.startCurvePreview()
                } label: {
                    Label(model.localized(.addCurvePoint), systemImage: "plus")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.addCurvePoint))

                Button(role: .destructive) {
                    if let selectedCurvePointID {
                        parameters.deleteCurvePoint(selectedCurvePointID)
                        self.selectedCurvePointID = parameters.curveEditorPoints.first?.id
                        model.startCurvePreview()
                    }
                } label: {
                    Label(model.localized(.deleteCurvePoint), systemImage: "trash")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.deleteCurvePoint))
                .disabled(selectedCurvePointID == nil || parameters.curveEditorPoints.count <= 2)
            }
            .font(.caption2.monospacedDigit())
            .foregroundStyle(.secondary)
            .buttonStyle(.bordered)

            if let histogram = model.curveReferenceHistogram ?? model.histogram {
                HStack {
                    Text("\(model.language == .simplifiedChinese ? "最小" : "min") \(histogram.minimum, format: .number.precision(.fractionLength(3)))")
                    Spacer()
                    Text("\(model.language == .simplifiedChinese ? "均值" : "mean") \(histogram.mean, format: .number.precision(.fractionLength(3)))")
                    Spacer()
                    Text("\(model.language == .simplifiedChinese ? "最大" : "max") \(histogram.maximum, format: .number.precision(.fractionLength(3)))")
                }
                .font(.caption2)
                .foregroundStyle(.secondary)
            }

            HStack {
                Button(model.localized(.refreshHistogram)) {
                    model.startRefreshHistogram()
                }
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

                Button(model.localized(.previewAction)) {
                    model.startCurvePreview()
                }
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

                Button(model.localized(.applyCurve)) {
                    model.startApplyCurve()
                }
                .disabled(model.canProcessCurrentInput == false || model.isProcessing)

                Spacer()

                Button {
                    model.resetCurveEditor()
                    selectedCurvePointID = parameters.curveEditorPoints[1].id
                } label: {
                    Label(model.localized(.resetAction), systemImage: "arrow.counterclockwise")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.resetAction))
            }
            .buttonStyle(.bordered)

            Picker(
                model.localized(.curvePreset),
                selection: Binding(
                    get: { parameters.matchingCurvePreset },
                    set: { preset in
                        parameters.applyCurvePreset(preset)
                        model.startCurvePreview()
                    }
                )
            ) {
                ForEach(CurvePreset.allCases) { preset in
                    Text(model.curvePresetName(preset))
                        .tag(preset)
                }
            }
            .pickerStyle(.menu)

            HStack {
                Button {
                    model.saveCurrentCurvePreset()
                } label: {
                    Label(model.localized(.saveCurvePreset), systemImage: "plus")
                }

                Spacer()

                #if os(macOS)
                Button {
                    if let url = NativeFilePanels.openCurvePresetLibraryURL(
                        message: model.localized(.importCurvePresetsMessage)
                    ) {
                        model.importCustomCurvePresets(from: url)
                    }
                } label: {
                    Label(model.localized(.importCurvePresets), systemImage: "square.and.arrow.down")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.importCurvePresets))

                Button {
                    if let url = NativeFilePanels.exportCurvePresetLibraryURL(
                        message: model.localized(.exportCurvePresetsMessage)
                    ) {
                        model.exportCustomCurvePresets(to: url)
                    }
                } label: {
                    Label(model.localized(.exportCurvePresets), systemImage: "square.and.arrow.up")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.exportCurvePresets))
                .disabled(model.customCurvePresets.isEmpty)
                #endif
            }
            .buttonStyle(.bordered)

            if model.customCurvePresets.isEmpty == false {
                VStack(alignment: .leading, spacing: 6) {
                    Text(model.localized(.customCurvePresets))
                        .font(.caption)
                        .foregroundStyle(.secondary)

                    ForEach(model.customCurvePresets) { preset in
                        HStack(spacing: 6) {
                            TextField(
                                model.localized(.nameLabel),
                                text: Binding(
                                    get: { preset.name },
                                    set: { model.renameCustomCurvePreset(preset.id, name: $0) }
                                )
                            )
                            .textFieldStyle(.roundedBorder)
                            .font(.caption)

                            Button {
                                model.applyCustomCurvePreset(preset)
                            } label: {
                                Label(model.localized(.applyCurve), systemImage: "paintbrush")
                                    .labelStyle(.iconOnly)
                            }
                            .help(model.localized(.applyCurve))

                            Button(role: .destructive) {
                                model.deleteCustomCurvePreset(preset.id)
                            } label: {
                                Label(model.localized(.deleteOperation), systemImage: "trash")
                                    .labelStyle(.iconOnly)
                            }
                            .help(model.localized(.deleteOperation))
                        }
                    }
                }
            }
        }
    }
}

private struct CurveEditorView: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let snapshot: HistogramSnapshot?
    @Binding var parameters: ProcessingParameters
    @Binding var selectedPointID: CurveEditorPoint.ID?
    @State private var draftPoints: [CurveEditorPoint] = []
    @State private var isDraggingPoint = false

    var body: some View {
        GeometryReader { proxy in
            let size = proxy.size
            let points = currentPoints

            ZStack {
                RoundedRectangle(cornerRadius: 6)
                    .fill(.quaternary)
                    .overlay {
                        RoundedRectangle(cornerRadius: 6)
                            .stroke(.secondary.opacity(0.22), lineWidth: 1)
                    }
                    .contentShape(Rectangle())
                    .gesture(
                        SpatialTapGesture()
                            .onEnded { value in
                                addPoint(at: value.location, in: size)
                            }
                    )

                gridPath(in: size)
                    .stroke(.secondary.opacity(0.22), style: StrokeStyle(lineWidth: 1, dash: [3, 4]))

                if let snapshot, snapshot.bins.isEmpty == false {
                    let maximum = max(snapshot.bins.max() ?? 1, 1)
                    Path { path in
                        let width = size.width
                        let height = size.height
                        let step = width / CGFloat(max(snapshot.bins.count - 1, 1))

                        path.move(to: CGPoint(x: 0, y: height))
                        for index in snapshot.bins.indices {
                            let normalized = CGFloat(snapshot.bins[index]) / CGFloat(maximum)
                            let x = CGFloat(index) * step
                            let y = height - normalized * height
                            path.addLine(to: CGPoint(x: x, y: y))
                        }
                        path.addLine(to: CGPoint(x: width, y: height))
                        path.closeSubpath()
                    }
                    .fill(Color.accentColor.opacity(0.45))

                    Path { path in
                        let width = size.width
                        let height = size.height
                        let step = width / CGFloat(max(snapshot.bins.count - 1, 1))

                        for index in snapshot.bins.indices {
                            let normalized = CGFloat(snapshot.bins[index]) / CGFloat(maximum)
                            let x = CGFloat(index) * step
                            let y = height - normalized * height
                            if index == snapshot.bins.startIndex {
                                path.move(to: CGPoint(x: x, y: y))
                            } else {
                                path.addLine(to: CGPoint(x: x, y: y))
                            }
                        }
                    }
                    .stroke(Color.accentColor, lineWidth: 1.5)
                } else {
                    Text(model.language == .simplifiedChinese ? "直方图" : "Histogram")
                        .font(.caption)
                        .foregroundStyle(.secondary)
                }

                referencePath(in: size)
                    .stroke(.secondary.opacity(0.35), style: StrokeStyle(lineWidth: 1, dash: [4, 4]))

                curvePath(points: points, in: size)
                    .stroke(channelColor.opacity(0.95), style: StrokeStyle(lineWidth: 2.5, lineCap: .round, lineJoin: .round))

                ForEach(points) { point in
                    let selected = point.id == selectedPointID
                    Circle()
                        .fill(selected ? channelColor : Color.primary)
                        .frame(width: selected ? 15 : 12, height: selected ? 15 : 12)
                        .overlay {
                            Circle()
                                .stroke(Color.primary.opacity(selected ? 0.32 : 0.18), lineWidth: 2)
                        }
                        .shadow(radius: 2, y: 1)
                        .position(position(for: point, in: size))
                        .gesture(
                            DragGesture(minimumDistance: 0)
                                .onChanged { value in
                                    isDraggingPoint = true
                                    selectedPointID = point.id
                                    updateDraft(point.id, with: value.location, in: size)
                                    scheduleInteractivePreviewIfNeeded()
                                }
                                .onEnded { value in
                                    selectedPointID = point.id
                                    updateDraft(point.id, with: value.location, in: size)
                                    commitDraftPointsAndKeepPreview()
                                    isDraggingPoint = false
                                }
                        )
                }
            }
        }
        .onAppear {
            syncDraftPoints()
        }
        .onChange(of: parameters.curvePoints) { _, _ in
            guard isDraggingPoint == false else {
                return
            }
            syncDraftPoints()
        }
        .onDisappear {
            model.cancelCurvePreview()
        }
    }

    private var channelColor: Color {
        switch parameters.curveChannel {
        case .rgb:
            return .accentColor
        case .red:
            return .red
        case .green:
            return .green
        case .blue:
            return .blue
        case .luminance:
            return .primary
        }
    }

    private var currentPoints: [CurveEditorPoint] {
        normalizedDraftPoints(draftPoints.isEmpty ? parameters.curveEditorPoints : draftPoints)
    }

    private func syncDraftPoints() {
        draftPoints = parameters.curveEditorPoints
    }

    private func ensureDraftPoints() {
        if draftPoints.isEmpty {
            draftPoints = parameters.curveEditorPoints
        }
    }

    private func gridPath(in size: CGSize) -> Path {
        Path { path in
            for fraction in [0.25, 0.5, 0.75] {
                let x = size.width * CGFloat(fraction)
                path.move(to: CGPoint(x: x, y: 0))
                path.addLine(to: CGPoint(x: x, y: size.height))

                let y = size.height * CGFloat(fraction)
                path.move(to: CGPoint(x: 0, y: y))
                path.addLine(to: CGPoint(x: size.width, y: y))
            }
        }
    }

    private func referencePath(in size: CGSize) -> Path {
        Path { path in
            path.move(to: CGPoint(x: 0, y: size.height))
            path.addLine(to: CGPoint(x: size.width, y: 0))
        }
    }

    private func curvePath(points: [CurveEditorPoint], in size: CGSize) -> Path {
        Path { path in
            let samples = 96
            for sample in 0...samples {
                let input = Double(sample) / Double(samples)
                let output = curveOutput(for: input, points: points)
                let point = CGPoint(
                    x: CGFloat(input) * size.width,
                    y: (1.0 - CGFloat(output)) * size.height
                )
                if sample == 0 {
                    path.move(to: point)
                } else {
                    path.addLine(to: point)
                }
            }
        }
    }

    private func curveOutput(for input: Double, points: [CurveEditorPoint]) -> Double {
        sampleCurve(input, points: points)
    }

    private func position(for point: CurveEditorPoint, in size: CGSize) -> CGPoint {
        CGPoint(
            x: CGFloat(point.input) * size.width,
            y: (1.0 - CGFloat(point.output)) * size.height
        )
    }

    private func updateDraft(_ id: CurveEditorPoint.ID, with location: CGPoint, in size: CGSize) {
        ensureDraftPoints()
        let width = max(size.width, 1.0)
        let height = max(size.height, 1.0)
        let input = min(max(Double(location.x / width), 0.0), 1.0)
        let output = min(max(1.0 - Double(location.y / height), 0.0), 1.0)
        guard let index = draftPoints.firstIndex(where: { $0.id == id }) else {
            return
        }
        draftPoints[index].input = input
        draftPoints[index].output = output
    }

    private func scheduleInteractivePreviewIfNeeded() {
        let normalized = normalizedDraftPoints(draftPoints)
        guard normalized != parameters.curveEditorPoints else {
            return
        }
        model.scheduleCurvePreview(points: normalized)
    }

    private func commitDraftPointsAndKeepPreview() {
        let normalized = normalizedDraftPoints(draftPoints)
        guard normalized != parameters.curveEditorPoints else {
            draftPoints = parameters.curveEditorPoints
            return
        }
        parameters.replaceCurvePoints(normalized)
        draftPoints = parameters.curveEditorPoints
        // Dragging edits one pending curve against the session's original image.
        // The explicit Apply button is the only action that commits an operation.
        model.scheduleCurvePreview(points: parameters.curveEditorPoints)
    }

    private func addPoint(at location: CGPoint, in size: CGSize) {
        let width = max(size.width, 1.0)
        let height = max(size.height, 1.0)
        let input = min(max(Double(location.x / width), 0.0), 1.0)
        let output = min(max(1.0 - Double(location.y / height), 0.0), 1.0)
        parameters.addCurvePoint(input: input, output: output)
        draftPoints = parameters.curveEditorPoints
        selectedPointID = parameters.curveEditorPoints.min { left, right in
            squaredDistance(left, input: input, output: output) < squaredDistance(right, input: input, output: output)
        }?.id
    }

    private func normalizedDraftPoints(_ points: [CurveEditorPoint]) -> [CurveEditorPoint] {
        let source = points.count >= 2 ? points : parameters.curveEditorPoints
        var normalized = source.map { point in
            CurveEditorPoint(
                id: point.id,
                input: min(max(point.input, 0.0), 1.0),
                output: min(max(point.output, 0.0), 1.0)
            )
        }
        normalized.sort { left, right in
            if abs(left.input - right.input) < 0.000_001 {
                return left.id.uuidString < right.id.uuidString
            }
            return left.input < right.input
        }

        for index in normalized.indices.dropFirst() where normalized[index].input <= normalized[index - 1].input {
            normalized[index].input = min(normalized[index - 1].input + 0.01, 1.0)
        }
        for index in normalized.indices.dropLast().reversed() where normalized[index].input >= normalized[index + 1].input {
            normalized[index].input = max(normalized[index + 1].input - 0.01, 0.0)
        }
        return normalized
    }

    private func squaredDistance(_ point: CurveEditorPoint, input: Double, output: Double) -> Double {
        let dx = point.input - input
        let dy = point.output - output
        return dx * dx + dy * dy
    }
}

struct ParameterSlider: View {
    let title: String
    @Binding var value: Double
    let range: ClosedRange<Double>

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack {
                Text(title)
                Spacer()
                Text(value, format: .number.precision(.fractionLength(2)))
                    .foregroundStyle(.secondary)
            }
            Slider(value: $value, in: range)
        }
    }
}
