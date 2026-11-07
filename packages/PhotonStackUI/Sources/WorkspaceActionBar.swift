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
            DeepSkyControl(model: model, context: "toolbar")
                .labelStyle(.iconOnly)

            AstroDevelopControl(model: model)
                .labelStyle(.titleAndIcon)
                .disabled(canAdjustCurrentInput == false)

            ResultComparisonControl(model: model)
                .labelStyle(.titleAndIcon)

            Button {
                model.startInspectSelectedAsset()
            } label: {
                Label(model.localized(.inspectAction), systemImage: "info.circle")
            }
            .disabled(canInspectAsset == false)
            .help(model.localized(.inspectAction))

            if model.latestDeepSkyResultURL == nil {
                Button {
                    model.startBuildPreview()
                } label: {
                    Label(model.localized(.sourcePreviewAction), systemImage: "photo")
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
                    Label(model.language == .simplifiedChinese ? "自动拉伸" : "Auto Stretch", systemImage: "sun.max")
                }
                .accessibilityIdentifier("adjustments.auto-stretch")
                .help(model.language == .simplifiedChinese ? "生成可撤销的拉伸图层，不改动原片；目标背景值在参数面板设置。" : "Create a reversible stretch layer without changing originals. Set target background in the parameters panel.")

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
                Label(model.localized(.inspectorAdjustmentsGroup), systemImage: "slider.horizontal.3")
            }
            .help(model.localized(.inspectorAdjustmentsGroup))
            .disabled(canAdjustCurrentInput == false)

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

                Button(role: .cancel) {
                    model.cancelCurrentTask()
                } label: {
                    Label(model.localized(.cancelAction), systemImage: "xmark.circle")
                        .labelStyle(.iconOnly)
                }
                .help(model.localized(.cancelAction))
                .disabled(model.canCancel == false)

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
        .labelStyle(.iconOnly)
        .buttonStyle(.bordered)
        .controlSize(.regular)
        .padding(12)
        .background(.bar)
    }
}
