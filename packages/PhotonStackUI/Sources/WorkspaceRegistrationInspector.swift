import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct RegistrationSection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @State private var referenceID: PhotonStackAsset.ID?
    @State private var movingID: PhotonStackAsset.ID?
    @State private var alignment: AlignmentMethod = .distortion

    var body: some View {
        let assets = model.batchRegistrationAssets.isEmpty ? model.project.assets : model.batchRegistrationAssets
        let reference = selectedAsset(referenceID, in: assets) ?? assets.first
        let moving = selectedAsset(movingID, in: assets) ?? defaultMovingAsset(in: assets, reference: reference)
        let batchMovingCandidates = model.batchRegistrationAssets.filter { $0.id != reference?.id }
        let selectedBatchMovingCount = model.batchRegistrationSelectedMovingCount(reference: reference)

        VStack(alignment: .leading, spacing: 10) {
            Text(model.localized(.registrationSection))
                .font(.headline)

            if assets.count < 2 {
                Text(model.localized(.registrationNeedsTwoFrames))
                    .foregroundStyle(.secondary)
            } else {
                Picker(model.localized(.referenceFrame), selection: referenceBinding(assets: assets)) {
                    ForEach(assets) { asset in
                        Text(asset.displayName)
                            .tag(Optional(asset.id))
                    }
                }
                .pickerStyle(.menu)

                Picker(model.localized(.movingFrame), selection: movingBinding(assets: assets)) {
                    ForEach(assets) { asset in
                        Text(asset.displayName)
                            .tag(Optional(asset.id))
                    }
                }
                .pickerStyle(.menu)

                Picker(model.localized(.alignLabel), selection: $alignment) {
                    Text(model.alignmentMethodName(.translation)).tag(AlignmentMethod.translation)
                    Text(model.alignmentMethodName(.similarity)).tag(AlignmentMethod.similarity)
                    Text(model.alignmentMethodName(.affine)).tag(AlignmentMethod.affine)
                    Text(model.alignmentMethodName(.distortion)).tag(AlignmentMethod.distortion)
                }
                .pickerStyle(.segmented)

                Button {
                    if let reference, let moving {
                        model.startRegister(reference: reference, moving: moving, alignment: alignment)
                    }
                } label: {
                    Label(model.localized(.registerFrames), systemImage: "scope")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canRegister(reference: reference, moving: moving) == false || model.isProcessing)

                Button {
                    if let reference {
                        model.startRegisterBatch(reference: reference, alignment: alignment)
                    }
                } label: {
                    Label(model.localized(.registerBatchFrames), systemImage: "rectangle.stack")
                        .frame(maxWidth: .infinity)
                }
                .buttonStyle(.bordered)
                .disabled(model.canRegisterBatch(reference: reference) == false || model.isProcessing)

                BatchRegistrationFrameSelection(
                    model: model,
                    reference: reference,
                    movingCandidates: batchMovingCandidates,
                    selectedCount: selectedBatchMovingCount
                )

                if let report = model.registrationReport {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(model.localized(.registrationReport))
                            .font(.caption)
                            .foregroundStyle(.secondary)
                        LabeledContent(model.localized(.registrationMatches), value: "\(report.matches)")
                        LabeledContent(model.localized(.registrationOffset), value: String(format: "%.2f, %.2f", report.dx, report.dy))
                        LabeledContent(model.localized(.registrationScale), value: String(format: "%.4f", report.scale))
                        LabeledContent(model.localized(.registrationRotation), value: String(format: "%.3f°", report.rotationDegrees))
                    }
                    .font(.caption)
                }
            }
        }
        .disabled(model.canModifyProcessingConfiguration == false)
        .onAppear {
            initializeSelection(from: assets)
        }
        .onChange(of: assets.map(\.id)) { _, _ in
            initializeSelection(from: assets)
        }
    }

    private func referenceBinding(assets: [PhotonStackAsset]) -> Binding<PhotonStackAsset.ID?> {
        Binding(
            get: { referenceID ?? assets.first?.id },
            set: { newValue in
                referenceID = newValue
                if movingID == newValue {
                    movingID = assets.first { $0.id != newValue }?.id
                }
            }
        )
    }

    private func movingBinding(assets: [PhotonStackAsset]) -> Binding<PhotonStackAsset.ID?> {
        Binding(
            get: { movingID ?? assets.dropFirst().first?.id ?? assets.first?.id },
            set: { newValue in
                movingID = newValue
                if referenceID == newValue {
                    referenceID = assets.first { $0.id != newValue }?.id
                }
            }
        )
    }

    private func selectedAsset(_ id: PhotonStackAsset.ID?, in assets: [PhotonStackAsset]) -> PhotonStackAsset? {
        guard let id else {
            return nil
        }
        return assets.first { $0.id == id }
    }

    private func defaultMovingAsset(in assets: [PhotonStackAsset], reference: PhotonStackAsset?) -> PhotonStackAsset? {
        assets.first { $0.id != reference?.id }
    }

    private func initializeSelection(from assets: [PhotonStackAsset]) {
        guard assets.isEmpty == false else {
            referenceID = nil
            movingID = nil
            return
        }
        if referenceID == nil || selectedAsset(referenceID, in: assets) == nil {
            referenceID = assets.first?.id
        }
        if movingID == nil || selectedAsset(movingID, in: assets) == nil || movingID == referenceID {
            movingID = assets.first { $0.id != referenceID }?.id
        }
    }
}

