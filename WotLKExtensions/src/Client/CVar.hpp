#pragma once

#include <cstdint>

class CVar
{
public:
    int32_t m_padding[13];

    static void ApplyPatches();

    static int32_t RegisterGlueCVarsCustom();
    // The original's FUN_10114540 queue (list 0x10BDD1A0): drained after the client's world CVar
    // registration 0x51D9B0 (called from world UI load at 0x52AB2D), not at glue. Callbacks queued here
    // may touch world-only client state.
    static void RegisterWorldQueue();

    static int32_t Register(const char* name, const char* description, uint32_t size, const char* defaultVal, void* callback, uint32_t flags, bool a7, int32_t a8, bool a9);
    static void Set(CVar* cVar, int32_t value, bool a3, bool a4, bool a5, bool a6);

    static CVar* Lookup(const char* name);

private:
    CVar() = delete;
    ~CVar() = delete;

    static void FillCustomGlueCVarVector();

    static void AddToGlueCVarVector(const char* name, const char* description, uint32_t size, const char* defaultVal, void* callback, uint32_t flags, bool a7, int32_t a8, bool a9);
};
