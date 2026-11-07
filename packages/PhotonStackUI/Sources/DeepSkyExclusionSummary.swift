import PhotonStackAppCore

struct DeepSkyExclusionSummary {
    struct Item: Identifiable {
        let code: String
        let title: String
        let count: Int
        var id: String { code }
    }

    let items: [Item]
    let hasMultipleReasonsPerFrame: Bool

    init(report: DeepSkyReport, selection: DeepSkyReviewSelection, chinese: Bool) {
        var counts: [String: Int] = [:]
        var hasMultiple = false
        for frame in report.frames {
            let causes = DeepSkyReviewEvidence.exclusionCodes(frame, selection: selection)
            if causes.count > 1 { hasMultiple = true }
            for cause in causes { counts[cause, default: 0] += 1 }
        }
        items = counts.map { code, count in
            Item(code: code, title: DeepSkyReviewEvidence.title(for: code, chinese: chinese), count: count)
        }.sorted { lhs, rhs in
            lhs.count == rhs.count ? lhs.code < rhs.code : lhs.count > rhs.count
        }
        hasMultipleReasonsPerFrame = hasMultiple
    }
}
