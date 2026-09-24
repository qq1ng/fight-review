#pragma once
// ImGui config for the render harness only: asserts stay on in release builds and report instead of vanishing.
void UishotAssert(const char* aExpr, const char* aFile, int aLine);
#define IM_ASSERT(_EXPR) ((_EXPR) ? (void)0 : UishotAssert(#_EXPR, __FILE__, __LINE__))
