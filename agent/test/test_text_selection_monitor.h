#pragma once

#include "agentxx/expand/text_selection_monitor.h"
#include <iostream>
#include <memory>
#include <string>
#include <thread>

#if XX_IS_WIN_D
#include <windows.h>
#endif

std::shared_ptr<agentxx::expand::TextSelectionMonitor>
test_text_selection_monitor();
