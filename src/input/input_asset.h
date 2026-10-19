// Reading and writing Unity's `.inputactions` asset, so a team with existing Unity Input Actions can open one here.
//
// The file is JSON, and the shape is Unity's (com.unity.inputsystem, InputActionAsset.ToJson):
//
//   { "name": "...", "maps": [ { "name": "...", "id": "...", "actions": [...], "bindings": [...] } ],
//     "controlSchemes": [ { "name": "...", "bindingGroup": "...", "devices": [ { "devicePath": "<Keyboard>" } ] } ] }
//
// This reader takes the subset the engine implements and reports everything it had to skip, by name, in `warnings`.
// It never silently drops a binding: a binding whose path is a layout the engine does not have, or whose processors
// are unknown, is listed so the caller can decide. The writer emits exactly the fields Unity writes, so a file saved
// here opens in Unity with the same maps, actions and bindings.
#pragma once

#include <string>
#include <vector>

#include "input/input.h"

namespace ukiyo::input {

struct AssetLoadResult {
    bool ok = false;
    std::vector<std::string> warnings;   // things skipped, by name
    std::string error;                   // set when ok is false
};

// Parses an asset from JSON text into `state`, replacing its maps and control schemes. The caller owns the devices:
// load the asset first, add the devices, then enable a map, exactly as Unity builds devices from the asset.
AssetLoadResult loadAssetJson(State& state, const std::string& text);
AssetLoadResult loadAssetFile(State& state, const std::string& path);

// Writes the state back out in Unity's shape. `assetName` defaults to state.assetName.
std::string saveAssetJson(const State& state);
bool saveAssetFile(const State& state, const std::string& path, std::string* error);

// A deterministic GUID for an object that has none, so saving an asset that was built in code produces a stable file
// instead of a new one on every run. `seed` is typically "map/action/binding" with its index.
std::string makeGuid(const std::string& seed);

} // namespace ukiyo::input
