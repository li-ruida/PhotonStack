import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct EditGraphSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(model.localized(.editGraphSection))
                    .font(.headline)

                Spacer()

                Button {
                    model.undoEditGraphChange()
                    replayIfNeeded()
                } label: {
                    Label(model.localized(.undoEditGraph), systemImage: "arrow.uturn.backward")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .keyboardShortcut("z", modifiers: [.command])
                .help(model.localized(.undoEditGraph))
                .disabled(model.canUndoEditGraph == false || model.isProcessing)

                Button {
                    model.redoEditGraphChange()
                    replayIfNeeded()
                } label: {
                    Label(model.localized(.redoEditGraph), systemImage: "arrow.uturn.forward")
                        .labelStyle(.iconOnly)
                }
                .buttonStyle(.borderless)
                .keyboardShortcut("z", modifiers: [.command, .shift])
                .help(model.localized(.redoEditGraph))
                .disabled(model.canRedoEditGraph == false || model.isProcessing)

                Button {
                    model.startReplayEditGraph()
                } label: {
                    Label(model.localized(.replayEditGraph), systemImage: "play.fill")
                        .labelStyle(.iconOnly)
                        .foregroundStyle(model.editGraphNeedsReplay ? Color.accentColor : Color.primary)
                }
                .buttonStyle(.borderless)
                .help(model.localized(model.editGraphNeedsReplay ? .editGraphNeedsReplay : .replayEditGraph))
                .disabled(model.canReplayEditGraph == false)
            }

            if model.project.editGraph.operations.isEmpty {
                Text(model.localized(.noEditOperations))
                    .foregroundStyle(.secondary)
            } else {
                ForEach(model.project.editGraph.operations) { operation in
                    VStack(alignment: .leading, spacing: 6) {
                        HStack(spacing: 8) {
                            Button {
                                model.toggleEditOperation(operation.id)
                                replayIfNeeded()
                            } label: {
                                Image(systemName: operation.isEnabled ? "checkmark.circle" : "circle")
                                    .foregroundStyle(operation.isEnabled ? .green : .secondary)
                                    .frame(width: 18)
                            }
                            .buttonStyle(.plain)
                            .disabled(model.isProcessing)

                            VStack(alignment: .leading, spacing: 2) {
                                Text(operation.parameters["mode"] == "display-grid-v1"
                                    ? (model.language == .simplifiedChinese ? "细纹抑制" : "Grid reduction")
                                    : model.language.processingOperationName(operation.kind))
                                    .lineLimit(1)
                                if let output = operation.parameters["output"] {
                                    Text(URL(fileURLWithPath: output).lastPathComponent)
                                        .font(.caption2)
                                        .foregroundStyle(.secondary)
                                        .lineLimit(1)
                                }
                            }

                            Spacer()

                            HStack(spacing: 4) {
                                Button {
                                    model.moveEditOperation(operation.id, direction: .up)
                                    replayIfNeeded()
                                } label: {
                                    Label(model.localized(.moveOperationUp), systemImage: "chevron.up")
                                        .labelStyle(.iconOnly)
                                }
                                .disabled(model.isProcessing || model.canMoveEditOperation(operation.id, direction: .up) == false)

                                Button {
                                    model.moveEditOperation(operation.id, direction: .down)
                                    replayIfNeeded()
                                } label: {
                                    Label(model.localized(.moveOperationDown), systemImage: "chevron.down")
                                        .labelStyle(.iconOnly)
                                }
                                .disabled(model.isProcessing || model.canMoveEditOperation(operation.id, direction: .down) == false)

                                Button(role: .destructive) {
                                    model.deleteEditOperation(operation.id)
                                    replayIfNeeded()
                                } label: {
                                    Label(model.localized(.deleteOperation), systemImage: "trash")
                                        .labelStyle(.iconOnly)
                                }
                                .disabled(model.isProcessing)
                            }
                            .buttonStyle(.borderless)
                        }

                        let editableParameters = model.editableEditOperationParameters(for: operation)
                        if let curveParameters = model.curveParameters(for: operation) {
                            CurveNodePreviewView(points: curveParameters.curveEditorPoints)
                                .frame(height: 54)
                                .padding(.leading, 26)

                            if let presetName = curvePresetName(for: operation) {
                                LabeledContent(model.localized(.curvePreset), value: presetName)
                                    .font(.caption2)
                                    .foregroundStyle(.secondary)
                                    .padding(.leading, 26)
                            }
                        }

                        if editableParameters.isEmpty == false {
                            LazyVGrid(columns: [GridItem(.adaptive(minimum: 88), spacing: 6)], alignment: .leading, spacing: 6) {
                                ForEach(editableParameters) { parameter in
                                    EditOperationParameterControl(
                                        model: model,
                                        operationID: operation.id,
                                        parameter: parameter,
                                        onCommit: replayIfNeeded
                                    )
                                }
                            }
                            .padding(.leading, 26)
                        }
                    }
                    .font(.caption)
                    .padding(.vertical, 3)
                }
            }
        }
        .disabled(model.canModifyWorkspaceProducts == false)
    }

    private func replayIfNeeded() {
        if model.editGraphNeedsReplay {
            model.startReplayEditGraph()
        }
    }

    private func curvePresetName(for operation: EditOperation) -> String? {
        model.editOperationPresetName(operation)
    }
}

