// MH-Forever shim (GPL-3.0-or-later): PPSSPP's logging is dropped; the decryptor reports through its return value.
#pragma once
namespace Log { enum { Loader }; }
#define ERROR_LOG(...) ((void)0)
#define WARN_LOG(...) ((void)0)
#define INFO_LOG(...) ((void)0)
#define DEBUG_LOG(...) ((void)0)
