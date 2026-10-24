// Version control, from the editor.
//
// A game project is a directory of files that wants to be in git. The editor can run the three commands that matter —
// status, stage-and-commit, push — and show what changed. They are `git` invocations, not a reimplementation of git: the
// editor decides nothing about merging, branching or history.
//
// What lives here is the part worth testing: turning `git status --porcelain=v1 -b` into entries an editor can list,
// and building the command lines. The process invocation is the platform's (QProcess in the editor).
#pragma once

#include <string>
#include <vector>

namespace ukiyo::vcs {

struct Change {
    enum class Kind { Modified, Added, Deleted, Renamed, Untracked, Conflict, Ignored, Unknown };
    Kind kind = Kind::Unknown;
    std::string path;       // as git reports it, forward slashes
    std::string original;   // the old path of a rename
    bool staged = false;    // the change is in the index
    char index = ' ';       // the raw XY letters, for anything that wants them
    char worktree = ' ';
};

struct Status {
    std::string branch;
    bool detached = false;
    bool ahead = false;
    bool behind = false;
    std::string upstream;
    std::vector<Change> changes;
    // What the git command reported when it failed, so the editor can show it instead of an empty list.
    std::string error;
    bool ok = false;
};

// Parses `git status --porcelain=v1 -b`. `errorText` is the command's stderr, which is what makes a failure readable:
// "not a git repository" is the common one.
Status parseStatus(const std::string& porcelain, const std::string& errorText = std::string());

// True when the project directory is inside a git work tree.
Status readStatus(const std::string& projectDir);

// Command lines. The editor runs each as argv (no shell), so a path with spaces is one argument.
std::vector<std::string> statusArgs();
std::vector<std::string> addAllArgs();
std::vector<std::string> commitArgs(const std::string& message);
std::vector<std::string> pushArgs(const std::string& remote, const std::string& branch);
std::vector<std::string> logArgs(int count);
std::vector<std::string> diffArgs(const std::string& path);

// One line per commit from `git log --pretty=format:%h%x1f%an%x1f%ar%x1f%s`.
struct Commit {
    std::string hash;
    std::string author;
    std::string when;
    std::string subject;
};
std::vector<Commit> parseLog(const std::string& output);

// A commit message that says what an editor commit is, for the empty case: the file list, at most a handful of names.
std::string defaultCommitMessage(const std::vector<Change>& changes);

} // namespace ukiyo::vcs
