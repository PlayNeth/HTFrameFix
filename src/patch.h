// What the other parts use from patch.cpp.
#pragma once
#include "common.h"
#include "game.h"

bool PatchBytes(DWORD at, const void* bytes, size_t len);
bool PatchCall(DWORD at, const void* target, int len);
bool PatchJmp(DWORD at, const void* target, int len);
bool SigsMatch(const ht::Sig* sigs, size_t n);
bool CheckSigs();
template <typename T> static inline T& G(DWORD a) { return *reinterpret_cast<T*>(a); }
