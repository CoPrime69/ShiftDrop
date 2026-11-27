import Foundation

// Recipes: every wheel slice is a small JSON recipe (same format as the Windows app, docs/recipes.md).
// "icon" is an SF Symbol name here (the Windows recipes carry a Segoe Fluent code point instead).

struct Step {
    var action: String
    var params: [String: Any]
    func int(_ key: String, _ fallback: Int) -> Int { (params[key] as? Int) ?? fallback }
    func string(_ key: String, _ fallback: String) -> String { (params[key] as? String) ?? fallback }
}

struct Recipe {
    var id: String
    var label: String
    var detail: String
    var ring: String        // convert, tools, reduce, files
    var suffix: String
    var icon: String?
    var steps: [Step]
}

enum RecipeStore {
    /// The built-in slices: shiftdrop-macos/recipes/builtin.json, the one data file (copied into the app's
    /// Resources by build_app.sh). Same recipe format as the Windows app, docs/recipes.md.
    static let all: [Recipe] = {
        guard let url = builtInURL(), let text = try? String(contentsOf: url, encoding: .utf8) else {
            assertionFailure("builtin.json is missing")
            return []
        }
        return parse(text)
    }()

    private static func builtInURL() -> URL? {
        if let bundled = Bundle.main.url(forResource: "builtin", withExtension: "json", subdirectory: "recipes") { return bundled }
        #if DEBUG
        // swift run / swift test: straight from the source tree (this file is app/Sources/ShiftDrop/).
        return URL(fileURLWithPath: #filePath).deletingLastPathComponent().deletingLastPathComponent().deletingLastPathComponent()
            .deletingLastPathComponent().appendingPathComponent("recipes/builtin.json")
        #else
        return nil
        #endif
    }

    static func find(_ id: String) -> Recipe? { all.first { $0.id == id } }

    /// User recipes: ~/Library/Application Support/ShiftDrop/recipes/*.json (same id replaces a built-in).
    static var userFolder: URL {
        FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0].appendingPathComponent("ShiftDrop/recipes")
    }

    static func loadUser() -> [Recipe] {
        let files = (try? FileManager.default.contentsOfDirectory(at: userFolder, includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.pathExtension == "json" }.sorted { $0.lastPathComponent < $1.lastPathComponent }.flatMap { url -> [Recipe] in
            guard let data = try? Data(contentsOf: url), let text = String(data: data, encoding: .utf8) else { return [] }
            return parse(stripComments(text))
        }
    }

    private static func stripComments(_ s: String) -> String {
        s.split(separator: "\n", omittingEmptySubsequences: false).filter { !$0.trimmingCharacters(in: .whitespaces).hasPrefix("//") }.joined(separator: "\n")
    }

    static func parse(_ text: String) -> [Recipe] {
        guard let data = text.data(using: .utf8), let json = try? JSONSerialization.jsonObject(with: data) else { return [] }
        let items = (json as? [[String: Any]]) ?? ((json as? [String: Any]).map { [$0] } ?? [])
        return items.compactMap { d in
            guard let id = d["id"] as? String, let label = d["label"] as? String, let steps = d["steps"] as? [[String: Any]], !steps.isEmpty else { return nil }
            return Recipe(id: id, label: label, detail: (d["description"] as? String) ?? label, ring: (d["ring"] as? String) ?? "convert",
                          suffix: (d["suffix"] as? String) ?? "converted", icon: d["icon"] as? String,
                          steps: steps.compactMap { s in (s["action"] as? String).map { Step(action: $0, params: (s["params"] as? [String: Any]) ?? [:]) } })
        }
    }
}