private struct EditOperationParameterControl: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let operationID: EditOperation.ID
    let parameter: EditableEditOperationParameter
    let onCommit: () -> Void

    @State private var draft: String
    @FocusState private var isFocused: Bool

    init(
        model: PhotonStackWorkspaceModel,
        operationID: EditOperation.ID,
        parameter: EditableEditOperationParameter,
        onCommit: @escaping () -> Void
    ) {
        self.model = model
        self.operationID = operationID
        self.parameter = parameter
        self.onCommit = onCommit
        _draft = State(initialValue: parameter.value)
    }

    @ViewBuilder
    var body: some View {
        switch parameter.editor {
        case .toggle:
            Toggle(parameter.key, isOn: toggleBinding)
                .toggleStyle(.checkbox)
                .font(.caption2)
        case let .options(values):
            Picker(parameter.key, selection: optionBinding) {
                ForEach(values, id: \.self) { value in
                    Text(value).tag(value)
                }
            }
            .pickerStyle(.menu)
            .font(.caption2)
        case .decimal, .integer, .text:
            TextField(parameter.key, text: $draft)
                .textFieldStyle(.roundedBorder)
                .font(.caption2.monospacedDigit())
                .focused($isFocused)
                .onSubmit(commitDraft)
                .onChange(of: isFocused) { wasFocused, isFocused in
                    if wasFocused && isFocused == false {
                        commitDraft()
                    }
                }
                .onChange(of: parameter.value) { _, value in
                    if isFocused == false {
                        draft = value
                    }
                }
        }
    }

    private var toggleBinding: Binding<Bool> {
        Binding(
            get: { model.parameterValue(operationID, key: parameter.key) == "true" },
            set: { commit(String($0)) }
        )
    }

    private var optionBinding: Binding<String> {
        Binding(
            get: { model.parameterValue(operationID, key: parameter.key) },
            set: { value in
                commit(value)
            }
        )
    }

    private func commitDraft() {
        commit(draft)
        draft = model.parameterValue(operationID, key: parameter.key)
    }

    private func commit(_ value: String) {
        let previousValue = model.parameterValue(operationID, key: parameter.key)
        if model.updateEditOperationParameter(operationID, key: parameter.key, value: value),
           model.parameterValue(operationID, key: parameter.key) != previousValue {
            onCommit()
        }
    }
}

private struct CurveNodePreviewView: View {
    let points: [CurveEditorPoint]

    var body: some View {
        GeometryReader { proxy in
            let size = proxy.size

            ZStack {
                RoundedRectangle(cornerRadius: 6)
                    .fill(.quaternary.opacity(0.7))

                Path { path in
                    path.move(to: CGPoint(x: 0, y: size.height))
                    path.addLine(to: CGPoint(x: size.width, y: 0))
                }
                .stroke(.secondary.opacity(0.35), style: StrokeStyle(lineWidth: 1, dash: [3, 4]))

                curvePath(in: size)
                    .stroke(Color.accentColor, style: StrokeStyle(lineWidth: 2, lineCap: .round, lineJoin: .round))

                ForEach(points) { point in
                    Circle()
                        .fill(Color.accentColor)
                        .frame(width: 8, height: 8)
                        .position(position(for: point, in: size))
                }
            }
        }
    }

    private func curvePath(in size: CGSize) -> Path {
        Path { path in
            let curve = WorkspaceSampledCurve(points: points)
            let samples = 48
            for sample in 0...samples {
                let input = Double(sample) / Double(samples)
                let output = curve.value(at: input)
                let point = CGPoint(x: CGFloat(input) * size.width, y: (1.0 - CGFloat(output)) * size.height)
                if sample == 0 {
                    path.move(to: point)
                } else {
                    path.addLine(to: point)
                }
            }
        }
    }

    private func position(for point: CurveEditorPoint, in size: CGSize) -> CGPoint {
        CGPoint(x: CGFloat(point.input) * size.width, y: (1.0 - CGFloat(point.output)) * size.height)
    }
}
