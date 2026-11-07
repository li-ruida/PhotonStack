import PhotonStackAppCore
import PhotonStackProcessing
import Foundation
#if os(macOS)
import AppKit
#endif
import os
import SwiftUI
import UniformTypeIdentifiers

func sampleCurve(_ input: Double, points: [CurveEditorPoint]) -> Double {
    let sorted = points.sorted { $0.input < $1.input }
    guard sorted.count >= 2 else {
        return min(max(input, 0.0), 1.0)
    }
    let x = min(max(input, 0.0), 1.0)
    if x <= sorted[0].input {
        return min(max(sorted[0].output, 0.0), 1.0)
    }

    let tangents = curveTangents(for: sorted)
    for index in sorted.indices.dropFirst() where x <= sorted[index].input {
        return min(max(hermiteInterpolate(
            x,
            from: sorted[index - 1],
            to: sorted[index],
            startTangent: tangents[index - 1],
            endTangent: tangents[index]
        ), 0.0), 1.0)
    }
    return min(max(sorted[sorted.count - 1].output, 0.0), 1.0)
}

private func curveTangents(for points: [CurveEditorPoint]) -> [Double] {
    guard points.count >= 2 else {
        return Array(repeating: 0.0, count: points.count)
    }

    let slopes = points.indices.dropLast().map { index in
        let span = max(points[index + 1].input - points[index].input, 0.000_001)
        return (points[index + 1].output - points[index].output) / span
    }

    var tangents = Array(repeating: 0.0, count: points.count)
    tangents[0] = slopes[0]
    tangents[points.count - 1] = slopes[slopes.count - 1]
    for index in 1..<(points.count - 1) {
        tangents[index] = limitedCurveTangent(leftSlope: slopes[index - 1], rightSlope: slopes[index])
    }
    return tangents
}

private func limitedCurveTangent(leftSlope: Double, rightSlope: Double) -> Double {
    guard leftSlope * rightSlope > 0 else {
        return 0.0
    }
    let sign = leftSlope < 0 ? -1.0 : 1.0
    let average = 0.5 * (leftSlope + rightSlope)
    let limit = 3.0 * min(abs(leftSlope), abs(rightSlope))
    return sign * min(abs(average), limit)
}

private func hermiteInterpolate(
    _ input: Double,
    from start: CurveEditorPoint,
    to end: CurveEditorPoint,
    startTangent: Double,
    endTangent: Double
) -> Double {
    let span = max(end.input - start.input, 0.000_001)
    let t = min(max((input - start.input) / span, 0.0), 1.0)
    let t2 = t * t
    let t3 = t2 * t
    let h00 = 2.0 * t3 - 3.0 * t2 + 1.0
    let h10 = t3 - 2.0 * t2 + t
    let h01 = -2.0 * t3 + 3.0 * t2
    let h11 = t3 - t2
    return h00 * start.output + h10 * span * startTangent + h01 * end.output + h11 * span * endTangent
}
