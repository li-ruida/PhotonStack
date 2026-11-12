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
    @State private var selectedTab: AdjustmentInspectorTab = .basic
    private var photoDraft: PhotoAdjustmentDraft { model.photoDraft }
    @State private var detailTool: DetailTool = .denoise
    @StateObject private var advanced = AdvancedToolSettings()

    private var chinese: Bool { model.language == .simplifiedChinese }

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack(spacing: 0) {
                ForEach(AdjustmentInspectorTab.allCases, id: \.self) { tab in
                    Button {
                        selectedTab = tab
                    } label: {
                        Text(tab.shortTitle(chinese: chinese))
                            .font(.system(size: 12, weight: selectedTab == tab ? .semibold : .regular))
                            .foregroundStyle(selectedTab == tab ? .primary : .secondary)
                            .frame(maxWidth: .infinity, minHeight: 28)
                            .contentShape(Rectangle())
                            .overlay(alignment: .bottom) {
                                if selectedTab == tab {
                                    Capsule().fill(Color.accentColor).frame(width: 22, height: 2)
                                }
                            }
                    }
                    .buttonStyle(.plain)
                    .help(tab.title(chinese: chinese))
                    .accessibilityLabel(tab.title(chinese: chinese))
                    .accessibilityAddTraits(selectedTab == tab ? .isSelected : [])
                    .accessibilityIdentifier("finish.tab." + tab.rawValue)
                }
            }
            .background(alignment: .bottom) { Divider() }

            if !model.canModifyProcessingConfiguration {
                Text(chinese ? "任务进行中，可浏览其他工具。" : "Processing. You can browse other tools.")
                    .font(.caption).foregroundStyle(.secondary)
            } else if model.editGraphNeedsReplay {
                Button(model.localized(.replayEditGraph)) { model.startReplayEditGraph() }
                    .disabled(!model.canReplayEditGraph)
            } else if !model.canProcessCurrentInput {
                Text(chinese ? "请选择可读取的图像或图像图层。" : "Select an available image or image layer.")
                    .font(.caption).foregroundStyle(.secondary)
            }
            if selectedTab != .basic && selectedTab != .composition {
                PhotoDraftNotice(draft: photoDraft, chinese: chinese) { selectedTab = .basic }
            }
            selectedContent
                .disabled(!model.canModifyProcessingConfiguration ||
                          (selectedTab != .export && (!model.canProcessCurrentInput || model.editGraphNeedsReplay)))
        }
        .controlSize(.small)

    }

    @ViewBuilder
    private var selectedContent: some View {
        switch selectedTab {
        case .basic, .composition:
            if let input = model.currentInputURL {
                PhotoAdjustmentEditor(model: model, input: input, draft: photoDraft, composition: selectedTab == .composition)
                    .id(input)
            }
        case .curves:
            HistogramCurvesSection(model: model, parameters: $parameters)
        case .detail:
            ParametersSection(parameters: $parameters, model: model, tool: $detailTool)
        case .export:
            ExportSettingsSection(model: model)
        case .advanced:
            AdvancedProcessingSection(model: model, parameters: $parameters, settings: advanced)
        }
    }
}

private enum AdjustmentInspectorTab: String, CaseIterable {
    case basic, curves, detail, composition, advanced, export

    func title(chinese: Bool) -> String {
        switch self {
        case .basic: chinese ? "明暗与色彩" : "Light & color"
        case .curves: chinese ? "直方图 / 曲线" : "Histogram / curves"
        case .detail: chinese ? "细节与星点" : "Detail & stars"
        case .composition: chinese ? "裁切与旋转" : "Crop & rotate"
        case .advanced: chinese ? "高级处理" : "Advanced"
        case .export: chinese ? "导出设置" : "Export settings"
        }
    }

    func shortTitle(chinese: Bool) -> String {
        switch self {
        case .basic: chinese ? "调色" : "Tone"
        case .curves: chinese ? "曲线" : "Curves"
        case .detail: chinese ? "细节" : "Detail"
        case .composition: chinese ? "构图" : "Crop"
        case .advanced: chinese ? "高级" : "Tools"
        case .export: chinese ? "导出" : "Export"
        }
    }
}

private enum DetailTool: String, CaseIterable {
    case denoise, sharpen, contrast, stretch, stars, coma

    var name: LocalizedTextKey {
        switch self {
        case .denoise: .denoiseAction
        case .sharpen: .sharpenAction
        case .contrast: .contrastAction
        case .stretch: .autoStretchAction
        case .stars: .starsAction
        case .coma: .comaAction
        }
    }
}

private struct ParametersSection: View {
    @Binding var parameters: ProcessingParameters
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var tool: DetailTool

