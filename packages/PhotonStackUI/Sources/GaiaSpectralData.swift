import CryptoKit
import Foundation
#if canImport(FoundationXML)
import FoundationXML
#endif
import PhotonStackProcessing

enum GaiaDataError: LocalizedError {
    case invalid(String)
    var errorDescription: String? { if case .invalid(let message) = self { return message }; return nil }
}
func gaiaSHA(_ data: Data) -> String { SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined() }

struct GaiaSpectralField: Codable, Equatable, Sendable {
    var type, command, sha: String
    var schema, width, height: Int
    var ra, dec, radius: Double
    var valid: Bool {
        type == "complete" && command == "spcc-field" && schema == 1 && SpectralCalibrationReport.digest(sha) &&
        (16...100_000).contains(width) && (16...100_000).contains(height) &&
        [ra, dec, radius].allSatisfy(\.isFinite) && (0..<360).contains(ra) && (-90...90).contains(dec) && radius > 0 && radius <= 20
    }
    static func read(_ source: URL, expected: String? = nil, service: any ProcessingService) async throws -> Self {
        var args = ["spcc-field", "--input", source.path]
        if let expected { args += ["--expected-sha", expected] }
        let response = try await service.runCLI(arguments: args)
        let value = try JSONDecoder().decode(Self.self, from: Data(response.standardOutput.utf8))
        guard value.valid, expected == nil || value.sha == expected else { throw GaiaDataError.invalid("Invalid solved field report") }
        try Task.checkCancellation(); return value
    }
}

struct GaiaSpectralQuery: Codable, Equatable, Sendable {
    var observationUTC = ""
    var minimumMagnitude = 9.0, maximumMagnitude = 15.0
    var maximumStars = 400
    /// UTC Julian year, matching the explicit offline importer. No unknown
    /// distance/radial velocity or apparent-place correction is invented.
    var epoch: Double? {
        let pattern = #"^\d{4}-\d{2}-\d{2}(T\d{2}:\d{2}:\d{2}(\.\d{1,9})?Z?)?$"#
        guard observationUTC.range(of: pattern, options: .regularExpression) != nil else { return nil }
        let text = observationUTC.count == 10 ? observationUTC + "T00:00:00Z" : observationUTC + (observationUTC.hasSuffix("Z") ? "" : "Z")
        let base = String(text.prefix(19)), f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX"); f.calendar = Calendar(identifier: .gregorian)
        f.timeZone = TimeZone(secondsFromGMT: 0); f.isLenient = false; f.dateFormat = "yyyy-MM-dd'T'HH:mm:ss"
        guard let date = f.date(from: base), f.string(from: date) == base else { return nil }
        let fraction = text.dropFirst(19).dropLast()
        let seconds = fraction.isEmpty ? 0 : Double(fraction)
        guard let seconds, (0..<1).contains(seconds) else { return nil }
        let year = 2000 + ((date.timeIntervalSince1970 + seconds) / 86400 + 2440587.5 - 2451545) / 365.25
        return (1800...2200).contains(year) ? year : nil
    }
    var valid: Bool {
        epoch != nil && minimumMagnitude.isFinite && maximumMagnitude.isFinite &&
        minimumMagnitude >= 3 && maximumMagnitude <= 18 && minimumMagnitude < maximumMagnitude && (20...1000).contains(maximumStars)
    }
    func radius(_ field: GaiaSpectralField) -> Double { field.radius + 0.01 + abs((epoch ?? 2016) - 2016) * 20 / 3600 }
    func tapURL(_ field: GaiaSpectralField) throws -> URL {
        guard valid, field.valid else { throw GaiaDataError.invalid("Check UTC observation date, magnitude limits and source count") }
        let query = "SELECT TOP \(maximumStars + 1) source_id,ra,dec,pmra,pmdec,ref_epoch,phot_g_mean_mag FROM gaiadr3.gaia_source WHERE has_xp_sampled = 'True' AND phot_g_mean_mag BETWEEN \(minimumMagnitude) AND \(maximumMagnitude) AND 1=CONTAINS(POINT('ICRS',ra,dec),CIRCLE('ICRS',\(field.ra),\(field.dec),\(radius(field)))) ORDER BY phot_g_mean_mag,source_id"
        // Request TABLEDATA explicitly; the default may switch to BINARY2.
        // Keep MAXREC above TOP so a complete bounded query is not flagged as
        // server overflow. The extra TOP row detects our own candidate limit.
        return GaiaSpectralData.url(path: "/tap-server/tap/sync", parameters: ["REQUEST": "doQuery", "LANG": "ADQL", "FORMAT": "votable_plain", "MAXREC": String(maximumStars + 2), "QUERY": query])
    }
}

