// Copyright (c) Microsoft Corporation.
// Licensed under the MIT license.

#include "pch.h"
#include "Remoting.h"

#include "AttachRequest.g.cpp"
#include "CommandlineArgs.g.cpp"
#include "FindTargetWindowArgs.g.cpp"
#include "ProposeCommandlineResult.g.cpp"
#include "RenameRequestArgs.g.cpp"
#include "RequestReceiveContentArgs.g.cpp"
#include "SummonWindowBehavior.g.cpp"
#include "SummonWindowSelectionArgs.g.cpp"
#include "WindowActivatedArgs.g.cpp"
#include "WindowRequestedArgs.g.cpp"

using namespace winrt;
using namespace winrt::Microsoft::Terminal;
using namespace winrt::Windows::Foundation;

namespace winrt::TerminalApp::implementation
{
    void CommandlineArgs::Commandline(const winrt::array_view<const winrt::hstring>& value)
    {
        _args = { value.begin(), value.end() };
    }

    winrt::com_array<winrt::hstring> CommandlineArgs::Commandline()
    {
        return winrt::com_array<winrt::hstring>{ _args.begin(), _args.end() };
    }
}
