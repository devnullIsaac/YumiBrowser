/*
    Yumi SDK — Logging and Debugging WASM Imports
    Copyright (C) 2026  DevNullIsaac

    This program is free software: you can redistribute it and/or modify
    it under the terms of the GNU Affero General Public License as published by
    the Free Software Foundation, either version 3 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU Affero General Public License for more details.

    You should have received a copy of the GNU Affero General Public License
    along with this program.  If not, see <https://www.gnu.org/licenses/>.
*/

#ifndef WASM_LOG_H
#define WASM_LOG_H

/**
 * @file wasm_log.h
 * @brief WebAssembly guest imports for host-side logging and debugging.
 *
 * @details
 * This header provides a lightweight logging interface for WASM guest modules
 * running inside Yumi Browser. All functions are imported from the host
 * environment (`env` module) and are implemented in `log_bindings.c` on the
 * host side.
 *
 * ## Log Levels
 * The host maintains five severity levels. Messages are routed to the
 * appropriate output stream (stdout, stderr, or a dashboard log panel)
 * depending on the level:
 *
 * | Level | Name  | Typical Use Case                              |
 * |-------|-------|-----------------------------------------------|
 * | 0     | DEBUG | Verbose tracing, disabled in release builds   |
 * | 1     | INFO  | General operational messages                  |
 * | 2     | WARN  | Recoverable issues, deprecated API usage      |
 * | 3     | ERROR | Runtime errors that may affect functionality  |
 * | 4     | FATAL | Unrecoverable errors, usually followed by exit|
 *
 * ## Convenience Macros
 * The macros at the bottom of this file (LOG, LOG_DEBUG, LOG_INFO, etc.)
 * compute string lengths at compile time using `sizeof`, eliminating the
 * need to pass explicit lengths for string literals.
 *
 * @code
 *   LOG_INFO("Application started");
 *   LOG_VAL_INT("Player count", 42);
 *   LOG_ASSERT(ptr != NULL, "Pointer must not be null");
 * @endcode
 */

#include <stdint.h>

/* ================================================================== */
/*  Log levels                                                         */
/* ================================================================== */

/**
 * @brief Debug-level severity.
 * @details Used for verbose tracing. May be compiled out in release builds.
 */
#define LOG_LEVEL_DEBUG 0

/**
 * @brief Information-level severity.
 * @details General operational messages that are always useful.
 */
#define LOG_LEVEL_INFO  1

/**
 * @brief Warning-level severity.
 * @details Indicates a potential problem or deprecated usage.
 */
#define LOG_LEVEL_WARN  2

/**
 * @brief Error-level severity.
 * @details Indicates a runtime error that may affect functionality.
 */
#define LOG_LEVEL_ERROR 3

/**
 * @brief Fatal-level severity.
 * @details Indicates an unrecoverable error. Usually followed by termination.
 */
#define LOG_LEVEL_FATAL 4

/** @cond INTERNAL */
#define IMPORT __attribute__((import_module("env")))
/** @endcond */

/* ================================================================== */
/*  Core logging                                                       */
/* ================================================================== */

/**
 * @brief Write a raw string message to the host log.
 *
 * @details
 * Sends a string to the host's logging system without an explicit level.
 * The host typically treats this as INFO-level output.
 *
 * @param[in] ptr  Pointer to the message bytes in WASM linear memory.
 * @param[in] len  Byte length of the message.
 */
IMPORT __attribute__((import_name("log_write")))
void log_write(const char *ptr, int len);

/**
 * @brief Write a string message at a specific log level.
 *
 * @details
 * Sends a string to the host's logging system with an explicit severity.
 * The host may filter, colorize, or route the message based on this level.
 *
 * @param[in] level  Log severity (LOG_LEVEL_DEBUG through LOG_LEVEL_FATAL).
 * @param[in] ptr    Pointer to the message bytes in WASM linear memory.
 * @param[in] len    Byte length of the message.
 */
IMPORT __attribute__((import_name("log_write_level")))
void log_write_level(int level, const char *ptr, int len);

/* ================================================================== */
/*  Quick debug helpers (no string formatting needed)                  */
/* ================================================================== */

/**
 * @brief Print a single signed 32-bit integer.
 *
 * @details
 * Outputs the value in both decimal and hexadecimal notation.
 * Useful for quick printf-free debugging.
 *
 * @param[in] value  The integer to print.
 */
