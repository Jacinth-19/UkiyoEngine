// `ukiyo_hub --selftest`: headless checks for the hub, its dialog, and the splash. Run with
// QT_QPA_PLATFORM=offscreen in CI; prints PASS/FAIL lines and returns the number of failures.
#pragma once

namespace editor::hub {

int runHubSelfTest();

} // namespace editor::hub
