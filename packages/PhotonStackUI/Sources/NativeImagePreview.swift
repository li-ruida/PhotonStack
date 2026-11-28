#if os(macOS)
import AppKit
import CoreImage
import ImageIO
import SwiftUI

enum NativePreviewZoom {
    static func scrollMagnification(pixelMagnification: Double, backingScale: Double) -> Double {
        pixelMagnification / validScale(backingScale)
    }

    static func pixelMagnification(scrollMagnification: Double, backingScale: Double) -> Double {
        scrollMagnification * validScale(backingScale)
    }

    private static func validScale(_ scale: Double) -> Double {
        scale.isFinite && scale > 0 ? scale : 1
    }
}

final class NativePreviewScrollView: NSScrollView {
    var backingPropertiesChanged: (() -> Void)?
    var userMagnificationChanged: ((Double) -> Void)?
    var viewportLaidOut: (() -> Void)?

    override func layout() {
        super.layout()
        viewportLaidOut?()
    }

    override func viewDidMoveToWindow() {
        super.viewDidMoveToWindow()
        backingPropertiesChanged?()
    }

    override func viewDidChangeBackingProperties() {
        super.viewDidChangeBackingProperties()
        backingPropertiesChanged?()
    }

    override func magnify(with event: NSEvent) {
        super.magnify(with: event)
        userMagnificationChanged?(Double(magnification))
    }

    override func smartMagnify(with event: NSEvent) {
        super.smartMagnify(with: event)
        userMagnificationChanged?(Double(magnification))
    }
}

enum NativePreviewImageDecoder {
    private static let displayContext = CIContext(options: [.cacheIntermediates: false])
    static func decodeCGImage(contentsOf url: URL, maximumPixelSize: Int? = nil) -> CGImage? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, nil) else {
            return nil
        }

        let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any]
        let width = (properties?[kCGImagePropertyPixelWidth] as? NSNumber)?.intValue ?? 0
        let height = (properties?[kCGImagePropertyPixelHeight] as? NSNumber)?.intValue ?? 0
        let maximumDimension = min(max(width, height), maximumPixelSize ?? Int.max)
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

    static func sourceSize(at url: URL) -> CGSize? {
        guard let source = CGImageSourceCreateWithURL(url as CFURL, [kCGImageSourceShouldCache: false] as CFDictionary),
              let properties = CGImageSourceCopyPropertiesAtIndex(source, 0, nil) as? [CFString: Any],
              let width = properties[kCGImagePropertyPixelWidth] as? NSNumber,
              let height = properties[kCGImagePropertyPixelHeight] as? NSNumber else { return nil }
        let rotated = (5...8).contains((properties[kCGImagePropertyOrientation] as? NSNumber)?.intValue ?? 1)
        return rotated ? CGSize(width: height.intValue, height: width.intValue)
                       : CGSize(width: width.intValue, height: height.intValue)
    }

    private static func displayImage(from image: CGImage) -> CGImage? {
        guard image.bitsPerComponent > 8 else {
            return image
        }
        guard let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) else {
            return nil
        }
        let input = CIImage(cgImage: image)
        return displayContext.createCGImage(
            input,
            from: input.extent,
            format: .RGBA8,
            colorSpace: colorSpace
        )
    }
}

