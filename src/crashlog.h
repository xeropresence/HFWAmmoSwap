#pragma once

namespace crashlog
{
    // Logs faulting address and an unwound stack for access violations to HFWAmmoSwap.log.
    void Install();
}
