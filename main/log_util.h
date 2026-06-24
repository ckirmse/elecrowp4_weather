#pragma once

// Logging helpers matching the CIC project style (lprintf/eprintf wrappers).
// Defined in main.cc, declared here for use across all translation units.

void lprintf(const char * tag, const char * fmt, ...) __attribute__((format(printf, 2, 3)));
void eprintf(const char * tag, const char * fmt, ...) __attribute__((format(printf, 2, 3)));