// A serial actor keeps ImageIO/CI work off the main actor and bounds both
// concurrent decoding and retained pixels. The cache dies with its preview.
actor NativePreviewImageLoader {
    private struct Entry {
        let image: CGImage
        let modified: Date
        let size: Int
        var cost: Int { image.bytesPerRow * image.height }
    }
    private struct Key: Hashable {
        let url: URL
        let maximumPixelSize: Int?
    }
    private var entries: [Key: Entry] = [:]
    private var order: [Key] = []
    // Keep one native 61 MP RGBA8 preview on smaller machines, and two on
    // larger machines. Entry count and byte cost still bound retained memory.
    private let byteLimit = Int(min(512 * 1024 * 1024,
                                   max(256 * 1024 * 1024, ProcessInfo.processInfo.physicalMemory / 64)))

    func sourceSize(_ url: URL) -> CGSize? { NativePreviewImageDecoder.sourceSize(at: url) }

    func load(_ url: URL, maximumPixelSize: Int? = nil) throws -> CGImage? {
        try Task.checkCancellation()
        let key = Key(url: url, maximumPixelSize: maximumPixelSize)
        let attributes = try FileManager.default.attributesOfItem(atPath: url.path)
        let modified = attributes[.modificationDate] as? Date ?? .distantPast
        let size = attributes[.size] as? Int ?? 0
        if let entry = entries[key], entry.modified == modified, entry.size == size {
            order.removeAll { $0 == key }; order.append(key)
            return entry.image
        }
        guard let image = NativePreviewImageDecoder.decodeCGImage(contentsOf: url, maximumPixelSize: maximumPixelSize) else { return nil }
        try Task.checkCancellation()
        let entry = Entry(image: image, modified: modified, size: size)
        entries.removeValue(forKey: key); order.removeAll { $0 == key }
        if entry.cost <= byteLimit {
            entries[key] = entry; order.append(key)
            while order.count > 2 || entries.values.reduce(0, { $0 + $1.cost }) > byteLimit {
                entries.removeValue(forKey: order.removeFirst())
            }
        }
        return image
    }
}

// At native size and above, show the actual source samples. NSImageView's
// default interpolation otherwise blends even an integer 2x enlargement.
// Keep the normal smooth downsampling when fitting a larger image to a window.
final class NativePreviewImageView: NSImageView {
    var samplePoint: ((CGPoint) -> Void)? {
        didSet {
            if (samplePoint == nil) != (oldValue == nil) { window?.invalidateCursorRects(for: self) }
        }
    }

    override func resetCursorRects() {
        super.resetCursorRects()
        if samplePoint != nil { addCursorRect(visibleRect, cursor: .crosshair) }
    }

    override func mouseDown(with event: NSEvent) {
        guard let samplePoint, image != nil else { super.mouseDown(with: event); return }
        let point = convert(event.locationInWindow, from: nil)
        guard bounds.width > 0, bounds.height > 0, bounds.contains(point) else { return }
        let y = (point.y - bounds.minY) / bounds.height
        samplePoint(CGPoint(x: (point.x - bounds.minX) / bounds.width, y: isFlipped ? y : 1 - y))
    }

    var alignmentStars: [CGPoint] = [] {
        didSet { if alignmentStars != oldValue { needsDisplay = true } }
    }

    var usesNearestNeighbor = false {
        didSet {
            guard usesNearestNeighbor != oldValue else { return }
            layer?.magnificationFilter = usesNearestNeighbor ? .nearest : .linear
            layer?.minificationFilter = usesNearestNeighbor ? .nearest : .linear
            needsDisplay = true
        }
    }

    override func draw(_ dirtyRect: NSRect) {
        let context = NSGraphicsContext.current
        let previousInterpolation = context?.imageInterpolation
        if usesNearestNeighbor { context?.imageInterpolation = .none }
        defer {
            if let previousInterpolation { context?.imageInterpolation = previousInterpolation }
        }
        super.draw(dirtyRect)
        guard image != nil, !alignmentStars.isEmpty else { return }
        let zoom = max(0.1, enclosingScrollView?.magnification ?? 1)
        let radius = 4.5 / zoom
        let path = NSBezierPath()
        for star in alignmentStars {
            let center = CGPoint(x: star.x * bounds.width, y: (isFlipped ? star.y : 1 - star.y) * bounds.height)
            let ring = CGRect(x: center.x - radius, y: center.y - radius, width: 2 * radius, height: 2 * radius)
            if dirtyRect.intersects(ring) { path.appendOval(in: ring) }
        }
        path.lineWidth = 2 / zoom
        NSColor.black.withAlphaComponent(0.8).setStroke()
        path.stroke()
        path.lineWidth = 1 / zoom
        NSColor.systemMint.setStroke()
        path.stroke()
    }
}

struct NativeImagePreview: NSViewRepresentable {
    let url: URL
    @Binding var magnification: Double
    @Binding var fitToWindow: Bool
    var decodedImage: CGImage? = nil
    var decodedSize: CGSize? = nil
    var samplePoint: ((CGPoint) -> Void)? = nil
    var centerFirstImage = false
    var alignmentStars: [CGPoint] = []

