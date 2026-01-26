/**
 * @file version.h
 * @brief Version configuration for Cardputer ADV Doom
 * @note Easily configurable version string - just update the numbers below
 */

#pragma once

// Version number components
#define VERSION_MAJOR 0
#define VERSION_MINOR 0
#define VERSION_PATCH 1

// Stringify macros
#define STRINGIFY(x) #x
#define TOSTRING(x) STRINGIFY(x)

// Complete version string
#define VERSION_STRING "v" TOSTRING(VERSION_MAJOR) "." TOSTRING(VERSION_MINOR) "." TOSTRING(VERSION_PATCH)

// Build information (can be extended with build date, git hash, etc.)
#define BUILD_INFO "Cardputer ADV Doom " VERSION_STRING