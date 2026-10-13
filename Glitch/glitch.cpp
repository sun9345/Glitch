// Console image glitcher.
// The user chooses the sequence of operations at runtime (repeats allowed).
// Settings and effects are in glitch_core.h.
//
// Build (g++):  g++ -std=c++17 -O2 glitch.cpp -o glitch
// Build (MSVC): cl /std:c++17 /O2 /EHsc glitch.cpp

#include "glitch_core.h"

// Parses input like "1 3 5 5 6" or "1,3,5" into zero-based operation indices.
// Operations that need a second image are rejected when only one file was given.
static bool parseSequence(const std::string& line, bool hasMap,
                          std::vector<size_t>& seq, std::string& err) {
    seq.clear();
    std::string token;
    auto flush = [&]() -> bool {
        if (token.empty()) return true;
        if (token.size() > 6) { err = "number too large: " + token; return false; }
        size_t n = std::stoul(token);
        token.clear();
        if (n < 1 || n > OPERATIONS.size()) {
            err = "no operation with number " + std::to_string(n);
            return false;
        }
        if (OPERATIONS[n - 1].runWithMap && !hasMap) {
            err = "operation " + std::to_string(n) + " (" + OPERATIONS[n - 1].name +
                  ") needs two images; restart and enter two file paths";
            return false;
        }
        seq.push_back(n - 1);
        return true;
    };
    for (char ch : line) {
        if (std::isdigit(static_cast<unsigned char>(ch))) {
            token += ch;
        } else if (ch == ' ' || ch == ',' || ch == ';' || ch == '\t' || ch == '\r') {
            if (!flush()) return false;
        } else {
            err = std::string("unexpected character '") + ch + "'";
            return false;
        }
    }
    if (!flush()) return false;
    if (seq.empty()) { err = "no operations entered"; return false; }
    return true;
}

// Parses the file prompt: one path, or two paths (image + displacement map).
// Paths containing spaces should be quoted (drag & drop adds quotes automatically).
static bool parsePaths(const std::string& line, std::vector<fs::path>& out, std::string& err) {
    out.clear();
    auto isSpace = [](char c) { return std::isspace(static_cast<unsigned char>(c)) != 0; };

    size_t a = 0, b = line.size();
    while (a < b && isSpace(line[a])) ++a;
    while (b > a && isSpace(line[b - 1])) --b;
    std::string s = line.substr(a, b - a);
    if (s.empty()) { err = "no path entered"; return false; }

    std::error_code ec;

    // 1) The whole line as a single path (also handles unquoted paths with spaces)
    std::string whole = s;
    if (whole.size() >= 2 && (whole.front() == '"' || whole.front() == '\'') &&
        whole.back() == whole.front())
        whole = whole.substr(1, whole.size() - 2);
    if (fs::is_regular_file(fs::path(whole), ec)) {
        out.emplace_back(whole);
        return true;
    }

    // 2) Otherwise split into tokens (quote-aware)
    std::vector<std::string> tokens;
    for (size_t i = 0; i < s.size();) {
        if (isSpace(s[i])) { ++i; continue; }
        std::string t;
        if (s[i] == '"' || s[i] == '\'') {
            char q = s[i++];
            while (i < s.size() && s[i] != q) t += s[i++];
            if (i < s.size()) ++i;
        } else {
            while (i < s.size() && !isSpace(s[i])) t += s[i++];
        }
        tokens.push_back(t);
    }
    if (tokens.empty() || tokens.size() > 2) {
        err = "enter one or two file paths (wrap paths that contain spaces in quotes)";
        return false;
    }
    for (const std::string& t : tokens) {
        if (!fs::is_regular_file(fs::path(t), ec)) {
            err = "file not found: " + t;
            return false;
        }
        out.emplace_back(t);
    }
    return true;
}

int main() {
    Image img, map;
    bool hasMap = false;
    fs::path src;

    // Ask for the file(s): one image, or two (primary image + displacement map)
    while (true) {
        std::cout << "Path to image (add a second path to use it as a displacement map): ";
        std::string line;
        if (!std::getline(std::cin, line)) return 1;

        std::vector<fs::path> paths;
        std::string err;
        if (!parsePaths(line, paths, err)) {
            std::cout << "Error: " << err << "\n";
            continue;
        }
        if (!loadImage(paths[0], img)) {
            std::cout << "Error: failed to read the image " << paths[0].string()
                      << " (supported: JPG, PNG, BMP, TGA, etc.)\n";
            continue;
        }
        hasMap = (paths.size() == 2);
        if (hasMap && !loadImage(paths[1], map)) {
            std::cout << "Error: failed to read the displacement map " << paths[1].string() << "\n";
            continue;
        }
        src = paths[0];
        break;
    }
    std::cout << "Image: " << img.w << "x" << img.h << "\n";
    if (hasMap) std::cout << "Displacement map: " << map.w << "x" << map.h << "\n";

    // Copy in the same directory: name_glitch.ext (JPG stays JPG, everything else -> PNG)
    std::string ext = src.extension().string();
    std::string lower = ext;
    std::transform(lower.begin(), lower.end(), lower.begin(),
                   [](unsigned char c) { return std::tolower(c); });
    if (lower != ".jpg" && lower != ".jpeg") ext = ".png";
    fs::path dst = src.parent_path() / (src.stem().string() + "_glitch" + ext);

    // Ask which operations to apply
    std::cout << "\nAvailable operations:\n";
    for (size_t i = 0; i < OPERATIONS.size(); ++i) {
        std::cout << "  " << (i + 1) << ") " << OPERATIONS[i].name;
        if (OPERATIONS[i].runWithMap)
            std::cout << (hasMap ? "  [uses the second image as map]"
                                 : "  [unavailable: needs two images]");
        std::cout << "\n";
    }
    std::cout << "Enter operation numbers in the order to apply them (repeats allowed),\n"
                 "separated by spaces or commas, e.g. 1 3 5 5 6\n";

    std::vector<size_t> sequence;
    while (true) {
        std::cout << "Sequence: ";
        std::string line;
        if (!std::getline(std::cin, line)) return 1;
        std::string err;
        if (parseSequence(line, hasMap, sequence, err)) break;
        std::cout << "Error: " << err << ". Try again.\n";
    }

    // Apply the operations one by one
    for (size_t step = 0; step < sequence.size(); ++step) {
        const Operation& op = OPERATIONS[sequence[step]];
        std::cout << "[" << (step + 1) << "/" << sequence.size() << "] " << op.name << "\n";
        if (op.runWithMap) op.runWithMap(img, map);
        else               op.run(img);
    }

    if (!saveImage(dst, img)) {
        std::cerr << "Failed to save file: " << dst.string() << "\n";
        return 1;
    }
    std::cout << "Done: " << dst.string() << "\n";
    return 0;
}
