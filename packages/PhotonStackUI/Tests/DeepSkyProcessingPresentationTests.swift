import Foundation
import PhotonStackAppCore
import Testing
@testable import PhotonStackUI

@Test func deepSkyCombinationProgressAdvancesWithinSmallOverallRange() {
    #expect(DeepSkyProcessingPresentation.combinationDetail(step: 0, total: 3840, overall: 0.823, chinese: true)
        == "像素合成与剔除 · 本阶段 0%（总流程 82%）")
    #expect(DeepSkyProcessingPresentation.combinationDetail(step: 1536, total: 3840, overall: 0.8338, chinese: true)
        == "像素合成与剔除 · 本阶段 40%（总流程 83%）")
    #expect(DeepSkyProcessingPresentation.combinationDetail(step: 1920, total: 3840, overall: 0.8365, chinese: false)
        == "Combining and rejecting pixels · stage 50% (overall 84%)")
    #expect(DeepSkyProcessingPresentation.combinationDetail(step: 3840, total: 3840, overall: 0.85, chinese: true)
        == "像素合成与剔除 · 本阶段 100%（总流程 85%）")
    for (step, total) in [(-1, 3840), (3841, 3840), (1, 0)] {
        #expect(DeepSkyProcessingPresentation.combinationDetail(step: step, total: total, overall: 0.84, chinese: true) == nil)
    }
    #expect(DeepSkyProcessingPresentation.combinationDetail(step: nil, total: nil, overall: 0.84, chinese: true) == nil)
    #expect(DeepSkyProcessingPresentation.combinationDetail(step: 1, total: 2, overall: .nan, chinese: true) == nil)
}

@Test func deepSkyProcessingElapsedUsesTheJobStartAcrossReopening() {
    let start = Date(timeIntervalSince1970: 1000)
    let job = ProcessingJob(title: "Deep Sky Recipe", kind: .stack, status: .running, createdAt: start)
    let first = DeepSkyProcessingPresentation(job: job, message: job.title, fraction: 0, chinese: true)
    #expect(first.title == "正在分析与堆栈")
    #expect(first.detail == "正在准备…")
    #expect(first.elapsed(at: start.addingTimeInterval(3599)) == "已用时 59:59")
    // A recreated sheet/presentation uses the job timestamp, not its mount time.
    let reopened = DeepSkyProcessingPresentation(job: job, message: "像素合成与剔除 · 85%", fraction: 0.85, chinese: true)
    #expect(reopened.elapsed(at: start.addingTimeInterval(3661)) == "已用时 1:01:01")
    #expect(reopened.elapsed(at: start.addingTimeInterval(-5)) == "已用时 0:00")
    #expect(reopened.detail == "像素合成与剔除 · 85%")
    let next = ProcessingJob(title: "Deep Sky Background & Denoise", kind: .stack, createdAt: start.addingTimeInterval(4000))
    let refinish = DeepSkyProcessingPresentation(job: next, message: nil, fraction: nil, chinese: true)
    #expect(refinish.title == "正在修整成片")
    #expect(refinish.elapsed(at: start.addingTimeInterval(4002)) == "已用时 0:02")
}

@Test func deepSkyProcessingDistinguishesFinalizationFromReadyResult() {
    let job = ProcessingJob(title: "Deep Sky Frame Assessment", kind: .stack)
    let done = DeepSkyProcessingPresentation(job: job, message: "complete · 100%", fraction: 1, chinese: false)
    #expect(done.title == "Analyzing light frames")
    #expect(done.detail == "Saving and loading the result…")
    let preparing = DeepSkyProcessingPresentation(job: nil, message: nil, fraction: nil, chinese: false)
    #expect(preparing.title == "Processing images")
    #expect(preparing.elapsed(at: .now) == nil)
}
