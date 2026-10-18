#pragma once
#include "common/types.h"
namespace Core { using AccessViolationHandler = bool (*)(void*, void*);
struct SignalDispatch { AccessViolationHandler h=nullptr; void RegisterAccessViolationHandler(AccessViolationHandler x,u32){h=x;} };
struct Signals { static SignalDispatch* Instance(){ static SignalDispatch d; return &d; } }; }
