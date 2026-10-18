#pragma once
#include "common/types.h"
namespace Shader { using PFN_SrtWalker = void (*)(const u32*, u32*);
void BbSrtVmRun(PFN_SrtWalker, const u32*, u32*); }
