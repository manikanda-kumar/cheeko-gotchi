// Host entry for cheeko_runtime: the app project's app_main() hands the app
// instance (from CHEEKO_APP's CreateCheekoApp()) to the runtime, which owns it.
#pragma once

#include "cheeko.h"

namespace cheeko {
namespace host {

// Brings up the board, calls OnStart(), then runs the event loop forever.
[[noreturn]] void Run(CheekoApp* app);

}  // namespace host
}  // namespace cheeko
