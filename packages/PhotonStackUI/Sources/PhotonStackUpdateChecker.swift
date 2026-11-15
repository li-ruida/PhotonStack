import Foundation
import SwiftUI

@MainActor
public final class PhotonStackUpdateChecker: ObservableObject {
    enum Status: Equatable {
        case idle, checking, current, noRelease, failed
        case available(String)
    }

    static let automaticKey = "checkForUpdatesOnLaunch"
    static let releasesPage = URL(string: "https://github.com/li-ruida/PhotonStack/releases")!
    private let defaults: UserDefaults
    private let fetchTag: () async throws -> String?
    let installedVersion: String
    private var checkedOnLaunch = false
    private var notifyOnCompletion = false
    @Published private(set) var status = Status.idle
    @Published var launchNotice: String?
    @Published var automatic: Bool {
        didSet {
            defaults.set(automatic, forKey: Self.automaticKey)
            if !automatic {
                notifyOnCompletion = false
                launchNotice = nil
            }
        }
    }

    public convenience init() {
        self.init(version: PhotonStackBuildIdentity.current.version, defaults: .standard,
                  fetchTag: Self.latestTag)
    }

    init(version: String, defaults: UserDefaults, fetchTag: @escaping () async throws -> String?) {
        self.installedVersion = version
        self.defaults = defaults
        self.fetchTag = fetchTag
        defaults.register(defaults: [Self.automaticKey: true])
        self.automatic = defaults.bool(forKey: Self.automaticKey)
    }

    func checkOnLaunch() async {
        guard !checkedOnLaunch else { return }
        checkedOnLaunch = true
        if automatic { await check(atLaunch: true) }
    }

    func check(atLaunch: Bool = false) async {
        guard status != .checking else { return }
        status = .checking
        notifyOnCompletion = atLaunch && automatic
        do {
            guard let tag = try await fetchTag() else {
                status = .noRelease
                return
            }
            guard let remote = Self.versionComponents(tag),
                  let installed = Self.versionComponents(installedVersion) else {
                status = .failed
                return
            }
            if installed.lexicographicallyPrecedes(remote) {
                status = .available(tag)
                if notifyOnCompletion { launchNotice = tag }
            } else {
                status = .current
            }
        } catch {
            status = .failed
        }
    }

    // Stable release tags use numeric major, minor and patch components.
    private static func versionComponents(_ value: String) -> [Int]? {
        let version = value.hasPrefix("v") ? value.dropFirst() : value[...]
        let parts = version.split(separator: ".", omittingEmptySubsequences: false)
        guard (1...3).contains(parts.count) else { return nil }
        var numbers: [Int] = []
        for part in parts {
            guard !part.isEmpty, part.utf8.allSatisfy({ (48...57).contains($0) }),
                  let number = Int(part) else { return nil }
            numbers.append(number)
        }
        return numbers + Array(repeating: 0, count: 3 - numbers.count)
    }

    private static func latestTag() async throws -> String? {
        struct Release: Decodable { let tag_name: String }
        let url = URL(string: "https://api.github.com/repos/li-ruida/PhotonStack/releases/latest")!
        var request = URLRequest(url: url, cachePolicy: .reloadIgnoringLocalCacheData, timeoutInterval: 15)
        request.setValue("application/vnd.github+json", forHTTPHeaderField: "Accept")
        request.setValue("PhotonStack", forHTTPHeaderField: "User-Agent")
        let (data, response) = try await URLSession.shared.data(for: request)
        guard let http = response as? HTTPURLResponse else { throw URLError(.badServerResponse) }
        if http.statusCode == 404 { return nil }
        guard http.statusCode == 200 else { throw URLError(.badServerResponse) }
        return try JSONDecoder().decode(Release.self, from: data).tag_name
    }

    static func releasePage(tag: String) -> URL {
        releasesPage.appendingPathComponent("tag").appendingPathComponent(tag)
    }
}

public extension View {
    func checkPhotonStackUpdatesOnLaunch(_ checker: PhotonStackUpdateChecker, chinese: Bool) -> some View {
        modifier(LaunchUpdateNotice(checker: checker, chinese: chinese))
    }
}

private struct LaunchUpdateNotice: ViewModifier {
    @ObservedObject var checker: PhotonStackUpdateChecker
    @Environment(\.openURL) private var openURL
    var chinese: Bool

    func body(content: Content) -> some View {
        content
            .task { await checker.checkOnLaunch() }
            .alert(chinese ? "发现 PhotonStack 新版本" : "PhotonStack update available",
                   isPresented: Binding(get: { checker.launchNotice != nil },
                                        set: { if !$0 { checker.launchNotice = nil } }),
                   presenting: checker.launchNotice) { tag in
                Button(chinese ? "查看新版" : "View release") {
                    openURL(PhotonStackUpdateChecker.releasePage(tag: tag))
                }
                Button(chinese ? "稍后" : "Later", role: .cancel) {}
            } message: { tag in
                Text(chinese
                     ? "当前版本为 \(checker.installedVersion)，GitHub 已发布 \(tag)。"
                     : "You have \(checker.installedVersion). Version \(tag) is available on GitHub.")
            }
    }
}
