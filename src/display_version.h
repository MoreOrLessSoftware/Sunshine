/**
 * @file src/display_version.h
 * @brief Declarations for the version Sunshine shows to people.
 */
#pragma once

// standard includes
#include <string_view>

/**
 * @brief Get the version shown in the log and the web UI.
 *
 * It is worked out on every build (see cmake/scripts/display_version.cmake): the git tag the
 * build was made from, or else its branch and when it was built, as <branch>-YYYY.MM.DD.hhmm.
 * CI builds show their release version. PROJECT_VERSION stays the numeric version, for
 * installers, file versions and comparing against releases.
 *
 * @return The display version.
 */
std::string_view display_version();
