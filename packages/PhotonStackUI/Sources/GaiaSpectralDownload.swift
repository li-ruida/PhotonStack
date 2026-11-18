import Darwin
import Foundation
import PhotonStackProcessing

actor GaiaResponseCache {
    typealias Fetch = @Sendable (URL) async throws -> Data
    struct Receipt: Codable { var url, sha: String }
    let root: URL
    private let fetch: Fetch
    init(root: URL? = nil, fetch: @escaping Fetch = GaiaResponseCache.network) {
        self.root = root ?? FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0].appendingPathComponent("PhotonStack/GaiaDR3")
        self.fetch = fetch
    }
    static func allowed(_ url: URL) -> Bool {
        url.scheme == "https" && url.host == "gea.esac.esa.int" && url.user == nil && url.password == nil && url.port == nil &&
        ["/tap-server/tap/sync", "/data-server/data"].contains(url.path)
    }
    static func network(_ url: URL) async throws -> Data {
        guard allowed(url) else { throw GaiaDataError.invalid("Unsupported Gaia endpoint") }
        let config = URLSessionConfiguration.ephemeral
        config.timeoutIntervalForRequest = 45; config.timeoutIntervalForResource = 90; config.httpShouldSetCookies = false
        let session = URLSession(configuration: config)
        defer { session.invalidateAndCancel() }
        var request = URLRequest(url: url); request.setValue("PhotonStack/GaiaDR3", forHTTPHeaderField: "User-Agent")
        let (bytes, response) = try await session.bytes(for: request)
        guard let http = response as? HTTPURLResponse, http.statusCode == 200, let final = http.url, allowed(final) else {
            throw GaiaDataError.invalid("Gaia service returned HTTP \((response as? HTTPURLResponse)?.statusCode ?? 0); retry the download")
        }
        let limit = url.path.contains("tap-server") ? 2 * 1024 * 1024 : 4 * 1024 * 1024
        guard response.expectedContentLength <= limit else { throw GaiaDataError.invalid("Gaia response exceeds the download limit") }
        var data = Data(), buffer: [UInt8] = []; buffer.reserveCapacity(8192)
        for try await byte in bytes {
            buffer.append(byte)
            if buffer.count == 8192 {
                try Task.checkCancellation(); data.append(contentsOf: buffer); buffer.removeAll(keepingCapacity: true)
                guard data.count <= limit else { throw GaiaDataError.invalid("Gaia response exceeds the download limit") }
            }
        }
        data.append(contentsOf: buffer)
        guard !data.isEmpty, data.count <= limit else { throw GaiaDataError.invalid("Empty or oversized Gaia response") }
        try Task.checkCancellation(); return data
    }
    func get(_ url: URL, refresh: Bool, validate: @Sendable (Data) throws -> Void = { _ in }) async throws -> (Data, Bool) {
        guard Self.allowed(url) else { throw GaiaDataError.invalid("Unsupported Gaia endpoint") }
        let index = root.appendingPathComponent(gaiaSHA(Data(url.absoluteString.utf8)) + ".json")
        if !refresh, let metadata = try? Self.read(index, limit: 16384),
           let record = try? JSONDecoder().decode(Receipt.self, from: metadata), record.url == url.absoluteString,
           SpectralCalibrationReport.digest(record.sha), let data = try? Self.read(root.appendingPathComponent(record.sha + ".data"), limit: 4 * 1024 * 1024), gaiaSHA(data) == record.sha {
            try Task.checkCancellation()
            if (try? validate(data)) != nil { return (data, true) }
        }
        let data = try await fetch(url)
        guard !data.isEmpty, data.count <= 4 * 1024 * 1024 else { throw GaiaDataError.invalid("Empty or oversized Gaia response") }
        try Task.checkCancellation()
        try validate(data)
        let sha = gaiaSHA(data), record = Receipt(url: url.absoluteString, sha: sha)
        try FileManager.default.createDirectory(at: root, withIntermediateDirectories: true)
        try data.write(to: root.appendingPathComponent(sha + ".data"), options: .atomic)
        try JSONEncoder().encode(record).write(to: index, options: .atomic)
        return (data, false)
    }
    static func read(_ url: URL, limit: Int) throws -> Data {
        let v = try url.resourceValues(forKeys: [.fileSizeKey, .isRegularFileKey, .isSymbolicLinkKey])
        guard v.isRegularFile == true, v.isSymbolicLink != true, let size = v.fileSize, size <= limit else { throw GaiaDataError.invalid("Invalid or oversized Gaia snapshot") }
        let data = try Data(contentsOf: url)
        guard data.count <= limit else { throw GaiaDataError.invalid("Gaia snapshot grew during read") }; return data
    }
}

