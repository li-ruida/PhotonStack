import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct ActionBar: View {
    @ObservedObject var model: PhotonStackWorkspaceModel

    var body: some View {
        let canInspectAsset = model.selectedAsset != nil &&
            model.selectedAssetIsMissing == false &&
            model.isProcessing == false
        let canAdjustCurrentInput = model.canProcessCurrentInput &&
            model.isProcessing == false &&
            model.editGraphNeedsReplay == false

        HStack(spacing: 8) {
            ResultComparisonControl(model: model)
                .labelStyle(.titleAndIcon)

            Button {
                model.startInspectSelectedAsset()
            } label: {
                Label(model.localized(.inspectAction), systemImage: "info.circle")
                    .labelStyle(.titleAndIcon)
            }
            .disabled(canInspectAsset == false)
            .help(model.localized(.inspectAction))

            if model.latestDeepSkyResultURL == nil {
                Button {
                    model.startBuildPreview()
                } label: {
                    Label(model.localized(.sourcePreviewAction), systemImage: "photo")
                        .labelStyle(.titleAndIcon)
                }
                .accessibilityLabel(
                    "\(model.localized(.sourcePreviewAction)): \(model.selectedAsset?.displayName ?? model.localized(.sourcePreviewAction))"
                )
                .disabled(canInspectAsset == false)
                .help(model.localized(.sourcePreviewAction))
            }

            Menu {
                Button {
                    model.startAutoStretchPreview()
                } label: {
                    Label(model.localized(.autoStretchAction), systemImage: "sun.max")
                }
                .accessibilityIdentifier("adjustments.auto-stretch")
                .help(model.localized(.autoStretchHelp))

                Button {
                    model.startLocalContrastPreview()
                } label: {
                    Label(model.localized(.contrastAction), systemImage: "circle.lefthalf.filled")
                }

                Button {
                    model.startDenoisePreview()
                } label: {
                    Label(model.localized(.denoiseAction), systemImage: "wand.and.stars")
                }

                Button {
                    model.startSharpenPreview()
                } label: {
                    Label(model.localized(.sharpenAction), systemImage: "scope")
                }

                Divider()

                Button {
                    model.startReduceStarsPreview()
                } label: {
                    Label(model.localized(.starsAction), systemImage: "sparkles")
                }

                Button {
                    model.startReduceComaPreview()
                } label: {
                    Label(model.localized(.comaAction), systemImage: "oval.portrait")
                }
            } label: {
                Label(model.localized(.quickAdjustments), systemImage: "slider.horizontal.3")
                    .labelStyle(.titleAndIcon)
            }
            .help(model.localized(.quickAdjustments))
            .disabled(canAdjustCurrentInput == false)
            .fixedSize()

            Spacer()

            ControlGroup {
                Button {
                    model.stepBackPreview()
                } label: {
                    Label(model.localized(.backAction), systemImage: "arrow.uturn.backward")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.backAction))
                .disabled(model.canStepBack == false || model.isProcessing)

                Button {
                    model.resetPreviewToOriginal()
                } label: {
                    Label(model.localized(.resetAction), systemImage: "arrow.counterclockwise")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.resetAction))
                .disabled(model.canResetPreviewToOriginal == false || model.isProcessing)

                if model.canCancel {
                    Button(role: .cancel) {
                        model.cancelCurrentTask()
                    } label: {
                        Label(model.localized(.cancelAction), systemImage: "xmark.circle")
                            .labelStyle(.iconOnly)
                    }
                    .help(model.localized(.cancelAction))
                }

                #if os(macOS)
                Button {
                    if let output = NativeFilePanels.exportImageURL(
                        message: model.localized(.exportImageMessage)
                    ) {
                        model.startExportCurrentImage(to: output)
                    }
                } label: {
                    Label(model.localized(.exportAction), systemImage: "square.and.arrow.up")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(model.editGraphNeedsReplay ? .editGraphNeedsReplay : .exportAction))
                .disabled(model.canExportCurrentImage == false)
                #endif
            }
        }
        .buttonStyle(.bordered)
        .controlSize(.regular)
        .padding(12)
        .background(.bar)
    }
}
