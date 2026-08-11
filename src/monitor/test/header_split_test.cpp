#include "aio_manager.h"
#include "aio_types.h"
#include "callback_registry.h"
#include "engine.h"
#include "engine_type.h"
#include "wake_pipe.h"

#include <cassert>
#include <type_traits>

int main()
{
    using TLSSMON::AioHandle;
    using TLSSMON::AioManager;
    using TLSSMON::CallbackRegistry;
    using TLSSMON::WakeupPipe;

    static_assert(!std::is_copy_constructible_v<AioManager>);

    AioHandle handle;
    assert(!handle);

    CallbackRegistry registry;
    WakeupPipe wakeup;
    AioManager manager(registry, wakeup);
}