enum GaiaSpectralData {
    struct Star: Sendable { var id: String; var ra, dec, pmra, pmdec: Double; var magnitude: Double }
    struct Spectrum: Sendable { var ra, dec: Double; var flux, error: [Double] }
    struct Skipped: Codable, Equatable, Sendable { var id, reason: String }
    struct Candidates: Sendable { var stars: [Star]; var skipped: [Skipped]; var limited: Bool; var csv: Data }
    struct Catalog: Sendable { var data: Data; var count: Int; var skipped: [Skipped] }
    static func url(path: String, parameters: [String: String]) -> URL {
        var c = URLComponents(); c.scheme = "https"; c.host = "gea.esac.esa.int"; c.path = path
        c.queryItems = parameters.sorted { $0.key < $1.key }.map { URLQueryItem(name: $0.key, value: $0.value) }
        return c.url!
    }
    static func spectrumURL(_ ids: [String]) -> URL {
        url(path: "/data-server/data", parameters: ["RETRIEVAL_TYPE": "XP_SAMPLED", "ID": ids.joined(separator: ","), "RELEASE": "Gaia DR3", "FORMAT": "csv", "DATA_STRUCTURE": "DATAMODEL_GAIA"])
    }
    static func id(_ s: String) throws -> String {
        guard !s.isEmpty, s.utf8.allSatisfy({ (48...57).contains($0) }), let n = UInt64(s), n > 0, n <= UInt64(Int64.max), String(n) == s else { throw GaiaDataError.invalid("Invalid Gaia DR3 source ID") }
        return s
    }
    static func number(_ s: String) throws -> Double {
        guard let n = Double(s.trimmingCharacters(in: .whitespaces)), n.isFinite else { throw GaiaDataError.invalid("Nonfinite or missing Gaia value") }; return n
    }
    static func separation(_ ra: Double, _ dec: Double, _ r: Double, _ d: Double) -> Double {
        let rad = Double.pi / 180
        let a = pow(sin((dec-d)*rad/2), 2) + cos(dec*rad)*cos(d*rad)*pow(sin((ra-r)*rad/2), 2)
        return 2 * asin(sqrt(min(1, max(0, a)))) / rad
    }
    static func candidates(_ data: Data, query: GaiaSpectralQuery, field: GaiaSpectralField) throws -> Candidates {
        guard query.valid, field.valid else { throw GaiaDataError.invalid("Invalid Gaia query") }
        let rows = try GaiaVOTable.read(data)
        guard !rows.isEmpty, rows.count <= query.maximumStars + 1 else { throw GaiaDataError.invalid("Empty or oversized Gaia result") }
        var stars: [Star] = [], skipped: [Skipped] = [], ids = Set<String>()
        let keys = ["source_id", "ra", "dec", "pmra", "pmdec", "ref_epoch", "phot_g_mean_mag"]
        var csv = keys.joined(separator: ",") + "\n"
        var previousMagnitude = -Double.infinity, previousID: UInt64 = 0
        for (index, row) in rows.enumerated() {
            guard keys.allSatisfy({ row[$0] != nil }) else { throw GaiaDataError.invalid("Missing Gaia astrometry columns") }
            let sid = try id(row["source_id"]!), ra = try number(row["ra"]!), dec = try number(row["dec"]!), mag = try number(row["phot_g_mean_mag"]!)
            guard ids.insert(sid).inserted, (0..<360).contains(ra), (-90...90).contains(dec),
                  try number(row["ref_epoch"]!) == 2016, (query.minimumMagnitude...query.maximumMagnitude).contains(mag),
                  separation(ra, dec, field.ra, field.dec) <= query.radius(field) + 1e-7,
                  mag > previousMagnitude || (mag == previousMagnitude && UInt64(sid)! > previousID) else { throw GaiaDataError.invalid("Gaia result does not match its query or reference epoch") }
            previousMagnitude = mag; previousID = UInt64(sid)!
            guard index < query.maximumStars else { continue }
            csv += keys.map { row[$0]! }.joined(separator: ",") + "\n"
            if row["pmra"]!.isEmpty || row["pmdec"]!.isEmpty { skipped.append(.init(id: sid, reason: "proper-motion-missing")); continue }
            let ma = try number(row["pmra"]!), md = try number(row["pmdec"]!)
            if hypot(ma, md) > 20000 { skipped.append(.init(id: sid, reason: "proper-motion-outside-query-margin")); continue }
            stars.append(Star(id: sid, ra: ra, dec: dec, pmra: ma, pmdec: md, magnitude: mag))
        }
        guard !stars.isEmpty else { throw GaiaDataError.invalid("No Gaia candidates with supported proper motions") }
        return Candidates(stars: stars, skipped: skipped, limited: rows.count > query.maximumStars, csv: Data(csv.utf8))
    }
    static func spectra(_ data: Data, expected: [Star]) throws -> [String: Spectrum] {
        let rows = try GaiaCSV.read(data), stars = Dictionary(uniqueKeysWithValues: expected.map { ($0.id, $0) })
        guard rows.count <= expected.count else { throw GaiaDataError.invalid("Unexpected Gaia spectra") }
        var result: [String: Spectrum] = [:]
        for row in rows {
            guard let sid = row["source_id"], let star = stars[sid], result[sid] == nil,
                  let ras = row["ra"], let decs = row["dec"], let fs = row["flux"], let es = row["flux_error"] else { throw GaiaDataError.invalid("Missing, duplicate or unrequested Gaia spectrum") }
            let ra = try number(ras), dec = try number(decs)
            guard (0..<360).contains(ra), (-90...90).contains(dec), separation(ra, dec, star.ra, star.dec) <= 1e-6 else { throw GaiaDataError.invalid("XP spectrum position does not match its Gaia source ID") }
            func samples(_ s: String) throws -> [Double] {
                guard s.first == "(", s.last == ")" else { throw GaiaDataError.invalid("Expected a Gaia sampled CSV array") }
                let a = s.dropFirst().dropLast().split(separator: ",", omittingEmptySubsequences: false)
                guard a.count == 343 else { throw GaiaDataError.invalid("Gaia XP requires 343 samples from 336 to 1020 nm") }
                return try a.map { try number(String($0)) }
            }
            let flux = try samples(fs), error = try samples(es)
            guard error.allSatisfy({ $0 >= 0 }) else { throw GaiaDataError.invalid("Negative Gaia flux uncertainty") }
            result[sid] = Spectrum(ra: ra, dec: dec, flux: flux, error: error)
        }
        return result
    }
    static func propagate(_ star: Star, epoch: Double) -> (Double, Double) {
        let rad = Double.pi / 180, r = star.ra * rad, d = star.dec * rad
        let dt = (epoch - 2016) * rad / 3.6e6, ma = star.pmra, md = star.pmdec
        let x = cos(d)*cos(r) + dt*(-ma*sin(r) - md*sin(d)*cos(r))
        let y = cos(d)*sin(r) + dt*(ma*cos(r) - md*sin(d)*sin(r))
        let z = sin(d) + dt*md*cos(d)
        return ((atan2(y, x)/rad + 360).truncatingRemainder(dividingBy: 360), atan2(z, hypot(x, y))/rad)
    }
    static func catalog(_ candidates: Candidates, spectra: [String: Spectrum], query: GaiaSpectralQuery, provenance: String) throws -> Catalog {
        guard query.valid, SpectralCalibrationReport.digest(provenance) else { throw GaiaDataError.invalid("Invalid catalog provenance") }
        var rows = "", count = 0, skipped = candidates.skipped
        for star in candidates.stars {
            guard let s = spectra[star.id] else { skipped.append(.init(id: star.id, reason: "spectrum-missing")); continue }
            let (ra, dec) = propagate(star, epoch: query.epoch!)
            rows += "\"\(star.id)\" \(ra) \(dec) 343\n"
            for i in 0..<343 { rows += "\(336+i*2) \(s.flux[i]) \(s.error[i])\n" }
            count += 1
        }
        guard count > 0 else { throw GaiaDataError.invalid("No usable Gaia XP spectra were returned") }
        let header = "PhotonStack-SpectralCatalog-1\n\"Gaia DR3 XP sampled\" \(query.epoch!) \"ESA Gaia DR3; 336..1020 nm step 2; F_lambda W m^-2 nm^-1; normalized tangent proper motion from J2016.0; observation UTC \(query.observationUTC); acquisition SHA256 \(provenance)\" \"ESA/Gaia/DPAC; https://www.cosmos.esa.int/web/gaia-users/credits\"\n\(count)\n"
        return Catalog(data: Data((header + rows).utf8), count: count, skipped: skipped)
    }
}

