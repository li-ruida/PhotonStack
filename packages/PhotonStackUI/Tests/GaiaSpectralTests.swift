import Foundation
import Testing
import PhotonStackProcessing
@testable import PhotonStackUI

// Explicit opt-in: ordinary test runs never access ESA or write persistent data.
@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_GAIA_LIVE_OUTPUT"] != nil))
func gaiaLiveDownloadAndOfflineReopen() async throws {
    let env = ProcessInfo.processInfo.environment
    let source = URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_SPCC_SOURCE"]))
    let output = URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_GAIA_LIVE_OUTPUT"]))
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(env["PHOTONSTACK_TEST_CLI"])))
    let field = try await GaiaSpectralField.read(source, service: service)
    let query = GaiaSpectralQuery(observationUTC: try #require(env["PHOTONSTACK_TEST_GAIA_DATE"]))
    let cacheRoot = output.deletingLastPathComponent().appendingPathComponent("live-cache")
    let downloader = GaiaSpectralDownload(cache: GaiaResponseCache(root: cacheRoot), service: service)
    let result = try await downloader.download(source: source, field: field, query: query, to: output, refresh: false) { _ in }
    #expect(result.manifest.count > 0)
    let offline = GaiaSpectralDownload(cache: GaiaResponseCache(root: cacheRoot) { _ in
        throw GaiaDataError.invalid("Offline reopen must not access the network")
    }, service: service)
    let reopened = try await offline.reopen(output, source: source, field: field)
    #expect(reopened.manifest.catalogSHA == result.manifest.catalogSHA)
    #expect(reopened.manifest.count == result.manifest.count)
}

private func gaiaField() -> GaiaSpectralField {
    .init(type: "complete", command: "spcc-field", sha: String(repeating: "a", count: 64), schema: 1, width: 360, height: 360, ra: 30, dec: 20, radius: 1)
}
private func gaiaXML(_ rows: [[String]], status: String = "OK") -> Data {
    let names = ["source_id", "ra", "dec", "pmra", "pmdec", "ref_epoch", "phot_g_mean_mag"]
    let units = ["", "deg", "deg", "mas.yr**-1", "mas.yr**-1", "yr", "mag"]
    let fields = names.enumerated().map { "<FIELD name=\"\($0.element)\" datatype=\"\($0.offset == 0 ? "long" : "double")\" unit=\"\(units[$0.offset])\"/>" }.joined()
    return Data(("<VOTABLE><RESOURCE><INFO name=\"QUERY_STATUS\" value=\"\(status)\"/><TABLE>" + fields + "<DATA><TABLEDATA>" + rows.map { "<TR>" + $0.map { "<TD>\($0)</TD>" }.joined() + "</TR>" }.joined() + "</TABLEDATA></DATA></TABLE></RESOURCE></VOTABLE>").utf8)
}
private func gaiaXP(_ ids: [String]) -> Data {
    let flux = "(" + (0..<343).map { $0 == 0 ? "-1e-15" : "2e-15" }.joined(separator: ", ") + ")"
    let error = "(" + Array(repeating: "1e-17", count: 343).joined(separator: ",") + ")"
    return Data(("source_id,solution_id,ra,dec,flux,flux_error\r\n" + ids.map { "\($0),1,30,20,\"\(flux)\",\"\(error)\"\r\n" }.joined()).utf8)
}

