#if os(macOS)
import AppKit
import CoreImage
import ImageIO
import SwiftUI

enum NativePreviewImageDecoder {
    static func decodeCGImage(contentsOf url: URL) -> CGImage? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else {
            return nil
        }

        let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any]
        let width = (properties?[kCGImagePropertyPixelWidth] as? NSNumber)?.intValue ?? 0
        let height = (properties?[kCGImagePropertyPixelHeight] as? NSNumber)?.intValue ?? 0
        let maximumDimension = max(width, height)
        if maximumDimension > 0 {
            let thumbnailOptions = [
                kCGImageSourceCreateThumbnailFromImageAlways: true,
                kCGImageSourceCreateThumbnailWithTransform: true,
                kCGImageSourceThumbnailMaxPixelSize: maximumDimension,
                kCGImageSourceShouldCache: true,
                kCGImageSourceShouldCacheImmediately: true,
            ] as CFDictionary
            if let image = CGImageSourceCreateThumbnailAtIndex(source, 0, thumbnailOptions) {
                return displayImage(from: image)
            }
        }

        guard let image = CGImageSourceCreateImageAtIndex(source, 0, [
            kCGImageSourceShouldCache: true,
            kCGImageSourceShouldCacheImmediately: true,
        ] as CFDictionary) else {
            return nil
        }
        return displayImage(from: image)
    }

    private static func displayImage(from image: CGImage) -> CGImage? {
        guard image.bitsPerComponent > 8 else {
            return image
        }
        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
            return nil
        }
        let input = CIImage(cgImage: image)
        return CIContext(options: [.cacheIntermediates: false]).createCGImage(
            input,
            from: input.extent,
            format: .RGBA8,
            colorSpace: colorSpace
        )
    }
}

struct NativeImagePreview: NSViewRepresentable {
    let url: URL
    @Binding var magnification: Double
    @Binding var fitToWindow: Bool

    func makeNSView(context: Context) -> NSScrollView {
        let imageView = NSImageView()
        imageView.imageScaling = .scaleProportionallyUpOrDown
        imageView.imageAlignment = .alignCenter
        imageView.wantsLayer = true
        imageView.layerContentsRedrawPolicy = .onSetNeedsDisplay

        let scrollView = NSScrollView()
        scrollView.contentView = CenteringClipView()
        scrollView.drawsBackground = true
        scrollView.backgroundColor = .black
        scrollView.hasHorizontalScroller = true
        scrollView.hasVerticalScroller = true
        scrollView.autohidesScrollers = true
        scrollView.allowsMagnification = true
        scrollView.minMagnification = 0.1
        scrollView.maxMagnification = 8.0
        scrollView.magnification = 1.0
        scrollView.documentView = imageView

        context.coordinator.imageView = imageView
        context.coordinator.scrollView = scrollView
        return scrollView
    }

