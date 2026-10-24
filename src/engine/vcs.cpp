#include "engine/vcs.h"

#include <cstdio>
#include <sstream>

namespace ukiyo::vcs {

namespace {

Change::Kind kindOf(char c) {
    switch (c) {
        case 'M': return Change::Kind::Modified;
        case 'A': return Change::Kind::Added;
        case 'D': return Change::Kind::Deleted;
        case 'R': return Change::Kind::Renamed;
        case 'C': return Change::Kind::Renamed;
        case 'U': return Change::Kind::Conflict;
        case '?': return Change::Kind::Untracked;
        case '!': return Change::Kind::Ignored;
        default: break;
    }
    return Change::Kind::Unknown;
}

} // namespace

Status parseStatus(const std::string& porcelain, const std::string& errorText) {
    Status s;
    if (!errorText.empty()) {
        s.error = errorText;
        return s;
    }
    std::istringstream in(porcelain);
    std::string line;
    bool first = true;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        if (first && line.rfind("## ", 0) == 0) {
            // "## main...origin/main [ahead 2, behind 1]", "## HEAD (no branch)", "## main"
            std::string rest = line.substr(3);
            const size_t ellipse = rest.find("...");
            if (ellipse != std::string::npos) {
                s.branch = rest.substr(0, ellipse);
                const std::string tail = rest.substr(ellipse + 3);
                const size_t space = tail.find(' ');
                s.upstream = space == std::string::npos ? tail : tail.substr(0, space);
                s.ahead = tail.find("ahead") != std::string::npos;
                s.behind = tail.find("behind") != std::string::npos;
            } else {
                const size_t space = rest.find(' ');
                s.branch = space == std::string::npos ? rest : rest.substr(0, space);
                const std::string tail = space == std::string::npos ? std::string() : rest.substr(space);
                s.ahead = tail.find("ahead") != std::string::npos;
                s.behind = tail.find("behind") != std::string::npos;
            }
            if (s.branch.rfind("HEAD", 0) == 0) {
                s.detached = true;
                s.branch = "HEAD";
            }
            first = false;
            continue;
        }
        first = false;
        if (line.size() < 4) continue;
        Change c;
        c.index = line[0];
        c.worktree = line[1];
        std::string rest = line.substr(3);
        const char x = c.index == '?' ? '?' : (c.index != ' ' ? c.index : c.worktree);
        c.kind = kindOf(x);
        c.staged = c.index != ' ' && c.index != '?';
        // A rename is "R  old -> new".
        const size_t arrow = rest.find(" -> ");
        if (arrow != std::string::npos) {
            c.original = rest.substr(0, arrow);
            rest = rest.substr(arrow + 4);
        }
        c.path = rest;
        s.changes.push_back(c);
    }
    s.ok = true;
    return s;
}

std::vector<std::string> statusArgs() { return {"status", "--porcelain=v1", "-b"}; }
std::vector<std::string> addAllArgs() { return {"add", "-A"}; }
std::vector<std::string> commitArgs(const std::string& message) { return {"commit", "-m", message}; }
std::vector<std::string> pushArgs(const std::string& remote, const std::string& branch) {
    if (remote.empty() || branch.empty()) return {"push"};
    return {"push", remote, branch};
}
std::vector<std::string> logArgs(int count) {
    return {"log", "-n", std::to_string(count < 1 ? 1 : count), "--pretty=format:%h%x1f%an%x1f%ar%x1f%s"};
}
std::vector<std::string> diffArgs(const std::string& path) { return {"diff", "--", path}; }

std::vector<Commit> parseLog(const std::string& output) {
    std::vector<Commit> out;
    std::istringstream in(output);
    std::string line;
    while (std::getline(in, line)) {
        if (line.empty()) continue;
        Commit c;
        size_t start = 0;
        std::string fields[4];
        for (int i = 0; i < 4; ++i) {
            const size_t sep = line.find('\x1f', start);
            if (i == 3) {
                fields[3] = line.substr(start);
                break;
            }
            if (sep == std::string::npos) break;
            fields[i] = line.substr(start, sep - start);
            start = sep + 1;
        }
        c.hash = fields[0];
        c.author = fields[1];
        c.when = fields[2];
        c.subject = fields[3];
        if (!c.hash.empty()) out.push_back(c);
    }
    return out;
}

std::string defaultCommitMessage(const std::vector<Change>& changes) {
    if (changes.empty()) return "Editor snapshot";
    std::string msg = changes.size() == 1 ? "Edit " : "Edits: ";
    for (size_t i = 0; i < changes.size() && i < 3; ++i) {
        if (i) msg += ", ";
        msg += changes[i].path;
    }
    if (changes.size() > 3) msg += " and " + std::to_string(changes.size() - 3) + " more";
    return msg;
}

} // namespace ukiyo::vcs
