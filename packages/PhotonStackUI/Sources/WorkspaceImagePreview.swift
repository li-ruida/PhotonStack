import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

struct PhotonStackPreview: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @StateObject private var scientificPreviews = DeepSkyReviewPreviewStore()
    @State private var magnification = 1.0
    @State private var fitToWindow = true
    @State private var showArtifactMarkedPreview = false

    var body: some View {
        VStack(spacing: 0) {
            if model.latestDeepSkyResultURL != nil {
                WorkspacePreviewSourceControl(model: model, imageURL: currentURL)
            }
            PreviewZoomBar(
                model: model,
                magnification: $magnification,
                fitToWindow: $fitToWindow,
                showArtifactMarkedPreview: $showArtifactMarkedPreview,
                hasArtifactMarkedPreview: model.artifactMarkedPreviewURL != nil,
                imageURL: currentURL
            )

            ZStack {
                Color.black

                if let url = currentURL {
                    if ["fit", "fits", "fts"].contains(url.pathExtension.lowercased()) {
                        WorkspaceFITSPreview(model: model, previews: scientificPreviews, input: url,
                            magnification: $magnification, fitToWindow: $fitToWindow)
                            .padding(16)
                    } else {
                        PlatformImageView(url: url, preview: model.photoPreview, edits: model.photoDraft, input: model.currentInputURL, allowsDraft: !showArtifactMarkedPreview && model.interactiveCurveURL == nil, magnification: $magnification, fitToWindow: $fitToWindow)
                            .padding(16)
                    }
                } else {
                    ContentUnavailableView(model.localized(.noImage), systemImage: "photo")
                }
            }
            .onReceive(model.photoDraft.$picking) { if $0 { showArtifactMarkedPreview = false } }
            .onChange(of: model.selectedAssetID) { _, _ in
                magnification = 1.0
                fitToWindow = true
                showArtifactMarkedPreview = false
            }
            .onChange(of: model.previewURL) { oldValue, newValue in
                guard oldValue != newValue, newValue != nil else {
                    return
                }
                magnification = 1.0
                fitToWindow = true
            }
            .onChange(of: model.artifactMarkedPreviewURL) { _, url in
                showArtifactMarkedPreview = url != nil
            }
        }
    }

    private var currentURL: URL? {
        if showArtifactMarkedPreview, let markedPreview = model.artifactMarkedPreviewURL {
            return markedPreview
        }
        return model.interactiveCurveURL ?? model.previewURL ?? model.selectedAsset?.originalURL
    }
}

private struct PreviewZoomBar: View {
    @ObservedObject var model: PhotonStackWorkspaceModel
    @Binding var magnification: Double
    @Binding var fitToWindow: Bool
    @Binding var showArtifactMarkedPreview: Bool
    let hasArtifactMarkedPreview: Bool
    let imageURL: URL?

    var body: some View {
        HStack(spacing: 8) {
            Button {
                fitToWindow = true
            } label: {
                Label(model.localized(.fitToWindow), systemImage: "arrow.up.left.and.arrow.down.right")
                    .labelStyle(.iconOnly)
            }
            .help(model.localized(.fitToWindow))

            Button {
                fitToWindow = false
                magnification = 1.0
            } label: {
                Text(model.localized(.actualSize))
                    .font(.caption)
                    .frame(minWidth: 34)
            }
            .help(model.localized(.actualSize))

            Divider()
                .frame(height: 18)

            Button {
                fitToWindow = false
                magnification = max(0.1, magnification / 1.25)
            } label: {
                Label(model.localized(.zoomOut), systemImage: "minus.magnifyingglass")
                    .labelStyle(.iconOnly)
            }
            .help(model.localized(.zoomOut))

            Slider(
                value: Binding(
                    get: { magnification },
                    set: { value in
                        fitToWindow = false
                        magnification = value
                    }
                ),
                in: 0.1...8.0
            )
            .frame(minWidth: 70, idealWidth: 120, maxWidth: 150)
            .accessibilityLabel(model.language == .simplifiedChinese ? "缩放比例" : "Zoom level")

            Button {
                fitToWindow = false
                magnification = min(8.0, magnification * 1.25)
            } label: {
                Label(model.localized(.zoomIn), systemImage: "plus.magnifyingglass")
                    .labelStyle(.iconOnly)
            }
            .help(model.localized(.zoomIn))

            Text("\(Int((magnification * 100).rounded()))%")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)
                .frame(width: 48, alignment: .trailing)

            if hasArtifactMarkedPreview {
                Divider()
                    .frame(height: 18)

                Picker("", selection: $showArtifactMarkedPreview) {
                    Text(model.localized(.originalPreview)).tag(false)
                    Text(model.localized(.markedPreview)).tag(true)
                }
                .pickerStyle(.menu)
                .frame(maxWidth: 128)
                .help(model.localized(.artifactMarkedPreview))
            }

            Spacer()

            if model.latestDeepSkyResultURL == nil, let imageURL {
                Text(imageURL.lastPathComponent)
                    .font(.caption)
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
                    .truncationMode(.middle)
                    .help(imageURL.path)
                    .accessibilityLabel("\(model.localized(.currentImageLabel)): \(imageURL.lastPathComponent)")
                    .accessibilityIdentifier("workspace.current-image")
            }
        }
        .buttonStyle(.bordered)
        .padding(.horizontal, 12)
        .padding(.vertical, 8)
        .background(.bar)
        .disabled(imageURL == nil)
    }
}

private struct PlatformImageView: View {
    let url: URL
    @ObservedObject var preview: PhotoAdjustmentPreview
    @ObservedObject var edits: PhotoAdjustmentDraft
    let input: URL?
    let allowsDraft: Bool
    private var draft: PhotoAdjustmentPreview.Frame? {
        guard allowsDraft, let frame = preview.frame, frame.input == input else { return nil }
        return frame
    }
    @Binding var magnification: Double
    @Binding var fitToWindow: Bool

    private var samplePoint: ((CGPoint) -> Void)? {
        guard edits.picking, let draft, draft.settings == edits.settings else { return nil }
        return { point in edits.sample(input: draft.input, point: point, displayed: draft.settings) }
    }

    var body: some View {
        #if os(macOS)
        NativeImagePreview(url: url, magnification: $magnification, fitToWindow: $fitToWindow, decodedImage: draft?.image, decodedSize: draft?.sourceSize, samplePoint: samplePoint)
        #else
        if let draft {
            Image(decorative: draft.image, scale: 1).resizable().scaledToFit()
                .overlay {
                    if let samplePoint {
                        GeometryReader { geometry in
                            Color.clear.contentShape(Rectangle()).onTapGesture { point in
                                samplePoint(CGPoint(x: point.x / geometry.size.width, y: point.y / geometry.size.height))
                            }
                        }
                    }
                }
        } else if let data = try? Data(contentsOf: url), let image = UIImage(data: data) {
            Image(uiImage: image)
                .resizable()
                .scaledToFit()
        } else {
            ContentUnavailableView(url.lastPathComponent, systemImage: "exclamationmark.triangle")
        }
        #endif
    }
}