    var body: some View {
        VStack(alignment: .leading, spacing: 12) {
            Picker(model.language == .simplifiedChinese ? "工具" : "Tool", selection: $tool) {
                ForEach(DetailTool.allCases, id: \.self) { Text(model.localized($0.name)).tag($0) }
            }
            switch tool {
            case .stretch:
                ParameterSlider(title: model.localized(.targetBackground), value: $parameters.stretchTargetBackground, range: 0.05...0.65)
                    .accessibilityIdentifier("parameters.stretch-target")
            case .denoise:
                ParameterSlider(title: model.localized(.denoiseParameter), value: $parameters.denoiseAmount, range: 0...1)
                ParameterSlider(title: model.localized(.chromaDenoiseParameter), value: $parameters.denoiseChromaAmount, range: 0...1)
                Stepper("\(model.localized(.denoiseRadius))  \(parameters.denoiseRadius)", value: $parameters.denoiseRadius, in: 1...8)
            case .sharpen:
                ParameterSlider(title: model.localized(.sharpenParameter), value: $parameters.sharpenAmount, range: 0...1)
                Stepper("\(model.localized(.sharpenRadius))  \(parameters.sharpenRadius)", value: $parameters.sharpenRadius, in: 1...8)
            case .contrast:
                ParameterSlider(title: model.localized(.localContrast), value: $parameters.localContrastAmount, range: 0...1)
                Stepper("\(model.localized(.contrastRadius))  \(parameters.localContrastRadius)", value: $parameters.localContrastRadius, in: 1...64)
            case .stars:
                ParameterSlider(title: model.localized(.starReduction), value: $parameters.starReductionAmount, range: 0...1)
                Toggle(model.localized(.profileAwareStars), isOn: $parameters.profileAwareStarReduction)
                Toggle(model.localized(.edgeAwareStars), isOn: $parameters.edgeAwareStarReduction)
            case .coma:
                ParameterSlider(title: model.localized(.comaReduction), value: $parameters.comaReductionAmount, range: 0...1)
                Stepper("\(model.localized(.comaRadius))  \(parameters.comaReductionRadius)", value: $parameters.comaReductionRadius, in: 2...16)
                ParameterSlider(title: model.localized(.comaEccentricity), value: $parameters.comaReductionEccentricity, range: 0.1...0.9)
                Toggle(model.localized(.edgeAwareComa), isOn: $parameters.edgeAwareComaReduction)
            }
            Button(model.language == .simplifiedChinese ? "应用" + model.localized(tool.name) : "Apply " + model.localized(tool.name)) {
                switch tool {
                case .stretch: model.startAutoStretchPreview()
                case .denoise: model.startDenoisePreview()
                case .sharpen: model.startSharpenPreview()
                case .contrast: model.startLocalContrastPreview()
                case .stars: model.startReduceStarsPreview()
                case .coma: model.startReduceComaPreview()
                }
            }
            .buttonStyle(.borderedProminent)
            .accessibilityIdentifier("finish.apply-detail")
            Divider()
            Stepper("\(model.localized(.previewWidth))  \(parameters.previewWidth) px", value: $parameters.previewWidth, in: 320...4096, step: 256)
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

            Picker(model.language == .simplifiedChinese ? "FITS 数值" : "FITS values", selection: Binding(
                get: { model.exportOptions.fitsValueMode }, set: { model.setFITSValueMode($0) }
            )) {
                Text(model.language == .simplifiedChinese ? "显示范围" : "Display range").tag(FITSValueMode.display)
                Text(model.language == .simplifiedChinese ? "科学数值" : "Scientific values").tag(FITSValueMode.scientific)
            }
            #if os(macOS)
            Button(model.localized(.exportAction)) {
                if let output = NativeFilePanels.exportImageURL(message: model.localized(.exportImageMessage)) {
                    model.startExportCurrentImage(to: output)
                }
            }
            .buttonStyle(.borderedProminent)
            .disabled(!model.canExportCurrentImage)
            #endif
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

                Group {
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

                }
                .allowsHitTesting(false)

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
                        .frame(width: 22, height: 22)
                        .contentShape(Rectangle())
                        .gesture(
                            DragGesture(minimumDistance: 0, coordinateSpace: .named("curve-editor"))
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
                        .position(position(for: point, in: size))
                }
            }
            .coordinateSpace(name: "curve-editor")
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
            let curve = WorkspaceSampledCurve(points: points)
            let samples = 96
            for sample in 0...samples {
                let input = Double(sample) / Double(samples)
                let output = curve.value(at: input)
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
        model.scheduleCurvePreview(points: normalized)
    }

    private func commitDraftPointsAndKeepPreview() {
        let normalized = normalizedDraftPoints(draftPoints)
        guard normalized != parameters.curveEditorPoints else {
            draftPoints = parameters.curveEditorPoints
            model.scheduleCurvePreview(points: normalized)
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
        model.startCurvePreview()
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

    var continuous = false
    var resetValue: Double? = nil
    var resetLabel = "Reset"
    var fractionLength: ClosedRange<Int> = 2...2
    @State private var dragValue: Double?
    @FocusState private var editingNumber: Bool
    @FocusState private var trackFocused: Bool
    @Environment(\.layoutDirection) private var direction

    var body: some View {
        HStack(spacing: 6) {
            Text(title)
                .font(.system(size: 11))
                .frame(width: 84, alignment: .leading)
                .fixedSize(horizontal: false, vertical: true)
            track
                .disabled(range.lowerBound == range.upperBound)

            TextField(title, value: Binding(
                get: { dragValue ?? value },
                set: { if $0.isFinite { value = min(max($0, range.lowerBound), range.upperBound) } }
            ), format: .number.precision(.fractionLength(fractionLength)))
                .labelsHidden()
                .textFieldStyle(.plain)
                .font(.system(size: 11).monospacedDigit())
                .multilineTextAlignment(.trailing)
                .focused($editingNumber)
                .padding(.horizontal, 3)
                .frame(width: 48, height: 22)
                .background(alignment: .bottom) {
                    Rectangle().fill(editingNumber ? Color.accentColor : Color.secondary.opacity(0.24))
                        .frame(height: 1)
                }
                .accessibilityLabel(title)
            if let resetValue {
                Button { value = resetValue } label: {
                    Image(systemName: "arrow.counterclockwise")
                        .font(.system(size: 10))
                        .frame(width: 18, height: 22)
                        .contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .foregroundStyle(.secondary)
                .accessibilityLabel("\(resetLabel) \(title)")
                .help("\(resetLabel) \(title)")
                .disabled(value == resetValue)
                .opacity(value == resetValue ? 0 : 1)
                .accessibilityHidden(value == resetValue)
            }
        }
        .frame(minHeight: 24)
    }
    private var track: some View {
        GeometryReader { geometry in
            let radius: CGFloat = 4.5
            let length = max(1, geometry.size.width - 2 * radius)
            let midY = geometry.size.height / 2
            let span = range.upperBound - range.lowerBound
            let position: (Double) -> CGFloat = { setting in
                let fraction = span > 0 ? min(1, max(0, (setting - range.lowerBound) / span)) : 0
                return radius + (direction == .rightToLeft ? 1 - fraction : fraction) * length
            }
            let thumb = position(dragValue ?? value)
            let origin = position(resetValue ?? range.lowerBound)
            let setting: (CGFloat) -> Double = { x in
                let fraction = min(1, max(0, (x - radius) / length))
                return range.lowerBound + (direction == .rightToLeft ? 1 - fraction : fraction) * span
            }

            ZStack {
                Capsule().fill(.secondary.opacity(0.32))
                    .frame(width: length, height: 2)
                Capsule().fill(Color.accentColor)
                    .frame(width: abs(thumb - origin), height: 2)
                    .position(x: (origin + thumb) / 2, y: midY)
                if let resetValue, range.lowerBound < resetValue, resetValue < range.upperBound {
                    Rectangle().fill(.secondary)
                        .frame(width: 1, height: 6)
                        .position(x: origin, y: midY)
                }
                Circle().fill(.primary)
                    .frame(width: 2 * radius, height: 2 * radius)
                    .position(x: thumb, y: midY)
            }
            .frame(width: geometry.size.width, height: geometry.size.height)
            .contentShape(Rectangle())
            .gesture(DragGesture(minimumDistance: 0, coordinateSpace: .local)
                .onChanged { gesture in
                    if dragValue == nil { trackFocused = true }
                    let next = setting(gesture.location.x)
                    dragValue = next
                    if continuous && value != next { value = next }
                }
                .onEnded { gesture in
                    let final = setting(gesture.location.x)
                    if value != final { value = final }
                    dragValue = nil
                })
        }
        .frame(height: 24)
        .focusable()
        .focused($trackFocused)
        .onKeyPress(keys: [.leftArrow, .rightArrow]) { event in
            let increases = (event.key == .rightArrow) == (direction == .leftToRight)
            let step = (range.upperBound - range.lowerBound) / 100
            value = min(range.upperBound, max(range.lowerBound, value + (increases ? step : -step)))
            return .handled
        }
        .accessibilityRepresentation {
            Slider(value: $value, in: range.lowerBound == range.upperBound ? range.lowerBound...(range.lowerBound + 1) : range) {
                Text(title)
            }
        }
    }

}