@Test func gaiaEpochParsingAndQueryBoundaries() throws {
    var q = GaiaSpectralQuery(observationUTC: "2000-01-01T12:00:00Z", maximumStars: 20)
    #expect(q.epoch == 2000 && q.valid)
    q.observationUTC = "2000-01-01T12:00:00.5"; #expect(abs(try #require(q.epoch) - (2000 + 0.5 / 31557600)) < 1e-12)
    for bad in ["", "2025-02-29", "2024-02-30", "2026-01-01T12:00:60", "2026-01-01T24:00:00", "now", "2026-01-01T12:00:00+08:00"] { q.observationUTC = bad; #expect(!q.valid) }
    q.observationUTC = "2024-02-29"; #expect(q.valid)
    let url = try q.tapURL(gaiaField())
    #expect(GaiaResponseCache.allowed(url))
    let items = URLComponents(url: url, resolvingAgainstBaseURL: false)!.queryItems!
    #expect(items.contains { $0.name == "QUERY" && $0.value!.contains("TOP 21") && $0.value!.contains("gaiadr3.gaia_source") })
    #expect(items.contains { $0.name == "FORMAT" && $0.value == "votable_plain" })
    #expect(items.contains { $0.name == "MAXREC" && $0.value == "22" })
    q.maximumStars = 1001; #expect(!q.valid)
    #expect(!GaiaResponseCache.allowed(URL(string: "https://example.com/data-server/data")!))
}

@Test func gaiaTablesPreserveIDsAndRejectWrongUnitsIncompleteAndMismatchedSpectra() throws {
    let field = gaiaField(), q = GaiaSpectralQuery(observationUTC: "2026-08-18T20:04:18.382445", maximumStars: 20)
    let id = "368697928472451200"
    let rows = [[id, "30", "20", "1", "-2", "2016", "10"], ["368697928472451201", "30.01", "20", "", "", "2016", "11"]]
    let xml = gaiaXML(rows), candidates = try GaiaSpectralData.candidates(xml, query: q, field: field)
    #expect(candidates.stars.map(\.id) == [id]); #expect(candidates.skipped.first?.reason == "proper-motion-missing")
    for bad in [gaiaXML(rows, status: "OVERFLOW"), gaiaXML(rows + [rows[0]]), Data(String(decoding: xml, as: UTF8.self).replacingOccurrences(of: "mas.yr**-1", with: "deg.yr**-1").utf8), Data("<html>service failure</html>".utf8), Data(xml.dropLast(20))] {
        #expect(throws: (any Error).self) { try GaiaSpectralData.candidates(bad, query: q, field: field) }
    }
    let xp = gaiaXP([id]), spectra = try GaiaSpectralData.spectra(xp, expected: candidates.stars)
    #expect(spectra[id]?.flux[0] == -1e-15 && spectra[id]?.error.count == 343)
    for bad in [gaiaXP([id, id]), gaiaXP(["123"]), Data(String(decoding: xp, as: UTF8.self).replacingOccurrences(of: ",30,20,", with: ",31,20,").utf8), Data(String(decoding: xp, as: UTF8.self).replacingOccurrences(of: "1e-17", with: "-1e-17").utf8), Data(xp.dropLast(10))] {
        #expect(throws: (any Error).self) { try GaiaSpectralData.spectra(bad, expected: candidates.stars) }
    }
    let catalog = try GaiaSpectralData.catalog(candidates, spectra: spectra, query: q, provenance: String(repeating: "b", count: 64))
    #expect(catalog.count == 1 && catalog.skipped.count == 1)
    #expect(String(decoding: catalog.data, as: UTF8.self).contains("336 -1e-15 1e-17"))
    let moving = GaiaSpectralData.Star(id: "123", ra: 359.9999, dec: 0, pmra: 1000, pmdec: 0, magnitude: 10)
    let (ra, dec) = GaiaSpectralData.propagate(moving, epoch: 2026)
    let expected = (359.9999 + atan(10 * Double.pi / 180 / 3600) * 180 / Double.pi).truncatingRemainder(dividingBy: 360)
    #expect(abs(ra - expected) < 1e-12 && dec == 0)
}

private actor GaiaFixtureServer {
    var calls = 0
    let table: Data
    var waiting = false
    init(table: Data) { self.table = table }
    func fetch(_ url: URL) async throws -> Data {
        calls += 1
        if waiting { try await Task.sleep(for: .seconds(30)) }
        if url.path.contains("tap-server") { return table }
        let ids = URLComponents(url: url, resolvingAgainstBaseURL: false)!.queryItems!.first { $0.name == "ID" }!.value!.split(separator: ",").map(String.init)
        return gaiaXP(ids)
    }
    func waitNext() { waiting = true }
}

@Test(.enabled(if: ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"] != nil && ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_SPCC_SOURCE"] != nil))
func gaiaDownloadReopenCacheCancellationAndSourceGuards() async throws {
    let fm = FileManager.default, root = fm.temporaryDirectory.appendingPathComponent("Gaia integration ' \(UUID())")
    try fm.createDirectory(at: root, withIntermediateDirectories: true); defer { try? fm.removeItem(at: root) }
    let service = CLIProcessingService(executableURL: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_CLI"])))
    let source = root.appendingPathComponent("source.fits")
    try fm.copyItem(at: URL(fileURLWithPath: try #require(ProcessInfo.processInfo.environment["PHOTONSTACK_TEST_SPCC_SOURCE"])), to: source)
    let field = try await GaiaSpectralField.read(source, service: service)
    // Fixture sources are at the actual query center; spectrum fixture must use
    // matching raw positions. No generated flux is claimed to be real Gaia data.
    let rows = (1...21).map { [String(1000+$0), String(field.ra), String(field.dec), "1", "-2", "2016", String(9.0+Double($0)/100)] }
    let server = GaiaFixtureServer(table: gaiaXML(rows))
    let cacheRoot = root.appendingPathComponent("cache")
    let cache = GaiaResponseCache(root: cacheRoot) { url in
        let data = try await server.fetch(url)
        if url.path.contains("tap-server") { return data }
        return Data(String(decoding: data, as: UTF8.self).replacingOccurrences(of: ",30,20,", with: ",\(field.ra),\(field.dec),").utf8)
    }
    let downloader = GaiaSpectralDownload(cache: cache, service: service)
    let query = GaiaSpectralQuery(observationUTC: "2026-08-18T20:04:18.382445", maximumStars: 20)
    let output = root.appendingPathComponent("output")
    let result = try await downloader.download(source: source, field: field, query: query, to: output, refresh: false) { _ in }
    #expect(result.manifest.count == 20 && result.manifest.limited && result.cacheHits == 0)
    #expect(await server.calls == 2)
    let reopened = try await downloader.reopen(output, source: source, field: field)
    #expect(reopened.manifest.catalogSHA == result.manifest.catalogSHA)
    #expect(await server.calls == 2)
    let second = try await downloader.download(source: source, field: field, query: query, to: root.appendingPathComponent("cached"), refresh: false) { _ in }
    #expect(second.cacheHits == 2 && second.manifest.catalogSHA == result.manifest.catalogSHA)
    #expect(await server.calls == 2)
    do { _ = try await downloader.download(source: source, field: field, query: query, to: output, refresh: false) { _ in }; Issue.record("Existing result replaced") } catch { }
    let catalog = output.appendingPathComponent("catalog.txt"), original = try Data(contentsOf: catalog)
    try (original + Data("changed".utf8)).write(to: catalog)
    do { _ = try await downloader.reopen(output, source: source, field: field); Issue.record("Changed derived catalog reused") } catch { }
    try original.write(to: catalog)
    let raw = output.appendingPathComponent("xp-0000.csv"), rawBytes = try Data(contentsOf: raw)
    try Data("changed".utf8).write(to: raw)
    do { _ = try await downloader.reopen(output, source: source, field: field); Issue.record("Changed raw spectrum reused") } catch { }
    try rawBytes.write(to: raw)
    // Corrupt cache data is fetched afresh, never supplied as a valid response.
    let cachedPayload = cacheRoot.appendingPathComponent(result.manifest.files[0].sha + ".data")
    try Data("bad cache".utf8).write(to: cachedPayload)
    _ = try await downloader.download(source: source, field: field, query: query, to: root.appendingPathComponent("repaired"), refresh: false) { _ in }
    #expect(await server.calls == 3)
    await server.waitNext()
    let cancelled = root.appendingPathComponent("cancelled")
    let pending = Task { try await downloader.download(source: source, field: field, query: query, to: cancelled, refresh: true) { _ in } }
    let deadline = Date().addingTimeInterval(5)
    while await server.calls < 4, Date() < deadline { try await Task.sleep(for: .milliseconds(10)) }
    #expect(await server.calls == 4)
    pending.cancel()
    do { _ = try await pending.value; Issue.record("Cancelled download succeeded") } catch { }
    #expect(!fm.fileExists(atPath: cancelled.path))
    #expect(try fm.contentsOfDirectory(atPath: root.path).allSatisfy { !$0.hasPrefix(".gaia-download-") })
    var bytes = try Data(contentsOf: source); bytes[bytes.count-1] ^= 1; try bytes.write(to: source)
    do { _ = try await downloader.reopen(output, source: source, field: field); Issue.record("Changed master reused") } catch { }
}
