// The part of mockgl.cpp the test program talks to (see driver.cpp).
#pragma once

/// Opens the file the trace goes to. Called before the first call to vitaGL.
bool MockGL_Open(const char *path);

/// Closes the trace and says on stderr what was counted. Returns how many things were
/// wrong: errors in what vitaGL was asked to do, calls from more than one thread, and
/// calls from the caller's thread although a render thread was to make them (or the
/// other way round).
unsigned int MockGL_Close(bool expectOtherThread);