/// TABLEDATA only, with explicit success status. XML error/overflow and binary
/// responses are rejected instead of silently treating a partial table as full.
private final class GaiaVOTable: NSObject, XMLParserDelegate {
    var fields: [String] = [], rows: [[String: String]] = [], statuses: [String] = []
    var cells: [String] = [], cell = "", inCell = false, bad = false, tables = 0
    static func read(_ data: Data) throws -> [[String: String]] {
        guard data.count <= 2 * 1024 * 1024, let text = String(data: data, encoding: .utf8),
              !text.contains("<!DOCTYPE"), !text.contains("<!ENTITY") else { throw GaiaDataError.invalid("Invalid or oversized Gaia table") }
        let delegate = GaiaVOTable(), parser = XMLParser(data: data)
        parser.delegate = delegate; parser.shouldResolveExternalEntities = false; parser.shouldProcessNamespaces = true
        guard parser.parse(), !delegate.bad, delegate.tables == 1, !delegate.fields.isEmpty,
              !delegate.statuses.isEmpty, delegate.statuses.allSatisfy({ $0 == "OK" }) else { throw GaiaDataError.invalid("Gaia query failed, overflowed or returned an unsupported table") }
        return delegate.rows
    }
    func parser(_ p: XMLParser, didStartElement name: String, namespaceURI: String?, qualifiedName: String?, attributes a: [String: String]) {
        if name == "TABLE" { tables += 1 }
        if name == "FIELD" {
            guard let value = a["name"], !fields.contains(value) else { bad = true; return }
            let units = ["ra": "deg", "dec": "deg", "pmra": "mas.yr**-1", "pmdec": "mas.yr**-1", "ref_epoch": "yr", "phot_g_mean_mag": "mag"]
            if let unit = units[value], a["unit"] != unit { bad = true }
            if value == "source_id", a["datatype"] != "long" { bad = true }
            fields.append(value)
        }
        if name == "INFO", a["name"] == "QUERY_STATUS" { statuses.append(a["value"] ?? "") }
        if ["BINARY", "BINARY2", "FITS"].contains(name) { bad = true }
        if name == "TR" { cells = [] }
        if name == "TD" { inCell = true; cell = "" }
    }
    func parser(_ p: XMLParser, foundCharacters text: String) { if inCell { cell += text } }
    func parser(_ p: XMLParser, didEndElement name: String, namespaceURI: String?, qualifiedName: String?) {
        if name == "TD" { inCell = false; cells.append(cell.trimmingCharacters(in: .whitespacesAndNewlines)) }
        if name == "TR" {
            guard cells.count == fields.count, rows.count < 1001 else { bad = true; p.abortParsing(); return }
            rows.append(Dictionary(uniqueKeysWithValues: zip(fields, cells)))
        }
    }
}

