import AppKit
import PhotonStackAppCore

@MainActor
enum PhotonStackMenuBarCoordinator {
    private static let trackingObserver = PhotonStackMenuTrackingObserver()

    static func schedule(language: AppLanguage) {
        trackingObserver.start(language: language)
        apply(language: language)
        Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(200))
            apply(language: language)
        }
    }

    private static func apply(language: AppLanguage) {
        guard let mainMenu = NSApp.mainMenu else {
            return
        }

        for item in mainMenu.items {
            guard let submenu = item.submenu else {
                continue
            }

            if isHelpMenu(item: item, submenu: submenu) {
                mainMenu.removeItem(item)
                continue
            }

            if containsAction("orderFrontStandardAboutPanel:", in: submenu) {
                trackingObserver.observe(menu: submenu)
            } else if submenu === NSApp.windowsMenu || ["Window", "窗口"].contains(item.title) {
                trackingObserver.observe(menu: submenu)
                setMenuTitle(
                    chinese: "窗口",
                    english: "Window",
                    item: item,
                    submenu: submenu,
                    language: language
                )
            } else if ["Edit", "编辑"].contains(item.title) || containsAction("undo:", in: submenu) {
                trackingObserver.observe(menu: submenu)
                setMenuTitle(
                    chinese: "编辑",
                    english: "Edit",
                    item: item,
                    submenu: submenu,
                    language: language
                )
            } else if ["View", "显示"].contains(item.title) || containsAction("toggleFullScreen:", in: submenu) {
                trackingObserver.observe(menu: submenu)
                setMenuTitle(
                    chinese: "显示",
                    english: "View",
                    item: item,
                    submenu: submenu,
                    language: language
                )
            }

            localizeItems(in: submenu, language: language)
        }

        for window in NSApp.windows where window.identifier?.rawValue == "com_apple_SwiftUI_Settings_window" {
            window.title = title(
                chinese: "PhotonStack 设置",
                english: "PhotonStack Settings",
                language: language
            )
        }
    }

    private static func isHelpMenu(item: NSMenuItem, submenu: NSMenu) -> Bool {
        let knownTitle = ["Help", "帮助"].contains(item.title)
        return knownTitle || containsAction("showHelp:", in: submenu)
    }

    private static func containsAction(_ selectorName: String, in menu: NSMenu) -> Bool {
        menu.items.contains { item in
            if item.action.map(NSStringFromSelector) == selectorName {
                return true
            }
            return item.submenu.map { containsAction(selectorName, in: $0) } ?? false
        }
    }

    private static func localizeItems(in menu: NSMenu, language: AppLanguage) {
        let settingsItems = menu.items.filter {
            ["Settings…", "设置…"].contains($0.title)
        }
        for duplicate in settingsItems.dropFirst() {
            menu.removeItem(duplicate)
        }

        for item in menu.items {
            if let translation = standardTitles.first(where: {
                item.title == $0.english || item.title == $0.chinese
            }) {
                item.title = title(
                    chinese: translation.chinese,
                    english: translation.english,
                    language: language
                )
            }
            if let submenu = item.submenu {
                localizeItems(in: submenu, language: language)
            }
        }
    }

    fileprivate static func localizeTrackedMenu(_ menu: NSMenu, language: AppLanguage) {
        localizeItems(in: menu, language: language)
    }

    private static func title(chinese: String, english: String, language: AppLanguage) -> String {
        language == .simplifiedChinese ? chinese : english
    }

    private static func setMenuTitle(
        chinese: String,
        english: String,
        item: NSMenuItem,
        submenu: NSMenu,
        language: AppLanguage
    ) {
        let localizedTitle = title(chinese: chinese, english: english, language: language)
        item.title = localizedTitle
        submenu.title = localizedTitle
    }

    private static let standardTitles: [(english: String, chinese: String)] = [
        ("About PhotonStack", "关于 PhotonStack"),
        ("Services", "服务"),
        ("Services Settings…", "服务设置…"),
        ("Settings…", "设置…"),
        ("Hide PhotonStack", "隐藏 PhotonStack"),
        ("Hide Others", "隐藏其他"),
        ("Show All", "全部显示"),
        ("Quit PhotonStack", "退出 PhotonStack"),
        ("Quit and Keep Windows", "退出并保留窗口"),
        ("Undo", "撤销"),
        ("Redo", "重做"),
        ("Cut", "剪切"),
        ("Copy", "复制"),
        ("Paste", "粘贴"),
        ("Delete", "删除"),
        ("Select All", "全选"),
        ("AutoFill", "自动填充"),
        ("Contact…", "联系人…"),
        ("Passwords…", "密码…"),
        ("Credit Card…", "信用卡…"),
        ("Start Dictation…", "开始听写…"),
        ("Emoji & Symbols", "表情与符号"),
        ("Enter Full Screen", "进入全屏幕"),
        ("Exit Full Screen", "退出全屏幕"),
        ("Close", "关闭"),
        ("Close All", "全部关闭"),
        ("Minimize", "最小化"),
        ("Minimize All", "全部最小化"),
        ("Zoom", "缩放"),
        ("Zoom All", "全部缩放"),
        ("Fill", "填充"),
        ("Center", "居中"),
        ("Move & Resize", "移动与调整大小"),
        ("Halves", "二分屏"),
        ("Left", "左侧"),
        ("Right", "右侧"),
        ("Top", "顶部"),
        ("Bottom", "底部"),
        ("Quarters", "四分屏"),
        ("Top Left", "左上"),
        ("Top Right", "右上"),
        ("Bottom Left", "左下"),
        ("Bottom Right", "右下"),
        ("Arrange", "排列"),
        ("Left & Right", "左侧与右侧"),
        ("Left & Quarters", "左侧与四分屏"),
        ("Right & Left", "右侧与左侧"),
        ("Right & Quarters", "右侧与四分屏"),
        ("Top & Bottom", "顶部与底部"),
        ("Top & Quarters", "顶部与四分屏"),
        ("Bottom & Top", "底部与顶部"),
        ("Bottom & Quarters", "底部与四分屏"),
        ("Return to Previous Size", "恢复之前的大小"),
        ("Full Screen Tile", "全屏幕拼贴"),
        ("Left of Screen", "屏幕左侧"),
        ("Right of Screen", "屏幕右侧"),
        ("Remove Window from Set", "从窗口组中移除"),
        ("Bring All to Front", "全部置于前面"),
        ("Arrange in Front", "排列到前面"),
    ]
}

