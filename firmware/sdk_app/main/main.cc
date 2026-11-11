// app_main shim: hand the CHEEKO_APP() instance to the runtime.
#include "cheeko_host.h"

extern "C" cheeko::CheekoApp* CreateCheekoApp();

extern "C" void app_main() { cheeko::host::Run(CreateCheekoApp()); }