    func updateNSView(_ scrollView: NSScrollView, context: Context) {
        if context.coordinator.url != url {
            context.coordinator.updateImage(url)
        }

        context.coordinator.applyZoom(
            magnification: magnification,
            fitToWindow: fitToWindow,
            updateMagnification: { value in
                self.magnification = value
            }
        )
        if fitToWindow {
            context.coordinator.scheduleDeferredFitZoom(
                magnification: magnification,
                updateMagnification: { value in
                    self.magnification = value
                }
            )
        }
    }

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    final class Coordinator {
        weak var imageView: NSImageView?
        weak var scrollView: NSScrollView?
        var url: URL?
        var naturalImageSize: CGSize = .zero
        private var deferredFitZoomScheduled = false

        @MainActor
        func updateImage(_ newURL: URL) {
            guard let image = Self.decodedImage(contentsOf: newURL) else {
                return
            }

            url = newURL
            setImage(image)
        }

        @MainActor
        func setImage(_ image: NSImage) {
            let previousVisibleRect = scrollView?.contentView.bounds
            let previousSize = naturalImageSize
            let nextSize = image.size
            let sizeChanged = abs(previousSize.width - nextSize.width) > 0.5 || abs(previousSize.height - nextSize.height) > 0.5

            NSAnimationContext.runAnimationGroup { context in
                context.duration = 0
                context.allowsImplicitAnimation = false
                imageView?.image = image
            }
            naturalImageSize = nextSize

            if sizeChanged || imageView?.frame.size == .zero {
                let width = max(naturalImageSize.width, 1)
                let height = max(naturalImageSize.height, 1)
                imageView?.frame = CGRect(x: 0, y: 0, width: width, height: height)
                if let previousVisibleRect {
                    scrollView?.contentView.scroll(to: previousVisibleRect.origin)
                    scrollView?.reflectScrolledClipView(scrollView?.contentView ?? NSClipView())
                }
            }
            imageView?.needsDisplay = true
        }

        private static func decodedImage(contentsOf url: URL) -> NSImage? {
            guard let cgImage = NativePreviewImageDecoder.decodeCGImage(contentsOf: url) else {
                return NSImage(contentsOf: url)
            }

            return NSImage(
                cgImage: cgImage,
                size: CGSize(width: cgImage.width, height: cgImage.height)
            )
        }

        @MainActor
        func applyZoom(magnification: Double, fitToWindow: Bool, updateMagnification: @MainActor @escaping (Double) -> Void) {
            guard let scrollView else {
                return
            }

            let target = fitToWindow ? fittedMagnification(in: scrollView) : magnification
            let clamped = min(max(target, Double(scrollView.minMagnification)), Double(scrollView.maxMagnification))
            if abs(Double(scrollView.magnification) - clamped) > 0.001 {
                NSAnimationContext.runAnimationGroup { context in
                    context.duration = 0
                    context.allowsImplicitAnimation = false
                    scrollView.setMagnification(CGFloat(clamped), centeredAt: scrollView.contentView.bounds.center)
                }
            }

            if abs(magnification - clamped) > 0.001 {
                Task { @MainActor in
                    var transaction = Transaction()
                    transaction.disablesAnimations = true
                    withTransaction(transaction) {
                        updateMagnification(clamped)
                    }
                }
            }
        }

        @MainActor
        func scheduleDeferredFitZoom(magnification: Double, updateMagnification: @MainActor @escaping (Double) -> Void) {
            guard deferredFitZoomScheduled == false else {
                return
            }
            deferredFitZoomScheduled = true
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: 50_000_000)
                deferredFitZoomScheduled = false
                applyZoom(
                    magnification: magnification,
                    fitToWindow: true,
                    updateMagnification: updateMagnification
                )
            }
        }

        @MainActor
        private func fittedMagnification(in scrollView: NSScrollView) -> Double {
            guard naturalImageSize.width > 0, naturalImageSize.height > 0 else {
                return 1.0
            }

            let visibleSize = scrollView.contentSize
            guard visibleSize.width > 0, visibleSize.height > 0 else {
                return 1.0
            }

            let horizontalScale = visibleSize.width / naturalImageSize.width
            let verticalScale = visibleSize.height / naturalImageSize.height
            return Double(min(horizontalScale, verticalScale))
        }
    }

    final class CenteringClipView: NSClipView {
        override func constrainBoundsRect(_ proposedBounds: NSRect) -> NSRect {
            var rect = super.constrainBoundsRect(proposedBounds)
            guard let documentView else {
                return rect
            }

            let documentFrame = documentView.frame
            if documentFrame.width < bounds.width {
                rect.origin.x = (documentFrame.width - bounds.width) / 2.0
            }
            if documentFrame.height < bounds.height {
                rect.origin.y = (documentFrame.height - bounds.height) / 2.0
            }
            return rect
        }
    }
}

private extension CGRect {
    var center: CGPoint {
        CGPoint(x: midX, y: midY)
    }
}
#endif