actor GaiaSpectralDownload {
    struct File: Codable, Equatable, Sendable { var name, url, sha: String }
    struct Manifest: Codable, Sendable {
        var schema = 1
        var field: GaiaSpectralField
        var query: GaiaSpectralQuery
        var files: [File]
        var downloadedAt: Date
        var count: Int
        var skipped: [GaiaSpectralData.Skipped]
        var limited: Bool
        var catalogSHA, acquisitionSHA: String
    }
    struct Result: Sendable { var directory: URL; var manifest: Manifest; var cacheHits: Int }
    struct Progress: Sendable { var completed, total, cacheHits: Int }
    private let cache: GaiaResponseCache
    private let service: any ProcessingService
    init(cache: GaiaResponseCache = GaiaResponseCache(), service: any ProcessingService = CLIProcessingService.makeDefault()) {
        self.cache = cache; self.service = service
    }
    private func acquisition(_ files: [File]) -> String {
        gaiaSHA(Data(files.map { $0.name + "\n" + $0.url + "\n" + $0.sha }.joined(separator: "\n").utf8))
    }
    private func validateNative(_ file: URL, catalog: GaiaSpectralData.Catalog, query: GaiaSpectralQuery) async throws {
        struct Report: Decodable { var type, command, sha: String; var schema, count: Int; var epoch: Double }
        let response = try await service.runCLI(arguments: ["spcc-catalog-inspect", "--catalog", file.path, "--expected-sha", gaiaSHA(catalog.data)])
        let r = try JSONDecoder().decode(Report.self, from: Data(response.standardOutput.utf8))
        guard r.type == "complete", r.command == "spcc-catalog-inspect", r.schema == 1, r.sha == gaiaSHA(catalog.data), r.count == catalog.count, r.epoch == query.epoch else { throw GaiaDataError.invalid("Native catalog validation differs from the download") }
    }
    func download(source: URL, field: GaiaSpectralField, query: GaiaSpectralQuery, to output: URL, refresh: Bool,
                  progress: @Sendable (Progress) async -> Void) async throws -> Result {
        let fm = FileManager.default
        guard !fm.fileExists(atPath: output.path) else { throw GaiaDataError.invalid("Choose a new Gaia output directory") }
        let tap = try query.tapURL(field)
        let stage = output.deletingLastPathComponent().appendingPathComponent(".gaia-download-" + UUID().uuidString)
        try fm.createDirectory(at: stage, withIntermediateDirectories: false)
        defer { try? fm.removeItem(at: stage) }
        _ = try await GaiaSpectralField.read(source, expected: field.sha, service: service)
        await progress(.init(completed: 0, total: 1, cacheHits: 0))
        let (table, tapCached) = try await cache.get(tap, refresh: refresh) { _ = try GaiaSpectralData.candidates($0, query: query, field: field) }
        let candidates = try GaiaSpectralData.candidates(table, query: query, field: field)
        var hits = tapCached ? 1 : 0, files = [File(name: "gaia.xml", url: tap.absoluteString, sha: gaiaSHA(table))]
        try table.write(to: stage.appendingPathComponent("gaia.xml"), options: .withoutOverwriting)
        try candidates.csv.write(to: stage.appendingPathComponent("gaia.csv"), options: .withoutOverwriting)
        var spectra: [String: GaiaSpectralData.Spectrum] = [:]
        let batches = (candidates.stars.count + 39) / 40
        for start in stride(from: 0, to: candidates.stars.count, by: 40) {
            try Task.checkCancellation()
            await progress(.init(completed: start / 40 + 1, total: batches + 1, cacheHits: hits))
            let batch = Array(candidates.stars[start..<min(start + 40, candidates.stars.count)])
            let url = GaiaSpectralData.spectrumURL(batch.map(\.id)), (data, cached) = try await cache.get(url, refresh: refresh) { _ = try GaiaSpectralData.spectra($0, expected: batch) }
            let parsed = try GaiaSpectralData.spectra(data, expected: batch)
            for (id, spectrum) in parsed { spectra[id] = spectrum }
            let name = String(format: "xp-%04d.csv", start / 40)
            try data.write(to: stage.appendingPathComponent(name), options: .withoutOverwriting)
            files.append(File(name: name, url: url.absoluteString, sha: gaiaSHA(data))); if cached { hits += 1 }
        }
        let sha = acquisition(files), catalog = try GaiaSpectralData.catalog(candidates, spectra: spectra, query: query, provenance: sha)
        let catalogURL = stage.appendingPathComponent("catalog.txt")
        try catalog.data.write(to: catalogURL, options: .withoutOverwriting)
        try await validateNative(catalogURL, catalog: catalog, query: query)
        _ = try await GaiaSpectralField.read(source, expected: field.sha, service: service)
        let manifest = Manifest(field: field, query: query, files: files, downloadedAt: Date(), count: catalog.count,
            skipped: catalog.skipped, limited: candidates.limited, catalogSHA: gaiaSHA(catalog.data), acquisitionSHA: sha)
        let encoder = JSONEncoder(); encoder.outputFormatting = [.prettyPrinted, .sortedKeys]; encoder.dateEncodingStrategy = .iso8601
        try encoder.encode(manifest).write(to: stage.appendingPathComponent("query.json"), options: .withoutOverwriting)
        try Task.checkCancellation()
        guard renamex_np(stage.path, output.path, UInt32(RENAME_EXCL)) == 0 else { throw NSError(domain: NSPOSIXErrorDomain, code: Int(errno)) }
        await progress(.init(completed: batches + 1, total: batches + 1, cacheHits: hits))
        return Result(directory: output, manifest: manifest, cacheHits: hits)
    }
    func reopen(_ directory: URL, source: URL, field: GaiaSpectralField) async throws -> Result {
        let decoder = JSONDecoder(); decoder.dateDecodingStrategy = .iso8601
        let m = try decoder.decode(Manifest.self, from: GaiaResponseCache.read(directory.appendingPathComponent("query.json"), limit: 512 * 1024))
        guard m.schema == 1, m.field == field, m.query.valid, (1...1000).contains(m.count),
              (2...26).contains(m.files.count), m.files[0].name == "gaia.xml", m.files[0].url == (try m.query.tapURL(field)).absoluteString,
              acquisition(m.files) == m.acquisitionSHA else { throw GaiaDataError.invalid("Gaia snapshot belongs to another source or has invalid provenance") }
        func load(_ file: File) throws -> Data {
            guard SpectralCalibrationReport.digest(file.sha) else { throw GaiaDataError.invalid("Invalid Gaia snapshot digest") }
            let data = try GaiaResponseCache.read(directory.appendingPathComponent(file.name), limit: 4 * 1024 * 1024)
            guard gaiaSHA(data) == file.sha else { throw GaiaDataError.invalid("Gaia snapshot was changed: \(file.name)") }; return data
        }
        let candidates = try GaiaSpectralData.candidates(load(m.files[0]), query: m.query, field: field)
        guard m.files.count == 1 + (candidates.stars.count + 39) / 40 else { throw GaiaDataError.invalid("Incomplete Gaia spectrum batches") }
        var spectra: [String: GaiaSpectralData.Spectrum] = [:]
        for (index, file) in m.files.dropFirst().enumerated() {
            let batch = Array(candidates.stars[index * 40..<min((index + 1) * 40, candidates.stars.count)])
            guard file.name == String(format: "xp-%04d.csv", index), file.url == GaiaSpectralData.spectrumURL(batch.map(\.id)).absoluteString else { throw GaiaDataError.invalid("Gaia batch does not match the query") }
            for (id, spectrum) in try GaiaSpectralData.spectra(load(file), expected: batch) { spectra[id] = spectrum }
        }
        let catalog = try GaiaSpectralData.catalog(candidates, spectra: spectra, query: m.query, provenance: m.acquisitionSHA)
        let catalogURL = directory.appendingPathComponent("catalog.txt")
        guard catalog.count == m.count, catalog.skipped == m.skipped, candidates.limited == m.limited,
              gaiaSHA(catalog.data) == m.catalogSHA,
              try GaiaResponseCache.read(catalogURL, limit: 64 * 1024 * 1024) == catalog.data,
              try GaiaResponseCache.read(directory.appendingPathComponent("gaia.csv"), limit: 2 * 1024 * 1024) == candidates.csv else { throw GaiaDataError.invalid("Saved Gaia catalog differs from its original data or epoch") }
        try await validateNative(catalogURL, catalog: catalog, query: m.query)
        _ = try await GaiaSpectralField.read(source, expected: field.sha, service: service)
        return Result(directory: directory, manifest: m, cacheHits: m.files.count)
    }
}
