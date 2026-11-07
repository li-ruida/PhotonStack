import Foundation

/// Keeps the subprocess transport bounded independently of image-processing memory.
/// Progress is streamed; structured stdout must remain complete or the job fails.
final class ProcessOutputAccumulator: @unchecked Sendable {
    static let standardOutputLimit = 32 * 1_024 * 1_024
    static let standardErrorLimit = 256 * 1_024

    private let lock = NSLock()
    private let jobID: UUID?
    private let progressScope: ProcessingProgressScope?
    private let stdoutLimit: Int
    private let stderrLimit: Int
    private var stdout = Data()
    private var stderr = Data()
    private var stdoutRemainder = Data()
    private var stdoutExceededLimit = false
    private var stderrWasTruncated = false
    private var pendingProgress: ProcessingProgressEvent?
    private var lastProgressEmission: ContinuousClock.Instant?

    init(
        jobID: UUID?,
        progressScope: ProcessingProgressScope?,
        stdoutLimit: Int = standardOutputLimit,
        stderrLimit: Int = standardErrorLimit
    ) {
        precondition(stdoutLimit > 0 && stderrLimit > 0)
        self.jobID = jobID
        self.progressScope = progressScope
        self.stdoutLimit = stdoutLimit
        self.stderrLimit = stderrLimit
    }

    /// False means structured output overflowed. The caller must stop the command.
    @discardableResult
    func appendStdout(_ data: Data) -> Bool {
        lock.lock()
        var events: [ProcessingProgressEvent] = []
        var start = data.startIndex
        while !stdoutExceededLimit, start < data.endIndex {
            let newline = data[start...].firstIndex(of: 0x0A)
            let end = newline ?? data.endIndex
            let byteCount = data.distance(from: start, to: end)
            let separatorBytes = newline == nil ? 0 : 1
            guard byteCount + separatorBytes <= stdoutLimit - stdoutRemainder.count else {
                rejectOversizedOutput()
                break
            }
            stdoutRemainder.append(data[start..<end])
            if let newline {
                if let event = appendStdoutLine() { events.append(event) }
                start = data.index(after: newline)
            } else {
                break
            }
        }
        let accepted = !stdoutExceededLimit
        lock.unlock()
        // Notification observers are arbitrary synchronous code. Never call them
        // while holding the stream's lock (they may inspect or cancel a job).
        for event in events { emit(event) }
        return accepted
    }

    func finishStdout() {
        lock.lock()
        var events: [ProcessingProgressEvent] = []
        if !stdoutExceededLimit, !stdoutRemainder.isEmpty {
            // Preserve an unterminated final record without inventing an extra byte.
            if let event = appendStdoutLine(terminated: false) { events.append(event) }
        }
        if let event = takeProgress() { events.append(event) }
        lock.unlock()
        for event in events { emit(event) }
    }

    func appendStderr(_ data: Data) {
        guard !data.isEmpty else { return }
        lock.lock()
        if data.count >= stderrLimit {
            stderrWasTruncated = stderrWasTruncated || !stderr.isEmpty || data.count > stderrLimit
            stderr = Data(data.suffix(stderrLimit))
        } else {
            let overflow = max(0, stderr.count + data.count - stderrLimit)
            if overflow > 0 {
                stderr.removeFirst(overflow)
                stderrWasTruncated = true
            }
            stderr.append(data)
        }
        lock.unlock()
    }

    func snapshot() -> (stdout: String, stderr: String, stdoutExceededLimit: Bool) {
        lock.lock()
        defer { lock.unlock() }
        // A retained UTF-8 tail may begin inside a multi-byte character.
        let diagnostic = stderrWasTruncated ? stderr.drop(while: { $0 & 0xC0 == 0x80 }) : stderr[...]
        let marker = stderrWasTruncated ? "[Earlier diagnostic output omitted; showing the final \(stderrLimit) bytes.]\n" : ""
        return (String(decoding: stdout, as: UTF8.self), marker + String(decoding: diagnostic, as: UTF8.self), stdoutExceededLimit)
    }

    private func appendStdoutLine(terminated: Bool = true) -> ProcessingProgressEvent? {
        defer { stdoutRemainder.removeAll(keepingCapacity: true) }
        if let event = Self.progressEvent(from: stdoutRemainder, jobID: jobID, progressScope: progressScope) {
            pendingProgress = event
            let now = ContinuousClock.now
            if lastProgressEmission.map({ $0.duration(to: now) >= .milliseconds(200) }) ?? true {
                return takeProgress()
            }
        } else {
            guard stdoutRemainder.count + (terminated ? 1 : 0) <= stdoutLimit - stdout.count else {
                rejectOversizedOutput()
                return nil
            }
            stdout.append(stdoutRemainder)
            if terminated { stdout.append(0x0A) }
        }
        return nil
    }

    private func rejectOversizedOutput() {
        stdoutExceededLimit = true
        stdout.removeAll(keepingCapacity: false)
        stdoutRemainder.removeAll(keepingCapacity: false)
        pendingProgress = nil
    }

    private func takeProgress() -> ProcessingProgressEvent? {
        guard let event = pendingProgress else { return nil }
        pendingProgress = nil
        lastProgressEmission = .now
        return event
    }

    private func emit(_ event: ProcessingProgressEvent) {
        NotificationCenter.default.post(name: .photonStackProcessingProgress, object: event)
    }

    private static func progressEvent(
        from data: Data,
        jobID: UUID?,
        progressScope: ProcessingProgressScope?
    ) -> ProcessingProgressEvent? {
        guard let object = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              object["type"] as? String == "progress",
              let task = object["task"] as? String,
              let command = object["command"] as? String,
              let stage = object["stage"] as? String,
              let progressNumber = object["progress"] as? NSNumber,
              CFGetTypeID(progressNumber) != CFBooleanGetTypeID(),
              progressNumber.doubleValue.isFinite else { return nil }
        return ProcessingProgressEvent(
            jobID: jobID, task: task, command: command, stage: stage,
            progress: progressScope?.map(progressNumber.doubleValue) ?? progressNumber.doubleValue,
            step: (object["step"] as? NSNumber)?.intValue,
            total: (object["total"] as? NSNumber)?.intValue
        )
    }
}