private struct BatchRegistrationFrameSelection: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    let reference: PhotonStackAsset?
    let movingCandidates: [PhotonStackAsset]
    let selectedCount: Int

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                Text(model.localized(.batchRegistrationFrames))
                    .font(.caption)
                    .foregroundStyle(.secondary)

                Spacer()

                Text("\(selectedCount)/\(movingCandidates.count)")
                    .font(.caption.monospacedDigit())
                    .foregroundStyle(.secondary)
            }

            HStack(spacing: 8) {
                Button(model.localized(.selectAllFrames)) {
                    model.selectAllBatchRegistrationFrames()
                }
                .disabled(movingCandidates.isEmpty || model.isProcessing)

                Button(model.localized(.clearFrameSelection)) {
                    model.clearBatchRegistrationFrames()
                }
                .disabled(movingCandidates.isEmpty || model.isProcessing)

                if let selectedAsset = model.selectedAsset,
                   selectedAsset.id != reference?.id,
                   movingCandidates.contains(where: { $0.id == selectedAsset.id }) {
                    Button(model.localized(.onlyCurrentFrame)) {
                        model.selectOnlyBatchRegistrationFrame(selectedAsset)
                    }
                    .disabled(model.isProcessing)
                }
            }
            .buttonStyle(.bordered)
            .font(.caption)

            if movingCandidates.isEmpty {
                Text(model.localized(.registrationNeedsTwoFrames))
                    .font(.caption)
                    .foregroundStyle(.secondary)
            } else {
                ScrollView {
                    VStack(alignment: .leading, spacing: 6) {
                        ForEach(movingCandidates) { asset in
                            Toggle(
                                isOn: Binding(
                                    get: { model.isBatchRegistrationFrameSelected(asset) },
                                    set: { model.setBatchRegistrationFrame(asset, isSelected: $0) }
                                )
                            ) {
                                VStack(alignment: .leading, spacing: 2) {
                                    Text(asset.displayName)
                                        .lineLimit(1)
                                    Text(asset.originalURL.deletingLastPathComponent().path)
                                        .font(.caption2)
                                        .foregroundStyle(.secondary)
                                        .lineLimit(1)
                                        .truncationMode(.middle)
                                }
                            }
                            .toggleStyle(.checkbox)
                            .disabled(model.isProcessing)
                        }
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(maxHeight: 128)

                if selectedCount == 0 {
                    Text(model.localized(.registrationNoFramesSelected))
                        .font(.caption2)
                        .foregroundStyle(.red)
                }
            }
        }
    }
}