IMPORT __attribute__((import_name("log_int")))
void log_int(int value);

/**
 * @brief Print a single 32-bit floating-point value.
 *
 * @details
 * Outputs the value in decimal notation with reasonable precision.
 *
 * @param[in] value  The float to print.
 */
IMPORT __attribute__((import_name("log_float")))
void log_float(float value);

/**
 * @brief Print two signed 32-bit integers.
 *
 * @details
 * Outputs both values on a single line, separated by a comma.
 *
 * @param[in] a  First integer.
 * @param[in] b  Second integer.
 */
IMPORT __attribute__((import_name("log_int2")))
void log_int2(int a, int b);

/**
 * @brief Print a labeled integer value.
 *
 * @details
 * Outputs a label string followed by an integer, formatted as:
 * `label: value (0xhex)`
 *
 * @param[in] label      Pointer to the label string in WASM memory.
 * @param[in] label_len  Byte length of the label.
 * @param[in] value      The integer value to print.
 */
IMPORT __attribute__((import_name("log_fmt_int")))
void log_fmt_int(const char *label, int label_len, int value);

/**
 * @brief Print a labeled floating-point value.
 *
 * @details
 * Outputs a label string followed by a float, formatted as:
 * `label: value`
 *
 * @param[in] label      Pointer to the label string in WASM memory.
 * @param[in] label_len  Byte length of the label.
 * @param[in] value      The float value to print.
 */
IMPORT __attribute__((import_name("log_fmt_float")))
void log_fmt_float(const char *label, int label_len, float value);

/* ================================================================== */
/*  Assertions                                                         */
/* ================================================================== */

/**
 * @brief Runtime assertion with message.
 *
 * @details
 * If @p condition evaluates to zero (false), the host prints the
 * provided message to stderr and may trigger a breakpoint or abort.
 * The condition value is returned so the macro can be used in expressions.
 *
 * @param[in] condition  Expression to evaluate. Zero triggers the assertion.
 * @param[in] msg        Pointer to the assertion message in WASM memory.
 * @param[in] msg_len    Byte length of the message.
 * @return The value of @p condition.
 */
IMPORT __attribute__((import_name("log_assert")))
int log_assert(int condition, const char *msg, int msg_len);

/** @cond INTERNAL */
#undef IMPORT
/** @endcond */

/* ================================================================== */
/*  Convenience macros                                                 */
/* ================================================================== */

/**
 * @defgroup LogMacros Compile-time string length macros
 * @brief Macros that automatically compute string lengths for string literals.
 *
 * @details
 * These macros wrap the log functions and use `sizeof(string_literal) - 1`
 * to compute the byte length at compile time. They only work with string
 * literals (not char pointers).
 *
 * @{
 */

/** @brief Write a string literal to the log at default level. */
#define LOG(s)       log_write((s), (int)sizeof(s) - 1)

/** @brief Write a string literal at DEBUG level. */
#define LOG_DEBUG(s) log_write_level(LOG_LEVEL_DEBUG, (s), (int)sizeof(s) - 1)

/** @brief Write a string literal at INFO level. */
#define LOG_INFO(s)  log_write_level(LOG_LEVEL_INFO,  (s), (int)sizeof(s) - 1)

/** @brief Write a string literal at WARN level. */
#define LOG_WARN(s)  log_write_level(LOG_LEVEL_WARN,  (s), (int)sizeof(s) - 1)

/** @brief Write a string literal at ERROR level. */
#define LOG_ERROR(s) log_write_level(LOG_LEVEL_ERROR, (s), (int)sizeof(s) - 1)

/** @brief Write a string literal at FATAL level. */
#define LOG_FATAL(s) log_write_level(LOG_LEVEL_FATAL, (s), (int)sizeof(s) - 1)

/** @brief Print a labeled integer using string literals. */
#define LOG_VAL_INT(label, v)   log_fmt_int((label), (int)sizeof(label) - 1, (v))

/** @brief Print a labeled float using string literals. */
#define LOG_VAL_FLOAT(label, v) log_fmt_float((label), (int)sizeof(label) - 1, (v))

/** @brief Assert a condition with a string-literal message. */
#define LOG_ASSERT(cond, msg) log_assert((cond), (msg), (int)sizeof(msg) - 1)

/** @} */

#endif /* WASM_LOG_H */