@MainActor
private final class PhotonStackMenuTrackingObserver: NSObject, NSMenuDelegate {
    private var language: AppLanguage = .systemDefault
    private var isObserving = false

    func start(language: AppLanguage) {
        self.language = language
        guard isObserving == false else {
            return
        }
        isObserving = true
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(menuDidBeginTracking(_:)),
            name: NSMenu.didBeginTrackingNotification,
            object: nil
        )
        NotificationCenter.default.addObserver(
            self,
            selector: #selector(menuDidChange(_:)),
            name: NSMenu.didAddItemNotification,
            object: nil
        )
    }

    func observe(menu: NSMenu) {
        menu.delegate = self
    }

    func menuWillOpen(_ menu: NSMenu) {
        localize(menu)
    }

    @objc private func menuDidBeginTracking(_ notification: Notification) {
        guard let menu = notification.object as? NSMenu else {
            return
        }
        localize(menu)
    }

    @objc private func menuDidChange(_ notification: Notification) {
        guard let menu = notification.object as? NSMenu else {
            return
        }
        let language = language
        Task { @MainActor in
            await Task.yield()
            PhotonStackMenuBarCoordinator.localizeTrackedMenu(menu, language: language)
        }
    }

    private func localize(_ menu: NSMenu) {
        PhotonStackMenuBarCoordinator.localizeTrackedMenu(menu, language: language)
        let language = language
        Task { @MainActor in
            try? await Task.sleep(for: .milliseconds(20))
            PhotonStackMenuBarCoordinator.localizeTrackedMenu(menu, language: language)
        }
    }
}
