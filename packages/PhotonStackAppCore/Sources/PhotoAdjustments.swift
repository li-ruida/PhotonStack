import CoreGraphics
import Foundation

/// Display-referred edits. Values are offsets from a neutral, unedited image.
public struct PhotoAdjustments: Equatable, Sendable {
    public enum Control: String, CaseIterable, Sendable {
        case exposure, contrast, highlights, shadows, blackPoint, whitePoint, warmth, tint, saturation, vibrance

        public var range: ClosedRange<Double> {
            switch self {
            case .blackPoint: 0...254
            case .whitePoint: 1...255
            case .exposure: -3...3
            default: -1...1
            }
        }

        public var neutralValue: Double { self == .whitePoint ? 255 : 0 }

        public func title(chinese: Bool) -> String {
            switch self {
            case .exposure: chinese ? "曝光（EV）" : "Exposure (EV)"
            case .contrast: chinese ? "对比度" : "Contrast"
            case .highlights: chinese ? "高光" : "Highlights"
            case .shadows: chinese ? "阴影" : "Shadows"
            case .blackPoint: chinese ? "黑场色阶" : "Input black level"
            case .whitePoint: chinese ? "白场色阶" : "Input white level"
            case .warmth: chinese ? "冷暖" : "Warmth"
            case .tint: chinese ? "色调（绿／品红）" : "Tint (green / magenta)"
            case .saturation: chinese ? "饱和度" : "Saturation"
            case .vibrance: chinese ? "自然饱和度" : "Vibrance"
            }
        }
    }

    public enum Aspect: String, CaseIterable, Sendable {
        case original, square = "1:1", landscape = "3:2", portrait = "2:3", wide = "16:9", tall = "9:16", classic = "4:3"
        public var ratio: Double? {
            switch self {
            case .original: nil
            case .square: 1
            case .landscape: 3.0 / 2
            case .portrait: 2.0 / 3
            case .wide: 16.0 / 9
            case .tall: 9.0 / 16
            case .classic: 4.0 / 3
            }
        }
    }

    /// Mean RGB in linear sRGB, sampled before applying any color adjustments.
    public struct NeutralSample: Equatable, Sendable {
        public let red: Double
        public let green: Double
        public let blue: Double

        public init?(red: Double, green: Double, blue: Double) {
            guard [red, green, blue].allSatisfy({ $0.isFinite && $0 >= 0.0003 && $0 < 0.99 }) else { return nil }
            self.red = red
            self.green = green
            self.blue = blue
        }

        public var luminance: Double { 0.2126 * red + 0.7152 * green + 0.0722 * blue }
    }

    public var neutralSample: NeutralSample?
    public var aspect: Aspect = .original
    public private(set) var quarterTurns = 0
    public var cropScale = 1.0
    public var mirror = false
    public var flipVertical = false
    public var horizontalPosition = 0.5
    public var verticalPosition = 0.5

    public mutating func rotate(clockwise: Bool) {
        quarterTurns = (quarterTurns + (clockwise ? 1 : 3)) % 4
    }

    /// Coordinates use Core Image's bottom-left origin. Positions in the
    /// inspector increase rightward and downward in the displayed image.
    public func cropRect(in size: CGSize) -> CGRect {
        guard size.width > 0, size.height > 0 else { return .zero }
        let ratio = aspect.ratio ?? (size.width / size.height)
        let width = max(1, (min(size.width, size.height * ratio) / cropScale).rounded())
        let height = max(1, (min(size.height, size.width / ratio) / cropScale).rounded())
        let x = ((size.width - width) * horizontalPosition).rounded()
        let y = ((size.height - height) * (1 - verticalPosition)).rounded()
        return CGRect(x: x, y: y, width: width, height: height)
    }

    private var values: [Control: Double] = [:]
    public init() {}

    public subscript(control: Control) -> Double {
        get { values[control] ?? control.neutralValue }
        set {
            guard newValue.isFinite else { return }
            let range = allowedRange(for: control)
            let value = min(max(newValue, range.lowerBound), range.upperBound)
            values[control] = value == control.neutralValue ? nil : value
        }
    }

    public func allowedRange(for control: Control) -> ClosedRange<Double> {
        switch control {
        case .blackPoint: 0...(self[.whitePoint] - 1)
        case .whitePoint: (self[.blackPoint] + 1)...255
        default: control.range
        }
    }

    public var isNeutral: Bool { neutralSample == nil && values.isEmpty && cropScale == 1 && aspect == .original && quarterTurns == 0 && !mirror && !flipVertical }

    public var operationParameters: [String: String] {
        var result = Dictionary(uniqueKeysWithValues: Control.allCases.map { ($0.rawValue, String(self[$0])) })
        if let neutralSample {
            result["sampleRed"] = String(neutralSample.red)
            result["sampleGreen"] = String(neutralSample.green)
            result["sampleBlue"] = String(neutralSample.blue)
        }
        result["aspect"] = aspect.rawValue
        result["quarterTurns"] = String(quarterTurns)
        result["cropScale"] = String(cropScale)
        result["mirror"] = String(mirror)
        if flipVertical { result["flipVertical"] = "true" }
        result["horizontalPosition"] = String(horizontalPosition)
        result["verticalPosition"] = String(verticalPosition)
        return result
    }

    public init?(operationParameters: [String: String]) {
        guard let aspect = Aspect(rawValue: operationParameters["aspect"] ?? "original"),
              let turns = Int(operationParameters["quarterTurns"] ?? "0"), (0...3).contains(turns),
              let scale = Double(operationParameters["cropScale"] ?? "1"), (1...3).contains(scale),
              let mirror = Bool(operationParameters["mirror"] ?? "false"),
              let vertical = Bool(operationParameters["flipVertical"] ?? "false"),
              let x = Double(operationParameters["horizontalPosition"] ?? "0.5"), (0...1).contains(x),
              let y = Double(operationParameters["verticalPosition"] ?? "0.5"), (0...1).contains(y)
        else { return nil }
        self.aspect = aspect
        self.quarterTurns = turns
        cropScale = scale
        self.mirror = mirror
        flipVertical = vertical
        horizontalPosition = x
        verticalPosition = y
        let sampleKeys = ["sampleRed", "sampleGreen", "sampleBlue"]
        if sampleKeys.contains(where: { operationParameters[$0] != nil }) {
            let channels = sampleKeys.compactMap { operationParameters[$0].flatMap(Double.init) }
            guard channels.count == 3,
                  let sample = NeutralSample(red: channels[0], green: channels[1], blue: channels[2]) else { return nil }
            neutralSample = sample
        }
        for control in Control.allCases {
            if (control == .blackPoint || control == .whitePoint) && operationParameters[control.rawValue] == nil { continue }
            guard let text = operationParameters[control.rawValue], let value = Double(text),
                  value.isFinite, allowedRange(for: control).contains(value) else { return nil }
            self[control] = value
        }
    }

    public static func supports(_ url: URL) -> Bool {
        [.tiff, .jpeg, .heif, .png].contains(AssetKind.detect(from: url))
    }
}
