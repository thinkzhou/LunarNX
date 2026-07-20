#pragma once

#include <string>

namespace lunar::ui {

/// Applies the persisted language override before Borealis initializes i18n.
void configureAppLocale();

/// Returns auto, en-US, zh-Hans, or zh-Hant. Invalid values fall back to auto.
std::string getConfiguredLanguage();

/// Persists a validated language override while preserving other config keys.
bool saveConfiguredLanguage(const std::string& language);

/// Returns the restart notice in the newly selected language.
std::string getLanguageRestartNotice(const std::string& language);

} // namespace lunar::ui
