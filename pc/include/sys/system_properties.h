// Windows stand-in for Android system properties. The port's debug switches (debug.boz64.*) are read from
// boz64.props next to the .exe ("name=value" lines) or from environment variables named like the property with
// dots as underscores (debug_boz64_trace=1).
#pragma once
#define PROP_VALUE_MAX 92
extern "C" int __system_property_get(const char* name, char* value);
