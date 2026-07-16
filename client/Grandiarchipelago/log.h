#pragma once

namespace grandia_ap {

// Call from DllMain before any other logging (sets path next to grandia.exe).
void InitializeLogging();
void ShutdownLogging();

void LogInfo(const char* fmt, ...);
void LogWarn(const char* fmt, ...);
void LogDebug(const char* fmt, ...);

}  // namespace grandia_ap
