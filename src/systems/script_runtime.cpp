#include "systems/script_runtime.h"

#include <algorithm>

namespace ukiyo {

void ScriptRuntime::setSearchPaths(std::vector<std::string> roots) {
    roots_ = std::move(roots);
    for (auto& host : hosts_) host->setSearchPaths(roots_);
}

ScriptHost& ScriptRuntime::ensure(const std::string& entity) {
    if (ScriptHost* host = find(entity)) return *host;
    auto host = std::make_unique<ScriptHost>();
    if (setup_) setup_(*host);
    host->setSearchPaths(roots_);
    host->setEntityName(entity);
    hosts_.push_back(std::move(host));
    entities_.push_back(entity);
    paths_.push_back(std::string());
    refreshInfo();
    return *hosts_.back();
}

bool ScriptRuntime::add(const std::string& entity, const std::string& path, std::string* error) {
    auto host = std::make_unique<ScriptHost>();
    if (setup_) setup_(*host);
    host->setSearchPaths(roots_);
    host->setEntityName(entity);
    std::string why;
    if (!host->loadFile(path, &why)) {
        if (error) *error = (entity.empty() ? std::string("script") : entity) + ": " + why;
        return false;
    }
    // A reload keeps the script's place in the order: the entity's turn in the lifecycle must not change because the
    // file was edited.
    for (size_t i = 0; i < hosts_.size(); ++i) {
        if (entities_[i] != entity) continue;
        hosts_[i] = std::move(host);
        paths_[i] = path;
        refreshInfo();
        return true;
    }
    hosts_.push_back(std::move(host));
    entities_.push_back(entity);
    paths_.push_back(path);
    refreshInfo();
    return true;
}

bool ScriptRuntime::reload(const std::string& entity, std::string* error) {
    const ScriptInfo* old = nullptr;
    for (const ScriptInfo& i : info_)
        if (i.entity == entity) old = &i;
    if (!old) {
        if (error) *error = "no script for " + (entity.empty() ? std::string("the scene") : entity);
        return false;
    }
    const std::string path = old->path;
    return add(entity, path, error);
}

void ScriptRuntime::remove(const std::string& entity) {
    for (size_t i = 0; i < hosts_.size(); ++i) {
        if (entities_[i] != entity) continue;
        hosts_.erase(hosts_.begin() + static_cast<long>(i));
        entities_.erase(entities_.begin() + static_cast<long>(i));
        paths_.erase(paths_.begin() + static_cast<long>(i));
        refreshInfo();
        return;
    }
}

void ScriptRuntime::clear() {
    hosts_.clear();
    entities_.clear();
    paths_.clear();
    info_.clear();
}

ScriptHost* ScriptRuntime::find(const std::string& entity) {
    for (size_t i = 0; i < hosts_.size(); ++i)
        if (entities_[i] == entity) return hosts_[i].get();
    return nullptr;
}

void ScriptRuntime::callStart() {
    for (auto& h : hosts_) h->callStart();
    refreshInfo();
}

void ScriptRuntime::callUpdate(float dt) {
    for (auto& h : hosts_) h->callUpdate(dt);
    refreshInfo();
}

void ScriptRuntime::callSceneLoaded() {
    for (auto& h : hosts_) h->callSceneLoaded();
    refreshInfo();
}

void ScriptRuntime::callDestroy() {
    for (auto& h : hosts_) h->callDestroy();
    refreshInfo();
}

bool ScriptRuntime::anyFailed() const {
    for (const auto& h : hosts_)
        if (h->hasFailed()) return true;
    return false;
}

std::string ScriptRuntime::lastError() const {
    for (size_t i = 0; i < hosts_.size(); ++i) {
        if (hosts_[i]->error().empty()) continue;
        const std::string& where = entities_[i];
        return (where.empty() ? std::string("script: ") : where + ": ") + hosts_[i]->lastError();
    }
    return {};
}

std::vector<std::string> ScriptRuntime::entitiesWith(const char* fn) const {
    std::vector<std::string> out;
    for (size_t i = 0; i < hosts_.size(); ++i)
        if (!hosts_[i]->hasFailed() && hosts_[i]->hasFunction(fn)) out.push_back(entities_[i]);
    return out;
}

void ScriptRuntime::refreshInfo() {
    info_.clear();
    info_.reserve(hosts_.size());
    for (size_t i = 0; i < hosts_.size(); ++i) {
        ScriptInfo entry;
        entry.entity = entities_[i];
        entry.path = paths_[i];
        entry.loaded = hosts_[i]->loaded();
        entry.stopped = hosts_[i]->hasFailed();
        entry.error = hosts_[i]->lastError();
        info_.push_back(entry);
    }
}

} // namespace ukiyo