    func makeNSView(context: Context) -> NSScrollView {
        let imageView = NativePreviewImageView()
        imageView.imageScaling = .scaleProportionallyUpOrDown
        imageView.imageAlignment = .alignCenter
        imageView.wantsLayer = true
        imageView.layerContentsRedrawPolicy = .onSetNeedsDisplay

        let scrollView = NativePreviewScrollView()
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
        (context.coordinator.imageView as? NativePreviewImageView)?.samplePoint = samplePoint
        (context.coordinator.imageView as? NativePreviewImageView)?.alignmentStars = alignmentStars
        context.coordinator.centerFirstImage = centerFirstImage
        if let nativeScrollView = scrollView as? NativePreviewScrollView {
            nativeScrollView.viewportLaidOut = { [weak coordinator = context.coordinator] in
                coordinator?.centerInitialImageIfNeeded()
                if self.fitToWindow, self.decodedImage == nil, let coordinator, let scrollView = coordinator.scrollView {
                    let limit = Coordinator.previewPixelLimit(in: scrollView)
                    if coordinator.maximumPixelSize != limit {
                        coordinator.updateImage(self.url, maximumPixelSize: limit)
                    }
                }
            }
            nativeScrollView.backingPropertiesChanged = { [weak coordinator = context.coordinator] in
                coordinator?.applyZoom(
                    magnification: self.magnification,
                    fitToWindow: self.fitToWindow,
                    updateMagnification: { self.magnification = $0 }
                )
            }
            nativeScrollView.userMagnificationChanged = { [weak scrollView, weak coordinator = context.coordinator] value in
                guard let scrollView else { return }
                coordinator?.cancelDeferredFitZoom()
                self.fitToWindow = false
                self.magnification = NativePreviewZoom.pixelMagnification(
                    scrollMagnification: value,
                    backingScale: Coordinator.backingScale(for: scrollView)
                )
            }
        }
        context.coordinator.imageLoaded = { [weak coordinator = context.coordinator] in
            coordinator?.applyZoom(magnification: self.magnification, fitToWindow: self.fitToWindow,
                                   updateMagnification: { self.magnification = $0 })
        }
        let maximumPixelSize = decodedImage == nil && fitToWindow ? Coordinator.previewPixelLimit(in: scrollView) : nil
        if context.coordinator.url != url || context.coordinator.decodedImage !== decodedImage ||
            context.coordinator.maximumPixelSize != maximumPixelSize {
            context.coordinator.updateImage(url, decodedImage: decodedImage, decodedSize: decodedSize,
                                            maximumPixelSize: maximumPixelSize)
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

    static func dismantleNSView(_ nsView: NSScrollView, coordinator: Coordinator) {
        coordinator.cancelImageLoad()
        coordinator.imageLoaded = nil
    }

    func makeCoordinator() -> Coordinator {
        Coordinator()
    }

    @MainActor final class Coordinator {
        weak var imageView: NSImageView?
        weak var scrollView: NSScrollView?
        var url: URL?
        var decodedImage: CGImage?
        var maximumPixelSize: Int?
        var naturalImageSize: CGSize = .zero
        var centerFirstImage = false
        private var needsInitialCenter = false
        private var deferredFitZoomScheduled = false
        private var fitZoomGeneration = 0

        private let imageLoader = NativePreviewImageLoader()
        private var imageTask: Task<Void, Never>?
        private var imageGeneration = 0
        var imageLoaded: (() -> Void)?

        func cancelImageLoad() {
            imageGeneration &+= 1
            imageTask?.cancel()
            imageTask = nil
        }

        static func previewPixelLimit(in view: NSScrollView) -> Int {
            let pixels = max(view.bounds.width, view.bounds.height) * backingScale(for: view)
            // Quantization avoids decoding again for every point of a resize.
            return max(512, Int(ceil(pixels / 256)) * 256)
        }

        func updateImage(_ newURL: URL, decodedImage: CGImage? = nil, decodedSize: CGSize? = nil,
                         maximumPixelSize: Int? = nil) {
            cancelImageLoad()
            let sourceChanged = url != newURL
            url = newURL
            self.maximumPixelSize = maximumPixelSize
            self.decodedImage = decodedImage
            if let image = decodedImage {
                setImage(NSImage(cgImage: image, size: decodedSize ?? CGSize(width: image.width, height: image.height)))
                imageLoaded?()
                return
            }
            // Never display the old image under the new frame's label.
            if sourceChanged { imageView?.image = nil }
            let generation = imageGeneration
            let loader = imageLoader
            imageTask = Task { [weak self] in
                guard let image = try? await loader.load(newURL, maximumPixelSize: maximumPixelSize) else { return }
                let sourceSize = await loader.sourceSize(newURL) ?? CGSize(width: image.width, height: image.height)
                guard !Task.isCancelled, let self, generation == self.imageGeneration else { return }
                self.setImage(NSImage(cgImage: image, size: sourceSize))
                self.imageLoaded?()
            }
        }

        @MainActor
        func setImage(_ image: NSImage) {
            let previousVisibleRect = scrollView?.contentView.bounds
            let previousSize = naturalImageSize
            let nextSize = image.size
            if centerFirstImage && previousSize == .zero { needsInitialCenter = true }
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

        @MainActor
        func applyZoom(magnification: Double, fitToWindow: Bool, updateMagnification: @MainActor @escaping (Double) -> Void) {
            if fitToWindow == false {
                cancelDeferredFitZoom()
            }
            guard let scrollView else {
                return
            }

            // The document uses one point per source pixel. NSScrollView zoom
            // operates in points, while the toolbar reports physical screen pixels.
            let scale = Self.backingScale(for: scrollView)
            scrollView.minMagnification = CGFloat(NativePreviewZoom.scrollMagnification(pixelMagnification: 0.1, backingScale: scale))
            scrollView.maxMagnification = CGFloat(NativePreviewZoom.scrollMagnification(pixelMagnification: 8, backingScale: scale))
            let target = fitToWindow
                ? NativePreviewZoom.pixelMagnification(scrollMagnification: fittedMagnification(in: scrollView), backingScale: scale)
                : magnification
            let clamped = min(max(target, 0.1), 8)
            (imageView as? NativePreviewImageView)?.usesNearestNeighbor = clamped >= 1
            let scrollTarget = NativePreviewZoom.scrollMagnification(pixelMagnification: clamped, backingScale: scale)
            if abs(Double(scrollView.magnification) - scrollTarget) > 0.0001 {
                NSAnimationContext.runAnimationGroup { context in
                    context.duration = 0
                    context.allowsImplicitAnimation = false
                    scrollView.setMagnification(CGFloat(scrollTarget), centeredAt: scrollView.contentView.bounds.center)
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
            centerInitialImageIfNeeded()
        }

        /// Layout may arrive after the decoded image. Consume this request once,
        /// so later image switches, viewport changes and user pans never recenter.
        func centerInitialImageIfNeeded() {
            guard needsInitialCenter, let scrollView, naturalImageSize.width > 0, naturalImageSize.height > 0,
                  scrollView.contentSize.width > 0, scrollView.contentSize.height > 0 else { return }
            needsInitialCenter = false
            let visible = scrollView.contentView.bounds.size
            scrollView.contentView.scroll(to: NSPoint(x: (naturalImageSize.width - visible.width) / 2,
                                                     y: (naturalImageSize.height - visible.height) / 2))
            scrollView.reflectScrolledClipView(scrollView.contentView)
        }

        @MainActor
        static func backingScale(for view: NSView) -> Double {
            Double(view.window?.backingScaleFactor ?? NSScreen.main?.backingScaleFactor ?? 1)
        }

        @MainActor
        func cancelDeferredFitZoom() {
            fitZoomGeneration &+= 1
            deferredFitZoomScheduled = false
        }

        @MainActor
        func scheduleDeferredFitZoom(magnification: Double, updateMagnification: @MainActor @escaping (Double) -> Void) {
            guard deferredFitZoomScheduled == false else {
                return
            }
            deferredFitZoomScheduled = true
            let generation = fitZoomGeneration
            Task { @MainActor in
                try? await Task.sleep(nanoseconds: 50_000_000)
                guard generation == fitZoomGeneration else { return }
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