private enum GaiaCSV {
    static func read(_ data: Data) throws -> [[String: String]] {
        guard data.count <= 4 * 1024 * 1024, let text = String(data: data, encoding: .utf8), !text.contains("\0") else { throw GaiaDataError.invalid("Invalid Gaia CSV response") }
        var records: [[String]] = [], fields: [String] = [], field: [UInt8] = [], quoted = false, closed = false
        let bytes = Array(text.utf8); var i = 0
        func finishField() { fields.append(String(decoding: field, as: UTF8.self)); field = []; closed = false }
        while i < bytes.count {
            let b = bytes[i]
            if quoted {
                if b == 34 { if i+1 < bytes.count && bytes[i+1] == 34 { field.append(34); i += 1 } else { quoted = false; closed = true } }
                else { field.append(b) }
            } else if b == 34 && field.isEmpty && !closed { quoted = true }
            else if b == 44 { finishField() }
            else if b == 10 || b == 13 {
                finishField(); records.append(fields); fields = []
                if b == 13 && i+1 < bytes.count && bytes[i+1] == 10 { i += 1 }
            } else { guard !closed && b != 34 else { throw GaiaDataError.invalid("Invalid CSV quoting") }; field.append(b) }
            i += 1
        }
        guard !quoted else { throw GaiaDataError.invalid("Truncated Gaia CSV array") }
        if !field.isEmpty || !fields.isEmpty || closed { finishField(); records.append(fields) }
        guard let header = records.first, !header.isEmpty, Set(header).count == header.count, header.contains("source_id") else { throw GaiaDataError.invalid("Missing Gaia CSV header") }
        return try records.dropFirst().map { row in
            guard row.count == header.count else { throw GaiaDataError.invalid("Truncated Gaia CSV row") }
            return Dictionary(uniqueKeysWithValues: zip(header, row))
        }
    }
}
