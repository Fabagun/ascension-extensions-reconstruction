#pragma once
// Rewrites the build number in outgoing AUTH_LOGON_CHALLENGE packets so this client reports the
// same build the real Extensions.dll's client does (12344), matching this authserver's realmlist
// gamebuild -- the exe itself is a stock 12340 build, but the server only shows this realm as
// "currently down" for sessions that report a build the realm's gamebuild column doesn't match.
// See FINDINGS.md 2026-09-22 "gamebuild reported by the wire, not the exe" for the measurement.
namespace NetPatch
{
    void Install();
    // Socket hooks only, with the build-field rewrite left off.
    void InstallDiagnostics();
}
